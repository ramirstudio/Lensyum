#pragma once

#include "Aperture.h"
#include "Lens.h"

#include <memory>
#include <vector>

namespace lensyum {

// Everything that changes the shape of the point spread functions.
struct OpticsSettings {
    int lensPreset = 0;
    LensSettings lens;
    ApertureShape aperture;

    // Character controls on top of the aberrations the prescription already has.
    double impression = 0.0;    // -1..1: negative = soft creamy disc, positive = hard bright rim
    double impressionPower = 3.0; // how close to the rim the impression zone sits
    double coma = 0.0;          // -1..1: comet-shaped highlights towards the corners
    double astigmatismMm = 0.0; // focus split between radial and tangential foci at the corner,
                                // in mm of sensor travel (edge bokeh stretches into lines)

    double coverage = 1.0;      // how far into the frame the lens's edge character reaches (0..3)
    double sensorWidthMm = 36.0;
    double frameWidthPx = 1920, frameHeightPx = 1080; // full-resolution frame the sensor spans
    double pixelAspect = 1.0;
    double maxBlurPx = 150.0; // largest blur radius at full resolution
    int quality = 1;          // 0 draft .. 3 best

    uint64_t hash() const;
};

// Pupil-to-image mapping of the lens, sampled over the image field (radially: the glass is
// rotationally symmetric) and over signed defocus. Each entry lives in a normalised frame where
// +y points away from the optical centre and a unit radius equals the geometric blur radius.
// A texel holds the light reaching it per channel (mechanical vignetting included, iris not)
// and the mean pupil position it came from, so the iris, which need not be symmetric, is
// applied at render time in the lens' own orientation.
class PsfAtlas {
public:
    int fieldCount = 0;
    int defocusCount = 0;
    int res = 0;          // texels per side at mip 0
    int mipCount = 0;
    double extent = 1.75; // half size of the PSF frame in normalised units
    std::vector<double> defocusPx; // signed full-resolution blur radius of each entry, ascending
    std::vector<float> unitMax;    // per defocus entry, the largest Mip::unit over the field
    double maxReachPx = 0;         // largest footprint radius (px) any entry reaches, unit included

    // Lens figures for reporting and for depth -> blur conversion.
    double efl = 0, fNumber = 0, marginalSlope = 0, pxPerMm = 0;

    // unit: how many geometric blur radii one normalised unit stands for. It is 1 unless the lens's own
    // aberrations at this defocus are larger than the geometric blur (a PSF that would otherwise be
    // cut off by the square texture frame); the renderer scales the footprint by it.
    struct Mip { int res; size_t offset; float maxU; float unit; };
    // [field][defocus][mip]
    std::vector<Mip> mips;
    // kTexelFloats per texel: light in R, G, B, then pupil x and y multiplied by (R + G + B),
    // so bilinear filtering of pupil positions is weighted by light.
    static constexpr int kTexelFloats = 5;
    std::vector<float> texels;

    const Mip& mip(int f, int d, int m) const { return mips[(static_cast<size_t>(f) * defocusCount + d) * mipCount + m]; }
    const float* data(const Mip& m) const { return texels.data() + m.offset; }

    int nearestDefocus(double signedPx) const;
};

std::shared_ptr<const PsfAtlas> buildPsfAtlas(const OpticsSettings& s);

// The iris (blades, custom shape, obstruction, rings, texture) rasterised over the stop.
// Coordinates are in stop radii, +y up as the aperture is seen in the image.
struct ApertureTexture {
    int res = 0;
    double extent = 1.1;
    std::vector<float> t;
    float sample(double x, double y) const {
        const double fx = (x + extent) / (2 * extent) * res - 0.5;
        const double fy = (extent - y) / (2 * extent) * res - 0.5;
        if (fx < 0 || fy < 0 || fx >= res - 1 || fy >= res - 1) return 0.0f;
        const int x0 = static_cast<int>(fx), y0 = static_cast<int>(fy);
        const float ax = static_cast<float>(fx - x0), ay = static_cast<float>(fy - y0);
        const float* r0 = &t[static_cast<size_t>(y0) * res + x0];
        const float* r1 = r0 + res;
        const float a = r0[0] + (r0[1] - r0[0]) * ax, b = r1[0] + (r1[1] - r1[0]) * ax;
        return a + (b - a) * ay;
    }
};
std::shared_ptr<const ApertureTexture> acquireApertureTexture(const ApertureShape& a);

// Thread-safe cache around buildPsfAtlas. Focus distance is quantised so that animated focus
// pulls reuse the same atlas (PSF shape barely changes with focus distance). The iris is not
// part of the key: aperture changes never retrace the lens.
std::shared_ptr<const PsfAtlas> acquirePsfAtlas(const OpticsSettings& s);

struct QualityProfile {
    int raysPerSide; // stratified samples across the entrance beam, per field position
    int psfRes;
    int fieldCount;
    int defocusPerSign;
    double levelCap;     // blur radius (px) above which ordinary pixels are splatted at lower resolution
    double levelCapHigh; // same for highlights
};
QualityProfile qualityProfile(int quality);

} // namespace lensyum
