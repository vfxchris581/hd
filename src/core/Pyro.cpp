// Volume nodes: Pyro Solver (smoke / fire on a staggered MAC grid) and Fog Volume.
#include "Node.h"
#include "Noise.h"
#include "Volume.h"
#include <algorithm>
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

// Trilinear lookup into a grid of size (sx, sy, sz), coordinates in index space, clamped.
inline float interp(const std::vector<float>& f, int sx, int sy, int sz, float x, float y, float z) {
    x = std::min(std::max(x, 0.f), float(sx - 1));
    y = std::min(std::max(y, 0.f), float(sy - 1));
    z = std::min(std::max(z, 0.f), float(sz - 1));
    int i = std::min(int(x), sx - 2), j = std::min(int(y), sy - 2), k = std::min(int(z), sz - 2);
    float fx = x - float(i), fy = y - float(j), fz = z - float(k);
    const float* p = f.data() + size_t(i) + size_t(sx) * (size_t(j) + size_t(sy) * size_t(k));
    const size_t sxy = size_t(sx) * size_t(sy);
    float c00 = p[0] + (p[1] - p[0]) * fx;
    float c10 = p[sx] + (p[sx + 1] - p[sx]) * fx;
    float c01 = p[sxy] + (p[sxy + 1] - p[sxy]) * fx;
    float c11 = p[sxy + sx] + (p[sxy + sx + 1] - p[sxy + sx]) * fx;
    float c0 = c00 + (c10 - c00) * fy, c1 = c01 + (c11 - c01) * fy;
    return c0 + (c1 - c0) * fz;
}

struct StepParms {
    float dt = 1.f / 24.f, time = 0;
    float buoyancy = 4, weight = 0.5f;
    Vec3 wind{0, 0, 0};
    float dissipation = 0.3f, cooling = 0.9f;
    float vorticity = 3, turb = 1, turbFreq = 1.2f, turbSpeed = 0.5f;
    bool ground = true;
    int iterations = 40;
    bool emit = true;
    float srcDensity = 1, srcTemp = 1;
    Vec3 srcVel{0, 0, 0};
};

// Smoke simulation on a MAC grid: u/v/w live on cell faces, scalars at cell centres.
struct PyroSim {
    int nx = 0, ny = 0, nz = 0;
    float dx = 0.1f;
    Vec3 origin;
    std::vector<float> u, v, w, d, t, p, div;
    std::vector<float> nu, nv, nw, nd, nt;
    std::vector<float> cu, cv, cw, ox, oy, oz, fx, fy, fz;
    std::vector<unsigned char> solid, src;

    size_t C(int i, int j, int k) const { return size_t(i) + size_t(nx) * (size_t(j) + size_t(ny) * size_t(k)); }
    size_t U(int i, int j, int k) const { return size_t(i) + size_t(nx + 1) * (size_t(j) + size_t(ny) * size_t(k)); }
    size_t Vi(int i, int j, int k) const { return size_t(i) + size_t(nx) * (size_t(j) + size_t(ny + 1) * size_t(k)); }
    size_t W(int i, int j, int k) const { return size_t(i) + size_t(nx) * (size_t(j) + size_t(ny) * size_t(k)); }

    void init(int x, int y, int z, float h, const Vec3& org) {
        nx = x; ny = y; nz = z; dx = h; origin = org;
        size_t n = size_t(nx) * ny * nz;
        u.assign(size_t(nx + 1) * ny * nz, 0); v.assign(size_t(nx) * (ny + 1) * nz, 0); w.assign(size_t(nx) * ny * (nz + 1), 0);
        nu = u; nv = v; nw = w;
        for (auto* f : {&d, &t, &p, &div, &nd, &nt, &cu, &cv, &cw, &ox, &oy, &oz, &fx, &fy, &fz}) f->assign(n, 0);
        solid.assign(n, 0);
        src.assign(n, 0);
    }

    Vec3 velAt(float x, float y, float z) const {
        return {interp(u, nx + 1, ny, nz, x, y - 0.5f, z - 0.5f), interp(v, nx, ny + 1, nz, x - 0.5f, y, z - 0.5f),
                interp(w, nx, ny, nz + 1, x - 0.5f, y - 0.5f, z)};
    }

    void enforceSolids(bool ground) {
        parallelFor(nz, [&](int k0, int k1) {
            for (int k = k0; k < k1; ++k)
                for (int j = 0; j < ny; ++j)
                    for (int i = 0; i < nx; ++i) {
                        if (!solid[C(i, j, k)]) continue;
                        u[U(i, j, k)] = u[U(i + 1, j, k)] = 0;
                        v[Vi(i, j, k)] = v[Vi(i, j + 1, k)] = 0;
                    }
        });
        // w faces are shared between z slabs, so do them serially
        for (int k = 0; k < nz; ++k)
            for (int j = 0; j < ny; ++j)
                for (int i = 0; i < nx; ++i)
                    if (solid[C(i, j, k)]) w[W(i, j, k)] = w[W(i, j, k + 1)] = 0;
        if (ground)
            for (int k = 0; k < nz; ++k)
                for (int i = 0; i < nx; ++i) v[Vi(i, 0, k)] = 0;
    }

    void addSources(const StepParms& s) {
        if (!s.emit) return;
        bool hasVel = dot(s.srcVel, s.srcVel) > 0;
        for (int k = 0; k < nz; ++k)
            for (int j = 0; j < ny; ++j)
                for (int i = 0; i < nx; ++i) {
                    size_t c = C(i, j, k);
                    if (!src[c] || solid[c]) continue;
                    d[c] = std::max(d[c], s.srcDensity);
                    t[c] = std::max(t[c], s.srcTemp);
                    if (hasVel) {
                        u[U(i, j, k)] = u[U(i + 1, j, k)] = s.srcVel.x;
                        v[Vi(i, j, k)] = v[Vi(i, j + 1, k)] = s.srcVel.y;
                        w[W(i, j, k)] = w[W(i, j, k + 1)] = s.srcVel.z;
                    }
                }
    }

    void addForces(const StepParms& s) {
        const bool vort = s.vorticity > 0;
        // cell-centred velocity
        if (vort)
            parallelFor(nz, [&](int k0, int k1) {
                for (int k = k0; k < k1; ++k)
                    for (int j = 0; j < ny; ++j)
                        for (int i = 0; i < nx; ++i) {
                            size_t c = C(i, j, k);
                            cu[c] = 0.5f * (u[U(i, j, k)] + u[U(i + 1, j, k)]);
                            cv[c] = 0.5f * (v[Vi(i, j, k)] + v[Vi(i, j + 1, k)]);
                            cw[c] = 0.5f * (w[W(i, j, k)] + w[W(i, j, k + 1)]);
                        }
            });
        // vorticity (curl of velocity)
        if (vort) {
            const float inv = 0.5f / dx;
            const size_t sx = 1, sy = size_t(nx), sz = size_t(nx) * size_t(ny);
            parallelFor(nz, [&](int k0, int k1) {
                for (int k = k0; k < k1; ++k)
                    for (int j = 0; j < ny; ++j)
                        for (int i = 0; i < nx; ++i) {
                            size_t c = C(i, j, k);
                            if (i == 0 || j == 0 || k == 0 || i == nx - 1 || j == ny - 1 || k == nz - 1) { ox[c] = oy[c] = oz[c] = 0; continue; }
                            ox[c] = ((cw[c + sy] - cw[c - sy]) - (cv[c + sz] - cv[c - sz])) * inv;
                            oy[c] = ((cu[c + sz] - cu[c - sz]) - (cw[c + sx] - cw[c - sx])) * inv;
                            oz[c] = ((cv[c + sx] - cv[c - sx]) - (cu[c + sy] - cu[c - sy])) * inv;
                        }
            });
        }
        // forces at cell centres
        const size_t sx = 1, sy = size_t(nx), sz = size_t(nx) * size_t(ny);
        parallelFor(nz, [&](int k0, int k1) {
            auto mag = [&](size_t c) { return std::sqrt(ox[c] * ox[c] + oy[c] * oy[c] + oz[c] * oz[c]); };
            for (int k = k0; k < k1; ++k)
                for (int j = 0; j < ny; ++j)
                    for (int i = 0; i < nx; ++i) {
                        size_t c = C(i, j, k);
                        Vec3 f(0, s.buoyancy * t[c] - s.weight * d[c], 0);
                        float presence = std::min(1.f, std::max(d[c], t[c]) * 3.f);
                        if (presence > 0.01f) {
                            f += s.wind * presence;
                            if (s.turb > 0) {
                                Vec3 pw = origin + Vec3(i + 0.5f, j + 0.5f, k + 0.5f) * dx;
                                f += curlNoise3(pw * s.turbFreq + Vec3(0, s.time * s.turbSpeed, 0)) * (s.turb * presence);
                            }
                        }
                        if (vort && i > 0 && j > 0 && k > 0 && i < nx - 1 && j < ny - 1 && k < nz - 1) {
                            Vec3 g(mag(c + sx) - mag(c - sx), mag(c + sy) - mag(c - sy), mag(c + sz) - mag(c - sz));
                            float gl = length(g);
                            if (gl > 1e-6f) f += cross(g / gl, Vec3(ox[c], oy[c], oz[c])) * (s.vorticity * dx);
                        }
                        fx[c] = f.x; fy[c] = f.y; fz[c] = f.z;
                    }
        });
        // centres -> faces
        parallelFor(nz, [&](int k0, int k1) {
            for (int k = k0; k < k1; ++k)
                for (int j = 0; j < ny; ++j)
                    for (int i = 0; i < nx; ++i) {
                        size_t c = C(i, j, k);
                        if (i > 0) u[U(i, j, k)] += s.dt * 0.5f * (fx[c] + fx[c - 1]);
                        if (j > 0) v[Vi(i, j, k)] += s.dt * 0.5f * (fy[c] + fy[c - sy]);
                        if (k > 0) w[W(i, j, k)] += s.dt * 0.5f * (fz[c] + fz[c - sz]);
                    }
        });
    }

    // Make the velocity field divergence free (red-black Gauss-Seidel with over-relaxation).
    void project(const StepParms& s) {
        parallelFor(nz, [&](int k0, int k1) {
            for (int k = k0; k < k1; ++k)
                for (int j = 0; j < ny; ++j)
                    for (int i = 0; i < nx; ++i)
                        div[C(i, j, k)] = u[U(i + 1, j, k)] - u[U(i, j, k)] + v[Vi(i, j + 1, k)] - v[Vi(i, j, k)] + w[W(i, j, k + 1)] - w[W(i, j, k)];
        });
        const float omega = 1.7f;
        const size_t sy = size_t(nx), sz = size_t(nx) * size_t(ny);
        for (int it = 0; it < s.iterations; ++it)
            for (int colour = 0; colour < 2; ++colour)
                parallelFor(nz, [&](int k0, int k1) {
                    for (int k = k0; k < k1; ++k)
                        for (int j = 0; j < ny; ++j)
                            for (int i = (j + k + colour) & 1; i < nx; i += 2) {
                                size_t c = C(i, j, k);
                                if (solid[c]) continue;
                                float sum = 0;
                                int diag = 0;
                                if (i > 0) { if (!solid[c - 1]) { sum += p[c - 1]; ++diag; } } else ++diag;
                                if (i < nx - 1) { if (!solid[c + 1]) { sum += p[c + 1]; ++diag; } } else ++diag;
                                if (j > 0) { if (!solid[c - sy]) { sum += p[c - sy]; ++diag; } } else if (!s.ground) ++diag;
                                if (j < ny - 1) { if (!solid[c + sy]) { sum += p[c + sy]; ++diag; } } else ++diag;
                                if (k > 0) { if (!solid[c - sz]) { sum += p[c - sz]; ++diag; } } else ++diag;
                                if (k < nz - 1) { if (!solid[c + sz]) { sum += p[c + sz]; ++diag; } } else ++diag;
                                if (!diag) continue;
                                p[c] += omega * ((sum - div[c]) / float(diag) - p[c]);
                            }
                });
        // subtract the pressure gradient
        auto P = [&](int i, int j, int k) { return (i < 0 || j < 0 || k < 0 || i >= nx || j >= ny || k >= nz) ? 0.f : p[C(i, j, k)]; };
        auto S = [&](int i, int j, int k) { return !(i < 0 || j < 0 || k < 0 || i >= nx || j >= ny || k >= nz) && solid[C(i, j, k)]; };
        parallelFor(nz + 1, [&](int k0, int k1) {
            for (int k = k0; k < k1; ++k)
                for (int j = 0; j <= ny; ++j)
                    for (int i = 0; i <= nx; ++i) {
                        if (j < ny && k < nz) {
                            float& f = u[U(i, j, k)];
                            if (S(i - 1, j, k) || S(i, j, k)) f = 0; else f -= P(i, j, k) - P(i - 1, j, k);
                        }
                        if (i < nx && k < nz) {
                            float& f = v[Vi(i, j, k)];
                            if (S(i, j - 1, k) || S(i, j, k) || (j == 0 && s.ground)) f = 0; else f -= P(i, j, k) - P(i, j - 1, k);
                        }
                        if (i < nx && j < ny) {
                            float& f = w[W(i, j, k)];
                            if (S(i, j, k - 1) || S(i, j, k)) f = 0; else f -= P(i, j, k) - P(i, j, k - 1);
                        }
                    }
        });
    }

    void advect(const StepParms& s) {
        const float a = s.dt / dx;
        parallelFor(nz + 1, [&](int k0, int k1) {
            for (int k = k0; k < k1; ++k)
                for (int j = 0; j <= ny; ++j)
                    for (int i = 0; i <= nx; ++i) {
                        if (j < ny && k < nz) {
                            float x = float(i), y = j + 0.5f, z = k + 0.5f;
                            Vec3 vel = velAt(x, y, z) * a;
                            nu[U(i, j, k)] = interp(u, nx + 1, ny, nz, x - vel.x, y - vel.y - 0.5f, z - vel.z - 0.5f);
                        }
                        if (i < nx && k < nz) {
                            float x = i + 0.5f, y = float(j), z = k + 0.5f;
                            Vec3 vel = velAt(x, y, z) * a;
                            nv[Vi(i, j, k)] = interp(v, nx, ny + 1, nz, x - vel.x - 0.5f, y - vel.y, z - vel.z - 0.5f);
                        }
                        if (i < nx && j < ny) {
                            float x = i + 0.5f, y = j + 0.5f, z = float(k);
                            Vec3 vel = velAt(x, y, z) * a;
                            nw[W(i, j, k)] = interp(w, nx, ny, nz + 1, x - vel.x - 0.5f, y - vel.y - 0.5f, z - vel.z);
                        }
                        if (i < nx && j < ny && k < nz) {
                            size_t c = C(i, j, k);
                            // open sides absorb smoke; solids hold none
                            bool edge = i == 0 || k == 0 || i == nx - 1 || k == nz - 1 || j == ny - 1 || (j == 0 && !s.ground);
                            if (edge || solid[c]) { nd[c] = 0; nt[c] = 0; continue; }
                            float x = i + 0.5f, y = j + 0.5f, z = k + 0.5f;
                            Vec3 vel = velAt(x, y, z) * a;
                            float bx = x - vel.x - 0.5f, by = y - vel.y - 0.5f, bz = z - vel.z - 0.5f;
                            nd[c] = interp(d, nx, ny, nz, bx, by, bz);
                            nt[c] = interp(t, nx, ny, nz, bx, by, bz);
                        }
                    }
        });
        u.swap(nu); v.swap(nv); w.swap(nw); d.swap(nd); t.swap(nt);
        const float kd = std::exp(-s.dissipation * s.dt), kt = std::exp(-s.cooling * s.dt);
        for (auto& x : d) x *= kd;
        for (auto& x : t) x *= kt;
    }

    void step(const StepParms& s) {
        addSources(s);
        addForces(s);
        enforceSolids(s.ground);
        project(s);
        advect(s);
    }
};

struct PyroCache {
    std::shared_ptr<const std::vector<float>> d, t;
};

struct PyroState {
    uint64_t sig = 0;
    bool valid = false;
    PyroSim sim;
    int simFrame = 0;
    std::map<int, PyroCache> frames;
    bool srcBuilt = false, solidBuilt = false;
};

void cookPyro(CookContext& c) {
    PyroState& st = c.state<PyroState>();
    uint64_t sig = c.setupSignature();
    if (st.sig != sig) {
        st = PyroState();
        st.sig = sig;
    }
    Node& node = c.node();
    node.simCacheStart = 0; node.simCacheEnd = -1;
    const int frame = int(std::floor(c.frame() + 0.5f));
    const int start = c.i("startframe");
    if (frame < start) return;
    if (frame - start > 100000) { c.error("Frame is too far from the start frame."); return; }

    const float dx = std::max(c.f("voxelsize"), 1e-3f);
    const Vec3 size = c.v("size"), center = c.v("center");
    const double fx = std::ceil(double(size.x) / dx), fy = std::ceil(double(size.y) / dx), fz = std::ceil(double(size.z) / dx);
    if (fx < 4 || fy < 4 || fz < 4) { c.error("Domain is smaller than 4 voxels across. Reduce Voxel Size or enlarge Size."); return; }
    if (fx * fy * fz > 4.0e6) { c.error("Too many voxels (over 4 million). Increase Voxel Size or reduce Size."); return; }
    const int nx = int(fx), ny = int(fy), nz = int(fz);
    const Vec3 origin = center - Vec3(float(nx), float(ny), float(nz)) * (dx * 0.5f);
    const float fps = c.fps();

    if (!st.frames.count(frame)) {
        if (!st.valid || frame <= st.simFrame) {
            st.sim.init(nx, ny, nz, dx, origin);
            st.simFrame = start - 1;
            st.valid = true;
            st.srcBuilt = st.solidBuilt = false;
        }
        const bool srcAnimated = c.inputIsAnimated(0), solidAnimated = c.inputIsAnimated(1);
        Geometry tmp;
        for (int k = st.simFrame + 1; k <= frame; ++k) {
            c.setEvalFrame(float(k));
            const float radius = c.f("sourceradius");
            if (!st.srcBuilt || srcAnimated) {
                const Geometry* g = &c.input(0);
                if (srcAnimated && k != frame) { c.inputAtFrame(0, float(k), tmp); g = &tmp; }
                voxelizeGeometry(*g, nx, ny, nz, origin, dx, radius, st.sim.src);
                st.srcBuilt = true;
            }
            if (c.hasInput(1) && (!st.solidBuilt || solidAnimated)) {
                const Geometry* g = &c.input(1);
                if (solidAnimated && k != frame) { c.inputAtFrame(1, float(k), tmp); g = &tmp; }
                voxelizeGeometry(*g, nx, ny, nz, origin, dx, radius, st.sim.solid);
                st.solidBuilt = true;
            }
            StepParms s;
            const int substeps = std::clamp(c.i("substeps"), 1, 10);
            s.dt = 1.f / fps / float(substeps);
            s.buoyancy = c.f("buoyancy"); s.weight = c.f("weight"); s.wind = c.v("wind");
            s.dissipation = std::max(0.f, c.f("dissipation")); s.cooling = std::max(0.f, c.f("cooling"));
            s.vorticity = c.f("vorticity"); s.turb = c.f("turb"); s.turbFreq = c.f("turbfreq"); s.turbSpeed = c.f("turbspeed");
            s.ground = c.b("ground");
            s.iterations = std::clamp(c.i("iterations"), 1, 500);
            const int emitEnd = c.i("emitend");
            s.emit = emitEnd <= 0 || k <= emitEnd;
            s.srcDensity = c.f("sourcedensity"); s.srcTemp = c.f("sourcetemp"); s.srcVel = c.v("sourcevel");
            for (int ss = 0; ss < substeps; ++ss) {
                s.time = (float(k - 1) + float(ss) / float(substeps)) / fps;
                st.sim.step(s);
            }
            PyroCache pc;
            pc.d = std::make_shared<const std::vector<float>>(st.sim.d);
            pc.t = std::make_shared<const std::vector<float>>(st.sim.t);
            st.frames[k] = std::move(pc);
            st.simFrame = k;
        }
        c.clearEvalFrame();
        // bound the cache (~768 MB): drop the oldest frames first
        const size_t perFrame = size_t(nx) * ny * nz * sizeof(float) * 2;
        const size_t maxFrames = std::max<size_t>(2, (size_t(768) << 20) / perFrame);
        while (st.frames.size() > maxFrames && st.frames.begin()->first != frame) st.frames.erase(st.frames.begin());
    }

    const PyroCache& pc = st.frames[frame];
    Volume vol;
    vol.nx = nx; vol.ny = ny; vol.nz = nz; vol.dx = dx; vol.origin = origin;
    vol.density = pc.d;
    vol.heat = pc.t;
    vol.color = c.v("color");
    vol.densityScale = c.f("rdensity");
    vol.fire = c.f("fire");
    c.out.volumes.push_back(vol);
    int lo = frame, hi = frame;
    while (st.frames.count(lo - 1)) --lo;
    while (st.frames.count(hi + 1)) ++hi;
    node.simCacheStart = lo;
    node.simCacheEnd = hi;
}

} // namespace

void registerVolumeNodes(std::vector<NodeType>& types) {
    {
        NodeType t; t.name = "pyrosolver"; t.label = "Pyro Solver"; t.category = "Simulation";
        t.description = "Smoke and fire simulation. Input 1 is the source shape, input 2 an optional collider.";
        t.minInputs = 1; t.maxInputs = 2;
        t.inputLabels = {"Source (closed mesh, or points)", "Collider (closed mesh)"};
        t.timeDependent = true;
        t.parms = {
            I("startframe", "Start Frame", 1, 1, 240),
            I("substeps", "Substeps", 1, 1, 4),
            V("center", "Domain Center", Vec3(0, 2.5f, 0), -5, 5),
            V("size", "Domain Size", Vec3(4, 5, 4), 0, 10),
            F("voxelsize", "Voxel Size", 0.08f, 0.02f, 0.3f, "Smaller = more detail, slower"),
            F("sourcedensity", "Source Density", 1, 0, 2),
            F("sourcetemp", "Source Temperature", 1, 0, 3),
            V("sourcevel", "Source Velocity", Vec3(0, 0, 0), -5, 5),
            F("sourceradius", "Point Source Radius", 0.15f, 0, 1, "Used when the source or collider has no polygons"),
            I("emitend", "Stop Emitting After Frame", 0, 0, 240, "0 = never stop"),
            F("buoyancy", "Buoyancy", 4, 0, 15, "Lift from temperature"),
            F("weight", "Smoke Weight", 0.5f, 0, 5),
            V("wind", "Wind", Vec3(0, 0, 0), -5, 5),
            F("dissipation", "Dissipation", 0.3f, 0, 3, "How fast smoke fades (per second)"),
            F("cooling", "Cooling", 0.9f, 0, 5, "How fast temperature drops (per second)"),
            F("vorticity", "Swirl (Vorticity)", 3, 0, 10),
            F("turb", "Turbulence", 1, 0, 5),
            F("turbfreq", "Turbulence Frequency", 1.2f, 0.1f, 5),
            F("turbspeed", "Turbulence Speed", 0.5f, 0, 3),
            T("ground", "Closed Floor", true),
            I("iterations", "Pressure Iterations", 40, 5, 150),
            V("color", "Smoke Color", Vec3(0.75f, 0.75f, 0.78f), 0, 1),
            F("rdensity", "Smoke Opacity", 1, 0, 5),
            F("fire", "Fire Intensity", 0, 0, 5, "Glow from temperature"),
        };
        for (auto& p : t.parms) if (p.name == "color") p.help = "color";
        t.cook = cookPyro;
        types.push_back(t);
    }
    {
        NodeType t; t.name = "fogvolume"; t.label = "Fog Volume"; t.category = "Volumes";
        t.description = "Fills a closed mesh (or points) with fog, optionally broken up with noise.";
        t.minInputs = 1; t.maxInputs = 1;
        t.parms = {
            F("voxelsize", "Voxel Size", 0.05f, 0.01f, 0.3f),
            F("density", "Density", 1, 0, 3),
            F("noise", "Noise Amount", 0, 0, 1),
            F("noisefreq", "Noise Frequency", 2, 0.1f, 8),
            V("noiseoffset", "Noise Offset", Vec3(0, 0, 0), -5, 5),
            F("pointradius", "Point Radius", 0.15f, 0, 1),
            V("color", "Color", Vec3(0.8f, 0.8f, 0.82f), 0, 1),
            F("rdensity", "Opacity", 1, 0, 5),
        };
        for (auto& p : t.parms) if (p.name == "color") p.help = "color";
        t.cook = [](CookContext& c) {
            const Geometry& in = c.input(0);
            if (in.numPoints() == 0) return;
            const float dx = std::max(c.f("voxelsize"), 1e-3f), rad = c.f("pointradius");
            Vec3 mn, mx;
            { Geometry pointsOnly; pointsOnly.P = in.P; pointsOnly.bounds(mn, mx); }
            Vec3 pad(dx * 2 + (in.numPrims() ? 0.f : rad));
            mn = mn - pad; mx = mx + pad;
            const double fx = std::ceil(double(mx.x - mn.x) / dx), fy = std::ceil(double(mx.y - mn.y) / dx), fz = std::ceil(double(mx.z - mn.z) / dx);
            if (fx * fy * fz > 8.0e6) { c.error("Too many voxels (over 8 million). Increase Voxel Size."); return; }
            const int nx = std::max(2, int(fx)), ny = std::max(2, int(fy)), nz = std::max(2, int(fz));
            std::vector<unsigned char> mask;
            voxelizeGeometry(in, nx, ny, nz, mn, dx, rad, mask);
            auto dens = std::make_shared<std::vector<float>>(mask.size(), 0.f);
            const float density = c.f("density"), amt = std::clamp(c.f("noise"), 0.f, 1.f), freq = c.f("noisefreq");
            const Vec3 off = c.v("noiseoffset");
            parallelFor(nz, [&](int k0, int k1) {
                for (int k = k0; k < k1; ++k)
                    for (int j = 0; j < ny; ++j)
                        for (int i = 0; i < nx; ++i) {
                            size_t idx = size_t(i) + size_t(nx) * (size_t(j) + size_t(ny) * size_t(k));
                            if (!mask[idx]) continue;
                            float v = density;
                            if (amt > 0) {
                                Vec3 p = mn + Vec3(i + 0.5f, j + 0.5f, k + 0.5f) * dx;
                                float n = fbm3(p * freq + off, 4) * 0.5f + 0.5f;
                                v *= std::clamp(1.f - amt + amt * (n * 2.2f - 0.6f), 0.f, 2.f);
                            }
                            (*dens)[idx] = v;
                        }
            });
            Volume vol;
            vol.nx = nx; vol.ny = ny; vol.nz = nz; vol.dx = dx; vol.origin = mn;
            vol.density = dens;
            vol.color = c.v("color");
            vol.densityScale = c.f("rdensity");
            c.out.volumes.push_back(vol);
        };
        types.push_back(t);
    }
}

} // namespace hd
