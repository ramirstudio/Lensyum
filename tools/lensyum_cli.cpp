// Command-line harness for the Lensyum engine: renders test scenes or images to PPM so the
// optics can be checked without After Effects.
//
//   lensyum_cli grid  out.ppm [key=value ...]
//   lensyum_cli scene out.ppm [key=value ...]
//   lensyum_cli image in.ppm out.ppm [depth=depth.pgm] [key=value ...]
//   lensyum_cli lenses

#include "lensyum/Renderer.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

using namespace lensyum;

namespace {

float srgbToLinear(float v) { return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); }
float linearToSrgb(float v) {
    v = std::max(0.0f, std::min(v, 1.0f));
    return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
}

bool readPnm(const char* path, int& w, int& h, int& channels, std::vector<float>& data) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    char magic[3] = {0};
    int maxv = 0;
    if (std::fscanf(f, "%2s %d %d %d", magic, &w, &h, &maxv) != 4) { std::fclose(f); return false; }
    std::fgetc(f);
    channels = std::strcmp(magic, "P6") == 0 ? 3 : 1;
    std::vector<unsigned char> raw(static_cast<size_t>(w) * h * channels);
    const size_t n = std::fread(raw.data(), 1, raw.size(), f);
    std::fclose(f);
    if (n != raw.size()) return false;
    data.resize(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) data[i] = raw[i] / float(maxv);
    return true;
}

void writePpm(const char* path, const Image& img) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return;
    std::fprintf(f, "P6\n%d %d\n255\n", img.width, img.height);
    for (int y = 0; y < img.height; ++y)
        for (int x = 0; x < img.width; ++x) {
            const float* p = img.px(x, y);
            const float a = p[3];
            for (int c = 0; c < 3; ++c) {
                // Composite over black for viewing.
                const unsigned char v = static_cast<unsigned char>(std::lround(linearToSrgb(p[c] * (a > 0 ? 1.0f : 0.0f)) * 255.0f));
                std::fputc(v, f);
            }
        }
    std::fclose(f);
}

struct Args {
    std::map<std::string, std::string> kv;
    double num(const char* k, double d) const { auto it = kv.find(k); return it == kv.end() ? d : std::atof(it->second.c_str()); }
    std::string str(const char* k, const char* d) const { auto it = kv.find(k); return it == kv.end() ? d : it->second; }
};

void applyArgs(const Args& a, RenderSettings& rs) {
    OpticsSettings& o = rs.optics;
    o.lensPreset = static_cast<int>(a.num("lens", 0));
    o.lens.focalLengthMm = a.num("f", lensPreset(o.lensPreset).focalMm);
    o.lens.fNumber = a.num("N", 2.0);
    o.lens.focusDistanceMm = a.num("focus", 3.0) * 1000.0;
    o.lens.dispersion = a.num("ca", 1.0);
    o.lens.vignetting = a.num("cateye", 1.0);
    o.sensorWidthMm = a.num("sensor", 36.0);
    o.aperture.blades = static_cast<int>(a.num("blades", 0));
    o.aperture.curvature = a.num("curv", 0.0);
    o.aperture.rotationRad = a.num("rot", 0.0) * kPi / 180.0;
    o.aperture.obstruction = a.num("obst", 0.0);
    o.aperture.onion = a.num("onion", 0.0);
    o.aperture.onionFreq = a.num("onionfreq", 6.0);
    o.aperture.texture = a.num("texture", 0.0);
    o.aperture.textureScale = a.num("texscale", 1.0);
    o.aperture.textureSeed = static_cast<int>(a.num("seed", 0));
    o.impression = a.num("imp", 0.0);
    o.coma = a.num("coma", 0.0);
    o.astigmatismMm = a.num("astig", 0.0);
    o.aperture.lobes = a.num("lobes", 0.0);
    o.aperture.lobeCount = static_cast<int>(a.num("lobecount", 5));
    rs.fieldCurvatureMm = a.num("fc", 0.0);
    o.maxBlurPx = a.num("maxblur", 150.0);
    o.quality = static_cast<int>(a.num("quality", 1));
    rs.squeeze = a.num("squeeze", 1.0);
    rs.highlights.threshold = a.num("thr", 0.85);
    rs.highlights.gain = a.num("gain", 0.0);
    rs.defocus.amountPx = a.num("amount", 30.0);
    rs.defocus.scale = a.num("scale", 1.0);
    rs.layers = static_cast<int>(a.num("layers", 12));
    rs.view = static_cast<int>(a.num("view", 0));
    if (a.kv.count("mask")) {
        int w, h, c;
        std::vector<float> d;
        if (readPnm(a.str("mask", "").c_str(), w, h, c, d)) {
            o.aperture.maskW = w; o.aperture.maskH = h;
            o.aperture.mask.resize(static_cast<size_t>(w) * h);
            for (int i = 0; i < w * h; ++i) o.aperture.mask[i] = d[static_cast<size_t>(i) * c];
        }
    }
}

void setFrame(RenderSettings& rs, int w, int h) {
    rs.frame.layerW = w; rs.frame.layerH = h;
    rs.frame.centerX = 0.5 * w; rs.frame.centerY = 0.5 * h;
}

// Night street: far lights, an in-focus textured plane, near out-of-focus lights and a pole.
void buildScene(int W, int H, Image& img, std::vector<float>& dist) {
    img.resize(W, H);
    dist.assign(static_cast<size_t>(W) * H, 0.0f);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            float* p = img.px(x, y);
            const float t = y / float(H);
            p[0] = 0.010f + 0.02f * t; p[1] = 0.012f + 0.015f * t; p[2] = 0.030f; p[3] = 1.0f;
            dist[static_cast<size_t>(y) * W + x] = 40000.0f;
        }
    // Far lights.
    for (int i = 0; i < 260; ++i) {
        const uint32_t hsh = hash32(i * 977u + 13u);
        const int x = static_cast<int>(hashUnit(hsh) * W), y = static_cast<int>((0.05f + 0.55f * hashUnit(hash32(hsh))) * H);
        const float hue = hashUnit(hash32(hsh + 7));
        const float e = 2.0f + 10.0f * hashUnit(hash32(hsh + 9));
        float* p = img.px(x, y);
        p[0] = e * (0.9f + 0.3f * hue); p[1] = e * (0.6f + 0.2f * hue); p[2] = e * (0.25f + 0.6f * (1 - hue));
        dist[static_cast<size_t>(y) * W + x] = 30000.0f + 20000.0f * hashUnit(hash32(hsh + 3));
    }
    // In-focus plane at 3 m: checker panel in the middle.
    const int x0 = W * 3 / 10, x1 = W * 7 / 10, y0 = H * 35 / 100, y1 = H * 80 / 100;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            float* p = img.px(x, y);
            const bool c = ((x / 24) + (y / 24)) & 1;
            const float v = c ? 0.55f : 0.08f;
            p[0] = v; p[1] = v * 0.95f; p[2] = v * 0.9f;
            dist[static_cast<size_t>(y) * W + x] = 3000.0f;
        }
    // Near pole at 0.8 m.
    for (int y = 0; y < H; ++y)
        for (int x = W / 12; x < W / 12 + W / 30; ++x) {
            float* p = img.px(x, y);
            p[0] = 0.02f; p[1] = 0.02f; p[2] = 0.02f;
            dist[static_cast<size_t>(y) * W + x] = 800.0f;
        }
    // Near lights at 1 m, lower right.
    for (int i = 0; i < 6; ++i) {
        const int x = W * (70 + 5 * i) / 100, y = H * (85 + (i % 2) * 6) / 100;
        float* p = img.px(x, y);
        p[0] = 12.0f; p[1] = 8.0f; p[2] = 3.0f;
        dist[static_cast<size_t>(y) * W + x] = 1000.0f;
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: lensyum_cli grid|scene|image|lenses ...\n");
        return 1;
    }
    const std::string mode = argv[1];
    if (mode == "lenses") {
        for (int i = 0; i < lensPresetCount(); ++i) {
            LensSettings s;
            s.fNumber = 0.5;
            s.focalLengthMm = lensPreset(i).focalMm;
            const LensSystem L(lensPreset(i), s);
            std::printf("%d  %-28s  EFL %.2f mm  widest f/%.2f  marginal slope %.3f\n", i, lensPreset(i).name, L.efl(), L.openFNumber(),
                        L.marginalSlope());
        }
        return 0;
    }

    Args a;
    int firstKv = mode == "image" ? 4 : 3;
    for (int i = firstKv; i < argc; ++i) {
        const char* eq = std::strchr(argv[i], '=');
        if (eq) a.kv[std::string(argv[i], static_cast<size_t>(eq - argv[i]))] = eq + 1;
    }
    RenderSettings rs;
    applyArgs(a, rs);

    Image src;
    std::vector<float> depth;
    const char* outPath = argv[2];
    const int W = static_cast<int>(a.num("w", 1280)), H = static_cast<int>(a.num("h", 720));

    if (mode == "grid") {
        src.resize(W, H);
        rs.view = RenderSettings::kBokehGrid;
    } else if (mode == "scene") {
        std::vector<float> dist;
        buildScene(W, H, src, dist);
        const double nearMm = 500, farMm = 60000;
        depth.resize(dist.size());
        for (size_t i = 0; i < dist.size(); ++i) depth[i] = static_cast<float>((1.0 / dist[i] - 1.0 / farMm) / (1.0 / nearMm - 1.0 / farMm));
        rs.defocus.mode = DefocusSettings::kDepthMap;
        rs.defocus.depth = depth.data(); rs.defocus.depthW = W; rs.defocus.depthH = H;
        rs.defocus.whiteIsNear = true; rs.defocus.inverseDepth = true;
        rs.defocus.nearMm = nearMm; rs.defocus.farMm = farMm;
        rs.defocus.focusMm = rs.optics.lens.focusDistanceMm;
    } else if (mode == "image") {
        int w, h, c;
        std::vector<float> d;
        if (argc < 4 || !readPnm(argv[2], w, h, c, d)) { std::fprintf(stderr, "cannot read %s\n", argv[2]); return 1; }
        outPath = argv[3];
        src.resize(w, h);
        for (int i = 0; i < w * h; ++i) {
            for (int k = 0; k < 3; ++k) src.rgba[i * 4 + k] = srgbToLinear(d[static_cast<size_t>(i) * c + (c == 3 ? k : 0)]);
            src.rgba[i * 4 + 3] = 1.0f;
        }
        if (a.kv.count("depth")) {
            int dw, dh, dc;
            std::vector<float> dd;
            if (readPnm(a.str("depth", "").c_str(), dw, dh, dc, dd)) {
                depth.resize(static_cast<size_t>(dw) * dh);
                for (int i = 0; i < dw * dh; ++i) depth[i] = dd[static_cast<size_t>(i) * dc];
                rs.defocus.mode = DefocusSettings::kDepthMap;
                rs.defocus.depth = depth.data(); rs.defocus.depthW = dw; rs.defocus.depthH = dh;
                rs.defocus.whiteIsNear = a.num("whitenear", 1) != 0;
                rs.defocus.inverseDepth = a.num("inverse", 1) != 0;
                rs.defocus.nearMm = a.num("near", 0.5) * 1000; rs.defocus.farMm = a.num("far", 50) * 1000;
                rs.defocus.focusMm = rs.optics.lens.focusDistanceMm;
                if (a.kv.count("px")) {
                    rs.defocus.focusFromPoint = true;
                    rs.defocus.focusPointX = a.num("px", 0.5) * w; rs.defocus.focusPointY = a.num("py", 0.5) * h;
                }
            }
        }
    } else {
        std::fprintf(stderr, "unknown mode\n");
        return 1;
    }
    setFrame(rs, src.width, src.height);

    Image out;
    const auto t0 = std::chrono::steady_clock::now();
    acquirePsfAtlas([&] { OpticsSettings o = rs.optics; o.frameWidthPx = src.width; o.frameHeightPx = src.height; return o; }());
    const auto t1 = std::chrono::steady_clock::now();
    renderDefocus(src, out, rs);
    const auto t2 = std::chrono::steady_clock::now();
    std::fprintf(stderr, "atlas %.2fs  render %.2fs  (%dx%d)\n", std::chrono::duration<double>(t1 - t0).count(),
                 std::chrono::duration<double>(t2 - t1).count(), out.width, out.height);
    writePpm(outPath, out);
    return 0;
}
