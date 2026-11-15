#include "Renderer.h"
#include "GL.h"
#include <algorithm>
#include <cmath>
#include <set>

namespace hd {

// ---------------------------------------------------------------- Camera
Vec3 Camera::eye() const {
    float p = radians(pitch), y = radians(yaw);
    return target + Vec3(std::cos(p) * std::sin(y), std::sin(p), std::cos(p) * std::cos(y)) * dist;
}
Mat4 Camera::view() const { return Mat4::lookAt(eye(), target, Vec3(0, 1, 0)); }
Mat4 Camera::proj(float aspect) const {
    float n = std::max(nearClip, dist * 0.002f);
    float f = std::max(farClip, dist * 100.f);
    return Mat4::perspective(radians(fovY), std::max(aspect, 1e-3f), n, f);
}
void Camera::tumble(float dx, float dy) {
    yaw -= dx * 0.3f;
    pitch = std::clamp(pitch + dy * 0.3f, -89.5f, 89.5f);
}
void Camera::pan(float dx, float dy, float vh) {
    float p = radians(pitch), y = radians(yaw);
    Vec3 fwd = -Vec3(std::cos(p) * std::sin(y), std::sin(p), std::cos(p) * std::cos(y));
    Vec3 right = normalize(cross(fwd, Vec3(0, 1, 0)));
    Vec3 up = cross(right, fwd);
    float worldPerPixel = 2.f * dist * std::tan(radians(fovY) * 0.5f) / std::max(vh, 1.f);
    target += (right * -dx + up * dy) * worldPerPixel;
}
void Camera::dolly(float amount) { dist = std::clamp(dist * std::exp(-amount), 1e-3f, 1e5f); }
void Camera::frame(const Vec3& mn, const Vec3& mx) {
    target = (mn + mx) * 0.5f;
    float r = std::max(length(mx - mn) * 0.5f, 0.05f);
    dist = r / std::sin(radians(fovY) * 0.5f) * 1.05f;
}
void Camera::home() { *this = Camera(); }

// ---------------------------------------------------------------- shaders
namespace {
const char* kMeshVS = R"(#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNrm;
layout(location=2) in vec3 aCol;
uniform mat4 uView, uProj;
out vec3 vN; out vec3 vCol; out vec3 vPosV;
void main() {
    vec4 pv = uView * vec4(aPos, 1.0);
    vPosV = pv.xyz;
    vN = mat3(uView) * aNrm;
    vCol = aCol;
    gl_Position = uProj * pv;
})";

const char* kMeshFS = R"(#version 330 core
in vec3 vN; in vec3 vCol; in vec3 vPosV;
uniform int uBackTint;
out vec4 frag;
void main() {
    vec3 N = normalize(vN);
    vec3 V = normalize(-vPosV);
    vec3 col = vCol;
    if (!gl_FrontFacing) { N = -N; if (uBackTint == 1) col *= vec3(0.55, 0.6, 1.1); }
    if (dot(N, V) < 0.0) N = -N; // flat-shaded fallbacks: always light the visible side
    vec3 L1 = normalize(vec3(-0.35, 0.65, 0.68));
    vec3 L2 = normalize(vec3(0.6, -0.15, 0.45));
    float d1 = max(dot(N, L1), 0.0);
    float d2 = max(dot(N, L2), 0.0);
    float hemi = 0.5 + 0.5 * N.y;
    vec3 H = normalize(L1 + V);
    float spec = pow(max(dot(N, H), 0.0), 48.0) * 0.18;
    float rim = pow(1.0 - max(dot(N, V), 0.0), 3.0) * 0.12;
    vec3 lit = col * (0.16 + 0.12 * hemi + 0.68 * d1 + 0.18 * d2) + vec3(spec + rim);
    frag = vec4(pow(lit, vec3(1.0/1.1)), 1.0);
})";

const char* kLineVS = R"(#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aCol;
uniform mat4 uView, uProj;
uniform float uDepthBias;
out vec3 vCol;
void main() {
    vCol = aCol;
    vec4 p = uProj * (uView * vec4(aPos, 1.0));
    p.z -= uDepthBias * p.w;
    gl_Position = p;
})";

const char* kLineFS = R"(#version 330 core
in vec3 vCol;
uniform vec4 uOverride;
uniform float uAlpha;
out vec4 frag;
void main() { frag = vec4(mix(vCol, uOverride.rgb, uOverride.a), uAlpha); }
)";

const char* kPointVS = R"(#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aCol;
layout(location=2) in float aScale;
uniform mat4 uView, uProj;
uniform float uPixelSize;    // used when uUseScale == 0
uniform int uUseScale;
uniform float uViewportH;
out vec3 vCol;
void main() {
    vec4 pv = uView * vec4(aPos, 1.0);
    gl_Position = uProj * pv;
    vCol = aCol;
    float s = uPixelSize;
    if (uUseScale == 1) s = 2.0 * aScale * uProj[1][1] * uViewportH * 0.5 / max(-pv.z, 1e-4);
    gl_PointSize = clamp(s, 1.5, 96.0);
})";

const char* kPointFS = R"(#version 330 core
in vec3 vCol;
out vec4 frag;
void main() {
    vec2 c = gl_PointCoord * 2.0 - 1.0;
    float r2 = dot(c, c);
    if (r2 > 1.0) discard;
    float z = sqrt(1.0 - r2);
    vec3 N = vec3(c.x, -c.y, z);
    float d = max(dot(N, normalize(vec3(-0.35, 0.65, 0.68))), 0.0);
    frag = vec4(vCol * (0.3 + 0.75 * d), 1.0);
})";

const char* kBgVS = R"(#version 330 core
out float vY;
void main() {
    vec2 p = vec2((gl_VertexID == 1) ? 3.0 : -1.0, (gl_VertexID == 2) ? 3.0 : -1.0);
    vY = p.y * 0.5 + 0.5;
    gl_Position = vec4(p, 0.999, 1.0);
})";

const char* kBgFS = R"(#version 330 core
in float vY;
out vec4 frag;
uniform vec3 uTop, uBot;
void main() {
    frag = vec4(mix(uBot, uTop, smoothstep(0.0, 1.0, vY)), 1.0);
})";

const char* kVolVS = R"(#version 330 core
layout(location=0) in vec3 aPos;
uniform mat4 uView, uProj;
uniform vec3 uBoxMin, uBoxMax;
out vec3 vWorld;
void main() {
    vWorld = mix(uBoxMin, uBoxMax, aPos);
    gl_Position = uProj * (uView * vec4(vWorld, 1.0));
})";

// Raymarches a 3D texture: r = density, g = heat, b = precomputed light transmittance.
const char* kVolFS = R"(#version 330 core
in vec3 vWorld;
uniform sampler3D uVol;
uniform sampler2D uDepth;
uniform vec3 uEye, uFwd, uBoxMin, uBoxMax, uColor;
uniform vec2 uViewport;
uniform float uProjA, uProjB, uDensity, uFire, uStep;
out vec4 frag;
void main() {
    vec3 rd = normalize(vWorld - uEye);
    vec3 inv = 1.0 / rd;
    vec3 t0 = (uBoxMin - uEye) * inv, t1 = (uBoxMax - uEye) * inv;
    vec3 tn = min(t0, t1), tf = max(t0, t1);
    float tmin = max(max(tn.x, max(tn.y, tn.z)), 0.0);
    float tmax = min(tf.x, min(tf.y, tf.z));
    float depth = texture(uDepth, gl_FragCoord.xy / uViewport).r;
    if (depth < 1.0) {
        float zv = -uProjB / (depth * 2.0 - 1.0 + uProjA);   // view-space z (negative)
        tmax = min(tmax, -zv / max(dot(rd, uFwd), 1e-4));
    }
    if (tmax <= tmin) discard;
    float jitter = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453);
    float t = tmin + uStep * jitter;
    vec3 col = vec3(0.0);
    float T = 1.0;
    vec3 size = uBoxMax - uBoxMin;
    for (int i = 0; i < 220; ++i) {
        if (t >= tmax || T < 0.01) break;
        vec3 s = texture(uVol, (uEye + rd * t - uBoxMin) / size).rgb;
        if (s.r > 0.002) {
            float a = 1.0 - exp(-s.r * uDensity * uStep * 6.0);
            vec3 lit = uColor * (0.22 + 0.95 * s.b) + vec3(0.03, 0.04, 0.06);
            col += T * a * lit;
            T *= 1.0 - a;
        }
        if (uFire > 0.0 && s.g > 0.02) {
            float h = s.g;
            vec3 glow = vec3(h * 3.2, h * h * 1.7, h * h * h * 0.7);
            col += T * glow * (uFire * uStep * 5.0);
        }
        t += uStep;
    }
    frag = vec4(col, 1.0 - T);
})";

unsigned compileShader(unsigned type, const char* src, std::string& err) {
    unsigned s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048]; GLsizei n = 0;
        glGetShaderInfoLog(s, sizeof log, &n, log);
        err = std::string("shader compile failed: ") + std::string(log, size_t(n));
        glDeleteShader(s);
        return 0;
    }
    return s;
}

unsigned linkProgram(const char* vs, const char* fs, std::string& err) {
    unsigned v = compileShader(GL_VERTEX_SHADER, vs, err);
    if (!v) return 0;
    unsigned f = compileShader(GL_FRAGMENT_SHADER, fs, err);
    if (!f) { glDeleteShader(v); return 0; }
    unsigned p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048]; GLsizei n = 0;
        glGetProgramInfoLog(p, sizeof log, &n, log);
        err = std::string("program link failed: ") + std::string(log, size_t(n));
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

void setMat(unsigned prog, const char* name, const Mat4& m) {
    glUniformMatrix4fv(glGetUniformLocation(prog, name), 1, GL_FALSE, m.m);
}

void push3(std::vector<float>& v, const Vec3& a) { v.push_back(a.x); v.push_back(a.y); v.push_back(a.z); }
} // namespace

// ---------------------------------------------------------------- Renderer
Renderer::~Renderer() { shutdown(); }

bool Renderer::init(std::string& error) {
    if (inited_) return true;
    progMesh_ = linkProgram(kMeshVS, kMeshFS, error);
    progLine_ = progMesh_ ? linkProgram(kLineVS, kLineFS, error) : 0;
    progPoint_ = progLine_ ? linkProgram(kPointVS, kPointFS, error) : 0;
    progBg_ = progPoint_ ? linkProgram(kBgVS, kBgFS, error) : 0;
    progVol_ = progBg_ ? linkProgram(kVolVS, kVolFS, error) : 0;
    if (!progVol_) return false;
    {
        // unit cube, outward-facing triangles
        static const float q[6][4][3] = {
            {{0, 0, 0}, {0, 0, 1}, {0, 1, 1}, {0, 1, 0}}, {{1, 0, 0}, {1, 1, 0}, {1, 1, 1}, {1, 0, 1}},
            {{0, 0, 0}, {1, 0, 0}, {1, 0, 1}, {0, 0, 1}}, {{0, 1, 0}, {0, 1, 1}, {1, 1, 1}, {1, 1, 0}},
            {{0, 0, 0}, {0, 1, 0}, {1, 1, 0}, {1, 0, 0}}, {{0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}}};
        std::vector<float> cube;
        for (auto& f : q)
            for (int idx : {0, 1, 2, 0, 2, 3})
                for (int k = 0; k < 3; ++k) cube.push_back(f[idx][k]);
        const int layout[1] = {3};
        upload(cube_, cube, 3, layout, 1);
    }
    glGenVertexArrays(1, &bgVao_);
    GLint maxSamples = 0;
    glGetIntegerv(GL_MAX_SAMPLES, &maxSamples);
    samples_ = std::max(1, std::min(4, int(maxSamples)));

    inited_ = true;
    return true;
}

void Renderer::rebuildGrid(const ViewOptions& opt) {
    const float key[8] = {float(opt.gridPlane), opt.gridOrigin.x, opt.gridOrigin.y, opt.gridOrigin.z, opt.gridSpacing, float(opt.gridCells), float(opt.gridRuler), 1.f};
    bool same = true;
    for (int i = 0; i < 8; ++i) same &= key[i] == gridKey_[i];
    if (same) return;
    for (int i = 0; i < 8; ++i) gridKey_[i] = key[i];
    // axes spanning the plane; axis lines take the usual X = red, Y = green, Z = blue
    const int u = opt.gridPlane == 2 ? 1 : 0, v = opt.gridPlane == 1 ? 1 : 2;
    const Vec3 axisCol[3] = {Vec3(0.85f, 0.25f, 0.25f), Vec3(0.35f, 0.8f, 0.3f), Vec3(0.3f, 0.45f, 0.95f)};
    const int half = std::clamp(opt.gridCells, 2, 400) / 2;
    const float sp = std::max(opt.gridSpacing, 1e-4f), ext = float(half) * sp;
    const int ruler = std::max(1, opt.gridRuler);
    std::vector<float> g;
    for (int i = -half; i <= half; ++i) {
        Vec3 c = (i % ruler == 0) ? Vec3(0.55f) : Vec3(0.38f);
        float pos = float(i) * sp;
        Vec3 a(0), b(0), c0(0), d(0);
        a[u] = -ext; a[v] = pos; b[u] = ext; b[v] = pos;   // parallel to u
        c0[v] = -ext; c0[u] = pos; d[v] = ext; d[u] = pos; // parallel to v
        Vec3 cu = i == 0 ? axisCol[u] : c, cv = i == 0 ? axisCol[v] : c;
        push3(g, a + opt.gridOrigin); push3(g, cu);
        push3(g, b + opt.gridOrigin); push3(g, cu);
        push3(g, c0 + opt.gridOrigin); push3(g, cv);
        push3(g, d + opt.gridOrigin); push3(g, cv);
    }
    const int lineLayout[2] = {3, 3};
    upload(grid_, g, 6, lineLayout, 2);
}

void Renderer::shutdown() {
    if (!inited_) return;
    for (auto& v : volumes_) if (v.tex) glDeleteTextures(1, &v.tex);
    volumes_.clear();
    if (depthTex_) glDeleteTextures(1, &depthTex_);
    if (volFbo_) glDeleteFramebuffers(1, &volFbo_);
    depthTex_ = volFbo_ = 0;
    for (Batch* b : {&tris_, &wire_, &curves_, &points_, &loose_, &normals_, &grid_, &cube_}) {
        if (b->vbo) glDeleteBuffers(1, &b->vbo);
        if (b->vao) glDeleteVertexArrays(1, &b->vao);
        *b = Batch();
    }
    for (unsigned p : {progMesh_, progLine_, progPoint_, progBg_, progVol_}) if (p) glDeleteProgram(p);
    if (bgVao_) glDeleteVertexArrays(1, &bgVao_);
    if (msFbo_) glDeleteFramebuffers(1, &msFbo_);
    if (resolveFbo_) glDeleteFramebuffers(1, &resolveFbo_);
    if (msColor_) glDeleteRenderbuffers(1, &msColor_);
    if (msDepth_) glDeleteRenderbuffers(1, &msDepth_);
    if (resolveTex_) glDeleteTextures(1, &resolveTex_);
    msFbo_ = resolveFbo_ = msColor_ = msDepth_ = resolveTex_ = 0;
    fbW_ = fbH_ = 0;
    inited_ = false;
}

void Renderer::upload(Batch& b, const std::vector<float>& data, int fpv, const int* layout, int nAttribs) {
    if (!b.vao) glGenVertexArrays(1, &b.vao);
    if (!b.vbo) glGenBuffers(1, &b.vbo);
    glBindVertexArray(b.vao);
    glBindBuffer(GL_ARRAY_BUFFER, b.vbo);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(data.size() * sizeof(float)), data.empty() ? nullptr : data.data(), GL_STATIC_DRAW);
    size_t off = 0;
    for (int i = 0; i < nAttribs; ++i) {
        glEnableVertexAttribArray(GLuint(i));
        glVertexAttribPointer(GLuint(i), layout[i], GL_FLOAT, GL_FALSE, GLsizei(fpv * sizeof(float)), reinterpret_cast<const void*>(off * sizeof(float)));
        off += size_t(layout[i]);
    }
    glBindVertexArray(0);
    b.count = int(data.size() / size_t(fpv));
}

void Renderer::clearGeometry() {
    Geometry empty;
    setGeometry(empty, 0);
}

void Renderer::setGeometry(const Geometry& g, uint64_t stamp) {
    if (stamp != 0 && stamp == stamp_) return;
    stamp_ = stamp;
    const Attrib* pN = g.findAttrib(AttribClass::Point, "N");
    const Attrib* qN = g.findAttrib(AttribClass::Prim, "N");
    const Attrib* pCd = g.findAttrib(AttribClass::Point, "Cd");
    const Attrib* qCd = g.findAttrib(AttribClass::Prim, "Cd");
    const Attrib* pscale = g.findAttrib(AttribClass::Point, "pscale");
    if (pN && pN->tuple != 3) pN = nullptr;
    if (qN && qN->tuple != 3) qN = nullptr;
    const Vec3 defCol(0.72f, 0.74f, 0.78f);

    std::vector<float> tri, wire, curve, pts, nrm;
    tri.reserve(g.numVertices() * 27);
    std::set<std::pair<int, int>> edges;
    for (size_t pi = 0; pi < g.numPrims(); ++pi) {
        const Prim& pr = g.prims[pi];
        if (pr.count < 2) continue;
        auto colorOf = [&](int pt) {
            if (pCd) return pCd->getV(size_t(pt));
            if (qCd) return qCd->getV(pi);
            return defCol;
        };
        if (pr.closed && pr.count >= 3) {
            Vec3 fn = qN ? qN->getV(pi) : g.primNormal(int(pi));
            for (int k = 1; k + 1 < pr.count; ++k) {
                int ids[3] = {g.vertices[size_t(pr.start)], g.vertices[size_t(pr.start + k)], g.vertices[size_t(pr.start + k + 1)]};
                for (int id : ids) {
                    push3(tri, g.P[size_t(id)]);
                    Vec3 n = pN ? pN->getV(size_t(id)) : fn;
                    if (dot(n, n) < 1e-12f) n = fn; // missing (zero) normals after merges
                    push3(tri, n);
                    push3(tri, colorOf(id));
                }
            }
            for (int k = 0; k < pr.count; ++k) {
                int a = g.vertices[size_t(pr.start + k)], b = g.vertices[size_t(pr.start + (k + 1) % pr.count)];
                if (edges.insert({std::min(a, b), std::max(a, b)}).second) {
                    push3(wire, g.P[size_t(a)]); push3(wire, Vec3(0.05f, 0.055f, 0.06f));
                    push3(wire, g.P[size_t(b)]); push3(wire, Vec3(0.05f, 0.055f, 0.06f));
                }
            }
        } else {
            int ne = pr.closed ? pr.count : pr.count - 1;
            for (int k = 0; k < ne; ++k) {
                int a = g.vertices[size_t(pr.start + k)], b = g.vertices[size_t(pr.start + (k + 1) % pr.count)];
                Vec3 ca = (pCd || qCd) ? colorOf(a) : Vec3(0.9f, 0.9f, 0.6f);
                Vec3 cb = (pCd || qCd) ? colorOf(b) : Vec3(0.9f, 0.9f, 0.6f);
                push3(curve, g.P[size_t(a)]); push3(curve, ca);
                push3(curve, g.P[size_t(b)]); push3(curve, cb);
            }
        }
    }
    Vec3 mn, mx;
    g.bounds(mn, mx);
    boundsMin_ = mn; boundsMax_ = mx;
    float diag = std::max(length(mx - mn), 1e-3f);
    pts.reserve(g.numPoints() * 7);
    // points not used by any primitive (particles, scattered points) are always drawn
    std::vector<char> used(g.numPoints(), 0);
    for (int vtx : g.vertices) used[size_t(vtx)] = 1;
    std::vector<float> loose;
    for (size_t i = 0; i < g.numPoints(); ++i) {
        Vec3 pc = pCd ? pCd->getV(i) : Vec3(0.95f, 0.85f, 0.35f);
        float psz = pscale ? pscale->getF(i) : 0.02f;
        push3(pts, g.P[i]);
        push3(pts, pc);
        pts.push_back(psz);
        if (!used[i]) { push3(loose, g.P[i]); push3(loose, pc); loose.push_back(psz); }
        if (pN) {
            push3(nrm, g.P[i]); push3(nrm, Vec3(0.2f, 0.9f, 1.f));
            push3(nrm, g.P[i] + pN->getV(i) * (diag * 0.04f)); push3(nrm, Vec3(0.2f, 0.9f, 1.f));
        }
    }
    hasPscale_ = pscale != nullptr;
    pointCloud_ = g.numPrims() == 0 && g.numPoints() > 0;

    const int meshLayout[3] = {3, 3, 3};
    const int lineLayout[2] = {3, 3};
    const int pointLayout[3] = {3, 3, 1};
    upload(tris_, tri, 9, meshLayout, 3);
    upload(wire_, wire, 6, lineLayout, 2);
    upload(curves_, curve, 6, lineLayout, 2);
    upload(points_, pts, 7, pointLayout, 3);
    upload(loose_, loose, 7, pointLayout, 3);
    upload(normals_, nrm, 6, lineLayout, 2);
    // ---- volumes -> 3D textures (density, heat, light)
    for (auto& v : volumes_) if (v.tex) glDeleteTextures(1, &v.tex);
    volumes_.clear();
    for (const Volume& vol : g.volumes) {
        if (volumes_.size() >= 8 || !vol.density || vol.density->size() != vol.voxels() || vol.voxels() == 0) continue;
        const int nx = vol.nx, ny = vol.ny, nz = vol.nz;
        const std::vector<float>& d = *vol.density;
        const std::vector<float>* heat = vol.heat && vol.heat->size() == d.size() ? vol.heat.get() : nullptr;
        std::vector<float> tex(d.size() * 3);
        // light transmittance toward a fixed key light, marched on the CPU
        const Vec3 L = normalize(Vec3(0.4f, 0.8f, 0.45f)) * 1.5f;
        const float k = 1.5f * vol.dx * 6.f * vol.densityScale * 0.7f;
        for (int z = 0; z < nz; ++z)
            for (int y = 0; y < ny; ++y)
                for (int x = 0; x < nx; ++x) {
                    size_t i = size_t(x) + size_t(nx) * (size_t(y) + size_t(ny) * size_t(z));
                    float light = 1.f;
                    if (d[i] > 0.002f) {
                        float sum = 0;
                        for (int s = 1; s <= 10; ++s) {
                            int sx = int(float(x) + L.x * float(s) + 0.5f), sy = int(float(y) + L.y * float(s) + 0.5f), sz = int(float(z) + L.z * float(s) + 0.5f);
                            if (sx < 0 || sy < 0 || sz < 0 || sx >= nx || sy >= ny || sz >= nz) break;
                            sum += d[size_t(sx) + size_t(nx) * (size_t(sy) + size_t(ny) * size_t(sz))];
                        }
                        light = std::exp(-sum * k);
                    }
                    tex[i * 3] = d[i];
                    tex[i * 3 + 1] = heat ? (*heat)[i] : 0.f;
                    tex[i * 3 + 2] = light;
                }
        GpuVolume gv;
        gv.mn = vol.bmin(); gv.mx = vol.bmax(); gv.color = vol.color;
        gv.density = vol.densityScale; gv.fire = vol.fire;
        gv.step = std::max(vol.dx * 0.7f, length(gv.mx - gv.mn) / 200.f);
        glGenTextures(1, &gv.tex);
        glBindTexture(GL_TEXTURE_3D, gv.tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexImage3D(GL_TEXTURE_3D, 0, GL_RGB16F, nx, ny, nz, 0, GL_RGB, GL_FLOAT, tex.data());
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_3D, 0);
        volumes_.push_back(gv);
    }
    stats_.volumes = volumes_.size();
    stats_.triangles = size_t(tris_.count / 3);
    stats_.lines = size_t(wire_.count / 2 + curves_.count / 2);
    stats_.points = size_t(points_.count);
}

void Renderer::ensureTargets(int w, int h) {
    if (w == fbW_ && h == fbH_ && msFbo_) return;
    if (!msFbo_) {
        glGenFramebuffers(1, &msFbo_);
        glGenFramebuffers(1, &resolveFbo_);
        glGenRenderbuffers(1, &msColor_);
        glGenRenderbuffers(1, &msDepth_);
        glGenTextures(1, &resolveTex_);
        glGenTextures(1, &depthTex_);
        glGenFramebuffers(1, &volFbo_);
    }
    fbW_ = w; fbH_ = h;
    glBindRenderbuffer(GL_RENDERBUFFER, msColor_);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples_, GL_RGBA8, w, h);
    glBindRenderbuffer(GL_RENDERBUFFER, msDepth_);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples_, GL_DEPTH24_STENCIL8, w, h);
    glBindFramebuffer(GL_FRAMEBUFFER, msFbo_);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, msColor_);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, msDepth_);

    glBindTexture(GL_TEXTURE_2D, resolveTex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    // resolved scene depth, sampled by the volume pass
    glBindTexture(GL_TEXTURE_2D, depthTex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, w, h, 0, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, resolveFbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, resolveTex_, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, depthTex_, 0);
    // colour-only target sharing the resolved image, so the depth texture can be read while drawing
    glBindFramebuffer(GL_FRAMEBUFFER, volFbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, resolveTex_, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void Renderer::drawBatch(const Batch& b, unsigned mode) {
    if (!b.count) return;
    glBindVertexArray(b.vao);
    glDrawArrays(mode, 0, b.count);
}

unsigned Renderer::render(int w, int h, const Camera& cam, const ViewOptions& opt) {
    if (!inited_ || w <= 0 || h <= 0) return resolveTex_;
    GLint prevFbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    GLint prevViewport[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_VIEWPORT, prevViewport);

    ensureTargets(w, h);
    glBindFramebuffer(GL_FRAMEBUFFER, msFbo_);
    glViewport(0, 0, w, h);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glEnable(GL_MULTISAMPLE);
    glClearColor(0.15f, 0.15f, 0.17f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // background gradient
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glUseProgram(progBg_);
    glUniform3f(glGetUniformLocation(progBg_, "uTop"), opt.bgTop.x, opt.bgTop.y, opt.bgTop.z);
    glUniform3f(glGetUniformLocation(progBg_, "uBot"), opt.bgBottom.x, opt.bgBottom.y, opt.bgBottom.z);
    glBindVertexArray(bgVao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);

    Mat4 V = cam.view(), P = cam.proj(float(w) / float(h));

    // grid (alpha blended, no depth write)
    if (opt.grid) {
        rebuildGrid(opt);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_FALSE);
        glUseProgram(progLine_);
        setMat(progLine_, "uView", V);
        setMat(progLine_, "uProj", P);
        glUniform1f(glGetUniformLocation(progLine_, "uDepthBias"), 0.f);
        glUniform4f(glGetUniformLocation(progLine_, "uOverride"), 0, 0, 0, 0);
        glUniform1f(glGetUniformLocation(progLine_, "uAlpha"), std::clamp(opt.gridOpacity, 0.f, 1.f));
        drawBatch(grid_, GL_LINES);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
    }

    // shaded surfaces
    if (opt.shaded) {
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(1.f, 1.f);
        glUseProgram(progMesh_);
        setMat(progMesh_, "uView", V);
        setMat(progMesh_, "uProj", P);
        glUniform1i(glGetUniformLocation(progMesh_, "uBackTint"), opt.backfaceTint ? 1 : 0);
        drawBatch(tris_, GL_TRIANGLES);
        glDisable(GL_POLYGON_OFFSET_FILL);
    }

    // lines: wireframe, curves, normals
    glUseProgram(progLine_);
    setMat(progLine_, "uView", V);
    setMat(progLine_, "uProj", P);
    glUniform1f(glGetUniformLocation(progLine_, "uAlpha"), 1.f);
    glUniform1f(glGetUniformLocation(progLine_, "uDepthBias"), 0.00002f);
    GLint ovr = glGetUniformLocation(progLine_, "uOverride");
    if (opt.wireframe || !opt.shaded) {
        if (opt.shaded) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glUniform1f(glGetUniformLocation(progLine_, "uAlpha"), 0.55f);
            glUniform4f(ovr, 0, 0, 0, 0);
        } else {
            glUniform4f(ovr, 0.82f, 0.86f, 0.9f, 1.f);
        }
        drawBatch(wire_, GL_LINES);
        glDisable(GL_BLEND);
        glUniform1f(glGetUniformLocation(progLine_, "uAlpha"), 1.f);
    }
    glUniform4f(ovr, 0, 0, 0, 0);
    drawBatch(curves_, GL_LINES);
    if (opt.normals) drawBatch(normals_, GL_LINES);

    // points: loose points always (world-sized when pscale exists), all points on request
    if (opt.points || loose_.count) {
        glEnable(GL_PROGRAM_POINT_SIZE);
        glUseProgram(progPoint_);
        setMat(progPoint_, "uView", V);
        setMat(progPoint_, "uProj", P);
        glUniform1f(glGetUniformLocation(progPoint_, "uPixelSize"), opt.pointSize);
        glUniform1f(glGetUniformLocation(progPoint_, "uViewportH"), float(h));
        glUniform1i(glGetUniformLocation(progPoint_, "uUseScale"), hasPscale_ ? 1 : 0);
        drawBatch(loose_, GL_POINTS);
        if (opt.points) {
            glUniform1i(glGetUniformLocation(progPoint_, "uUseScale"), 0);
            drawBatch(points_, GL_POINTS);
        }
        glDisable(GL_PROGRAM_POINT_SIZE);
    }
    glBindVertexArray(0);
    glUseProgram(0);

    // resolve MSAA
    glBindFramebuffer(GL_READ_FRAMEBUFFER, msFbo_);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, resolveFbo_);
    glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    if (!volumes_.empty()) drawVolumes(w, h, cam);
    glBindFramebuffer(GL_FRAMEBUFFER, GLuint(prevFbo));
    glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
    glDisable(GL_DEPTH_TEST);
    return resolveTex_;
}

void Renderer::drawVolumes(int w, int h, const Camera& cam) {
    glBindFramebuffer(GL_FRAMEBUFFER, volFbo_);
    glViewport(0, 0, w, h);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA); // premultiplied
    glEnable(GL_CULL_FACE);
    glCullFace(GL_FRONT); // draw back faces so it also works with the camera inside the volume
    Mat4 V = cam.view(), P = cam.proj(float(w) / float(h));
    Vec3 eye = cam.eye(), fwd = normalize(cam.target - eye);
    glUseProgram(progVol_);
    setMat(progVol_, "uView", V);
    setMat(progVol_, "uProj", P);
    glUniform3f(glGetUniformLocation(progVol_, "uEye"), eye.x, eye.y, eye.z);
    glUniform3f(glGetUniformLocation(progVol_, "uFwd"), fwd.x, fwd.y, fwd.z);
    glUniform1f(glGetUniformLocation(progVol_, "uProjA"), P.at(2, 2));
    glUniform1f(glGetUniformLocation(progVol_, "uProjB"), P.at(2, 3));
    glUniform1i(glGetUniformLocation(progVol_, "uVol"), 0);
    glUniform1i(glGetUniformLocation(progVol_, "uDepth"), 1);
    glUniform2f(glGetUniformLocation(progVol_, "uViewport"), float(w), float(h));
    glActiveTexture(GL_TEXTURE0 + 1);
    glBindTexture(GL_TEXTURE_2D, depthTex_);
    glActiveTexture(GL_TEXTURE0);
    // far-to-near so overlapping volumes blend in order
    std::vector<const GpuVolume*> order;
    for (auto& v : volumes_) order.push_back(&v);
    std::sort(order.begin(), order.end(), [&](const GpuVolume* a, const GpuVolume* b) {
        return length((a->mn + a->mx) * 0.5f - eye) > length((b->mn + b->mx) * 0.5f - eye);
    });
    for (const GpuVolume* v : order) {
        glBindTexture(GL_TEXTURE_3D, v->tex);
        glUniform3f(glGetUniformLocation(progVol_, "uBoxMin"), v->mn.x, v->mn.y, v->mn.z);
        glUniform3f(glGetUniformLocation(progVol_, "uBoxMax"), v->mx.x, v->mx.y, v->mx.z);
        glUniform3f(glGetUniformLocation(progVol_, "uColor"), v->color.x, v->color.y, v->color.z);
        glUniform1f(glGetUniformLocation(progVol_, "uDensity"), v->density);
        glUniform1f(glGetUniformLocation(progVol_, "uFire"), v->fire);
        glUniform1f(glGetUniformLocation(progVol_, "uStep"), v->step);
        drawBatch(cube_, GL_TRIANGLES);
    }
    glBindTexture(GL_TEXTURE_3D, 0);
    glActiveTexture(GL_TEXTURE0 + 1);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(0);
    glUseProgram(0);
    glCullFace(GL_BACK);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
}

void Renderer::readPixels(std::vector<uint8_t>& rgba, int& w, int& h) {
    w = fbW_; h = fbH_;
    rgba.assign(size_t(w) * size_t(h) * 4, 0);
    if (!resolveFbo_) return;
    GLint prev = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev);
    glBindFramebuffer(GL_FRAMEBUFFER, resolveFbo_);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    glBindFramebuffer(GL_FRAMEBUFFER, GLuint(prev));
}

} // namespace hd
