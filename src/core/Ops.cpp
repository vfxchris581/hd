#include "Ops.h"
#include <cctype>
#include <cmath>
#include <tuple>
#include <cstdlib>
#include <sstream>
#include <unordered_map>

namespace hd {

bool runOverElements(Geometry& g, AttribClass cls, const Program& prog, float frame, float fps,
                     std::vector<float>* results, std::string& error, float timeInc) {
    const auto& binds = prog.attribs();
    size_t count = cls == AttribClass::Point ? g.numPoints() : g.numPrims();
    // Resolve bindings
    struct B { Attrib* a = nullptr; bool isP = false; bool write = false; int tuple = 1; };
    std::vector<B> bs(binds.size());
    for (size_t i = 0; i < binds.size(); ++i) {
        const AttribBinding& ab = binds[i];
        B& b = bs[i];
        b.tuple = ab.tuple;
        b.write = ab.written;
        if (ab.name == "P") {
            b.isP = true;
            b.tuple = 3;
            if (cls == AttribClass::Prim) b.write = false;
            continue;
        }
        b.a = g.findAttrib(cls, ab.name);
        if (!b.a && ab.written) {
            Vec3 def(0);
            if (ab.name == "Cd") def = Vec3(1);
            if (ab.name == "pscale") def = Vec3(1);
            b.a = &g.addAttrib(cls, ab.name, ab.tuple, def);
        }
        if (b.a) b.tuple = b.a->tuple;
    }
    if (results) results->assign(count, 0.f);
    std::vector<Value> vals(binds.size());
    Program::Context ctx;
    ctx.attribValues = vals.data();
    ctx.globals[int(Global::Frame)] = frame;
    ctx.globals[int(Global::Time)] = (frame - 1.f) / fps;
    ctx.globals[int(Global::Fps)] = fps;
    ctx.globals[int(Global::TimeInc)] = timeInc > 0 ? timeInc : 1.f / fps;
    ctx.globals[int(Global::NumPt)] = float(count);
    for (size_t e = 0; e < count; ++e) {
        ctx.globals[int(Global::PtNum)] = float(e);
        for (size_t i = 0; i < bs.size(); ++i) {
            const B& b = bs[i];
            Value v;
            v.n = b.tuple == 3 ? 3 : 1;
            if (b.isP) {
                Vec3 p = cls == AttribClass::Point ? g.P[e] : g.primCenter(int(e));
                v = Value(p);
            } else if (b.a) {
                if (b.a->tuple == 3) v = Value(b.a->getV(e));
                else v = Value(b.a->getF(e));
            } else {
                v = b.tuple == 3 ? Value(Vec3(0)) : Value(0.f);
            }
            vals[i] = v;
        }
        Value r = prog.run(ctx);
        if (results) (*results)[e] = r.f();
        for (size_t i = 0; i < bs.size(); ++i) {
            const B& b = bs[i];
            if (!b.write) continue;
            if (b.isP) g.P[e] = vals[i].vec();
            else if (b.a) {
                if (b.a->tuple == 3) b.a->setV(e, vals[i].vec());
                else b.a->setF(e, vals[i].f());
            }
        }
    }
    (void)error;
    return true;
}

static bool parseInt(const std::string& s, long& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    out = std::strtol(s.c_str(), &end, 10);
    return end && *end == 0;
}

bool selectElements(const Geometry& g, AttribClass cls, const std::string& patternIn, float frame, float fps,
                    std::vector<char>& mask, std::string& error) {
    size_t count = cls == AttribClass::Point ? g.numPoints() : g.numPrims();
    std::string pattern = patternIn;
    // trim
    while (!pattern.empty() && std::isspace((unsigned char)pattern.front())) pattern.erase(pattern.begin());
    while (!pattern.empty() && std::isspace((unsigned char)pattern.back())) pattern.pop_back();
    bool invert = false;
    if (!pattern.empty() && pattern[0] == '!') { invert = true; pattern.erase(pattern.begin()); }
    if (pattern.empty() || pattern == "*") {
        mask.assign(count, 1);
    } else if (pattern.find('@') != std::string::npos || pattern.find('$') != std::string::npos) {
        auto prog = Program::compile(pattern, true, error);
        if (!prog) return false;
        Geometry tmp = g; // program is read-only in intent; run on a copy
        std::vector<float> res;
        if (!runOverElements(tmp, cls, *prog, frame, fps, &res, error)) return false;
        mask.assign(count, 0);
        for (size_t i = 0; i < count; ++i) mask[i] = res[i] != 0.f;
    } else {
        mask.assign(count, 0);
        std::istringstream ss(pattern);
        std::string tok;
        while (ss >> tok) {
            long a = 0, b = 0, step = 1;
            std::string range = tok;
            size_t colon = tok.find(':');
            if (colon != std::string::npos) {
                range = tok.substr(0, colon);
                if (!parseInt(tok.substr(colon + 1), step) || step <= 0) { error = "bad step in '" + tok + "'"; return false; }
            }
            size_t dash = range.find('-', 1);
            if (dash != std::string::npos) {
                if (!parseInt(range.substr(0, dash), a) || !parseInt(range.substr(dash + 1), b)) { error = "bad range '" + tok + "'"; return false; }
            } else {
                if (!parseInt(range, a)) { error = "bad group token '" + tok + "'"; return false; }
                b = a;
            }
            if (a > b) std::swap(a, b);
            for (long i = std::max(0L, a); i <= b && i < long(count); i += step) mask[size_t(i)] = 1;
        }
    }
    if (invert) for (auto& m : mask) m = !m;
    return true;
}

// ---------------------------------------------------------------- Catmull-Clark
namespace {
struct EdgeKey {
    uint64_t k;
    static EdgeKey make(int a, int b) {
        if (a > b) std::swap(a, b);
        return {(uint64_t(uint32_t(a)) << 32) | uint32_t(b)};
    }
    bool operator==(const EdgeKey& o) const { return k == o.k; }
};
struct EdgeHash { size_t operator()(const EdgeKey& e) const { return std::hash<uint64_t>()(e.k); } };

// Blend point attributes of `in` into new point `dst` of `out`
void blendAttribs(Geometry& out, size_t dst, const Geometry& in, const int* src, const float* w, int n) {
    for (auto& [name, a] : in.pointAttribs) {
        Attrib& d = out.pointAttribs[name];
        for (int k = 0; k < a.tuple; ++k) {
            float s = 0;
            for (int i = 0; i < n; ++i) s += a.data[size_t(src[i]) * a.tuple + k] * w[i];
            d.data[dst * a.tuple + k] = s;
        }
    }
}
} // namespace

Geometry catmullClark(const Geometry& in) {
    Geometry out;
    // collect closed polygons only
    std::vector<int> faces;
    for (size_t i = 0; i < in.numPrims(); ++i)
        if (in.prims[i].closed && in.prims[i].count >= 3) faces.push_back(int(i));

    size_t np = in.numPoints();
    // edges
    std::unordered_map<EdgeKey, int, EdgeHash> edgeIndex;
    struct Edge { int a, b; int f0 = -1, f1 = -1; };
    std::vector<Edge> edges;
    std::vector<std::vector<int>> pointEdges(np);
    std::vector<std::vector<int>> pointFaces(np);
    for (size_t fi = 0; fi < faces.size(); ++fi) {
        const Prim& pr = in.prims[size_t(faces[fi])];
        for (int k = 0; k < pr.count; ++k) {
            int a = in.vertices[size_t(pr.start + k)];
            int b = in.vertices[size_t(pr.start + (k + 1) % pr.count)];
            pointFaces[size_t(a)].push_back(int(fi));
            EdgeKey key = EdgeKey::make(a, b);
            auto it = edgeIndex.find(key);
            if (it == edgeIndex.end()) {
                edgeIndex[key] = int(edges.size());
                Edge e{a, b, int(fi), -1};
                pointEdges[size_t(a)].push_back(int(edges.size()));
                pointEdges[size_t(b)].push_back(int(edges.size()));
                edges.push_back(e);
            } else if (edges[size_t(it->second)].f1 < 0) {
                edges[size_t(it->second)].f1 = int(fi);
            }
        }
    }
    // Output layout: [original points][edge points][face points]
    size_t ne = edges.size(), nf = faces.size();
    size_t total = np + ne + nf;
    out.P.resize(total);
    for (auto& [name, a] : in.pointAttribs) {
        Attrib na; na.tuple = a.tuple; std::copy(a.defaults, a.defaults + 3, na.defaults);
        na.data.assign(total * size_t(a.tuple), 0.f);
        out.pointAttribs[name] = na;
    }
    for (auto& [name, a] : in.primAttribs) {
        Attrib na; na.tuple = a.tuple; std::copy(a.defaults, a.defaults + 3, na.defaults);
        out.primAttribs[name] = na;
    }
    // face points
    std::vector<Vec3> facePt(nf);
    for (size_t fi = 0; fi < nf; ++fi) {
        const Prim& pr = in.prims[size_t(faces[fi])];
        facePt[fi] = in.primCenter(faces[fi]);
        out.P[np + ne + fi] = facePt[fi];
        std::vector<float> w(size_t(pr.count), 1.f / pr.count);
        blendAttribs(out, np + ne + fi, in, &in.vertices[size_t(pr.start)], w.data(), pr.count);
    }
    // edge points
    std::vector<Vec3> edgeMid(ne);
    for (size_t ei = 0; ei < ne; ++ei) {
        const Edge& e = edges[ei];
        edgeMid[ei] = (in.P[size_t(e.a)] + in.P[size_t(e.b)]) * 0.5f;
        Vec3 p = edgeMid[ei];
        if (e.f1 >= 0) p = (in.P[size_t(e.a)] + in.P[size_t(e.b)] + facePt[size_t(e.f0)] + facePt[size_t(e.f1)]) * 0.25f;
        out.P[np + ei] = p;
        int src[2] = {e.a, e.b};
        float w[2] = {0.5f, 0.5f};
        blendAttribs(out, np + ei, in, src, w, 2);
    }
    // original points
    for (size_t pi = 0; pi < np; ++pi) {
        const Vec3& P = in.P[pi];
        int src[1] = {int(pi)};
        float w1[1] = {1.f};
        blendAttribs(out, pi, in, src, w1, 1);
        auto& pe = pointEdges[pi];
        if (pe.empty()) { out.P[pi] = P; continue; }
        std::vector<int> boundary;
        for (int ei : pe) if (edges[size_t(ei)].f1 < 0) boundary.push_back(ei);
        if (!boundary.empty()) {
            if (boundary.size() == 2) {
                const Edge& e0 = edges[size_t(boundary[0])];
                const Edge& e1 = edges[size_t(boundary[1])];
                Vec3 a = in.P[size_t(e0.a == int(pi) ? e0.b : e0.a)];
                Vec3 b = in.P[size_t(e1.a == int(pi) ? e1.b : e1.a)];
                out.P[pi] = P * 0.75f + (a + b) * 0.125f;
            } else out.P[pi] = P; // corner / non-manifold: keep
            continue;
        }
        float n = float(pe.size());
        Vec3 F(0), R(0);
        for (int fi : pointFaces[pi]) F += facePt[size_t(fi)];
        F /= float(pointFaces[pi].size());
        for (int ei : pe) R += edgeMid[size_t(ei)];
        R /= n;
        out.P[pi] = (F + R * 2.f + P * (n - 3.f)) / n;
    }
    // faces
    for (size_t fi = 0; fi < nf; ++fi) {
        int srcPrim = faces[fi];
        const Prim& pr = in.prims[size_t(srcPrim)];
        for (int k = 0; k < pr.count; ++k) {
            int v = in.vertices[size_t(pr.start + k)];
            int vn = in.vertices[size_t(pr.start + (k + 1) % pr.count)];
            int vp = in.vertices[size_t(pr.start + (k + pr.count - 1) % pr.count)];
            int eNext = int(np) + edgeIndex[EdgeKey::make(v, vn)];
            int ePrev = int(np) + edgeIndex[EdgeKey::make(vp, v)];
            int fpt = int(np + ne + fi);
            Prim q; q.start = int(out.vertices.size()); q.count = 4; q.closed = true;
            out.vertices.insert(out.vertices.end(), {v, eNext, fpt, ePrev});
            out.prims.push_back(q);
            for (auto& [name, a] : in.primAttribs) out.primAttribs[name].appendFrom(a, size_t(srcPrim));
        }
    }
    out.removeUnusedPoints();
    if (out.findAttrib(AttribClass::Point, "N")) out.computePointNormals();
    return out;
}

Geometry fusePoints(const Geometry& in, float dist) {
    Geometry out;
    dist = std::max(dist, 1e-7f);
    float inv = 1.f / dist;
    struct CellHash {
        size_t operator()(const std::tuple<int, int, int>& c) const {
            return size_t(std::get<0>(c)) * 73856093u ^ size_t(std::get<1>(c)) * 19349663u ^ size_t(std::get<2>(c)) * 83492791u;
        }
    };
    std::unordered_map<std::tuple<int, int, int>, std::vector<int>, CellHash> grid; // cell -> output points
    std::vector<int> remap(in.numPoints());
    for (auto& [name, a] : in.pointAttribs) { Attrib na; na.tuple = a.tuple; std::copy(a.defaults, a.defaults + 3, na.defaults); out.pointAttribs[name] = na; }
    for (auto& [name, a] : in.primAttribs) { Attrib na; na.tuple = a.tuple; std::copy(a.defaults, a.defaults + 3, na.defaults); out.primAttribs[name] = na; }
    for (size_t i = 0; i < in.numPoints(); ++i) {
        const Vec3& p = in.P[i];
        int cx = int(std::floor(p.x * inv)), cy = int(std::floor(p.y * inv)), cz = int(std::floor(p.z * inv));
        int found = -1;
        for (int dx = -1; dx <= 1 && found < 0; ++dx)
            for (int dy = -1; dy <= 1 && found < 0; ++dy)
                for (int dz = -1; dz <= 1 && found < 0; ++dz) {
                    auto it = grid.find({cx + dx, cy + dy, cz + dz});
                    if (it == grid.end()) continue;
                    for (int o : it->second)
                        if (length(out.P[size_t(o)] - p) <= dist) { found = o; break; }
                }
        if (found < 0) {
            found = int(out.P.size());
            out.P.push_back(p);
            for (auto& [name, a] : in.pointAttribs) out.pointAttribs[name].appendFrom(a, i);
            grid[{cx, cy, cz}].push_back(found);
        }
        remap[i] = found;
    }
    for (size_t pi = 0; pi < in.numPrims(); ++pi) {
        const Prim& pr = in.prims[pi];
        std::vector<int> pts;
        for (int k = 0; k < pr.count; ++k) {
            int v = remap[size_t(in.vertices[size_t(pr.start + k)])];
            if (pts.empty() || pts.back() != v) pts.push_back(v);
        }
        if (pr.closed && pts.size() > 1 && pts.front() == pts.back()) pts.pop_back();
        if ((pr.closed && pts.size() < 3) || pts.size() < 2) continue;
        out.addPrim(pts, pr.closed);
        for (auto& [name, a] : in.primAttribs) {
            Attrib& d = out.primAttribs[name];
            d.data.resize(d.data.size() - size_t(d.tuple)); // addPrim already resized with defaults
            d.appendFrom(a, pi);
        }
    }
    return out;
}

} // namespace hd
