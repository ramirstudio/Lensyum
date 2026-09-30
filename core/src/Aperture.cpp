#include "lensyum/Aperture.h"
#include "lensyum/Math.h"

#include <algorithm>
#include <cmath>

namespace lensyum {

namespace {

float valueNoise(double x, double y, uint32_t seed) {
    const int xi = static_cast<int>(std::floor(x)), yi = static_cast<int>(std::floor(y));
    const double fx = x - xi, fy = y - yi;
    const double ux = fx * fx * (3 - 2 * fx), uy = fy * fy * (3 - 2 * fy);
    const float a = hashUnit(hash2(xi, yi, seed)), b = hashUnit(hash2(xi + 1, yi, seed));
    const float c = hashUnit(hash2(xi, yi + 1, seed)), d = hashUnit(hash2(xi + 1, yi + 1, seed));
    return static_cast<float>(lerp(lerp(a, b, ux), lerp(c, d, ux), uy));
}

// Scattered soft specks: dust sitting on an element close to the stop.
float specks(double x, double y, double freq, uint32_t seed) {
    const double gx = x * freq, gy = y * freq;
    const int cx = static_cast<int>(std::floor(gx)), cy = static_cast<int>(std::floor(gy));
    float v = 0;
    for (int j = -1; j <= 1; ++j) {
        for (int i = -1; i <= 1; ++i) {
            const uint32_t h = hash2(cx + i, cy + j, seed ^ 0xA511E9B3U);
            if (hashUnit(h) > 0.55f) continue; // not every cell holds a speck
            const double px = cx + i + hashUnit(hash32(h + 1));
            const double py = cy + j + hashUnit(hash32(h + 2));
            const double r = 0.08 + 0.22 * hashUnit(hash32(h + 3));
            const double d = std::hypot(gx - px, gy - py) / r;
            if (d < 1.0) v = std::max(v, static_cast<float>(1.0 - smoothstep(0.55, 1.0, d)) * (0.5f + 0.5f * hashUnit(hash32(h + 4))));
        }
    }
    return v;
}

} // namespace

double ApertureShape::lobeEdge(double x, double y) const {
    const double phi = std::atan2(y, x) - lobeAngle;
    const double bump = std::pow(0.5 + 0.5 * std::cos(lobeCount * phi), std::max(lobePower, 0.2));
    return 1.0 - 0.3 * clampv(lobes, 0.0, 1.0) * (1.0 - bump);
}

float ApertureShape::transmission(double x, double y) const {
    const double r2 = x * x + y * y;
    if (r2 > 1.0) return 0.0f;
    const double rho = std::sqrt(r2);

    if (blades >= 3) {
        const double seg = 2.0 * kPi / blades;
        double phi = std::atan2(y, x) - rotationRad;
        phi = std::fmod(phi, seg);
        if (phi < 0) phi += seg;
        const double polyR = std::cos(kPi / blades) / std::cos(phi - 0.5 * seg);
        const double edge = lerp(polyR, 1.0, clampv(curvature, 0.0, 1.0));
        if (rho > edge) return 0.0f;
    }
    if (lobes > 0 && lobeCount >= 2 && !lobesFaceCenter) {
        if (rho > lobeEdge(x, y)) return 0.0f;
    }
    if (obstruction > 0 && rho < obstruction) return 0.0f;

    double t = 1.0;
    if (!mask.empty() && maskW > 1 && maskH > 1) {
        const double mx = (x * 0.5 + 0.5) * (maskW - 1);
        const double my = (0.5 - y * 0.5) * (maskH - 1);
        const int x0 = clampv(static_cast<int>(mx), 0, maskW - 2), y0 = clampv(static_cast<int>(my), 0, maskH - 2);
        const double fx = mx - x0, fy = my - y0;
        const float* m = mask.data();
        const double a = lerp(m[y0 * maskW + x0], m[y0 * maskW + x0 + 1], fx);
        const double b = lerp(m[(y0 + 1) * maskW + x0], m[(y0 + 1) * maskW + x0 + 1], fx);
        t *= clampv(lerp(a, b, fy), 0.0, 1.0);
        if (t <= 0) return 0.0f;
    }
    if (onion > 0) {
        // Ripple left by aspheric polishing: faint concentric rings, denser towards the rim.
        const double ring = std::sin(2.0 * kPi * onionFreq * r2);
        t *= 1.0 + 0.6 * onion * ring * (0.4 + 0.6 * rho);
    }
    if (texture > 0) {
        const uint32_t seed = static_cast<uint32_t>(textureSeed) * 7919U + 17U;
        const double s = std::max(textureScale, 0.05);
        const double grain = 0.55 * valueNoise(x * 9.0 / s, y * 9.0 / s, seed) +
                             0.30 * valueNoise(x * 23.0 / s, y * 23.0 / s, seed + 1) +
                             0.15 * valueNoise(x * 57.0 / s, y * 57.0 / s, seed + 2);
        const double dust = specks(x, y, 5.0 / s, seed);
        t *= 1.0 - texture * (0.65 * dust + 0.45 * std::max(0.0, grain - 0.35));
    }
    return static_cast<float>(std::max(t, 0.0));
}

void ApertureShape::hashInto(Hasher& h) const {
    h.add(blades); h.add(curvature); h.add(rotationRad); h.add(obstruction);
    h.add(lobes); h.add(lobeCount); h.add(lobePower); h.add(lobeAngle); h.add(lobesFaceCenter); h.add(onion); h.add(onionFreq); h.add(texture); h.add(textureScale); h.add(textureSeed);
    h.add(maskW); h.add(maskH);
    if (!mask.empty()) h.bytes(mask.data(), mask.size() * sizeof(float));
}

} // namespace lensyum
