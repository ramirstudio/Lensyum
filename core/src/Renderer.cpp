#include "lensyum/Renderer.h"
#include "lensyum/Parallel.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <cstdint>
#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace lensyum {

namespace {

constexpr int kMaxLevel = 5;
constexpr int kChannels = 5; // premultiplied RGB, alpha coverage, geometric coverage
constexpr int TF = PsfAtlas::kTexelFloats;
constexpr double kIrisSign = -1.0;

struct Entry {
    int32_t x, y;
    float w;
};

// A splat: one pixel, or at coarse levels a block of pixels merged into one.
struct Splat {
    float x, y; // centre in buffer pixels
    float c[4]; // weighted premultiplied colour
    float w;    // weight (geometric coverage)
    float s;    // signed blur radius, full-resolution pixels
};

struct Agg {
    float c[4], w, sw, xw, yw;
};

struct Source {
    float c[4]; // premultiplied linear RGBA after highlight boost
    float s;    // signed blur radius, full-resolution pixels
    bool highlight;
};

inline float luma(const float* c) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; }

double sampleDepth(const DefocusSettings& d, double u, double v) {
    // u, v address the frame edge to edge; texel centres sit at half-pixel offsets.
    const double x = clampv(u * d.depthW - 0.5, 0.0, d.depthW - 1.0);
    const double y = clampv(v * d.depthH - 0.5, 0.0, d.depthH - 1.0);
    const int x0 = std::min(static_cast<int>(x), d.depthW - 2 < 0 ? 0 : d.depthW - 2);
    const int y0 = std::min(static_cast<int>(y), d.depthH - 2 < 0 ? 0 : d.depthH - 2);
    const int x1 = std::min(x0 + 1, d.depthW - 1), y1 = std::min(y0 + 1, d.depthH - 1);
    const double fx = x - x0, fy = y - y0;
    const float* m = d.depth;
    const double a = lerp(m[y0 * d.depthW + x0], m[y0 * d.depthW + x1], fx);
    const double b = lerp(m[y1 * d.depthW + x0], m[y1 * d.depthW + x1], fx);
    return lerp(a, b, fy);
}

double depthToDistance(const DefocusSettings& d, double v) {
    const double t = d.whiteIsNear ? 1.0 - v : v; // 0 = near, 1 = far
    const double n = std::max(d.nearMm, 1.0), f = std::max(d.farMm, n + 1.0);
    if (d.inverseDepth) return 1.0 / lerp(1.0 / n, 1.0 / f, t);
    return lerp(n, f, t);
}

// Signed blur radius (full-res px) as a function of object distance, from the real lens:
// the paraxial image of each distance against the sensor, times the marginal ray slope.
class DepthToBlur {
public:
    DepthToBlur(const OpticsSettings& o, double focusMm, double nearMm, double farMm) {
        LensSettings ls = o.lens;
        ls.focusDistanceMm = focusMm;
        const LensSystem L(lensPreset(o.lensPreset), ls);
        const double pxPerMm = o.frameWidthPx / std::max(o.sensorWidthMm, 1.0);
        const double k = L.marginalSlope();
        minD_ = std::max(L.efl() * 1.2, 1.0);
        invNear_ = 1.0 / std::max(std::min(nearMm, farMm), minD_);
        invFar_ = 1.0 / std::max(std::max(nearMm, farMm), minD_ + 1.0);
        for (int i = 0; i < kLut; ++i) {
            const double inv = lerp(invNear_, invFar_, i / double(kLut - 1));
            const double zi = L.imageZ(1.0 / inv);
            lut_[i] = static_cast<float>(k * (L.sensorZ() - zi) * pxPerMm);
        }
    }
    float operator()(double distMm) const {
        const double inv = 1.0 / std::max(distMm, minD_);
        const double t = clampv((inv - invNear_) / (invFar_ - invNear_), 0.0, 1.0) * (kLut - 1);
        const int i = std::min(static_cast<int>(t), kLut - 2);
        return static_cast<float>(lerp(lut_[i], lut_[i + 1], t - i));
    }

private:
    static constexpr int kLut = 512;
    float lut_[kLut];
    double invNear_, invFar_, minD_;
};

struct LevelBuffer {
    int w = 0, h = 0;
    std::vector<float> acc;
    bool used = false;
};

} // namespace

void renderDefocus(const Image& srcIn, Image& dst, const RenderSettings& rsIn) {
    RenderSettings rs = rsIn;
    const bool lzTime = std::getenv("LENSYUM_TIME") != nullptr;
    auto lzT0 = std::chrono::steady_clock::now();
    auto lzMark = [&](const char* what) { if (!lzTime) return; auto t = std::chrono::steady_clock::now(); std::fprintf(stderr, "  %-10s %.3fs\n", what, std::chrono::duration<double>(t - lzT0).count()); lzT0 = t; };
    const int W = srcIn.width, H = srcIn.height;
    dst.resize(W, H);
    if (W <= 0 || H <= 0) return;

    const FrameMapping& fm = rs.frame;
    const double dsx = std::max(fm.downsampleX, 1e-3), dsy = std::max(fm.downsampleY, 1e-3);
    const double par = std::max(rs.optics.pixelAspect, 1e-3);
    rs.optics.frameWidthPx = fm.layerW;
    rs.optics.frameHeightPx = fm.layerH;
    const double maxBlur = std::max(rs.optics.maxBlurPx, 1.0);

    // The bokeh grid view feeds the engine a field of point lights instead of the layer.
    Image gridSrc;
    const Image* srcP = &srcIn;
    if (rs.view == RenderSettings::kBokehGrid) {
        gridSrc.resize(W, H);
        double r = std::fabs(rs.defocus.mode == DefocusSettings::kUniform ? rs.defocus.amountPx : 0.5 * maxBlur);
        r = clampv(r, 4.0, maxBlur);
        if (rs.defocus.mode == DefocusSettings::kUniform && rs.defocus.amountPx < 0) r = -r;
        rs.defocus.mode = DefocusSettings::kUniform;
        rs.defocus.amountPx = r;
        rs.defocus.scale = 1.0;
        for (int i = 0; i < W * H; ++i) gridSrc.rgba[i * 4 + 3] = 1.0f;
        const int cols = 9, rows = 5;
        const float energy = static_cast<float>(1.0 + 0.45 * kPi * r * r * dsx * dsy);
        for (int j = 0; j < rows; ++j)
            for (int i = 0; i < cols; ++i) {
                const double lx = fm.layerW * (0.06 + 0.88 * i / (cols - 1)) * dsx - fm.originX;
                const double ly = fm.layerH * (0.08 + 0.84 * j / (rows - 1)) * dsy - fm.originY;
                const int x = static_cast<int>(lx), y = static_cast<int>(ly);
                if (x < 0 || y < 0 || x >= W || y >= H) continue;
                float* p = gridSrc.px(x, y);
                p[0] = p[1] = p[2] = energy;
            }
        rs.highlights.gain = 0;
        srcP = &gridSrc;
    }
    const Image& src = *srcP;

    lzMark("start");
    std::shared_ptr<const PsfAtlas> atlasP = acquirePsfAtlas(rs.optics);
    const PsfAtlas& A = *atlasP;
    std::shared_ptr<const ApertureTexture> apP = acquireApertureTexture(rs.optics.aperture);
    const ApertureTexture& ap = *apP;

    lzMark("atlas+iris");
    // ---- Per-pixel source data -------------------------------------------------------
    // Large per-frame buffers live per thread and are reused across frames (After Effects keeps
    // its render threads), so pages are not faulted in again on every render.
    thread_local std::vector<Source> srcsTL;
    thread_local std::vector<float> accTL, outTL;
    srcsTL.resize(static_cast<size_t>(W) * H);
    std::vector<Source>& srcs = srcsTL;
    const DefocusSettings& df = rs.defocus;
    const bool useDepth = df.mode == DefocusSettings::kDepthMap && df.depth && df.depthW > 0 && df.depthH > 0;
    std::unique_ptr<DepthToBlur> d2b;
    if (useDepth) {
        double focus = df.focusMm;
        if (df.focusFromPoint)
            focus = depthToDistance(df, sampleDepth(df, df.focusPointX / fm.layerW, df.focusPointY / fm.layerH));
        d2b = std::make_unique<DepthToBlur>(rs.optics, focus, df.nearMm, df.farMm);
    }
    const double fcPx = rs.fieldCurvatureMm * A.marginalSlope * A.pxPerMm;
    const double filmPx = rs.filmbackOffsetMm * A.marginalSlope * A.pxPerMm;
    const double halfDiagFc = 0.5 * std::hypot(fm.layerW * par, fm.layerH);
    const double thr = rs.highlights.threshold;
    const double gain = std::max(rs.highlights.gain, 0.0);
    const double hiRange = std::max(1.0 - thr, 0.02);

    parallelFor(H, [&](int y) {
        for (int x = 0; x < W; ++x) {
            Source& S = srcs[static_cast<size_t>(y) * W + x];
            const float* c = src.px(x, y);
            for (int k = 0; k < 4; ++k) S.c[k] = c[k];
            const float a = std::max(c[3], 1e-5f);
            const double L = luma(c) / a;
            if (gain > 0 && L > thr) {
                const double t = std::min((L - thr) / hiRange, 1.0);
                const float boost = static_cast<float>(1.0 + gain * t * t);
                S.c[0] *= boost; S.c[1] *= boost; S.c[2] *= boost;
            }
            S.highlight = L >= thr;

            const double lx = (fm.originX + x + 0.5) / dsx, ly = (fm.originY + y + 0.5) / dsy;
            double s = df.amountPx;
            if (useDepth) s = (*d2b)(depthToDistance(df, sampleDepth(df, lx / fm.layerW, ly / fm.layerH)));
            s += filmPx;
            if (fcPx != 0.0) {
                // Field curvature: the plane of focus bows, corners defocus by fieldCurvatureMm.
                const double h2 = (((lx - fm.centerX) * par) * ((lx - fm.centerX) * par) + (ly - fm.centerY) * (ly - fm.centerY)) / (halfDiagFc * halfDiagFc);
                s += fcPx * std::min(h2, 1.5);
            }
            s *= df.scale;
            S.s = static_cast<float>(clampv(s, -maxBlur, maxBlur));
        }
    });

    if (rs.view == RenderSettings::kBlurMap) {
        parallelFor(H, [&](int y) {
            for (int x = 0; x < W; ++x) {
                const Source& S = srcs[static_cast<size_t>(y) * W + x];
                const float base = 0.2f * std::min(luma(S.c), 1.0f);
                const float m = static_cast<float>(std::min(std::fabs(S.s) / maxBlur, 1.0));
                float* o = dst.px(x, y);
                if (S.s >= 0) { o[0] = base + 0.95f * m; o[1] = base + 0.55f * m; o[2] = base + 0.15f * m; }
                else          { o[0] = base + 0.15f * m; o[1] = base + 0.55f * m; o[2] = base + 0.95f * m; }
                if (std::fabs(S.s) < 0.5f) { o[0] = o[1] = o[2] = base + 0.35f; }
                o[3] = 1.0f;
            }
        });
        return;
    }

    lzMark("sources");
    // ---- Depth slices -------------------------------------------------------------------
    const float dsMax = static_cast<float>(std::max(dsx, dsy));
    auto g = [](double s) { return (s < 0 ? -1.0 : 1.0) * std::log2(1.0 + std::fabs(s) / 1.5); };
    std::vector<float> rowMin(H), rowMax(H);
    parallelFor(H, [&](int y) {
        float lo = 1e30f, hi = -1e30f;
        for (int x = 0; x < W; ++x) {
            const float v = static_cast<float>(g(srcs[static_cast<size_t>(y) * W + x].s));
            lo = std::min(lo, v); hi = std::max(hi, v);
        }
        rowMin[y] = lo; rowMax[y] = hi;
    });
    const double gmin = *std::min_element(rowMin.begin(), rowMin.end());
    const double gmax = *std::max_element(rowMax.begin(), rowMax.end());
    int K = std::max(1, rs.layers);
    if (gmax - gmin < 1e-4) K = 1;
    else K = std::min(K, 1 + static_cast<int>(std::ceil((gmax - gmin) / 0.08)));

    // Slice and level of every pixel (parallel), then counted buckets filled in raster order.
    const QualityProfile qp = qualityProfile(rs.optics.quality);
    const int buckets = K * (kMaxLevel + 1);
    std::vector<uint16_t> bucketOf(static_cast<size_t>(W) * H);
    parallelFor(H, [&](int y) {
        for (int x = 0; x < W; ++x) {
            const Source& S = srcs[static_cast<size_t>(y) * W + x];
            const double rBuf = std::fabs(S.s) * dsMax;
            const double cap = S.highlight ? qp.levelCapHigh : qp.levelCap;
            int level = 0;
            if (rBuf > cap) level = std::min(kMaxLevel, static_cast<int>(std::ceil(std::log2(rBuf / cap))));
            int k = 0;
            if (K > 1) {
                const double lc = (g(S.s) - gmin) / (gmax - gmin) * (K - 1);
                const int k0 = std::min(static_cast<int>(lc), K - 1);
                // Dithered slice assignment: avoids banding between slices without splatting twice.
                k = (k0 + 1 < K && hashUnit(hash2(x, y, 0x2Fu)) < lc - k0) ? k0 + 1 : k0;
            }
            bucketOf[static_cast<size_t>(y) * W + x] = static_cast<uint16_t>(k * (kMaxLevel + 1) + level);
        }
    });
    lzMark("assign");
    std::vector<size_t> counts(buckets, 0);
    std::vector<float> bucketMaxR(buckets, 0.0f);
    for (size_t i = 0; i < bucketOf.size(); ++i) {
        ++counts[bucketOf[i]];
        bucketMaxR[bucketOf[i]] = std::max(bucketMaxR[bucketOf[i]], std::fabs(srcs[i].s));
    }
    std::vector<std::vector<std::vector<Entry>>> lists(K, std::vector<std::vector<Entry>>(kMaxLevel + 1));
    std::vector<std::vector<float>> listMaxR(K, std::vector<float>(kMaxLevel + 1, 0.0f));
    for (int bk = 0; bk < buckets; ++bk) {
        lists[bk / (kMaxLevel + 1)][bk % (kMaxLevel + 1)].reserve(counts[bk]);
        listMaxR[bk / (kMaxLevel + 1)][bk % (kMaxLevel + 1)] = bucketMaxR[bk];
    }
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const int bk = bucketOf[static_cast<size_t>(y) * W + x];
            lists[bk / (kMaxLevel + 1)][bk % (kMaxLevel + 1)].push_back({x, y, 1.0f});
        }

    lzMark("lists");
    // ---- Buffers ----------------------------------------------------------------------------
    std::vector<LevelBuffer> levels(kMaxLevel + 1);
    for (int L = 0; L <= kMaxLevel; ++L) {
        levels[L].w = (W + (1 << L) - 1) >> L;
        levels[L].h = (H + (1 << L) - 1) >> L;
    }
    levels[0].acc.swap(accTL);
    levels[0].acc.assign(static_cast<size_t>(W) * H * kChannels, 0.0f);
    outTL.assign(static_cast<size_t>(W) * H * kChannels, 0.0f);
    std::vector<float>& out = outTL;

    const double halfDiag = 0.5 * std::hypot(fm.layerW * par, fm.layerH);
    const double U = A.extent;
    const float squeeze = static_cast<float>(std::max(rs.squeeze, 0.1));

    lzMark("prep");
    // ---- Per-render kernel bank ----------------------------------------------------------
    // Light passing the iris for every atlas texel. With a round iris the result does not depend
    // on the field angle, so kernels are baked once (3 floats per texel) and the inner loop is a
    // plain bilinear fetch. Otherwise the iris is applied per pixel and only the normalising mass
    // (measured with the iris at the reference angle) is precomputed.
    const ApertureShape& aper = rs.optics.aperture;
    const bool lobesFace = aper.lobes > 0 && aper.lobesFaceCenter && aper.lobeCount >= 2;
    const bool irisRound = aper.isRound();
    const size_t mipTotal = A.mips.size();
    std::vector<size_t> bakedOff(mipTotal, 0);
    size_t bakedSize = 0;
    for (size_t i = 0; i < mipTotal; ++i) { bakedOff[i] = bakedSize; bakedSize += static_cast<size_t>(A.mips[i].res) * A.mips[i].res * 3; }
    std::vector<float> baked(irisRound ? bakedSize : 0);
    std::vector<float> mass(mipTotal * 3, 0.0f);
    parallelFor(static_cast<int>(mipTotal / A.mipCount), [&](int entry) {
        for (int m = 0; m < A.mipCount; ++m) {
            const size_t mi = static_cast<size_t>(entry) * A.mipCount + m;
            const PsfAtlas::Mip& mp = A.mips[mi];
            const float* T0 = A.data(mp);
            float* B = irisRound ? baked.data() + bakedOff[mi] : nullptr;
            double sum[3] = {0, 0, 0};
            for (int t = 0; t < mp.res * mp.res; ++t) {
                const float* v = T0 + static_cast<size_t>(t) * TF;
                const float light = v[0] + v[1] + v[2];
                float tr = 0.0f;
                if (light > 1e-12f) {
                    const float px = v[3] / light, py = v[4] / light;
                    tr = irisRound ? ap.sample(std::sqrt(px * px + py * py), 0.0) : ap.sample(kIrisSign * px, -kIrisSign * py);
                    if (lobesFace && tr > 0 && std::sqrt(px * px + py * py) > aper.lobeEdge(px, py)) tr = 0;
                }
                for (int c = 0; c < 3; ++c) {
                    const float w = v[c] * tr;
                    if (B) B[t * 3 + c] = w;
                    sum[c] += w;
                }
            }
            for (int c = 0; c < 3; ++c) mass[mi * 3 + c] = static_cast<float>(sum[c]);
        }
    });

    lzMark("bank");
    // ---- Splat one depth slice at one level ---------------------------------------------
    auto splatList = [&](const std::vector<Splat>& list, float maxR, int L) {
        LevelBuffer& lb = levels[L];
        const int scaleL = 1 << L;
        const double ax = par * squeeze * scaleL / dsx; // level px -> full-res physical units
        const double ay = scaleL / dsy;
        const double maxHy = 2.5 * maxR / ay + 2.0;
        const int bandH = 16;
        const int bands = (lb.h + bandH - 1) / bandH;

        parallelFor(bands, [&](int band) {
            const int by0 = band * bandH, by1 = std::min(lb.h, by0 + bandH);
            // Splats whose footprint may touch this band (list is sorted by y).
            const float yLo = static_cast<float>((by0 - maxHy - 1) * scaleL);
            const float yHi = static_cast<float>((by1 + maxHy + 1) * scaleL);
            auto itLo = std::lower_bound(list.begin(), list.end(), yLo, [](const Splat& e, float v) { return e.y < v; });
            for (auto it = itLo; it != list.end() && it->y <= yHi; ++it) {
                const Splat& S = *it;
                const double cxL = S.x / scaleL, cyL = S.y / scaleL;
                const double R = std::fabs(S.s);
                const double rBuf = R * dsMax / scaleL;

                if (rBuf < 0.5) {
                    // In focus (or tiny blur): deposit straight into the pixel.
                    const int px = static_cast<int>(cxL), py = static_cast<int>(cyL);
                    if (py < by0 || py >= by1 || px >= lb.w) continue;
                    float* a = &lb.acc[(static_cast<size_t>(py) * lb.w + px) * kChannels];
                    a[0] += S.c[0]; a[1] += S.c[1]; a[2] += S.c[2]; a[3] += S.c[3]; a[4] += S.w;
                    continue;
                }

                // Field position of this pixel.
                const double lx = (fm.originX + S.x) / dsx, ly = (fm.originY + S.y) / dsy;
                double vx = (lx - fm.centerX) * par, vy = ly - fm.centerY;
                const double vl = std::hypot(vx, vy);
                double ex = 0, ey = 1;
                if (vl > 1e-9) { ex = vx / vl; ey = vy / vl; }
                const double fpos = std::min(vl / halfDiag, 1.0) * (A.fieldCount - 1);
                int fi = static_cast<int>(fpos);
                if (fi < A.fieldCount - 1 && hashUnit(hash2(static_cast<int>(S.x * 4), static_cast<int>(S.y * 4), 0x51u)) < fpos - fi) ++fi;
                const int di = A.nearestDefocus(S.s);

                // Mip whose texel is about one output pixel.
                const double texPx = 2.0 * U * R / (A.res * std::max(ax, ay));
                int m = 0;
                while (m + 1 < A.mipCount && texPx * (1 << m) < 0.7) ++m;
                const size_t mi = (static_cast<size_t>(fi) * A.defocusCount + di) * A.mipCount + m;
                const PsfAtlas::Mip& mip = A.mips[mi];
                const float* T = A.data(mip);
                const float* B = irisRound ? baked.data() + bakedOff[mi] : nullptr;
                const float* M = &mass[mi * 3];
                const int res = mip.res;
                const double du = 2.0 * U / res;
                const double invR = 1.0 / R;

                auto deposit = [&]() {
                    // Nothing of this entry passes the iris: keep the energy as a point.
                    const int px = std::min(static_cast<int>(cxL), lb.w - 1), py = static_cast<int>(cyL);
                    if (py < by0 || py >= by1) return;
                    float* a = &lb.acc[(static_cast<size_t>(py) * lb.w + px) * kChannels];
                    a[0] += S.c[0]; a[1] += S.c[1]; a[2] += S.c[2]; a[3] += S.c[3]; a[4] += S.w;
                };
                if (M[1] <= 1e-12f) { deposit(); continue; }

                const double hx = mip.maxU * R / ax, hy = mip.maxU * R / ay;
                const int fx0 = std::max(0, static_cast<int>(std::floor(cxL - hx)));
                const int fx1 = std::min(lb.w - 1, static_cast<int>(std::ceil(cxL + hx)));
                const int fy0 = std::max(0, static_cast<int>(std::floor(cyL - hy)));
                const int fy1 = std::min(lb.h - 1, static_cast<int>(std::ceil(cyL + hy)));
                const int ry0 = std::max(fy0, by0), ry1 = std::min(fy1, by1 - 1);
                if (ry0 > ry1 || fx0 > fx1) continue;

                // Pupil position (entry frame) -> iris texture, which is fixed to the lens and seen
                // upright in background bokeh. The entry frame's x axis is (ey, -ex) in pixels.
                auto iris = [&](float pxu, float pyu) {
                    const double qx = pxu * ey + pyu * ex;
                    const double qy = -pxu * ex + pyu * ey;
                    float t = ap.sample(kIrisSign * qx, -kIrisSign * qy);
                    // Lobes that face the centre live in the entry frame, which turns with the field.
                    if (lobesFace && t > 0 && std::sqrt(pxu * pxu + pyu * pyu) > aper.lobeEdge(pxu, pyu)) t = 0;
                    return t;
                };
                auto kernel = [&](int i, int j, float* w3) {
                    const double qx = ax * (i + 0.5 - cxL), qy = ay * (j + 0.5 - cyL);
                    const double ux = (qx * ey - qy * ex) * invR, uy = (qx * ex + qy * ey) * invR;
                    const double tx = (ux + U) / du - 0.5, ty = (uy + U) / du - 0.5;
                    if (tx < 0 || ty < 0 || tx >= res - 1 || ty >= res - 1) { w3[0] = w3[1] = w3[2] = 0; return; }
                    const int x0 = static_cast<int>(tx), y0 = static_cast<int>(ty);
                    const float fx = static_cast<float>(tx - x0), fy = static_cast<float>(ty - y0);
                    if (B) {
                        const float* t00 = B + (static_cast<size_t>(y0) * res + x0) * 3;
                        const float* t10 = t00 + static_cast<size_t>(res) * 3;
                        for (int c = 0; c < 3; ++c) {
                            const float a = t00[c] + (t00[c + 3] - t00[c]) * fx;
                            const float b = t10[c] + (t10[c + 3] - t10[c]) * fx;
                            w3[c] = a + (b - a) * fy;
                        }
                        return;
                    }
                    const float* t00 = T + (static_cast<size_t>(y0) * res + x0) * TF;
                    const float* t10 = t00 + static_cast<size_t>(res) * TF;
                    float v[TF];
                    for (int c = 0; c < TF; ++c) {
                        const float a = t00[c] + (t00[c + TF] - t00[c]) * fx;
                        const float b = t10[c] + (t10[c + TF] - t10[c]) * fx;
                        v[c] = a + (b - a) * fy;
                    }
                    const float light = v[0] + v[1] + v[2];
                    if (light <= 1e-12f) { w3[0] = w3[1] = w3[2] = 0; return; }
                    const float t = iris(v[3] / light, v[4] / light);
                    w3[0] = v[0] * t; w3[1] = v[1] * t; w3[2] = v[2] * t;
                };

                // Normalisation: exact for small footprints; otherwise the precomputed light passing
                // the iris against the area one texel covers in output pixels.
                float norm[3];
                const int fw = fx1 - fx0 + 1, fh = fy1 - fy0 + 1;
                if (fw * fh <= 64) {
                    double sum[3] = {0, 0, 0};
                    float w3[3];
                    for (int j = fy0; j <= fy1; ++j)
                        for (int i = fx0; i <= fx1; ++i) { kernel(i, j, w3); sum[0] += w3[0]; sum[1] += w3[1]; sum[2] += w3[2]; }
                    if (sum[1] <= 1e-12) { deposit(); continue; }
                    for (int c = 0; c < 3; ++c) norm[c] = sum[c] > 1e-12 ? static_cast<float>(1.0 / sum[c]) : 0.0f;
                } else {
                    const double area = ax * ay * invR * invR / (du * du);
                    for (int c = 0; c < 3; ++c) norm[c] = M[c] > 1e-12f ? static_cast<float>(area / M[c]) : 0.0f;
                }
                const float cr = S.c[0] * norm[0], cg = S.c[1] * norm[1], cb = S.c[2] * norm[2];
                const float ca = S.c[3] * norm[1], cw = S.w * norm[1];

                float w3[3];
                for (int j = ry0; j <= ry1; ++j) {
                    float* row = &lb.acc[static_cast<size_t>(j) * lb.w * kChannels];
                    for (int i = fx0; i <= fx1; ++i) {
                        kernel(i, j, w3);
                        if (w3[1] == 0 && w3[0] == 0 && w3[2] == 0) continue;
                        float* a = row + static_cast<size_t>(i) * kChannels;
                        a[0] += cr * w3[0]; a[1] += cg * w3[1]; a[2] += cb * w3[2];
                        a[3] += ca * w3[1]; a[4] += cw * w3[1];
                    }
                }
            }
        });
    };

    // ---- Far to near: splat, merge levels, composite -------------------------------------
    std::vector<Splat> splats;
    std::vector<size_t> touched;
    std::vector<std::vector<Agg>> aggGrid(kMaxLevel + 1);
    for (int k = K - 1; k >= 0; --k) {
        bool any = false;
        for (int L = 0; L <= kMaxLevel; ++L) any = any || !lists[k][L].empty();
        if (!any) continue;

        // levels[0] is cleared row by row as each slice is composited.
        for (int L = 1; L <= kMaxLevel; ++L) {
            levels[L].used = !lists[k][L].empty();
            if (levels[L].used) levels[L].acc.assign(static_cast<size_t>(levels[L].w) * levels[L].h * kChannels, 0.0f);
        }
        for (int L = 0; L <= kMaxLevel; ++L) {
            const std::vector<Entry>& entries = lists[k][L];
            if (entries.empty()) continue;
            splats.clear();
            if (L == 0) {
                for (const Entry& e : entries) {
                    const Source& S = srcs[static_cast<size_t>(e.y) * W + e.x];
                    splats.push_back({e.x + 0.5f, e.y + 0.5f, {S.c[0] * e.w, S.c[1] * e.w, S.c[2] * e.w, S.c[3] * e.w}, e.w, S.s});
                }
            } else {
                // Merge the pixels of each 2^L block: at this blur size the difference is invisible
                // and the splat count drops by 4^L.
                const int lw = levels[L].w;
                std::vector<Agg>& grid = aggGrid[L];
                if (grid.empty()) grid.assign(static_cast<size_t>(lw) * levels[L].h, Agg{{0, 0, 0, 0}, 0, 0, 0, 0});
                touched.clear();
                for (const Entry& e : entries) {
                    const Source& S = srcs[static_cast<size_t>(e.y) * W + e.x];
                    const size_t cell = static_cast<size_t>(e.y >> L) * lw + (e.x >> L);
                    Agg& g = grid[cell];
                    if (g.w == 0) touched.push_back(cell);
                    for (int c = 0; c < 4; ++c) g.c[c] += S.c[c] * e.w;
                    g.w += e.w; g.sw += S.s * e.w;
                    g.xw += (e.x + 0.5f) * e.w; g.yw += (e.y + 0.5f) * e.w;
                }
                std::sort(touched.begin(), touched.end());
                for (size_t cell : touched) {
                    Agg& g = grid[cell];
                    const float inv = 1.0f / g.w;
                    splats.push_back({g.xw * inv, g.yw * inv, {g.c[0], g.c[1], g.c[2], g.c[3]}, g.w, g.sw * inv});
                    g = Agg{{0, 0, 0, 0}, 0, 0, 0, 0};
                }
                std::stable_sort(splats.begin(), splats.end(), [](const Splat& a, const Splat& b) { return a.y < b.y; });
            }
            splatList(splats, listMaxR[k][L], L);
        }

        parallelFor(H, [&](int y) {
            float* row = &levels[0].acc[static_cast<size_t>(y) * W * kChannels];
            for (int L = 1; L <= kMaxLevel; ++L) {
                const LevelBuffer& lb = levels[L];
                if (!lb.used) continue;
                const double sc = 1.0 / (1 << L);
                const double fy = (y + 0.5) * sc - 0.5;
                const int y0 = clampv(static_cast<int>(std::floor(fy)), 0, lb.h - 1);
                const int y1 = std::min(y0 + 1, lb.h - 1);
                const float ty = static_cast<float>(clampv(fy - std::floor(fy), 0.0, 1.0));
                for (int x = 0; x < W; ++x) {
                    const double fx = (x + 0.5) * sc - 0.5;
                    const int x0 = clampv(static_cast<int>(std::floor(fx)), 0, lb.w - 1);
                    const int x1 = std::min(x0 + 1, lb.w - 1);
                    const float tx = static_cast<float>(clampv(fx - std::floor(fx), 0.0, 1.0));
                    const float* a00 = &lb.acc[(static_cast<size_t>(y0) * lb.w + x0) * kChannels];
                    const float* a01 = &lb.acc[(static_cast<size_t>(y0) * lb.w + x1) * kChannels];
                    const float* a10 = &lb.acc[(static_cast<size_t>(y1) * lb.w + x0) * kChannels];
                    const float* a11 = &lb.acc[(static_cast<size_t>(y1) * lb.w + x1) * kChannels];
                    // Each level pixel holds 4^L full-res pixels worth of energy.
                    const float e = 1.0f / static_cast<float>(1 << (2 * L));
                    float* o = row + static_cast<size_t>(x) * kChannels;
                    for (int c = 0; c < kChannels; ++c) {
                        const float a = a00[c] + (a01[c] - a00[c]) * tx;
                        const float b = a10[c] + (a11[c] - a10[c]) * tx;
                        o[c] += (a + (b - a) * ty) * e;
                    }
                }
            }
            float* dstRow = &out[static_cast<size_t>(y) * W * kChannels];
            for (int x = 0; x < W; ++x) {
                float* s = row + static_cast<size_t>(x) * kChannels;
                float* o = dstRow + static_cast<size_t>(x) * kChannels;
                float cov = s[4];
                if (cov > 1.0f) {
                    const float inv = 1.0f / cov;
                    for (int c = 0; c < kChannels; ++c) s[c] *= inv;
                    cov = 1.0f;
                }
                const float a = std::min(s[3], 1.0f);
                o[0] = s[0] + (1.0f - a) * o[0];
                o[1] = s[1] + (1.0f - a) * o[1];
                o[2] = s[2] + (1.0f - a) * o[2];
                o[3] = s[3] + (1.0f - a) * o[3];
                o[4] = cov + (1.0f - cov) * o[4];
                s[0] = s[1] = s[2] = s[3] = s[4] = 0.0f;
            }
        });
    }

    accTL.swap(levels[0].acc);
    lzMark("slices");
    const float blend = static_cast<float>(clampv(rs.blendBack, 0.0, 1.0));
    // Renormalise by geometric coverage: fills the gaps left where hidden background would be.
    parallelFor(H, [&](int y) {
        for (int x = 0; x < W; ++x) {
            const float* o = &out[(static_cast<size_t>(y) * W + x) * kChannels];
            float* d = dst.px(x, y);
            if (o[4] > 1e-5f) {
                const float inv = 1.0f / o[4];
                d[0] = o[0] * inv; d[1] = o[1] * inv; d[2] = o[2] * inv;
                d[3] = std::min(o[3] * inv, 1.0f);
                if (blend < 1.0f && rs.view == RenderSettings::kResult) {
                    const float* sp = srcIn.px(x, y);
                    for (int c = 0; c < 4; ++c) d[c] = sp[c] + (d[c] - sp[c]) * blend;
                }
            } else {
                d[0] = d[1] = d[2] = d[3] = 0.0f;
            }
        }
    });
}

} // namespace lensyum
