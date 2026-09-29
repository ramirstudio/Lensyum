#include "lensyum/PsfAtlas.h"
#include "lensyum/Parallel.h"

#include <algorithm>
#include <cmath>
#include <list>
#include <mutex>
#include <utility>

namespace lensyum {

QualityProfile qualityProfile(int quality) {
    switch (clampv(quality, 0, 3)) {
    case 0: return {112, 64, 10, 11, 4.0, 16.0};
    case 1: return {160, 80, 12, 13, 8.0, 40.0};
    case 2: return {240, 96, 16, 15, 12.0, 64.0};
    default: return {360, 112, 20, 16, 24.0, 1e9};
    }
}

uint64_t OpticsSettings::hash() const {
    Hasher h;
    h.add(lensPreset);
    h.add(lens.focalLengthMm); h.add(lens.fNumber); h.add(lens.focusDistanceMm);
    h.add(lens.dispersion); h.add(lens.vignetting);
    h.add(impression); h.add(coma); h.add(astigmatismMm);
    h.add(sensorWidthMm); h.add(frameWidthPx); h.add(frameHeightPx); h.add(pixelAspect);
    h.add(maxBlurPx); h.add(quality);
    return h.h;
}

int PsfAtlas::nearestDefocus(double s) const {
    const int half = defocusCount / 2;
    const double rmin = defocusPx[half];
    const double ratio = half > 1 ? defocusPx[half + 1] / rmin : 2.0;
    const double a = std::max(std::fabs(s), rmin);
    const int j = clampv(static_cast<int>(std::lround(std::log(a / rmin) / std::log(ratio))), 0, half - 1);
    return s >= 0 ? half + j : half - 1 - j;
}

namespace {

constexpr int TF = PsfAtlas::kTexelFloats;

struct RaySample {
    float px, py, pz; // exit position
    float sx, sy;     // exit slope dx/dz, dy/dz
    float ex, ey;     // coma offset per mm of |defocus|
    float ix, iy;     // impression offset per mm of signed defocus
    float ux, uy;     // pupil position in stop radii
    int band;         // spectral sample index
};

void blurBinomial(std::vector<float>& img, int res) {
    std::vector<float> tmp(img.size());
    for (int y = 0; y < res; ++y)
        for (int x = 0; x < res; ++x)
            for (int c = 0; c < TF; ++c) {
                const float l = img[(y * res + std::max(x - 1, 0)) * TF + c];
                const float m = img[(y * res + x) * TF + c];
                const float r = img[(y * res + std::min(x + 1, res - 1)) * TF + c];
                tmp[(y * res + x) * TF + c] = 0.25f * l + 0.5f * m + 0.25f * r;
            }
    for (int y = 0; y < res; ++y)
        for (int x = 0; x < res; ++x)
            for (int c = 0; c < TF; ++c) {
                const float t = tmp[(std::max(y - 1, 0) * res + x) * TF + c];
                const float m = tmp[(y * res + x) * TF + c];
                const float b = tmp[(std::min(y + 1, res - 1) * res + x) * TF + c];
                img[(y * res + x) * TF + c] = 0.25f * t + 0.5f * m + 0.25f * b;
            }
}

float maxRadius(const float* img, int res, double extent) {
    float peak = 0;
    for (int i = 0; i < res * res; ++i) peak = std::max(peak, img[i * TF] + img[i * TF + 1] + img[i * TF + 2]);
    const float thr = peak * 1e-4f;
    const double du = 2.0 * extent / res;
    float r = 0;
    for (int y = 0; y < res; ++y)
        for (int x = 0; x < res; ++x) {
            const float* t = img + (y * res + x) * TF;
            if (t[0] + t[1] + t[2] <= thr) continue;
            const double ux = std::fabs((x + 0.5) * du - extent) + du;
            const double uy = std::fabs((y + 0.5) * du - extent) + du;
            r = std::max(r, static_cast<float>(std::hypot(ux, uy)));
        }
    return std::max(r, static_cast<float>(du));
}

} // namespace

std::shared_ptr<const PsfAtlas> buildPsfAtlas(const OpticsSettings& s) {
    const QualityProfile q = qualityProfile(s.quality);
    auto atlas = std::make_shared<PsfAtlas>();
    PsfAtlas& A = *atlas;

    const LensSystem L(lensPreset(s.lensPreset), s.lens);
    const double pxPerMm = s.frameWidthPx / std::max(s.sensorWidthMm, 1.0);
    const double halfDiagMm = 0.5 * std::hypot(s.frameWidthPx * s.pixelAspect, s.frameHeightPx) / pxPerMm;
    const double k = L.marginalSlope();
    const double dist = std::min(L.focusDistance(), 1e7);

    A.efl = L.efl();
    A.fNumber = L.fNumber();
    A.marginalSlope = k;
    A.pxPerMm = pxPerMm;
    A.fieldCount = q.fieldCount;
    A.res = q.psfRes;
    A.extent = 1.75;

    // Signed defocus entries, geometric in blur radius.
    const int per = q.defocusPerSign;
    const double rmin = 0.75, rmax = std::max(s.maxBlurPx, 2.0);
    std::vector<double> radii(per);
    for (int j = 0; j < per; ++j) radii[j] = rmin * std::pow(rmax / rmin, j / double(per - 1));
    A.defocusPx.clear();
    for (int j = per - 1; j >= 0; --j) A.defocusPx.push_back(-radii[j]);
    for (int j = 0; j < per; ++j) A.defocusPx.push_back(radii[j]);
    A.defocusCount = 2 * per;

    // Mip layout.
    std::vector<int> mipRes;
    for (int r = A.res; r >= 8; r /= 2) mipRes.push_back(r);
    A.mipCount = static_cast<int>(mipRes.size());
    size_t perEntry = 0;
    for (int r : mipRes) perEntry += static_cast<size_t>(r) * r * TF;
    const size_t entries = static_cast<size_t>(A.fieldCount) * A.defocusCount;
    A.texels.assign(entries * perEntry, 0.0f);
    A.mips.resize(entries * A.mipCount);
    for (size_t e = 0; e < entries; ++e) {
        size_t off = e * perEntry;
        for (int m = 0; m < A.mipCount; ++m) {
            A.mips[e * A.mipCount + m] = {mipRes[m], off, 1.0f};
            off += static_cast<size_t>(mipRes[m]) * mipRes[m] * TF;
        }
    }

    const bool spectral = s.lens.dispersion > 1e-4;
    const int bands = spectral ? kSpectralSamples : 1;
    const double lambdaMono = 550.0;
    const double imp = clampv(s.impression, -1.0, 1.0);
    const double cm = clampv(s.coma, -1.0, 1.0);

    parallelFor(A.fieldCount, [&](int fi) {
        const double hN = A.fieldCount > 1 ? fi / double(A.fieldCount - 1) : 0.0;
        const double imgH = hN * halfDiagMm;
        const double y0 = L.objectHeightForImage(imgH, dist);
        const V3 O(0, y0, -dist);

        Ray chief;
        if (!L.aimRay(y0, dist, kLambdaD, 0.0, chief) || !L.trace(chief, kLambdaD, 0)) {
            chief = Ray{V3(0, imgH, L.sensorZ()), V3(0, 0, 1)};
        }
        const double csx = chief.d.x / chief.d.z, csy = chief.d.y / chief.d.z;

        // Coarse pass: which part of the front element feeds the stop from this field point.
        const double a = L.frontRadius() * 1.05;
        const int C = 48;
        double bx0 = 1e30, bx1 = -1e30, by0 = 1e30, by1 = -1e30;
        for (int j = 0; j < C; ++j)
            for (int i = 0; i < C; ++i) {
                const double tx = -a + 2 * a * (i + 0.5) / C, ty = -a + 2 * a * (j + 0.5) / C;
                Ray r{O, (V3(tx, ty, 0) - O).normalized()};
                double sx, sy;
                if (!L.trace(r, kLambdaD, LensSystem::kCheckApertures, &sx, &sy)) continue;
                if (sx * sx + sy * sy > 1.0) continue;
                bx0 = std::min(bx0, tx); bx1 = std::max(bx1, tx);
                by0 = std::min(by0, ty); by1 = std::max(by1, ty);
            }
        if (bx0 > bx1) return; // fully vignetted: leave empty, the renderer falls back to a point
        const double cell = 2 * a / C;
        bx0 -= cell; bx1 += cell; by0 -= cell; by1 += cell;

        // Fine pass: stratified, jittered rays through the feeding region. The stop itself is not
        // tested here; rays just outside it keep the pupil mapping defined up to the iris edge.
        const int N = q.raysPerSide;
        std::vector<RaySample> samples;
        samples.reserve(static_cast<size_t>(N) * N * bands);
        for (int j = 0; j < N; ++j)
            for (int i = 0; i < N; ++i) {
                const uint32_t hsh = hash2(i, j, static_cast<uint32_t>(fi) * 131U + 7U);
                const double tx = bx0 + (bx1 - bx0) * (i + hashUnit(hsh)) / N;
                const double ty = by0 + (by1 - by0) * (j + hashUnit(hash32(hsh))) / N;
                const V3 dir = (V3(tx, ty, 0) - O).normalized();

                Ray g{O, dir};
                double gsx, gsy;
                if (!L.trace(g, bands == 1 ? lambdaMono : 530.0, LensSystem::kCheckApertures, &gsx, &gsy)) continue;
                const double rho2 = gsx * gsx + gsy * gsy;
                if (rho2 > 1.21) continue;

                const double vx = g.d.x / g.d.z - csx, vy = g.d.y / g.d.z - csy;
                // Coma: comet shape that points the same way on both sides of focus (|defocus|).
                const double ex = 0.30 * cm * hN * k * (2 * gsx * gsy);
                const double ey = 0.30 * cm * hN * k * (gsx * gsx + 3 * gsy * gsy - 1.0);
                // Impression: zonal spherical term that squeezes (hard rim) or spreads (soft disc) the
                // outer zone of the pupil identically in front of and behind focus (signed defocus).
                // The rim itself (rho = 1) stays put so the disc keeps its size.
                const double zone = 0.45 * imp * (1.0 - rho2);
                const double ix = zone * vx, iy = zone * vy;

                for (int b = 0; b < bands; ++b) {
                    Ray r{O, dir};
                    if (bands > 1) {
                        if (!L.trace(r, kSpectralLambda[b], LensSystem::kCheckApertures)) continue;
                    } else {
                        r = g;
                    }
                    samples.push_back({static_cast<float>(r.o.x), static_cast<float>(r.o.y), static_cast<float>(r.o.z),
                                       static_cast<float>(r.d.x / r.d.z), static_cast<float>(r.d.y / r.d.z),
                                       static_cast<float>(ex), static_cast<float>(ey),
                                       static_cast<float>(ix), static_cast<float>(iy),
                                       static_cast<float>(gsx), static_cast<float>(gsy), b});
                }
            }

        const int res = A.res;
        const double du = 2.0 * A.extent / res;
        std::vector<float> img(static_cast<size_t>(res) * res * TF);
        for (int d = 0; d < A.defocusCount; ++d) {
            const double delta = A.defocusPx[d] / (k * pxPerMm); // mm, >0 behind focus (background)
            // Astigmatism: the radial (y) and tangential (x) foci split by astigmatismMm at the corner.
            const double split = 0.5 * s.astigmatismMm * hN * hN;
            const double zx = L.sensorZ() + delta - split, zy = L.sensorZ() + delta + split;
            const double cx = chief.o.x + (zx - chief.o.z) * csx;
            const double cy = chief.o.y + (zy - chief.o.z) * csy;
            const double inv = 1.0 / (k * std::fabs(delta));
            const double ad = std::fabs(delta);
            std::fill(img.begin(), img.end(), 0.0f);
            for (const RaySample& rs : samples) {
                const double px = rs.px + (zx - rs.pz) * rs.sx + ad * rs.ex + delta * rs.ix;
                const double py = rs.py + (zy - rs.pz) * rs.sy + ad * rs.ey + delta * rs.iy;
                const double tx = ((px - cx) * inv + A.extent) / du - 0.5;
                const double ty = ((py - cy) * inv + A.extent) / du - 0.5;
                const int x0 = static_cast<int>(std::floor(tx)), y0i = static_cast<int>(std::floor(ty));
                if (x0 < 0 || y0i < 0 || x0 >= res - 1 || y0i >= res - 1) continue;
                const float fx = static_cast<float>(tx - x0), fy = static_cast<float>(ty - y0i);
                const float wts[4] = {(1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy};
                const int idx[4] = {y0i * res + x0, y0i * res + x0 + 1, (y0i + 1) * res + x0, (y0i + 1) * res + x0 + 1};
                float cw[3];
                for (int c = 0; c < 3; ++c) cw[c] = bands > 1 ? kSpectralRgb[rs.band][c] : 1.0f;
                const float total = cw[0] + cw[1] + cw[2];
                for (int n = 0; n < 4; ++n) {
                    float* t = &img[idx[n] * TF];
                    const float w = wts[n];
                    t[0] += cw[0] * w; t[1] += cw[1] * w; t[2] += cw[2] * w;
                    t[3] += total * w * rs.ux; t[4] += total * w * rs.uy;
                }
            }
            blurBinomial(img, res);

            // Scale so the green light of mip 0 sums to one; mips are plain box sums of it.
            double sumG = 0;
            for (int i = 0; i < res * res; ++i) sumG += img[i * TF + 1];
            const float scale = sumG > 0 ? static_cast<float>(1.0 / sumG) : 0.0f;

            const size_t entry = static_cast<size_t>(fi) * A.defocusCount + d;
            PsfAtlas::Mip* mips = &A.mips[entry * A.mipCount];
            float* dst0 = A.texels.data() + mips[0].offset;
            for (size_t i = 0; i < img.size(); ++i) dst0[i] = img[i] * scale;
            mips[0].maxU = maxRadius(dst0, res, A.extent);
            for (int m = 1; m < A.mipCount; ++m) {
                const float* src = A.texels.data() + mips[m - 1].offset;
                float* dst = A.texels.data() + mips[m].offset;
                const int r = mips[m].res, sr = mips[m - 1].res;
                for (int y = 0; y < r; ++y)
                    for (int x = 0; x < r; ++x)
                        for (int c = 0; c < TF; ++c)
                            dst[(y * r + x) * TF + c] = src[((2 * y) * sr + 2 * x) * TF + c] + src[((2 * y) * sr + 2 * x + 1) * TF + c] +
                                                        src[((2 * y + 1) * sr + 2 * x) * TF + c] + src[((2 * y + 1) * sr + 2 * x + 1) * TF + c];
                mips[m].maxU = maxRadius(dst, r, A.extent);
            }
        }
    });

    // Field positions the lens does not reach (a lens scaled or used beyond its image circle)
    // borrow the last field that still passes light, so no pixel ever loses its energy.
    for (int fi = 1; fi < A.fieldCount; ++fi) {
        for (int d = 0; d < A.defocusCount; ++d) {
            const size_t e = static_cast<size_t>(fi) * A.defocusCount + d;
            const PsfAtlas::Mip& m0 = A.mips[e * A.mipCount];
            const float* t = A.texels.data() + m0.offset;
            double light = 0;
            for (int i = 0; i < m0.res * m0.res; ++i) light += t[i * TF + 1];
            if (light > 1e-6) continue;
            const size_t src = static_cast<size_t>(fi - 1) * A.defocusCount + d;
            for (int m = 0; m < A.mipCount; ++m) {
                const PsfAtlas::Mip& ms = A.mips[src * A.mipCount + m];
                PsfAtlas::Mip& md = A.mips[e * A.mipCount + m];
                std::copy(A.texels.begin() + ms.offset, A.texels.begin() + ms.offset + static_cast<size_t>(ms.res) * ms.res * TF,
                          A.texels.begin() + md.offset);
                md.maxU = ms.maxU;
            }
        }
    }
    return atlas;
}

std::shared_ptr<const ApertureTexture> acquireApertureTexture(const ApertureShape& a) {
    Hasher h;
    a.hashInto(h);
    static std::mutex mtx;
    static uint64_t lastKey = 0;
    static std::shared_ptr<const ApertureTexture> last;
    std::lock_guard<std::mutex> lock(mtx);
    if (last && lastKey == h.h) return last;

    auto tex = std::make_shared<ApertureTexture>();
    tex->res = 384;
    tex->t.resize(static_cast<size_t>(tex->res) * tex->res);
    const int res = tex->res;
    const double e = tex->extent;
    parallelFor(res, [&](int y) {
        for (int x = 0; x < res; ++x) {
            // 2x2 supersampling keeps blade edges clean.
            float acc = 0;
            for (int sy = 0; sy < 2; ++sy)
                for (int sx = 0; sx < 2; ++sx) {
                    const double px = -e + 2 * e * (x + 0.25 + 0.5 * sx) / res;
                    const double py = e - 2 * e * (y + 0.25 + 0.5 * sy) / res;
                    acc += a.transmission(px, py);
                }
            tex->t[static_cast<size_t>(y) * res + x] = 0.25f * acc;
        }
    });
    last = tex;
    lastKey = h.h;
    return tex;
}

std::shared_ptr<const PsfAtlas> acquirePsfAtlas(const OpticsSettings& in) {
    OpticsSettings s = in;
    // 24 steps per decade of focus distance.
    const double f = std::max(s.lens.focusDistanceMm, 1.0);
    s.lens.focusDistanceMm = std::pow(10.0, std::round(std::log10(f) * 24.0) / 24.0);
    const uint64_t key = s.hash();

    static std::mutex mtx;
    static std::list<std::pair<uint64_t, std::shared_ptr<const PsfAtlas>>> cache;
    std::lock_guard<std::mutex> lock(mtx);
    for (auto it = cache.begin(); it != cache.end(); ++it) {
        if (it->first == key) {
            cache.splice(cache.begin(), cache, it);
            return cache.front().second;
        }
    }
    auto atlas = buildPsfAtlas(s);
    cache.emplace_front(key, atlas);
    while (cache.size() > 3) cache.pop_back();
    return atlas;
}

} // namespace lensyum
