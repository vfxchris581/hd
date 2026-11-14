#include "PathTracer.h"
#include "Volume.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <thread>

namespace hd {
namespace {

struct Ray { Vec3 o, d; };

struct Material { Vec3 base{0.5f}; float rough = 0.5f, metal = 0, emit = 0; };

struct Tri {
    Vec3 p0, e1, e2;
    Vec3 n0, n1, n2;
    Vec3 c0, c1, c2;
    float rough, metal, emit;
};
struct Sphere {
    Vec3 c; float r;
    Vec3 col; float rough, metal, emit;
};
struct BvhNode {
    Vec3 mn, mx;
    int left = -1, right = -1, first = 0, count = 0;
};
struct Hit {
    float t = 1e30f;
    int prim = -1;
    float u = 0, v = 0;
};

struct Rng {
    uint32_t s;
    explicit Rng(uint32_t seed) : s(seed * 747796405u + 2891336453u) { next(); }
    float next() {
        s = s * 747796405u + 2891336453u;
        uint32_t w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u;
        w = (w >> 22u) ^ w;
        return float(w >> 8) * (1.f / 16777216.f);
    }
};

inline Vec3 toLinear(const Vec3& c) {
    return {std::pow(std::max(c.x, 0.f), 2.2f), std::pow(std::max(c.y, 0.f), 2.2f), std::pow(std::max(c.z, 0.f), 2.2f)};
}
inline float maxc(const Vec3& v) { return std::max(v.x, std::max(v.y, v.z)); }

void basis(const Vec3& n, Vec3& t, Vec3& b) {
    t = std::fabs(n.x) > 0.5f ? normalize(cross(n, Vec3(0, 1, 0))) : normalize(cross(n, Vec3(1, 0, 0)));
    b = cross(n, t);
}

struct Scene {
    std::vector<Tri> tris;
    std::vector<Sphere> spheres;
    std::vector<int> order; // prim ids: < tris.size() is a triangle, otherwise a sphere
    std::vector<BvhNode> nodes;
    std::vector<Volume> vols;
    RenderSettings set;
    Vec3 sunDir;

    // ------------------------------------------------------------ build
    void addTube(const Vec3& a, const Vec3& b, float r, const Vec3& ca, const Vec3& cb, const Material& m) {
        Vec3 axis = b - a;
        if (length(axis) < 1e-9f) return;
        Vec3 t, bt;
        basis(normalize(axis), t, bt);
        Vec3 off[3];
        for (int i = 0; i < 3; ++i) {
            float ang = 2 * kPi * float(i) / 3.f;
            off[i] = t * std::cos(ang) + bt * std::sin(ang);
        }
        for (int i = 0; i < 3; ++i) {
            int j = (i + 1) % 3;
            Vec3 a0 = a + off[i] * r, a1 = a + off[j] * r, b0 = b + off[i] * r, b1 = b + off[j] * r;
            Tri t1{a0, a1 - a0, b1 - a0, off[i], off[j], off[j], ca, ca, cb, m.rough, m.metal, m.emit};
            Tri t2{a0, b1 - a0, b0 - a0, off[i], off[j], off[i], ca, cb, cb, m.rough, m.metal, m.emit};
            tris.push_back(t1);
            tris.push_back(t2);
        }
    }

    void build(const Geometry& g, const RenderSettings& s) {
        set = s;
        float az = radians(s.sunAzimuth), el = radians(s.sunElevation);
        sunDir = normalize(Vec3(std::cos(el) * std::sin(az), std::sin(el), std::cos(el) * std::cos(az)));
        for (auto& v : g.volumes)
            if (v.density && v.density->size() == v.voxels() && v.voxels() > 0) vols.push_back(v);

        const Attrib* pN = g.findAttrib(AttribClass::Point, "N");
        const Attrib* qN = g.findAttrib(AttribClass::Prim, "N");
        const Attrib* pCd = g.findAttrib(AttribClass::Point, "Cd");
        const Attrib* qCd = g.findAttrib(AttribClass::Prim, "Cd");
        const Attrib* ps = g.findAttrib(AttribClass::Point, "pscale");
        if (pN && pN->tuple != 3) pN = nullptr;
        if (qN && qN->tuple != 3) qN = nullptr;
        struct MatAttr { const Attrib* prim; const Attrib* point; float def; };
        auto ma = [&](const char* name, float def) { return MatAttr{g.findAttrib(AttribClass::Prim, name), g.findAttrib(AttribClass::Point, name), def}; };
        MatAttr aRough = ma("rough", 0.65f), aMetal = ma("metal", 0.f), aEmit = ma("emit", 0.f);
        auto val = [](const MatAttr& a, int prim, int pt) {
            if (a.prim && prim >= 0) return a.prim->getF(size_t(prim));
            if (a.point && pt >= 0) return a.point->getF(size_t(pt));
            return a.def;
        };
        const Vec3 defCol = toLinear(Vec3(0.72f, 0.74f, 0.78f));
        auto colorOf = [&](int prim, int pt) {
            if (pCd) return toLinear(pCd->getV(size_t(pt)));
            if (qCd && prim >= 0) return toLinear(qCd->getV(size_t(prim)));
            return defCol;
        };
        std::vector<char> used(g.numPoints(), 0);
        for (size_t pi = 0; pi < g.numPrims(); ++pi) {
            const Prim& pr = g.prims[pi];
            for (int k = 0; k < pr.count; ++k) used[size_t(g.vertices[size_t(pr.start + k)])] = 1;
            if (pr.count < 2) continue;
            int first = g.vertices[size_t(pr.start)];
            Material m;
            m.rough = std::clamp(val(aRough, int(pi), first), 0.02f, 1.f);
            m.metal = std::clamp(val(aMetal, int(pi), first), 0.f, 1.f);
            m.emit = std::max(0.f, val(aEmit, int(pi), first));
            if (pr.closed && pr.count >= 3) {
                Vec3 fn = qN ? qN->getV(pi) : g.primNormal(int(pi));
                auto nrm = [&](int pt) {
                    if (!pN) return fn;
                    Vec3 n = pN->getV(size_t(pt));
                    return dot(n, n) < 1e-12f ? fn : normalize(n);
                };
                for (int k = 1; k + 1 < pr.count; ++k) {
                    int a = first, b = g.vertices[size_t(pr.start + k)], c = g.vertices[size_t(pr.start + k + 1)];
                    Tri t{g.P[size_t(a)], g.P[size_t(b)] - g.P[size_t(a)], g.P[size_t(c)] - g.P[size_t(a)],
                          nrm(a), nrm(b), nrm(c), colorOf(int(pi), a), colorOf(int(pi), b), colorOf(int(pi), c), m.rough, m.metal, m.emit};
                    if (length(cross(t.e1, t.e2)) > 1e-14f) tris.push_back(t);
                }
            } else {
                int ne = pr.closed ? pr.count : pr.count - 1;
                for (int k = 0; k < ne; ++k) {
                    int a = g.vertices[size_t(pr.start + k)], b = g.vertices[size_t(pr.start + (k + 1) % pr.count)];
                    Vec3 ca = (pCd || qCd) ? colorOf(int(pi), a) : toLinear(Vec3(0.9f, 0.9f, 0.6f));
                    Vec3 cb = (pCd || qCd) ? colorOf(int(pi), b) : ca;
                    addTube(g.P[size_t(a)], g.P[size_t(b)], std::max(1e-5f, s.curveWidth * 0.5f), ca, cb, m);
                }
            }
        }
        for (size_t i = 0; i < g.numPoints(); ++i) {
            if (used[i]) continue;
            Sphere sp;
            sp.c = g.P[i];
            sp.r = std::max(1e-5f, ps ? ps->getF(i) : 0.02f);
            sp.col = pCd ? toLinear(pCd->getV(i)) : toLinear(Vec3(0.95f, 0.85f, 0.35f));
            sp.rough = std::clamp(val(aRough, -1, int(i)), 0.02f, 1.f);
            sp.metal = std::clamp(val(aMetal, -1, int(i)), 0.f, 1.f);
            sp.emit = std::max(0.f, val(aEmit, -1, int(i)));
            spheres.push_back(sp);
        }
        buildBvh();
    }

    void primBounds(int id, Vec3& mn, Vec3& mx) const {
        if (id < int(tris.size())) {
            const Tri& t = tris[size_t(id)];
            Vec3 a = t.p0, b = t.p0 + t.e1, c = t.p0 + t.e2;
            mn = vmin(a, vmin(b, c)); mx = vmax(a, vmax(b, c));
        } else {
            const Sphere& s = spheres[size_t(id) - tris.size()];
            mn = s.c - Vec3(s.r); mx = s.c + Vec3(s.r);
        }
    }

    void buildBvh() {
        size_t n = tris.size() + spheres.size();
        order.resize(n);
        std::vector<Vec3> cen(n), bmn(n), bmx(n);
        for (size_t i = 0; i < n; ++i) {
            order[i] = int(i);
            primBounds(int(i), bmn[i], bmx[i]);
            cen[i] = (bmn[i] + bmx[i]) * 0.5f;
        }
        nodes.clear();
        if (n == 0) return;
        nodes.reserve(n * 2);
        struct Job { int node, lo, hi; };
        std::vector<Job> stack;
        nodes.push_back(BvhNode());
        stack.push_back({0, 0, int(n)});
        while (!stack.empty()) {
            Job j = stack.back(); stack.pop_back();
            Vec3 mn(1e30f), mx(-1e30f), cmn(1e30f), cmx(-1e30f);
            for (int i = j.lo; i < j.hi; ++i) {
                int id = order[size_t(i)];
                mn = vmin(mn, bmn[size_t(id)]); mx = vmax(mx, bmx[size_t(id)]);
                cmn = vmin(cmn, cen[size_t(id)]); cmx = vmax(cmx, cen[size_t(id)]);
            }
            nodes[size_t(j.node)].mn = mn; nodes[size_t(j.node)].mx = mx;
            Vec3 ext = cmx - cmn;
            int axis = ext.x > ext.y ? (ext.x > ext.z ? 0 : 2) : (ext.y > ext.z ? 1 : 2);
            if (j.hi - j.lo <= 4 || ext[axis] < 1e-12f) {
                nodes[size_t(j.node)].first = j.lo;
                nodes[size_t(j.node)].count = j.hi - j.lo;
                continue;
            }
            int mid = (j.lo + j.hi) / 2;
            std::nth_element(order.begin() + j.lo, order.begin() + mid, order.begin() + j.hi,
                             [&](int a, int b) { return cen[size_t(a)][axis] < cen[size_t(b)][axis]; });
            int l = int(nodes.size());
            nodes.push_back(BvhNode());
            nodes.push_back(BvhNode());
            nodes[size_t(j.node)].left = l;
            nodes[size_t(j.node)].right = l + 1;
            stack.push_back({l, j.lo, mid});
            stack.push_back({l + 1, mid, j.hi});
        }
    }

    // ------------------------------------------------------------ intersection
    static bool slab(const Vec3& mn, const Vec3& mx, const Ray& r, const Vec3& inv, float tmax) {
        float t0 = 0, t1 = tmax;
        for (int a = 0; a < 3; ++a) {
            float ta = (mn[a] - r.o[a]) * inv[a], tb = (mx[a] - r.o[a]) * inv[a];
            if (ta > tb) std::swap(ta, tb);
            t0 = std::max(t0, ta); t1 = std::min(t1, tb);
            if (t0 > t1) return false;
        }
        return true;
    }

    bool hitPrim(int id, const Ray& r, Hit& h) const {
        if (id < int(tris.size())) {
            const Tri& t = tris[size_t(id)];
            Vec3 pv = cross(r.d, t.e2);
            float det = dot(t.e1, pv);
            if (std::fabs(det) < 1e-12f) return false;
            float inv = 1.f / det;
            Vec3 tv = r.o - t.p0;
            float u = dot(tv, pv) * inv;
            if (u < 0 || u > 1) return false;
            Vec3 qv = cross(tv, t.e1);
            float v = dot(r.d, qv) * inv;
            if (v < 0 || u + v > 1) return false;
            float tt = dot(t.e2, qv) * inv;
            if (tt < 1e-4f || tt >= h.t) return false;
            h.t = tt; h.prim = id; h.u = u; h.v = v;
            return true;
        }
        const Sphere& s = spheres[size_t(id) - tris.size()];
        Vec3 oc = r.o - s.c;
        float b = dot(oc, r.d), c = dot(oc, oc) - s.r * s.r;
        float disc = b * b - c;
        if (disc < 0) return false;
        float sq = std::sqrt(disc);
        float tt = -b - sq;
        if (tt < 1e-4f) tt = -b + sq;
        if (tt < 1e-4f || tt >= h.t) return false;
        h.t = tt; h.prim = id;
        return true;
    }

    bool intersect(const Ray& r, Hit& h, bool any) const {
        if (nodes.empty()) return false;
        Vec3 inv(1.f / r.d.x, 1.f / r.d.y, 1.f / r.d.z);
        int stack[64];
        int sp = 0;
        stack[sp++] = 0;
        bool found = false;
        while (sp) {
            const BvhNode& n = nodes[size_t(stack[--sp])];
            if (!slab(n.mn, n.mx, r, inv, h.t)) continue;
            if (n.count) {
                for (int i = 0; i < n.count; ++i)
                    if (hitPrim(order[size_t(n.first + i)], r, h)) {
                        found = true;
                        if (any) return true;
                    }
            } else if (sp < 62) {
                stack[sp++] = n.left;
                stack[sp++] = n.right;
            }
        }
        return found;
    }

    // ------------------------------------------------------------ volumes
    static float sampleField(const Volume& v, const std::vector<float>& f, const Vec3& p) {
        float x = (p.x - v.origin.x) / v.dx - 0.5f, y = (p.y - v.origin.y) / v.dx - 0.5f, z = (p.z - v.origin.z) / v.dx - 0.5f;
        if (x < -0.5f || y < -0.5f || z < -0.5f || x > v.nx - 0.5f || y > v.ny - 0.5f || z > v.nz - 0.5f) return 0;
        x = std::clamp(x, 0.f, float(v.nx - 1)); y = std::clamp(y, 0.f, float(v.ny - 1)); z = std::clamp(z, 0.f, float(v.nz - 1));
        int i = std::min(int(x), v.nx - 2), j = std::min(int(y), v.ny - 2), k = std::min(int(z), v.nz - 2);
        if (i < 0 || j < 0 || k < 0) return 0;
        float fx = x - float(i), fy = y - float(j), fz = z - float(k);
        const float* q = f.data() + size_t(i) + size_t(v.nx) * (size_t(j) + size_t(v.ny) * size_t(k));
        size_t sx = size_t(v.nx), sxy = sx * size_t(v.ny);
        float c00 = q[0] + (q[1] - q[0]) * fx, c10 = q[sx] + (q[sx + 1] - q[sx]) * fx;
        float c01 = q[sxy] + (q[sxy + 1] - q[sxy]) * fx, c11 = q[sxy + sx] + (q[sxy + sx + 1] - q[sxy + sx]) * fx;
        float c0 = c00 + (c10 - c00) * fy, c1 = c01 + (c11 - c01) * fy;
        return c0 + (c1 - c0) * fz;
    }
    static bool boxRange(const Volume& v, const Ray& r, float tmax, float& t0, float& t1) {
        Vec3 mn = v.bmin(), mx = v.bmax();
        t0 = 0; t1 = tmax;
        for (int a = 0; a < 3; ++a) {
            float inv = 1.f / r.d[a];
            float ta = (mn[a] - r.o[a]) * inv, tb = (mx[a] - r.o[a]) * inv;
            if (ta > tb) std::swap(ta, tb);
            t0 = std::max(t0, ta); t1 = std::min(t1, tb);
            if (t0 >= t1) return false;
        }
        return true;
    }
    static constexpr float kExt = 6.f; // extinction per unit density, matches the viewport

    // Transmittance of all volumes along a ray segment (for shadow rays).
    float transmittance(const Ray& r, float tmax, Rng& rng) const {
        float T = 1;
        for (auto& v : vols) {
            float t0, t1;
            if (!boxRange(v, r, tmax, t0, t1)) continue;
            int n = std::clamp(int((t1 - t0) / (v.dx * 1.5f)) + 1, 1, 48);
            float step = (t1 - t0) / float(n), sum = 0, t = t0 + step * rng.next();
            for (int i = 0; i < n; ++i, t += step) sum += sampleField(v, *v.density, r.o + r.d * t);
            T *= std::exp(-sum * step * kExt * v.densityScale);
            if (T < 0.003f) return 0;
        }
        return T;
    }

    // Single scattering + emission along a camera/bounce ray segment.
    void march(const Ray& r, float tmax, Rng& rng, Vec3& L, float& T) const {
        L = Vec3(0); T = 1;
        const Vec3 sunE = Vec3(1.f, 0.96f, 0.9f) * set.sunIntensity;
        const Vec3 amb = Vec3(0.55f, 0.65f, 0.85f) * (0.5f * set.skyIntensity);
        for (auto& v : vols) {
            float t0, t1;
            if (!boxRange(v, r, tmax, t0, t1)) continue;
            int n = std::clamp(int((t1 - t0) / v.dx) + 1, 1, 200);
            float step = (t1 - t0) / float(n), t = t0 + step * rng.next();
            const Vec3 albedo = toLinear(v.color);
            const bool hasHeat = v.fire > 0 && v.heat && v.heat->size() == v.density->size();
            for (int i = 0; i < n && T > 0.005f; ++i, t += step) {
                Vec3 p = r.o + r.d * t;
                float d = sampleField(v, *v.density, p);
                if (d > 0.003f) {
                    float a = 1.f - std::exp(-d * v.densityScale * step * kExt);
                    // light reaching this point through the volume
                    float sum = 0, ls = v.dx * 1.5f;
                    for (int k = 1; k <= 10; ++k) sum += sampleField(v, *v.density, p + sunDir * (ls * float(k)));
                    float Ts = std::exp(-sum * ls * kExt * v.densityScale * 0.7f);
                    L += albedo * (sunE * (Ts * 0.32f) + amb) * (T * a);
                    T *= 1.f - a;
                }
                if (hasHeat) {
                    float h = sampleField(v, *v.heat, p);
                    if (h > 0.02f) L += Vec3(h * 3.2f, h * h * 1.7f, h * h * h * 0.7f) * (T * v.fire * step * 9.f);
                }
            }
        }
    }

    // ------------------------------------------------------------ shading
    Vec3 sky(const Vec3& d) const {
        const Vec3 horizon(0.85f, 0.88f, 0.93f), zenith(0.36f, 0.50f, 0.80f), below(0.22f, 0.21f, 0.20f);
        Vec3 c = d.y >= 0 ? lerp(horizon, zenith, std::sqrt(std::min(1.f, d.y))) : lerp(horizon, below, std::min(1.f, -d.y * 5.f));
        return c * set.skyIntensity;
    }

    // screenV: 0 at the bottom of the image, 1 at the top (for the backdrop gradient)
    Vec3 radiance(Ray ray, Rng& rng, float screenV) const {
        Vec3 L(0), beta(1);
        const Vec3 sunE = Vec3(1.f, 0.96f, 0.9f) * set.sunIntensity;
        for (int bounce = 0; bounce <= set.bounces; ++bounce) {
            Hit h;
            bool hit = intersect(ray, h, false);
            bool groundHit = false;
            if (set.ground && ray.d.y < -1e-6f) {
                float tg = (set.groundY - ray.o.y) / ray.d.y;
                if (tg > 1e-4f && tg < h.t) { h.t = tg; groundHit = true; hit = true; }
            }
            if (!vols.empty()) {
                Vec3 Lv; float Tv;
                march(ray, hit ? h.t : 1e30f, rng, Lv, Tv);
                L += beta * Lv;
                beta *= Tv;
                if (maxc(beta) < 1e-3f) break;
            }
            if (!hit) {
                if (bounce == 0) {
                    // camera sees a neutral studio backdrop (same gradient as the viewport); lighting still comes from the sky
                    float t = screenV * screenV * (3 - 2 * screenV);
                    L += beta * toLinear(lerp(Vec3(0.13f, 0.135f, 0.15f), Vec3(0.40f, 0.43f, 0.48f), t)) * (0.8f / std::max(set.exposure, 1e-3f));
                } else L += beta * sky(ray.d);
                break;
            }

            Vec3 pos = ray.o + ray.d * h.t, n;
            Material m;
            if (groundHit) {
                n = Vec3(0, 1, 0);
                m.base = Vec3(0.18f); m.rough = 0.9f;
            } else if (h.prim < int(tris.size())) {
                const Tri& t = tris[size_t(h.prim)];
                float w = 1 - h.u - h.v;
                n = normalize(t.n0 * w + t.n1 * h.u + t.n2 * h.v);
                Vec3 gn = normalize(cross(t.e1, t.e2));
                if (dot(gn, ray.d) > 0) gn = -gn;
                if (dot(n, gn) < 0) n = -n;
                if (dot(n, ray.d) > 0) n = gn; // shading normal faces away: fall back to the geometric one
                m.base = t.c0 * w + t.c1 * h.u + t.c2 * h.v;
                m.rough = t.rough; m.metal = t.metal; m.emit = t.emit;
            } else {
                const Sphere& s = spheres[size_t(h.prim) - tris.size()];
                n = normalize(pos - s.c);
                if (dot(n, ray.d) > 0) n = -n;
                m.base = s.col; m.rough = s.rough; m.metal = s.metal; m.emit = s.emit;
            }
            if (m.emit > 0) L += beta * m.base * m.emit;

            const Vec3 wo = -ray.d;
            const Vec3 F0 = lerp(Vec3(0.04f), m.base, m.metal);
            const float alpha = m.rough * m.rough;
            const float np = std::clamp(2.f / (alpha * alpha) - 2.f, 0.f, 50000.f);
            const Vec3 R = normalize(n * (2 * dot(n, wo)) - wo);
            const Vec3 diff = m.base * (1.f - m.metal);
            Vec3 origin = pos + n * 1e-4f * std::max(1.f, h.t);

            // direct sunlight (slightly soft shadows)
            {
                Vec3 t, b;
                basis(sunDir, t, b);
                float a = 2 * kPi * rng.next(), rr = 0.02f * std::sqrt(rng.next());
                Vec3 sd = normalize(sunDir + t * (std::cos(a) * rr) + b * (std::sin(a) * rr));
                float ndl = dot(n, sd);
                if (ndl > 0) {
                    Ray sr{origin, sd};
                    Hit sh;
                    if (!intersect(sr, sh, true)) {
                        float Ts = vols.empty() ? 1.f : transmittance(sr, 1e30f, rng);
                        float spec = (np + 2.f) / (2 * kPi) * std::pow(std::max(dot(R, sd), 0.f), np);
                        spec = std::min(spec, 40.f);
                        L += beta * sunE * (diff * (1.f / kPi) + F0 * spec) * (ndl * Ts);
                    }
                }
            }

            // choose the next direction
            float cosv = std::max(dot(n, wo), 0.f);
            Vec3 F = F0 + (Vec3(1) - F0) * std::pow(1.f - cosv, 5.f);
            float pSpec = std::clamp(maxc(F), 0.25f, 1.f); // oversample the glossy lobe to keep reflections clean
            if (maxc(diff) <= 0) pSpec = 1;
            Vec3 dir;
            if (rng.next() < pSpec) {
                Vec3 t, b;
                basis(R, t, b);
                float ct = std::pow(rng.next(), 1.f / (np + 1.f)), st = std::sqrt(std::max(0.f, 1 - ct * ct)), ph = 2 * kPi * rng.next();
                dir = normalize(R * ct + t * (st * std::cos(ph)) + b * (st * std::sin(ph)));
                if (dot(dir, n) <= 0) break;
                beta = beta * F / pSpec;
            } else {
                Vec3 t, b;
                basis(n, t, b);
                float r1 = rng.next(), r2 = 2 * kPi * rng.next(), sr = std::sqrt(r1);
                dir = normalize(t * (sr * std::cos(r2)) + b * (sr * std::sin(r2)) + n * std::sqrt(1 - r1));
                beta = beta * diff * ((1.f - maxc(F)) / std::max(1e-3f, 1.f - pSpec));
            }
            if (bounce >= 2) {
                float q = std::clamp(maxc(beta), 0.05f, 0.95f);
                if (rng.next() > q) break;
                beta = beta / q;
            }
            ray = Ray{origin, dir};
        }
        // tame fireflies
        float m = maxc(L);
        if (!(m == m)) return Vec3(0);
        if (m > 30.f) L = L * (30.f / m);
        return L;
    }
};

inline unsigned char tone(float c, float exposure) {
    c *= exposure;
    c = (c * (2.51f * c + 0.03f)) / (c * (2.43f * c + 0.59f) + 0.14f); // ACES fit
    c = std::pow(std::clamp(c, 0.f, 1.f), 1.f / 2.2f);
    return (unsigned char)(c * 255.f + 0.5f);
}

} // namespace

// ---------------------------------------------------------------- PathTracer
struct PathTracer::Impl {
    std::thread thread;
    std::atomic<bool> cancel{false}, running{false};
    std::atomic<int> done{0};
    int target = 0;
    std::mutex m;
    std::vector<uint8_t> display;
    int w = 0, h = 0;
    uint64_t version = 0;
    std::chrono::steady_clock::time_point t0;
    std::atomic<double> elapsed{0};

    void run(std::shared_ptr<Scene> scene) {
        const RenderSettings& s = scene->set;
        const int W = s.width, H = s.height;
        std::vector<float> accum(size_t(W) * size_t(H) * 3, 0.f);
        Vec3 eye = s.eye();
        Vec3 fwd = normalize(s.camTarget - eye);
        if (length(fwd) < 0.5f) fwd = Vec3(0, 0, -1);
        Vec3 right = normalize(cross(fwd, Vec3(0, 1, 0)));
        if (length(right) < 0.5f) right = Vec3(1, 0, 0);
        Vec3 up = cross(right, fwd);
        const float th = std::tan(radians(s.camFov) * 0.5f), aspect = float(W) / float(H);
        std::vector<uint8_t> img(size_t(W) * size_t(H) * 4, 255);
        for (int sidx = 0; sidx < s.samples && !cancel; ++sidx) {
            parallelFor(H, [&](int y0, int y1) {
                for (int y = y0; y < y1 && !cancel; ++y)
                    for (int x = 0; x < W; ++x) {
                        Rng rng(uint32_t(x + y * W) * 9781u + uint32_t(sidx) * 6271u + 17u);
                        float px = (float(x) + rng.next()) / float(W) * 2 - 1, py = 1 - (float(y) + rng.next()) / float(H) * 2;
                        Ray r{eye, normalize(fwd + right * (px * th * aspect) + up * (py * th))};
                        Vec3 c = scene->radiance(r, rng, py * 0.5f + 0.5f);
                        float* a = &accum[(size_t(y) * size_t(W) + size_t(x)) * 3];
                        a[0] += c.x; a[1] += c.y; a[2] += c.z;
                    }
            });
            if (cancel) break;
            const float inv = s.exposure / float(sidx + 1);
            for (size_t i = 0, n = size_t(W) * size_t(H); i < n; ++i) {
                img[i * 4] = tone(accum[i * 3], inv);
                img[i * 4 + 1] = tone(accum[i * 3 + 1], inv);
                img[i * 4 + 2] = tone(accum[i * 3 + 2], inv);
            }
            {
                std::lock_guard<std::mutex> lk(m);
                display = img;
                w = W; h = H;
                ++version;
            }
            done = sidx + 1;
            elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        }
        running = false;
    }
};

PathTracer::PathTracer() : impl_(new Impl()) {}
PathTracer::~PathTracer() { stop(); }

void PathTracer::start(const Geometry& geo, const RenderSettings& settings) {
    stop();
    RenderSettings s = settings;
    s.width = std::clamp(s.width, 16, 4096);
    s.height = std::clamp(s.height, 16, 4096);
    s.samples = std::clamp(s.samples, 1, 100000);
    s.bounces = std::clamp(s.bounces, 0, 16);
    auto scene = std::make_shared<Scene>();
    scene->build(geo, s);
    impl_->cancel = false;
    impl_->done = 0;
    impl_->target = s.samples;
    impl_->running = true;
    impl_->elapsed = 0;
    impl_->t0 = std::chrono::steady_clock::now();
    impl_->thread = std::thread([this, scene] { impl_->run(scene); });
}

void PathTracer::stop() {
    impl_->cancel = true;
    if (impl_->thread.joinable()) impl_->thread.join();
    impl_->running = false;
}
void PathTracer::wait() {
    if (impl_->thread.joinable()) impl_->thread.join();
}
bool PathTracer::running() const { return impl_->running; }
int PathTracer::samplesDone() const { return impl_->done; }
int PathTracer::samplesTarget() const { return impl_->target; }
double PathTracer::secondsElapsed() const { return impl_->elapsed; }

bool PathTracer::fetch(std::vector<uint8_t>& rgba, int& w, int& h, uint64_t& version) {
    std::lock_guard<std::mutex> lk(impl_->m);
    if (impl_->version == version || impl_->display.empty()) return false;
    rgba = impl_->display;
    w = impl_->w; h = impl_->h;
    version = impl_->version;
    return true;
}

// ---------------------------------------------------------------- PNG (stored deflate)
bool writePng(const std::string& path, const std::vector<uint8_t>& rgba, int w, int h, std::string* error) {
    if (w <= 0 || h <= 0 || rgba.size() < size_t(w) * size_t(h) * 4) { if (error) *error = "no image to save"; return false; }
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t n = 0; n < 256; ++n) {
            uint32_t c = n;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[n] = c;
        }
        init = true;
    }
    std::vector<uint8_t> out;
    auto be32 = [&](std::vector<uint8_t>& v, uint32_t x) { v.push_back(uint8_t(x >> 24)); v.push_back(uint8_t(x >> 16)); v.push_back(uint8_t(x >> 8)); v.push_back(uint8_t(x)); };
    auto chunk = [&](const char* type, const std::vector<uint8_t>& data) {
        be32(out, uint32_t(data.size()));
        size_t start = out.size();
        out.insert(out.end(), type, type + 4);
        out.insert(out.end(), data.begin(), data.end());
        uint32_t c = 0xFFFFFFFFu;
        for (size_t i = start; i < out.size(); ++i) c = table[(c ^ out[i]) & 0xFF] ^ (c >> 8);
        be32(out, c ^ 0xFFFFFFFFu);
    };
    const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    out.insert(out.end(), sig, sig + 8);
    std::vector<uint8_t> ihdr;
    be32(ihdr, uint32_t(w)); be32(ihdr, uint32_t(h));
    ihdr.push_back(8); ihdr.push_back(2); ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);
    chunk("IHDR", ihdr);
    // raw scanlines: filter byte 0 + RGB
    std::vector<uint8_t> raw;
    raw.reserve(size_t(h) * (size_t(w) * 3 + 1));
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);
        for (int x = 0; x < w; ++x) {
            const uint8_t* p = &rgba[(size_t(y) * size_t(w) + size_t(x)) * 4];
            raw.push_back(p[0]); raw.push_back(p[1]); raw.push_back(p[2]);
        }
    }
    std::vector<uint8_t> z;
    z.push_back(0x78); z.push_back(0x01);
    uint32_t a = 1, b = 0;
    for (size_t pos = 0; pos < raw.size();) {
        size_t n = std::min<size_t>(65535, raw.size() - pos);
        z.push_back(pos + n == raw.size() ? 1 : 0);
        z.push_back(uint8_t(n & 0xFF)); z.push_back(uint8_t(n >> 8));
        z.push_back(uint8_t(~n & 0xFF)); z.push_back(uint8_t((~n >> 8) & 0xFF));
        z.insert(z.end(), raw.begin() + long(pos), raw.begin() + long(pos + n));
        for (size_t i = pos; i < pos + n; ++i) { a = (a + raw[i]) % 65521u; b = (b + a) % 65521u; }
        pos += n;
    }
    be32(z, (b << 16) | a);
    chunk("IDAT", z);
    chunk("IEND", {});
    std::ofstream f(path, std::ios::binary);
    if (!f) { if (error) *error = "cannot write '" + path + "'"; return false; }
    f.write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()));
    return bool(f);
}

} // namespace hd
