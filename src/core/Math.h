// HD — procedural 3D VFX environment
// Small, dependency-free vector/matrix math.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace hd {

constexpr float kPi = 3.14159265358979323846f;

struct Vec3 {
    float x = 0, y = 0, z = 0;
    Vec3() = default;
    constexpr Vec3(float a, float b, float c) : x(a), y(b), z(c) {}
    explicit constexpr Vec3(float s) : x(s), y(s), z(s) {}
    float& operator[](int i) { return (&x)[i]; }
    float operator[](int i) const { return (&x)[i]; }
    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(const Vec3& o) const { return {x * o.x, y * o.y, z * o.z}; }
    Vec3 operator/(const Vec3& o) const { return {x / o.x, y / o.y, z / o.z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    Vec3 operator/(float s) const { return {x / s, y / s, z / s}; }
    Vec3 operator-() const { return {-x, -y, -z}; }
    Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
    Vec3& operator-=(const Vec3& o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
    Vec3& operator*=(float s) { x *= s; y *= s; z *= s; return *this; }
    Vec3& operator/=(float s) { x /= s; y /= s; z /= s; return *this; }
    bool operator==(const Vec3& o) const { return x == o.x && y == o.y && z == o.z; }
    bool operator!=(const Vec3& o) const { return !(*this == o); }
};
inline Vec3 operator*(float s, const Vec3& v) { return v * s; }
inline float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float length(const Vec3& v) { return std::sqrt(dot(v, v)); }
inline Vec3 normalize(const Vec3& v) {
    float l = length(v);
    return l > 1e-20f ? v / l : Vec3(0, 0, 0);
}
inline Vec3 lerp(const Vec3& a, const Vec3& b, float t) { return a + (b - a) * t; }
inline Vec3 vmin(const Vec3& a, const Vec3& b) { return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)}; }
inline Vec3 vmax(const Vec3& a, const Vec3& b) { return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)}; }
inline float radians(float d) { return d * kPi / 180.0f; }

// Column-major 4x4 matrix (OpenGL convention): m[col*4 + row]
struct Mat4 {
    float m[16];
    Mat4() { setIdentity(); }
    void setIdentity() {
        for (int i = 0; i < 16; ++i) m[i] = (i % 5 == 0) ? 1.f : 0.f;
    }
    float& at(int row, int col) { return m[col * 4 + row]; }
    float at(int row, int col) const { return m[col * 4 + row]; }
    Mat4 operator*(const Mat4& o) const {
        Mat4 r;
        for (int c = 0; c < 4; ++c)
            for (int rr = 0; rr < 4; ++rr) {
                float s = 0;
                for (int k = 0; k < 4; ++k) s += at(rr, k) * o.at(k, c);
                r.at(rr, c) = s;
            }
        return r;
    }
    Vec3 transformPoint(const Vec3& p) const {
        float x = at(0, 0) * p.x + at(0, 1) * p.y + at(0, 2) * p.z + at(0, 3);
        float y = at(1, 0) * p.x + at(1, 1) * p.y + at(1, 2) * p.z + at(1, 3);
        float z = at(2, 0) * p.x + at(2, 1) * p.y + at(2, 2) * p.z + at(2, 3);
        float w = at(3, 0) * p.x + at(3, 1) * p.y + at(3, 2) * p.z + at(3, 3);
        if (w != 0.f && w != 1.f) { x /= w; y /= w; z /= w; }
        return {x, y, z};
    }
    Vec3 transformDir(const Vec3& d) const {
        return {at(0, 0) * d.x + at(0, 1) * d.y + at(0, 2) * d.z,
                at(1, 0) * d.x + at(1, 1) * d.y + at(1, 2) * d.z,
                at(2, 0) * d.x + at(2, 1) * d.y + at(2, 2) * d.z};
    }
    // Transforms a normal by the inverse-transpose of the upper 3x3.
    Vec3 transformNormal(const Vec3& n) const {
        float a = at(0, 0), b = at(0, 1), c = at(0, 2);
        float d = at(1, 0), e = at(1, 1), f = at(1, 2);
        float g = at(2, 0), h = at(2, 1), i = at(2, 2);
        // cofactor matrix == det * inverse-transpose
        Vec3 r{(e * i - f * h) * n.x + (f * g - d * i) * n.y + (d * h - e * g) * n.z,
               (c * h - b * i) * n.x + (a * i - c * g) * n.y + (b * g - a * h) * n.z,
               (b * f - c * e) * n.x + (c * d - a * f) * n.y + (a * e - b * d) * n.z};
        float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
        if (det < 0) r = -r;
        return normalize(r);
    }

    static Mat4 translate(const Vec3& t) {
        Mat4 r;
        r.at(0, 3) = t.x; r.at(1, 3) = t.y; r.at(2, 3) = t.z;
        return r;
    }
    static Mat4 scale(const Vec3& s) {
        Mat4 r;
        r.at(0, 0) = s.x; r.at(1, 1) = s.y; r.at(2, 2) = s.z;
        return r;
    }
    static Mat4 rotateX(float a) {
        Mat4 r; float c = std::cos(a), s = std::sin(a);
        r.at(1, 1) = c; r.at(1, 2) = -s; r.at(2, 1) = s; r.at(2, 2) = c;
        return r;
    }
    static Mat4 rotateY(float a) {
        Mat4 r; float c = std::cos(a), s = std::sin(a);
        r.at(0, 0) = c; r.at(0, 2) = s; r.at(2, 0) = -s; r.at(2, 2) = c;
        return r;
    }
    static Mat4 rotateZ(float a) {
        Mat4 r; float c = std::cos(a), s = std::sin(a);
        r.at(0, 0) = c; r.at(0, 1) = -s; r.at(1, 0) = s; r.at(1, 1) = c;
        return r;
    }
    // Houdini default transform order: Scale, Rotate (X,Y,Z), Translate (SRT, XYZ).
    static Mat4 srt(const Vec3& t, const Vec3& rDeg, const Vec3& s, const Vec3& pivot = Vec3(0)) {
        Mat4 R = rotateZ(radians(rDeg.z)) * rotateY(radians(rDeg.y)) * rotateX(radians(rDeg.x));
        return translate(t + pivot) * R * scale(s) * translate(-pivot);
    }
    // Rotation that takes +Z to dir, using up as a hint (like Houdini copy orientation).
    static Mat4 lookAlong(const Vec3& dir, const Vec3& upHint) {
        Vec3 z = normalize(dir);
        if (length(z) < 0.5f) return Mat4();
        Vec3 up = upHint;
        if (std::fabs(dot(normalize(up), z)) > 0.999f) up = std::fabs(z.y) < 0.9f ? Vec3(0, 1, 0) : Vec3(1, 0, 0);
        Vec3 x = normalize(cross(up, z));
        Vec3 y = cross(z, x);
        Mat4 r;
        r.at(0, 0) = x.x; r.at(1, 0) = x.y; r.at(2, 0) = x.z;
        r.at(0, 1) = y.x; r.at(1, 1) = y.y; r.at(2, 1) = y.z;
        r.at(0, 2) = z.x; r.at(1, 2) = z.y; r.at(2, 2) = z.z;
        return r;
    }
    static Mat4 perspective(float fovyRad, float aspect, float zn, float zf) {
        Mat4 r;
        float f = 1.0f / std::tan(fovyRad * 0.5f);
        for (float& v : r.m) v = 0;
        r.at(0, 0) = f / aspect;
        r.at(1, 1) = f;
        r.at(2, 2) = (zf + zn) / (zn - zf);
        r.at(2, 3) = 2 * zf * zn / (zn - zf);
        r.at(3, 2) = -1;
        return r;
    }
    static Mat4 lookAt(const Vec3& eye, const Vec3& target, const Vec3& up) {
        Vec3 f = normalize(target - eye);
        Vec3 s = normalize(cross(f, up));
        Vec3 u = cross(s, f);
        Mat4 r;
        r.at(0, 0) = s.x; r.at(0, 1) = s.y; r.at(0, 2) = s.z;
        r.at(1, 0) = u.x; r.at(1, 1) = u.y; r.at(1, 2) = u.z;
        r.at(2, 0) = -f.x; r.at(2, 1) = -f.y; r.at(2, 2) = -f.z;
        r.at(0, 3) = -dot(s, eye);
        r.at(1, 3) = -dot(u, eye);
        r.at(2, 3) = dot(f, eye);
        return r;
    }
};

// Deterministic hash-based random in [0,1)
inline float hashRandom(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU;
    x ^= x >> 15; x *= 0x846ca68bU;
    x ^= x >> 16;
    return (x & 0xFFFFFF) / float(0x1000000);
}
inline float hashRandom(float seed) {
    uint32_t bits;
    float s = seed + 0.0f; // normalise -0
    std::memcpy(&bits, &s, sizeof(bits));
    return hashRandom(bits * 2654435761u + 0x9e3779b9u);
}

} // namespace hd
