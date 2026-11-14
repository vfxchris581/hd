#pragma once
#include "Math.h"

namespace hd {

// Improved Perlin noise, roughly in [-1, 1].
float perlin3(float x, float y, float z);
// Fractal Brownian motion of perlin noise.
float fbm3(const Vec3& p, int octaves, float lacunarity = 2.0f, float roughness = 0.5f);
// Vector-valued noise (three decorrelated perlin lookups).
Vec3 vnoise3(const Vec3& p);
// Curl of a vector noise field — divergence-free, good for flows.
Vec3 curlNoise3(const Vec3& p);

} // namespace hd
