#pragma once

#include <cmath>
#include <cstdint>

namespace lensyum {

constexpr double kPi = 3.14159265358979323846;

struct V3 {
    double x = 0, y = 0, z = 0;
    V3() = default;
    V3(double x_, double y_, double z_) : x(x_), y(y_), z(z_) {}
    V3 operator+(const V3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    V3 operator-(const V3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    V3 operator*(double s) const { return {x * s, y * s, z * s}; }
    double length() const { return std::sqrt(x * x + y * y + z * z); }
    V3 normalized() const { double l = length(); return l > 0 ? V3(x / l, y / l, z / l) : *this; }
};

inline double dot(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

struct Ray {
    V3 o, d;
};

template <class T> inline T clampv(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline double lerp(double a, double b, double t) { return a + (b - a) * t; }
inline double smoothstep(double e0, double e1, double x) {
    double t = clampv((x - e0) / (e1 - e0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

// Small, fast integer hashing used for dithering and procedural pupil textures.
inline uint32_t hash32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU;
    x ^= x >> 15; x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}
inline uint32_t hash2(int32_t a, int32_t b, uint32_t seed = 0) {
    return hash32(static_cast<uint32_t>(a) * 0x9E3779B1U ^ hash32(static_cast<uint32_t>(b) + seed * 0x85EBCA77U));
}
inline float hashUnit(uint32_t h) { return (h >> 8) * (1.0f / 16777216.0f); }

// FNV-1a style accumulator for cache keys.
struct Hasher {
    uint64_t h = 1469598103934665603ULL;
    void bytes(const void* p, size_t n) {
        const unsigned char* c = static_cast<const unsigned char*>(p);
        for (size_t i = 0; i < n; ++i) { h ^= c[i]; h *= 1099511628211ULL; }
    }
    template <class T> void add(const T& v) { bytes(&v, sizeof(T)); }
};

} // namespace lensyum
