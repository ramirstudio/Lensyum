#pragma once

#include "Math.h"
#include <vector>

namespace lensyum {

// One refracting surface, listed from the object side towards the sensor.
// radius:    mm, 0 = flat. Positive = centre of curvature on the sensor side.
// thickness: mm to the next surface along the axis.
// nd, vd:    refractive index (d line) and Abbe number of the medium AFTER the surface.
// aperture:  clear diameter in mm.
struct Surface {
    double radius;
    double thickness;
    double nd;
    double vd;
    double aperture;
    bool stop;
};

struct LensPrescription {
    const char* name;
    double focalMm; // focal length the preset is used at
    std::vector<Surface> surfaces;
};

int lensPresetCount();
const LensPrescription& lensPreset(int index);

struct LensSettings {
    double focalLengthMm = 50.0;    // the whole prescription is scaled to this EFL
    double fNumber = 2.0;           // clamped to the design's widest aperture
    double focusDistanceMm = 3000;  // object distance from the front vertex
    double dispersion = 1.0;        // multiplier on glass dispersion (chromatic aberration)
    double vignetting = 1.0;        // 0 = open barrel, 1 = as designed, 2 = strong cat-eye
};

// A lens prescription scaled, stopped down and focused. Traces real rays with Snell's law.
class LensSystem {
public:
    enum TraceFlags { kCheckApertures = 1, kCheckStop = 2 };

    LensSystem(const LensPrescription& p, const LensSettings& s);

    // Trace from object space. Returns false if the ray is blocked or totally reflected.
    // stopX/stopY receive the stop crossing normalised by the current stop radius.
    // lastSurface >= 0 stops the trace after that surface.
    bool trace(Ray& r, double lambdaNm, int flags,
               double* stopX = nullptr, double* stopY = nullptr, int lastSurface = -1) const;

    // Paraxial image position (z) of an on-axis object at the given distance.
    double imageZ(double objectDistMm) const;

    // Ray from an object point (0, objY, -dist) aimed so that it crosses the stop at
    // height stopFrac * stopRadius (0 = chief ray). Returned ray is in object space.
    bool aimRay(double objY, double dist, double lambdaNm, double stopFrac, Ray& out) const;

    // Object height whose chief ray lands on the sensor at the given image height.
    double objectHeightForImage(double imageY, double dist) const;

    double efl() const { return efl_; }
    double openFNumber() const { return openF_; }
    double fNumber() const { return fNumber_; }
    double stopRadius() const { return stopR_; }
    double sensorZ() const { return sensorZ_; }
    double exitZ() const { return surfs_.back().z; }
    double frontRadius() const { return surfs_.front().ap; }
    double focusDistance() const { return focusDist_; }
    // Axial marginal ray slope after the lens: the defocus blur radius per mm of defocus.
    double marginalSlope() const { return marginalSlope_; }
    int stopIndex() const { return stopIndex_; }

private:
    struct Surf { double z, R, ap, nd, vd; bool stop; };
    std::vector<Surf> surfs_;
    int stopIndex_ = -1;
    double efl_ = 0, openF_ = 0, fNumber_ = 0, stopR_ = 0, sensorZ_ = 0;
    double focusDist_ = 0, marginalSlope_ = 0;
    double dispersion_ = 1.0;

    void build(const LensPrescription& p, double scale);
    double iorAfter(int i, double lambdaNm) const;
    double paraxialEfl() const;
};

// Wavelength samples used for spectral tracing and their RGB weights.
constexpr int kSpectralSamples = 6;
extern const double kSpectralLambda[kSpectralSamples];
extern const float kSpectralRgb[kSpectralSamples][3];
constexpr double kLambdaD = 587.6;

} // namespace lensyum
