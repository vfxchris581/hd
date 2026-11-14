#include "Geometry.h"
#include <limits>

namespace hd {

void Geometry::clear() {
    P.clear(); vertices.clear(); prims.clear();
    pointAttribs.clear(); primAttribs.clear();
    volumes.clear();
}

int Geometry::addPoint(const Vec3& p) {
    P.push_back(p);
    for (auto& [n, a] : pointAttribs) a.resize(P.size());
    return int(P.size()) - 1;
}

int Geometry::addPrim(const std::vector<int>& pts, bool closed) {
    Prim pr;
    pr.start = int(vertices.size());
    pr.count = int(pts.size());
    pr.closed = closed;
    vertices.insert(vertices.end(), pts.begin(), pts.end());
    prims.push_back(pr);
    for (auto& [n, a] : primAttribs) a.resize(prims.size());
    return int(prims.size()) - 1;
}

Attrib* Geometry::findAttrib(AttribClass c, const std::string& name) {
    auto& m = attribs(c);
    auto it = m.find(name);
    return it == m.end() ? nullptr : &it->second;
}
const Attrib* Geometry::findAttrib(AttribClass c, const std::string& name) const {
    auto& m = attribs(c);
    auto it = m.find(name);
    return it == m.end() ? nullptr : &it->second;
}

Attrib& Geometry::addAttrib(AttribClass c, const std::string& name, int tuple, Vec3 def) {
    auto& m = attribs(c);
    auto it = m.find(name);
    size_t n = c == AttribClass::Point ? P.size() : prims.size();
    if (it != m.end()) {
        if (it->second.tuple == tuple) return it->second;
        m.erase(it);
    }
    Attrib& a = m[name];
    a.tuple = tuple;
    a.defaults[0] = def.x; a.defaults[1] = def.y; a.defaults[2] = def.z;
    a.resize(n);
    return a;
}

void Geometry::removeAttrib(AttribClass c, const std::string& name) { attribs(c).erase(name); }

static void mergeAttribMaps(std::map<std::string, Attrib>& dst, size_t dstCount,
                            const std::map<std::string, Attrib>& src, size_t srcCount) {
    // ensure all src attribs exist in dst (padded with defaults for existing elements)
    for (auto& [name, sa] : src) {
        auto it = dst.find(name);
        if (it == dst.end()) {
            Attrib a;
            a.tuple = sa.tuple;
            std::copy(sa.defaults, sa.defaults + 3, a.defaults);
            a.resize(dstCount);
            dst[name] = std::move(a);
        }
    }
    for (auto& [name, da] : dst) {
        auto it = src.find(name);
        if (it == src.end()) {
            da.resize(dstCount + srcCount);
        } else {
            for (size_t i = 0; i < srcCount; ++i) da.appendFrom(it->second, i);
        }
    }
}

void Geometry::merge(const Geometry& o) {
    size_t np = P.size(), nprim = prims.size();
    int vOffset = int(vertices.size());
    mergeAttribMaps(pointAttribs, np, o.pointAttribs, o.P.size());
    mergeAttribMaps(primAttribs, nprim, o.primAttribs, o.prims.size());
    P.insert(P.end(), o.P.begin(), o.P.end());
    for (int v : o.vertices) vertices.push_back(v + int(np));
    for (Prim pr : o.prims) {
        pr.start += vOffset;
        prims.push_back(pr);
    }
    volumes.insert(volumes.end(), o.volumes.begin(), o.volumes.end());
}

void Geometry::transform(const Mat4& m) {
    for (auto& p : P) p = m.transformPoint(p);
    // Volumes stay axis aligned: they follow translation and (average) scale, not rotation.
    for (auto& vol : volumes) {
        float s = (length(m.transformDir(Vec3(1, 0, 0))) + length(m.transformDir(Vec3(0, 1, 0))) + length(m.transformDir(Vec3(0, 0, 1)))) / 3.f;
        Vec3 half = (vol.bmax() - vol.bmin()) * 0.5f;
        Vec3 c = m.transformPoint(vol.origin + half);
        vol.dx *= s;
        vol.origin = c - half * s;
    }
    for (const char* n : {"N", "v", "up"}) {
        Attrib* a = findAttrib(AttribClass::Point, n);
        if (!a || a->tuple != 3) continue;
        for (size_t i = 0; i < a->count(); ++i) {
            Vec3 d = a->getV(i);
            if (std::string(n) == "N") d = m.transformNormal(d);
            else d = m.transformDir(d);
            a->setV(i, d);
        }
    }
    if (Attrib* a = findAttrib(AttribClass::Prim, "N"))
        for (size_t i = 0; i < a->count(); ++i) a->setV(i, m.transformNormal(a->getV(i)));
}

void Geometry::bounds(Vec3& mn, Vec3& mx) const {
    if (P.empty() && volumes.empty()) { mn = mx = Vec3(0); return; }
    mn = Vec3(std::numeric_limits<float>::max());
    mx = Vec3(-std::numeric_limits<float>::max());
    for (auto& p : P) { mn = vmin(mn, p); mx = vmax(mx, p); }
    for (auto& v : volumes) { mn = vmin(mn, v.bmin()); mx = vmax(mx, v.bmax()); }
}

Vec3 Geometry::primNormal(int prim) const {
    const Prim& pr = prims[prim];
    if (pr.count < 3) return Vec3(0, 1, 0);
    // Newell's method — robust for non-planar polygons
    Vec3 n(0);
    for (int k = 0; k < pr.count; ++k) {
        const Vec3& a = P[vertices[pr.start + k]];
        const Vec3& b = P[vertices[pr.start + (k + 1) % pr.count]];
        n.x += (a.y - b.y) * (a.z + b.z);
        n.y += (a.z - b.z) * (a.x + b.x);
        n.z += (a.x - b.x) * (a.y + b.y);
    }
    // Houdini winding is clockwise when viewed from the front; we use counter-clockwise
    return normalize(n);
}

Vec3 Geometry::primCenter(int prim) const {
    const Prim& pr = prims[prim];
    Vec3 c(0);
    for (int k = 0; k < pr.count; ++k) c += P[vertices[pr.start + k]];
    return pr.count ? c / float(pr.count) : c;
}

float Geometry::primArea(int prim) const {
    const Prim& pr = prims[prim];
    if (pr.count < 3 || !pr.closed) return 0;
    Vec3 a = P[vertices[pr.start]];
    float area = 0;
    for (int k = 1; k + 1 < pr.count; ++k)
        area += 0.5f * length(cross(P[vertices[pr.start + k]] - a, P[vertices[pr.start + k + 1]] - a));
    return area;
}

void Geometry::computePointNormals() {
    Attrib& N = addAttrib(AttribClass::Point, "N", 3);
    std::vector<Vec3> acc(P.size(), Vec3(0));
    for (size_t pi = 0; pi < prims.size(); ++pi) {
        const Prim& pr = prims[pi];
        if (pr.count < 3 || !pr.closed) continue;
        // area weighted: Newell normal un-normalised
        Vec3 n(0);
        for (int k = 0; k < pr.count; ++k) {
            const Vec3& a = P[vertices[pr.start + k]];
            const Vec3& b = P[vertices[pr.start + (k + 1) % pr.count]];
            n.x += (a.y - b.y) * (a.z + b.z);
            n.y += (a.z - b.z) * (a.x + b.x);
            n.z += (a.x - b.x) * (a.y + b.y);
        }
        for (int k = 0; k < pr.count; ++k) acc[vertices[pr.start + k]] += n;
    }
    for (size_t i = 0; i < P.size(); ++i) {
        Vec3 n = normalize(acc[i]);
        if (length(n) < 0.5f) n = Vec3(0, 1, 0);
        N.setV(i, n);
    }
}

Geometry Geometry::extractPrims(const std::vector<int>& keepPrims) const {
    Geometry g;
    g.volumes = volumes;
    std::vector<int> remap(P.size(), -1);
    for (auto& [n, a] : pointAttribs) { Attrib na; na.tuple = a.tuple; std::copy(a.defaults, a.defaults + 3, na.defaults); g.pointAttribs[n] = na; }
    for (auto& [n, a] : primAttribs) { Attrib na; na.tuple = a.tuple; std::copy(a.defaults, a.defaults + 3, na.defaults); g.primAttribs[n] = na; }
    for (int pi : keepPrims) {
        const Prim& pr = prims[pi];
        std::vector<int> pts;
        pts.reserve(pr.count);
        for (int k = 0; k < pr.count; ++k) {
            int src = vertices[pr.start + k];
            if (remap[src] < 0) {
                remap[src] = int(g.P.size());
                g.P.push_back(P[src]);
                for (auto& [n, a] : pointAttribs) g.pointAttribs[n].appendFrom(a, src);
            }
            pts.push_back(remap[src]);
        }
        Prim np;
        np.start = int(g.vertices.size()); np.count = pr.count; np.closed = pr.closed;
        g.vertices.insert(g.vertices.end(), pts.begin(), pts.end());
        g.prims.push_back(np);
        for (auto& [n, a] : primAttribs) g.primAttribs[n].appendFrom(a, pi);
    }
    return g;
}

Geometry Geometry::extractPoints(const std::vector<char>& keep) const {
    Geometry g;
    g.volumes = volumes;
    std::vector<int> remap(P.size(), -1);
    for (auto& [n, a] : pointAttribs) { Attrib na; na.tuple = a.tuple; std::copy(a.defaults, a.defaults + 3, na.defaults); g.pointAttribs[n] = na; }
    for (auto& [n, a] : primAttribs) { Attrib na; na.tuple = a.tuple; std::copy(a.defaults, a.defaults + 3, na.defaults); g.primAttribs[n] = na; }
    for (size_t i = 0; i < P.size(); ++i) {
        if (!keep[i]) continue;
        remap[i] = int(g.P.size());
        g.P.push_back(P[i]);
        for (auto& [n, a] : pointAttribs) g.pointAttribs[n].appendFrom(a, i);
    }
    for (size_t pi = 0; pi < prims.size(); ++pi) {
        const Prim& pr = prims[pi];
        bool ok = true;
        for (int k = 0; k < pr.count && ok; ++k) ok = remap[vertices[pr.start + k]] >= 0;
        if (!ok) continue;
        Prim np;
        np.start = int(g.vertices.size()); np.count = pr.count; np.closed = pr.closed;
        for (int k = 0; k < pr.count; ++k) g.vertices.push_back(remap[vertices[pr.start + k]]);
        g.prims.push_back(np);
        for (auto& [n, a] : primAttribs) g.primAttribs[n].appendFrom(a, pi);
    }
    return g;
}

void Geometry::removeUnusedPoints() {
    std::vector<char> used(P.size(), 0);
    for (int v : vertices) used[v] = 1;
    *this = extractPoints(used);
}

void Geometry::syncAttribSizes() {
    for (auto& [n, a] : pointAttribs) a.resize(P.size());
    for (auto& [n, a] : primAttribs) a.resize(prims.size());
}

void Geometry::copyPointAttribs(const Geometry& from, int src, int dst) {
    for (auto& [n, a] : from.pointAttribs) {
        Attrib* d = findAttrib(AttribClass::Point, n);
        if (!d) continue;
        for (int k = 0; k < d->tuple; ++k)
            d->data[dst * d->tuple + k] = a.data[src * a.tuple + std::min(k, a.tuple - 1)];
    }
}

} // namespace hd
