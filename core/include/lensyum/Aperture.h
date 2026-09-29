#pragma once

#include "Math.h"

#include <cstdint>
#include <vector>

namespace lensyum {

// Transmission of the aperture stop, evaluated in stop coordinates normalised to the
// current stop radius (the unit disc is the open iris at the chosen f-stop).
struct ApertureShape {
    int blades = 0;              // 0 = perfectly round iris
    double curvature = 0.0;      // 0 = straight blades, 1 = round
    double rotationRad = 0.0;
    double obstruction = 0.0;
    double lobes = 0.0;          // 0..1: scalloped, lobed disc edge
    int lobeCount = 5;    // central obscuration radius (catadioptric donut), 0..0.9
    double onion = 0.0;          // aspheric polishing rings, 0..1
    double onionFreq = 6.0;
    double texture = 0.0;        // dust and grain on the glass, 0..1
    double textureScale = 1.0;
    int textureSeed = 0;

    // Optional custom aperture, greyscale, row 0 = top. Multiplies the iris.
    std::vector<float> mask;
    int maskW = 0, maskH = 0;

    float transmission(double x, double y) const;
    // True when the iris looks the same at every rotation (round, no custom mask, no texture).
    bool isRound() const { return (blades < 3 || curvature >= 0.999) && mask.empty() && texture <= 0.0 && lobes <= 0.0; }
    void hashInto(Hasher& h) const;
};

} // namespace lensyum
