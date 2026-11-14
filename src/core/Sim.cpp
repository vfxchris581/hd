// Simulation nodes: POP (particle) solver and Trail.
#include "Node.h"
#include "Noise.h"
#include "Ops.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <memory>

namespace hd {
namespace {

ParmDef F(const char* n, const char* l, float d, float mn = 0, float mx = 1, const char* help = "") {
    ParmDef p; p.name = n; p.label = l; p.type = ParmType::Float; p.def[0] = d; p.min = mn; p.max = mx; p.help = help; return p;
}
ParmDef I(const char* n, const char* l, int d, int mn = 0, int mx = 10, const char* help = "") {
    ParmDef p; p.name = n; p.label = l; p.type = ParmType::Int; p.def[0] = float(d); p.min = float(mn); p.max = float(mx); p.help = help; return p;
}
ParmDef T(const char* n, const char* l, bool d, const char* help = "") {
    ParmDef p; p.name = n; p.label = l; p.type = ParmType::Toggle; p.def[0] = d ? 1.f : 0.f; p.help = help; return p;
}
ParmDef V(const char* n, const char* l, Vec3 d, float mn = -1, float mx = 1, const char* help = "") {
    ParmDef p; p.name = n; p.label = l; p.type = ParmType::Vec3; p.def[0] = d.x; p.def[1] = d.y; p.def[2] = d.z; p.min = mn; p.max = mx; p.help = help; return p;
}
ParmDef M(const char* n, const char* l, std::vector<std::string> items, int d = 0) {
    ParmDef p; p.name = n; p.label = l; p.type = ParmType::Menu; p.menu = std::move(items); p.def[0] = float(d); return p;
}
ParmDef C(const char* n, const char* l, const char* d) {
    ParmDef p; p.name = n; p.label = l; p.type = ParmType::Code; p.defStr = d; return p;
}

struct PopFrame {
    Geometry g;       // particles at the end of this frame
    int nextId = 0;
    double carry = 0; // fractional births carried to the next frame
};

struct PopState {
    uint64_t sig = 0;
    std::map<int, PopFrame> frames;
    size_t totalPoints = 0;
};

// Surface / point sampler for the emitter geometry.
struct Emitter {
    const Geometry* geo = nullptr;
    struct Tri { int prim, a, b, c; };
    std::vector<Tri> tris;
    std::vector<double> cdf;
    double area = 0;
    const Attrib* N = nullptr;
    const Attrib* v = nullptr;
    const Attrib* Cd = nullptr;

    void build(const Geometry& g) {
        geo = &g;
        tris.clear(); cdf.clear(); area = 0;
        for (size_t pi = 0; pi < g.numPrims(); ++pi) {
            const Prim& pr = g.prims[pi];
            if (!pr.closed || pr.count < 3) continue;
            for (int k = 1; k + 1 < pr.count; ++k) {
                Tri t{int(pi), g.vertices[size_t(pr.start)], g.vertices[size_t(pr.start + k)], g.vertices[size_t(pr.start + k + 1)]};
                double a = 0.5 * length(cross(g.P[size_t(t.b)] - g.P[size_t(t.a)], g.P[size_t(t.c)] - g.P[size_t(t.a)]));
                if (a <= 0) continue;
                area += a;
                tris.push_back(t);
                cdf.push_back(area);
            }
        }
        N = g.findAttrib(AttribClass::Point, "N");
        v = g.findAttrib(AttribClass::Point, "v");
        Cd = g.findAttrib(AttribClass::Point, "Cd");
        if (N && N->tuple != 3) N = nullptr;
        if (v && v->tuple != 3) v = nullptr;
        if (Cd && Cd->tuple != 3) Cd = nullptr;
    }

    // Returns false if there is nothing to emit from.
    bool sample(uint32_t seed, Vec3& P, Vec3& nrm, Vec3& vel, Vec3& col, bool& hasCol) const {
        nrm = Vec3(0, 1, 0); vel = Vec3(0); hasCol = false;
        if (!geo || geo->numPoints() == 0) { P = Vec3(0); return true; }
        if (!tris.empty()) {
            double r = double(hashRandom(seed)) * area;
            size_t ti = size_t(std::lower_bound(cdf.begin(), cdf.end(), r) - cdf.begin());
            if (ti >= tris.size()) ti = tris.size() - 1;
            const Tri& t = tris[ti];
            float u = hashRandom(seed + 1), w = hashRandom(seed + 2);
            if (u + w > 1) { u = 1 - u; w = 1 - w; }
            float s = 1 - u - w;
            P = geo->P[size_t(t.a)] * s + geo->P[size_t(t.b)] * u + geo->P[size_t(t.c)] * w;
            if (N) nrm = normalize(N->getV(size_t(t.a)) * s + N->getV(size_t(t.b)) * u + N->getV(size_t(t.c)) * w);
            else nrm = geo->primNormal(t.prim);
            if (v) vel = v->getV(size_t(t.a)) * s + v->getV(size_t(t.b)) * u + v->getV(size_t(t.c)) * w;
            if (Cd) { col = Cd->getV(size_t(t.a)) * s + Cd->getV(size_t(t.b)) * u + Cd->getV(size_t(t.c)) * w; hasCol = true; }
            return true;
        }
        size_t i = std::min(geo->numPoints() - 1, size_t(hashRandom(seed) * float(geo->numPoints())));
        P = geo->P[i];
        if (N) nrm = N->getV(i);
        if (v) vel = v->getV(i);
        if (Cd) { col = Cd->getV(i); hasCol = true; }
        return true;
    }
};

Vec3 randomInSphere(uint32_t seed) {
    // rejection-free: direction * cbrt(radius)
    float z = hashRandom(seed) * 2 - 1;
    float a = hashRandom(seed + 1) * 2 * kPi;
    float r = std::sqrt(std::max(0.f, 1 - z * z));
    float rad = std::cbrt(hashRandom(seed + 2));
    return Vec3(r * std::cos(a), z, r * std::sin(a)) * rad;
}

void ensureParticleAttribs(Geometry& g) {
    g.addAttrib(AttribClass::Point, "v", 3);
    g.addAttrib(AttribClass::Point, "age", 1);
    g.addAttrib(AttribClass::Point, "life", 1, Vec3(1));
    g.addAttrib(AttribClass::Point, "id", 1);
    g.addAttrib(AttribClass::Point, "pscale", 1, Vec3(1));
    g.addAttrib(AttribClass::Point, "Cd", 3, Vec3(1));
}

void cookPopSolver(CookContext& c) {
    PopState& st = c.state<PopState>();
    uint64_t sig = c.setupSignature();
    if (st.sig != sig) {
        st.frames.clear();
        st.totalPoints = 0;
        st.sig = sig;
    }
    const int frame = int(std::floor(c.frame() + 0.5f));
    const int start = c.i("startframe");
    Node& node = c.node();
    node.simCacheStart = 0; node.simCacheEnd = -1;
    if (frame < start) {
        ensureParticleAttribs(c.out);
        return;
    }
    if (frame - start > 100000) { c.error("Frame is too far from the start frame."); return; }

    std::unique_ptr<Program> prog;
    {
        const std::string& code = c.s("snippet");
        bool blank = true;
        for (char ch : code) if (!std::isspace((unsigned char)ch)) { blank = false; break; }
        if (!blank) {
            std::string err;
            prog = Program::compile(code, false, err);
            if (!prog) { c.error("POP wrangle: " + err); return; }
        }
    }

    // Find the latest cached frame at or before the requested one.
    int have = start - 1;
    {
        auto it = st.frames.upper_bound(frame);
        if (it != st.frames.begin()) { --it; have = it->first; }
    }
    PopFrame cur;
    if (have >= start) cur = st.frames[have];
    else ensureParticleAttribs(cur.g);

    const float fps = c.fps();
    const bool animatedEmitter = c.inputIsAnimated(0);
    Geometry emitGeo;
    Emitter emitter;
    bool emitterBuilt = false;
    const uint32_t seed = uint32_t(c.i("seed")) * 9781u + 77u;

    for (int k = have + 1; k <= frame; ++k) {
        c.setEvalFrame(float(k));
        // emitter geometry for this frame
        if (!emitterBuilt || animatedEmitter) {
            if (k == frame || !animatedEmitter) emitGeo = c.input(0);
            else c.inputAtFrame(0, float(k), emitGeo);
            emitter.build(emitGeo);
            emitterBuilt = true;
        }
        Geometry& g = cur.g;
        const int substeps = std::clamp(c.i("substeps"), 1, 20);
        const float dtFrame = 1.f / fps;
        const float dt = dtFrame / float(substeps);
        const float simTime = float(k - 1) / fps;

        // ---- parameters at this frame
        const Vec3 gravity = c.v("gravity"), wind = c.v("wind");
        const float drag = c.f("drag");
        const float turb = c.f("turb"), turbFreq = c.f("turbfreq"), turbSpeed = c.f("turbspeed");
        const bool ground = c.b("ground");
        const float groundY = c.f("groundy"), bounce = c.f("bounce"), friction = std::clamp(c.f("friction"), 0.f, 1.f);
        const int behavior = c.i("behavior");
        const bool sphereOn = c.b("sphere");
        const Vec3 sphereC = c.v("spherec");
        const float sphereR = c.f("spherer");

        // ---- birth
        const int mode = c.i("emission");
        const float rate = std::max(0.f, c.f("rate"));
        const int maxPts = std::max(0, c.i("maxpts"));
        int births = 0;
        if (mode == 0) {
            double want = cur.carry + double(rate) / double(fps);
            births = int(std::floor(want));
            cur.carry = want - births;
        } else if (k == start) {
            births = int(rate);
        }
        births = std::max(0, std::min(births, maxPts - int(g.numPoints())));
        if (births > 0) {
            const Vec3 initVel = c.v("vel");
            const float alongN = c.f("speedn"), velVar = c.f("velvar"), inherit = c.f("inheritv");
            const float life = c.f("life"), lifeVar = std::clamp(c.f("lifevar"), 0.f, 1.f);
            const Vec3 birthCol = c.v("color");
            const bool colorLife = c.b("colorlife");
            size_t base = g.numPoints();
            g.P.resize(base + size_t(births));
            g.syncAttribSizes();
            Attrib& av = *g.findAttrib(AttribClass::Point, "v");
            Attrib& aage = *g.findAttrib(AttribClass::Point, "age");
            Attrib& alife = *g.findAttrib(AttribClass::Point, "life");
            Attrib& aid = *g.findAttrib(AttribClass::Point, "id");
            Attrib& aCd = *g.findAttrib(AttribClass::Point, "Cd");
            for (int b = 0; b < births; ++b) {
                size_t i = base + size_t(b);
                int id = cur.nextId++;
                uint32_t s = uint32_t(id) * 16u + seed;
                Vec3 P, n, ev, col;
                bool hasCol = false;
                emitter.sample(s, P, n, ev, col, hasCol);
                Vec3 vel = initVel + n * alongN + randomInSphere(s + 4) * velVar + ev * inherit;
                // distribute births across the frame so fast emitters don't band
                float sub = hashRandom(s + 8);
                g.P[i] = P + vel * (dtFrame * sub);
                av.setV(i, vel);
                aage.setF(i, 0);
                alife.setF(i, std::max(1e-3f, life * (1.f - lifeVar * hashRandom(s + 9))));
                aid.setF(i, float(id));
                aCd.setV(i, hasCol && !colorLife ? col : birthCol);
            }
        }

        // ---- integrate
        Attrib* av = g.findAttrib(AttribClass::Point, "v");
        Attrib* aage = g.findAttrib(AttribClass::Point, "age");
        Attrib* alife = g.findAttrib(AttribClass::Point, "life");
        for (int sstep = 0; sstep < substeps; ++sstep) {
            float t = simTime + dt * float(sstep);
            size_t n = g.numPoints();
            for (size_t i = 0; i < n; ++i) {
                Vec3 P = g.P[i], v = av->getV(i);
                Vec3 acc = gravity + (wind - v) * drag;
                if (turb != 0.f) acc += curlNoise3(P * turbFreq + Vec3(0, t * turbSpeed, 0)) * turb;
                v += acc * dt;
                P += v * dt;
                bool hit = false;
                if (ground && P.y < groundY) {
                    P.y = groundY;
                    if (v.y < 0) v.y = -v.y * bounce;
                    v.x *= 1 - friction; v.z *= 1 - friction;
                    hit = true;
                }
                if (sphereOn) {
                    Vec3 d = P - sphereC;
                    float len = length(d);
                    if (len < sphereR && len > 1e-6f) {
                        Vec3 nn = d / len;
                        P = sphereC + nn * sphereR;
                        float vn = dot(v, nn);
                        if (vn < 0) {
                            Vec3 vt = v - nn * vn;
                            v = vt * (1 - friction) - nn * (vn * bounce);
                        }
                        hit = true;
                    }
                }
                if (hit && behavior == 1) v = Vec3(0);
                if (hit && behavior == 2) alife->setF(i, 0);
                g.P[i] = P;
                av->setV(i, v);
                aage->setF(i, aage->getF(i) + dt);
            }
            if (prog) {
                std::string err;
                runOverElements(g, AttribClass::Point, *prog, float(k), fps, nullptr, err, dt);
                // the snippet may have created attributes; refresh pointers
                av = g.findAttrib(AttribClass::Point, "v");
                aage = g.findAttrib(AttribClass::Point, "age");
                alife = g.findAttrib(AttribClass::Point, "life");
                if (!av || !aage || !alife || av->tuple != 3) { c.error("POP wrangle must not change the type of v, age or life."); c.clearEvalFrame(); return; }
            }
        }

        // ---- death
        {
            size_t n = g.numPoints();
            std::vector<char> keep(n, 1);
            bool anyDead = false;
            for (size_t i = 0; i < n; ++i)
                if (aage->getF(i) >= alife->getF(i)) { keep[i] = 0; anyDead = true; }
            if (anyDead) g = g.extractPoints(keep);
        }

        // ---- appearance over life
        {
            Attrib* aCd = g.findAttrib(AttribClass::Point, "Cd");
            Attrib* aps = g.findAttrib(AttribClass::Point, "pscale");
            aage = g.findAttrib(AttribClass::Point, "age");
            alife = g.findAttrib(AttribClass::Point, "life");
            Attrib* aid = g.findAttrib(AttribClass::Point, "id");
            const bool colorLife = c.b("colorlife");
            const Vec3 c0 = c.v("color"), c1 = c.v("color2");
            const float ps = c.f("pscale"), psVar = std::clamp(c.f("pscalevar"), 0.f, 1.f), psEnd = c.f("pscaleend");
            for (size_t i = 0; i < g.numPoints(); ++i) {
                float u = std::clamp(aage->getF(i) / std::max(alife->getF(i), 1e-6f), 0.f, 1.f);
                if (colorLife && aCd) aCd->setV(i, lerp(c0, c1, u));
                if (aps && aid) {
                    float base = ps * (1.f - psVar * hashRandom(uint32_t(aid->getF(i)) * 31u + seed + 5u));
                    aps->setF(i, base * (1.f + (psEnd - 1.f) * u));
                }
            }
        }

        // ---- cache this frame
        auto old = st.frames.find(k);
        if (old != st.frames.end()) st.totalPoints -= old->second.g.numPoints();
        st.frames[k] = cur;
        st.totalPoints += cur.g.numPoints();
    }
    c.clearEvalFrame();

    // Keep memory bounded: drop the oldest cached frames first.
    const size_t cap = 40u * 1000u * 1000u;
    while (st.totalPoints > cap && st.frames.size() > 1 && st.frames.begin()->first != frame) {
        st.totalPoints -= st.frames.begin()->second.g.numPoints();
        st.frames.erase(st.frames.begin());
    }

    c.out = st.frames[frame].g;
    // report the contiguous cached range around the current frame for the timeline
    int lo = frame, hi = frame;
    while (st.frames.count(lo - 1)) --lo;
    while (st.frames.count(hi + 1)) ++hi;
    node.simCacheStart = lo;
    node.simCacheEnd = hi;
}

} // namespace

void registerSimNodes(std::vector<NodeType>& types) {
    {
        NodeType t; t.name = "popsolver"; t.label = "POP Solver"; t.category = "Simulation";
        t.description = "Particle simulation: emits from the input and applies forces and collisions frame by frame.";
        t.minInputs = 0; t.maxInputs = 1;
        t.inputLabels = {"Emitter (surface or points; uses N, v, Cd)"};
        t.timeDependent = true;
        t.parms = {
            I("startframe", "Start Frame", 1, 1, 240),
            I("substeps", "Substeps", 1, 1, 8),
            I("seed", "Seed", 0, 0, 100),
            M("emission", "Emission", {"Continuous (per second)", "Burst at Start Frame"}),
            F("rate", "Birth Rate / Count", 2000, 0, 20000, "Particles per second, or the total count for a burst"),
            I("maxpts", "Max Particles", 200000, 0, 1000000),
            F("life", "Life Expectancy", 2, 0, 10, "Seconds"),
            F("lifevar", "Life Variance", 0.3f, 0, 1),
            V("vel", "Initial Velocity", Vec3(0, 0, 0), -10, 10),
            F("speedn", "Speed Along Normal", 2, -10, 10),
            F("velvar", "Velocity Variance", 0.5f, 0, 5),
            F("inheritv", "Inherit Emitter v", 1, 0, 2),
            V("gravity", "Gravity", Vec3(0, -9.81f, 0), -20, 20),
            V("wind", "Wind", Vec3(0, 0, 0), -10, 10),
            F("drag", "Air Resistance", 0.1f, 0, 5),
            F("turb", "Turbulence", 0, 0, 10, "Curl-noise force amplitude"),
            F("turbfreq", "Turbulence Frequency", 0.6f, 0.01f, 4),
            F("turbspeed", "Turbulence Speed", 0.5f, 0, 4),
            T("ground", "Ground Collision", true),
            F("groundy", "Ground Height", 0, -5, 5),
            T("sphere", "Sphere Collider", false),
            V("spherec", "Sphere Center", Vec3(0, 0, 0), -5, 5),
            F("spherer", "Sphere Radius", 1, 0, 5),
            M("behavior", "On Collision", {"Bounce", "Stick", "Die"}),
            F("bounce", "Bounce", 0.5f, 0, 1),
            F("friction", "Friction", 0.2f, 0, 1),
            F("pscale", "Particle Size (pscale)", 0.03f, 0, 0.5f),
            F("pscalevar", "Size Variance", 0.3f, 0, 1),
            F("pscaleend", "Size at Death (multiplier)", 1, 0, 3),
            T("colorlife", "Color Over Life", true),
            V("color", "Birth Color", Vec3(1.0f, 0.85f, 0.4f), 0, 1),
            V("color2", "Death Color", Vec3(0.8f, 0.1f, 0.02f), 0, 1),
            C("snippet", "POP Wrangle (runs every substep)", ""),
        };
        for (auto& p : t.parms) if (p.name == "color" || p.name == "color2") p.help = "color";
        t.cook = cookPopSolver;
        types.push_back(t);
    }
    {
        NodeType t; t.name = "trail"; t.label = "Trail"; t.category = "Simulation";
        t.description = "Draws a streak behind every point using its velocity (v).";
        t.minInputs = 1; t.maxInputs = 1;
        t.parms = {F("length", "Trail Length (seconds)", 0.04f, 0, 0.5f), F("fade", "Tail Brightness", 0.15f, 0, 1), T("keeppts", "Keep Points", false)};
        t.cook = [](CookContext& c) {
            const Geometry& in = c.input(0);
            const Attrib* v = in.findAttrib(AttribClass::Point, "v");
            if (!v || v->tuple != 3) { c.error("Input has no vector attribute 'v'."); return; }
            const Attrib* Cd = in.findAttrib(AttribClass::Point, "Cd");
            float len = c.f("length"), fade = c.f("fade");
            Geometry& g = c.out;
            if (c.b("keeppts")) g = in;
            size_t base = g.numPoints();
            size_t n = in.numPoints();
            g.P.resize(base + n * 2);
            g.syncAttribSizes();
            Attrib& oCd = g.addAttrib(AttribClass::Point, "Cd", 3, Vec3(1));
            g.vertices.reserve(g.vertices.size() + n * 2);
            for (size_t i = 0; i < n; ++i) {
                size_t a = base + i * 2, b = a + 1;
                g.P[a] = in.P[i];
                g.P[b] = in.P[i] - v->getV(i) * len;
                Vec3 col = Cd && Cd->tuple == 3 ? Cd->getV(i) : Vec3(1);
                oCd.setV(a, col);
                oCd.setV(b, col * fade);
                Prim pr; pr.start = int(g.vertices.size()); pr.count = 2; pr.closed = false;
                g.vertices.push_back(int(a));
                g.vertices.push_back(int(b));
                g.prims.push_back(pr);
            }
            g.syncAttribSizes();
        };
        types.push_back(t);
    }
}

} // namespace hd
