#include "lensyum/Lens.h"

#include <algorithm>
#include <cmath>

namespace lensyum {

const double kSpectralLambda[kSpectralSamples] = {450.0, 490.0, 530.0, 570.0, 610.0, 650.0};
// Each column (R, G, B) sums to 1 so a neutral light stays neutral.
const float kSpectralRgb[kSpectralSamples][3] = {
    {0.00f, 0.00f, 0.55f},
    {0.00f, 0.15f, 0.35f},
    {0.00f, 0.45f, 0.10f},
    {0.15f, 0.35f, 0.00f},
    {0.45f, 0.05f, 0.00f},
    {0.40f, 0.00f, 0.00f},
};

// ---------------------------------------------------------------------------------------
// Lens library. Prescriptions go from the object side to the sensor; the last thickness is
// ignored (the sensor distance comes from focusing).
// ---------------------------------------------------------------------------------------
namespace {

std::vector<LensPrescription> makePresets() {
    std::vector<LensPrescription> v;

    // Double-Gauss f/2, 22 deg half field (Tronnier, US 2,673,491 as tabulated in
    // Smith, "Modern Lens Design"), scaled to 50 mm. Glass Abbe numbers assigned from the
    // catalogue glasses matching each index.
    v.push_back({"Double-Gauss 50 f/2", {
        {  29.475, 3.760, 1.670, 47.2, 25.2, false},
        {  84.830, 0.120, 1.000,  0.0, 25.2, false},
        {  19.275, 4.025, 1.670, 47.2, 23.0, false},
        {  40.770, 3.275, 1.699, 30.1, 23.0, false},
        {  12.750, 5.705, 1.000,  0.0, 18.0, false},
        {   0.000, 4.500, 1.000,  0.0, 17.1, true },
        { -14.495, 1.180, 1.603, 38.0, 17.0, false},
        {  40.770, 6.065, 1.658, 57.3, 20.0, false},
        { -20.385, 0.190, 1.000,  0.0, 20.0, false},
        { 437.065, 3.220, 1.717, 47.9, 20.0, false},
        { -39.730, 0.000, 1.000,  0.0, 20.0, false},
    }});

    return v;
}

const std::vector<LensPrescription>& presets() {
    static const std::vector<LensPrescription> p = makePresets();
    return p;
}

} // namespace

int lensPresetCount() { return static_cast<int>(presets().size()); }

const LensPrescription& lensPreset(int index) {
    const auto& p = presets();
    return p[clampv(index, 0, static_cast<int>(p.size()) - 1)];
}

// ---------------------------------------------------------------------------------------
// LensSystem
// ---------------------------------------------------------------------------------------

void LensSystem::build(const LensPrescription& p, double scale) {
    surfs_.clear();
    double z = 0;
    stopIndex_ = -1;
    for (size_t i = 0; i < p.surfaces.size(); ++i) {
        const Surface& s = p.surfaces[i];
        Surf o;
        o.z = z;
        o.R = s.radius * scale;
        o.ap = 0.5 * s.aperture * scale;
        o.nd = s.stop ? 1.0 : (s.nd > 0 ? s.nd : 1.0);
        o.vd = s.vd;
        o.stop = s.stop;
        if (s.stop) stopIndex_ = static_cast<int>(i);
        surfs_.push_back(o);
        z += s.thickness * scale;
    }
    if (stopIndex_ >= 0) stopR_ = surfs_[stopIndex_].ap;
}

double LensSystem::iorAfter(int i, double lambdaNm) const {
    const Surf& s = surfs_[i];
    if (s.nd <= 1.0 || s.vd <= 0) return s.nd;
    // Two-term Cauchy fit through nd and the F-C dispersion implied by the Abbe number.
    const double lF = 0.4861, lC = 0.6563, lD = kLambdaD * 1e-3, l = lambdaNm * 1e-3;
    const double dn = (s.nd - 1.0) / s.vd;
    const double B = dn / (1.0 / (lF * lF) - 1.0 / (lC * lC));
    return s.nd + dispersion_ * B * (1.0 / (l * l) - 1.0 / (lD * lD));
}

bool LensSystem::trace(Ray& r, double lambdaNm, int flags, double* stopX, double* stopY,
                       int lastSurface) const {
    double n1 = 1.0;
    const int end = lastSurface < 0 ? static_cast<int>(surfs_.size()) : lastSurface + 1;
    for (int i = 0; i < end; ++i) {
        const Surf& s = surfs_[i];
        double t;
        V3 nrm;
        if (s.R == 0.0) {
            if (r.d.z <= 0) return false;
            t = (s.z - r.o.z) / r.d.z;
            nrm = V3(0, 0, -1);
        } else {
            const V3 c(0, 0, s.z + s.R);
            const V3 oc = r.o - c;
            const double b = dot(oc, r.d);
            const double cc = dot(oc, oc) - s.R * s.R;
            const double disc = b * b - cc;
            if (disc < 0) return false;
            const double sq = std::sqrt(disc);
            // Rays travel towards +z: a convex-forward surface is the near hit, a concave one the far hit.
            t = (s.R > 0) ? (-b - sq) : (-b + sq);
            const V3 p = r.o + r.d * t;
            nrm = (p - c).normalized();
            if (dot(nrm, r.d) > 0) nrm = nrm * -1.0;
        }
        if (t < -1e-9) return false;
        const V3 p = r.o + r.d * t;
        const double h2 = p.x * p.x + p.y * p.y;
        if (s.stop) {
            if (stopX) *stopX = p.x / stopR_;
            if (stopY) *stopY = p.y / stopR_;
            if ((flags & kCheckStop) && h2 > stopR_ * stopR_) return false;
        } else if ((flags & kCheckApertures) && h2 > s.ap * s.ap) {
            return false;
        }
        r.o = p;
        const double n2 = s.stop ? n1 : iorAfter(i, lambdaNm);
        if (n2 != n1) {
            const double eta = n1 / n2;
            const double cosi = -dot(nrm, r.d);
            const double k = 1.0 - eta * eta * (1.0 - cosi * cosi);
            if (k < 0) return false;
            r.d = (r.d * eta + nrm * (eta * cosi - std::sqrt(k))).normalized();
        }
        n1 = n2;
    }
    return true;
}

double LensSystem::paraxialEfl() const {
    const double h = 1e-4 * surfs_.front().ap;
    Ray r{V3(0, h, -1.0), V3(0, 0, 1)};
    if (!trace(r, kLambdaD, 0)) return 0;
    const double m = r.d.y / r.d.z;
    return m != 0 ? -h / m : 0;
}

double LensSystem::imageZ(double dist) const {
    const double h = 1e-4 * surfs_.front().ap;
    Ray r;
    if (dist >= 1e8) {
        r = Ray{V3(0, h, -1.0), V3(0, 0, 1)};
    } else {
        r.o = V3(0, 0, -dist);
        r.d = (V3(0, h, 0) - r.o).normalized();
    }
    if (!trace(r, kLambdaD, 0)) return sensorZ_;
    const double m = r.d.y / r.d.z;
    if (m >= 0) return 1e9; // not converging: object inside the focal length
    return r.o.z - r.o.y / m;
}

bool LensSystem::aimRay(double objY, double dist, double lambdaNm, double stopFrac, Ray& out) const {
    const V3 O(0, objY, -dist);
    const double target = stopFrac; // in stop radii
    auto stopHeight = [&](double a, double& sy) {
        Ray r{O, (V3(0, a, 0) - O).normalized()};
        double sx;
        if (!trace(r, lambdaNm, 0, &sx, &sy, stopIndex_)) return false;
        return true;
    };
    double a0 = 0, a1 = 0.05 * surfs_.front().ap;
    double f0, f1;
    if (!stopHeight(a0, f0) || !stopHeight(a1, f1)) return false;
    f0 -= target; f1 -= target;
    for (int it = 0; it < 40 && std::fabs(f1) > 1e-10; ++it) {
        const double den = f1 - f0;
        if (den == 0) break;
        const double a2 = a1 - f1 * (a1 - a0) / den;
        double f2;
        if (!stopHeight(a2, f2)) return false;
        a0 = a1; f0 = f1; a1 = a2; f1 = f2 - target;
    }
    if (std::fabs(f1) > 1e-6) return false;
    out = Ray{O, (V3(0, a1, 0) - O).normalized()};
    return true;
}

double LensSystem::objectHeightForImage(double imageY, double dist) const {
    auto landing = [&](double y0, double& yi) {
        Ray r;
        if (!aimRay(y0, dist, kLambdaD, 0.0, r)) return false;
        if (!trace(r, kLambdaD, 0)) return false;
        yi = r.o.y + (sensorZ_ - r.o.z) * r.d.y / r.d.z;
        return true;
    };
    if (imageY == 0) return 0;
    double y0 = 1e-3 * std::max(1.0, dist / std::max(efl_, 1e-3)), yi0;
    if (!landing(y0, yi0) || yi0 == 0) return 0;
    double y1 = imageY / (yi0 / y0), yi1;
    if (!landing(y1, yi1)) return y1;
    yi0 -= imageY; yi1 -= imageY;
    for (int it = 0; it < 30 && std::fabs(yi1) > 1e-7; ++it) {
        const double den = yi1 - yi0;
        if (den == 0) break;
        const double y2 = y1 - yi1 * (y1 - y0) / den;
        double yi2;
        if (!landing(y2, yi2)) break;
        y0 = y1; yi0 = yi1; y1 = y2; yi1 = yi2 - imageY;
    }
    return y1;
}

LensSystem::LensSystem(const LensPrescription& p, const LensSettings& s) {
    dispersion_ = s.dispersion;

    build(p, 1.0);
    const double nativeEfl = paraxialEfl();
    const double scale = (nativeEfl > 0 && s.focalLengthMm > 0) ? s.focalLengthMm / nativeEfl : 1.0;
    build(p, scale);
    efl_ = paraxialEfl();

    // Widest entrance pupil: largest parallel axial ray that clears every aperture.
    auto passes = [&](double h) {
        Ray r{V3(0, h, -1.0), V3(0, 0, 1)};
        return trace(r, kLambdaD, kCheckApertures | kCheckStop);
    };
    double lo = 0, hi = surfs_.front().ap;
    for (int it = 0; it < 50; ++it) {
        const double mid = 0.5 * (lo + hi);
        (passes(mid) ? lo : hi) = mid;
    }
    const double epR = std::max(lo, 1e-6);
    openF_ = efl_ / (2.0 * epR);

    // Axial beam footprint on every surface; apertures never shrink below it so the
    // centre of frame is never clipped when the cat-eye control is pushed.
    std::vector<double> axial(surfs_.size(), 0.0);
    for (size_t i = 0; i < surfs_.size(); ++i) {
        Ray r{V3(0, epR * 0.999, -1.0), V3(0, 0, 1)};
        if (trace(r, kLambdaD, 0, nullptr, nullptr, static_cast<int>(i))) axial[i] = std::fabs(r.o.y);
    }
    const double vigScale = std::exp((1.0 - s.vignetting) * 0.45);
    for (size_t i = 0; i < surfs_.size(); ++i) {
        if (surfs_[i].stop) continue;
        surfs_[i].ap = std::max(axial[i] * 1.01, surfs_[i].ap * vigScale);
    }

    fNumber_ = std::max(s.fNumber, openF_);
    const double stopOpen = stopR_;
    stopR_ = stopOpen * openF_ / fNumber_;

    focusDist_ = std::max(s.focusDistanceMm, efl_ * 1.5);
    sensorZ_ = imageZ(focusDist_);

    Ray m;
    marginalSlope_ = 0;
    if (aimRay(0.0, std::min(focusDist_, 1e7), kLambdaD, 0.999, m) && trace(m, kLambdaD, 0))
        marginalSlope_ = std::fabs(m.d.y / m.d.z);
    if (marginalSlope_ <= 0) marginalSlope_ = 1.0 / (2.0 * fNumber_);
}

} // namespace lensyum
