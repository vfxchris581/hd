// Built-in SOP node types.
#include "GeoIO.h"
#include "Node.h"
#include "Noise.h"
#include "Ops.h"
#include <cctype>
#include <map>
#include <unordered_map>

namespace hd {
void registerSimNodes(std::vector<NodeType>& types); // Sim.cpp
namespace {

// ---- parameter builders
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
ParmDef S(const char* n, const char* l, const char* d = "", const char* help = "") {
    ParmDef p; p.name = n; p.label = l; p.type = ParmType::String; p.defStr = d; p.help = help; return p;
}
ParmDef M(const char* n, const char* l, std::vector<std::string> items, int d = 0) {
    ParmDef p; p.name = n; p.label = l; p.type = ParmType::Menu; p.menu = std::move(items); p.def[0] = float(d); return p;
}
ParmDef C(const char* n, const char* l, const char* d) {
    ParmDef p; p.name = n; p.label = l; p.type = ParmType::Code; p.defStr = d; return p;
}
ParmDef File(const char* n, const char* l, const char* d = "") {
    ParmDef p; p.name = n; p.label = l; p.type = ParmType::File; p.defStr = d; return p;
}

int clampi(int v, int lo, int hi) { return std::max(lo, std::min(hi, v)); }

// Orientation for planar generators: rotate ZX-plane geometry (normal +Y) to the chosen plane.
Mat4 planeOrient(int orient) {
    if (orient == 1) return Mat4::rotateX(kPi * 0.5f);   // XY plane (normal +Z)
    if (orient == 2) return Mat4::rotateZ(-kPi * 0.5f);  // YZ plane (normal +X)
    return Mat4();                                        // ZX plane (normal +Y)
}

void copyAttribLayout(const Geometry& in, Geometry& out) {
    for (auto& [n, a] : in.pointAttribs) { Attrib na; na.tuple = a.tuple; std::copy(a.defaults, a.defaults + 3, na.defaults); out.pointAttribs[n] = na; }
    for (auto& [n, a] : in.primAttribs) { Attrib na; na.tuple = a.tuple; std::copy(a.defaults, a.defaults + 3, na.defaults); out.primAttribs[n] = na; }
}

std::vector<NodeType> buildTypes() {
    std::vector<NodeType> types;
    auto add = [&](NodeType t) { types.push_back(std::move(t)); };

    // ================================================================ GENERATORS
    {
        NodeType t; t.name = "box"; t.label = "Box"; t.category = "Generators";
        t.description = "Creates a box with optional divisions.";
        t.parms = {V("size", "Size", Vec3(1), 0, 5), V("t", "Center", Vec3(0)), I("divs", "Divisions", 1, 1, 20), F("scale", "Uniform Scale", 1, 0, 5)};
        t.cook = [](CookContext& c) {
            Geometry& g = c.out;
            Vec3 size = c.v("size") * c.f("scale"), ctr = c.v("t");
            int d = clampi(c.i("divs"), 1, 200);
            int n1 = d + 1;
            std::unordered_map<int, int> lattice;
            auto pt = [&](int i, int j, int k) {
                int key = (i * n1 + j) * n1 + k;
                auto it = lattice.find(key);
                if (it != lattice.end()) return it->second;
                Vec3 p(float(i) / d - 0.5f, float(j) / d - 0.5f, float(k) / d - 0.5f);
                int id = g.addPoint(ctr + p * size);
                lattice[key] = id;
                return id;
            };
            // axis (0..2), sign, u axis, v axis with cross(u,v) == n
            struct Face { int axis, sign, u, v; };
            const Face faces[6] = {{0, 1, 1, 2}, {0, 0, 2, 1}, {1, 1, 2, 0}, {1, 0, 0, 2}, {2, 1, 0, 1}, {2, 0, 1, 0}};
            for (const Face& f : faces) {
                for (int a = 0; a < d; ++a)
                    for (int b = 0; b < d; ++b) {
                        int idx[4][3];
                        int uv[4][2] = {{a, b}, {a + 1, b}, {a + 1, b + 1}, {a, b + 1}};
                        std::vector<int> poly;
                        for (int q = 0; q < 4; ++q) {
                            idx[q][f.axis] = f.sign ? d : 0;
                            idx[q][f.u] = uv[q][0];
                            idx[q][f.v] = uv[q][1];
                            poly.push_back(pt(idx[q][0], idx[q][1], idx[q][2]));
                        }
                        g.addPrim(poly);
                    }
            }
        };
        add(t);
    }
    {
        NodeType t; t.name = "sphere"; t.label = "Sphere"; t.category = "Generators";
        t.description = "Creates a polygon sphere (or ellipsoid).";
        t.parms = {V("rad", "Radius", Vec3(1), 0, 5), V("t", "Center", Vec3(0)), I("rows", "Rows", 13, 3, 100), I("cols", "Columns", 24, 3, 200),
                   F("scale", "Uniform Scale", 1, 0, 5)};
        t.cook = [](CookContext& c) {
            Geometry& g = c.out;
            Vec3 rad = c.v("rad") * c.f("scale"), ctr = c.v("t");
            int rows = clampi(c.i("rows"), 3, 2000), cols = clampi(c.i("cols"), 3, 2000);
            Attrib* N = &g.addAttrib(AttribClass::Point, "N", 3);
            auto addP = [&](const Vec3& unit) {
                int id = g.addPoint(ctr + unit * rad);
                N = g.findAttrib(AttribClass::Point, "N");
                Vec3 n = normalize(Vec3(unit.x / std::max(rad.x, 1e-6f), unit.y / std::max(rad.y, 1e-6f), unit.z / std::max(rad.z, 1e-6f)));
                N->setV(size_t(id), n);
                return id;
            };
            int top = addP(Vec3(0, 1, 0));
            std::vector<std::vector<int>> rings;
            for (int i = 1; i < rows - 1; ++i) {
                float th = kPi * float(i) / float(rows - 1);
                std::vector<int> ring;
                for (int j = 0; j < cols; ++j) {
                    float ph = 2 * kPi * float(j) / float(cols);
                    ring.push_back(addP(Vec3(std::sin(th) * std::cos(ph), std::cos(th), std::sin(th) * std::sin(ph))));
                }
                rings.push_back(ring);
            }
            int bot = addP(Vec3(0, -1, 0));
            for (int j = 0; j < cols; ++j) {
                int jn = (j + 1) % cols;
                g.addPrim({top, rings.front()[size_t(jn)], rings.front()[size_t(j)]});
            }
            for (size_t i = 0; i + 1 < rings.size(); ++i)
                for (int j = 0; j < cols; ++j) {
                    int jn = (j + 1) % cols;
                    g.addPrim({rings[i][size_t(j)], rings[i][size_t(jn)], rings[i + 1][size_t(jn)], rings[i + 1][size_t(j)]});
                }
            for (int j = 0; j < cols; ++j) {
                int jn = (j + 1) % cols;
                g.addPrim({rings.back()[size_t(j)], rings.back()[size_t(jn)], bot});
            }
        };
        add(t);
    }
    {
        NodeType t; t.name = "grid"; t.label = "Grid"; t.category = "Generators";
        t.description = "Creates a planar grid of quads.";
        t.parms = {M("orient", "Orientation", {"ZX Plane", "XY Plane", "YZ Plane"}), F("sizex", "Size X", 10, 0, 50), F("sizey", "Size Y", 10, 0, 50),
                   V("t", "Center", Vec3(0)), I("rows", "Rows", 10, 2, 200), I("cols", "Columns", 10, 2, 200)};
        t.cook = [](CookContext& c) {
            Geometry& g = c.out;
            int rows = clampi(c.i("rows"), 2, 4000), cols = clampi(c.i("cols"), 2, 4000);
            float sx = c.f("sizex"), sz = c.f("sizey");
            for (int r = 0; r < rows; ++r)
                for (int col = 0; col < cols; ++col)
                    g.addPoint(Vec3((float(col) / (cols - 1) - 0.5f) * sx, 0, (float(r) / (rows - 1) - 0.5f) * sz));
            for (int r = 0; r + 1 < rows; ++r)
                for (int col = 0; col + 1 < cols; ++col) {
                    int a = r * cols + col;
                    g.addPrim({a, a + cols, a + cols + 1, a + 1});
                }
            Attrib& N = g.addAttrib(AttribClass::Point, "N", 3);
            for (size_t i = 0; i < g.numPoints(); ++i) N.setV(i, Vec3(0, 1, 0));
            g.transform(Mat4::translate(c.v("t")) * planeOrient(c.i("orient")));
        };
        add(t);
    }
    {
        NodeType t; t.name = "tube"; t.label = "Tube"; t.category = "Generators";
        t.description = "Creates an open or capped tube / cone.";
        t.parms = {F("rad1", "Radius Top", 1, 0, 5), F("rad2", "Radius Bottom", 1, 0, 5), F("height", "Height", 2, 0, 10), V("t", "Center", Vec3(0)),
                   I("rows", "Rows", 2, 2, 100), I("cols", "Columns", 12, 3, 200), T("cap", "End Caps", true)};
        t.cook = [](CookContext& c) {
            Geometry& g = c.out;
            int rows = clampi(c.i("rows"), 2, 2000), cols = clampi(c.i("cols"), 3, 2000);
            float r1 = c.f("rad1"), r2 = c.f("rad2"), h = c.f("height");
            Vec3 ctr = c.v("t");
            std::vector<std::vector<int>> rings;
            for (int i = 0; i < rows; ++i) {
                float s = float(i) / float(rows - 1); // 0 = top
                float r = r1 + (r2 - r1) * s;
                float y = h * 0.5f - h * s;
                std::vector<int> ring;
                for (int j = 0; j < cols; ++j) {
                    float ph = 2 * kPi * float(j) / float(cols);
                    ring.push_back(g.addPoint(ctr + Vec3(r * std::cos(ph), y, r * std::sin(ph))));
                }
                rings.push_back(ring);
            }
            for (int i = 0; i + 1 < rows; ++i)
                for (int j = 0; j < cols; ++j) {
                    int jn = (j + 1) % cols;
                    g.addPrim({rings[size_t(i)][size_t(j)], rings[size_t(i)][size_t(jn)], rings[size_t(i + 1)][size_t(jn)], rings[size_t(i + 1)][size_t(j)]});
                }
            if (c.b("cap")) {
                std::vector<int> topCap(rings.front().rbegin(), rings.front().rend());
                g.addPrim(topCap);
                g.addPrim(rings.back());
            }
        };
        add(t);
    }
    {
        NodeType t; t.name = "torus"; t.label = "Torus"; t.category = "Generators";
        t.description = "Creates a torus.";
        t.parms = {F("rad1", "Major Radius", 1, 0, 5), F("rad2", "Minor Radius", 0.4f, 0, 2), V("t", "Center", Vec3(0)),
                   I("rows", "Rows", 24, 3, 200), I("cols", "Columns", 12, 3, 200)};
        t.cook = [](CookContext& c) {
            Geometry& g = c.out;
            int rows = clampi(c.i("rows"), 3, 2000), cols = clampi(c.i("cols"), 3, 2000);
            float R = c.f("rad1"), r = c.f("rad2");
            Vec3 ctr = c.v("t");
            Attrib* N = &g.addAttrib(AttribClass::Point, "N", 3);
            for (int i = 0; i < rows; ++i) {
                float u = 2 * kPi * float(i) / float(rows);
                for (int j = 0; j < cols; ++j) {
                    float v = 2 * kPi * float(j) / float(cols);
                    Vec3 ringC(R * std::cos(u), 0, R * std::sin(u));
                    Vec3 p((R + r * std::cos(v)) * std::cos(u), r * std::sin(v), (R + r * std::cos(v)) * std::sin(u));
                    int id = g.addPoint(ctr + p);
                    N = g.findAttrib(AttribClass::Point, "N");
                    N->setV(size_t(id), normalize(p - ringC));
                }
            }
            for (int i = 0; i < rows; ++i)
                for (int j = 0; j < cols; ++j) {
                    int in = (i + 1) % rows, jn = (j + 1) % cols;
                    g.addPrim({i * cols + j, i * cols + jn, in * cols + jn, in * cols + j});
                }
        };
        add(t);
    }
    {
        NodeType t; t.name = "circle"; t.label = "Circle"; t.category = "Generators";
        t.description = "Creates a circle polygon or open arc.";
        t.parms = {M("type", "Type", {"Polygon", "Open Curve", "Points Only"}), M("orient", "Orientation", {"ZX Plane", "XY Plane", "YZ Plane"}),
                   F("rad", "Radius", 1, 0, 5), I("divs", "Divisions", 12, 3, 200), F("arc", "Arc Angle", 360, 0, 360), V("t", "Center", Vec3(0))};
        t.cook = [](CookContext& c) {
            Geometry& g = c.out;
            int divs = clampi(c.i("divs"), 3, 100000);
            float rad = c.f("rad"), arc = std::clamp(c.f("arc"), 0.f, 360.f);
            int type = c.i("type");
            bool full = arc >= 359.999f;
            int n = full ? divs : divs + 1;
            std::vector<int> pts;
            for (int i = 0; i < n; ++i) {
                float a = radians(arc) * float(i) / float(divs);
                // decreasing angle so the polygon faces +Y
                pts.push_back(g.addPoint(Vec3(rad * std::cos(-a), 0, rad * std::sin(-a))));
            }
            if (type == 0) g.addPrim(pts, full);
            else if (type == 1) g.addPrim(pts, false);
            g.transform(Mat4::translate(c.v("t")) * planeOrient(c.i("orient")));
        };
        add(t);
    }
    {
        NodeType t; t.name = "line"; t.label = "Line"; t.category = "Generators";
        t.description = "Creates a polyline.";
        t.parms = {V("origin", "Origin", Vec3(0)), V("dir", "Direction", Vec3(0, 1, 0)), F("dist", "Length", 1, 0, 10), I("points", "Points", 2, 2, 100)};
        t.cook = [](CookContext& c) {
            Geometry& g = c.out;
            int n = clampi(c.i("points"), 2, 1000000);
            Vec3 o = c.v("origin"), d = normalize(c.v("dir")) * c.f("dist");
            std::vector<int> pts;
            for (int i = 0; i < n; ++i) pts.push_back(g.addPoint(o + d * (float(i) / float(n - 1))));
            g.addPrim(pts, false);
        };
        add(t);
    }
    {
        NodeType t; t.name = "file"; t.label = "File"; t.category = "Generators";
        t.description = "Reads geometry from a Wavefront .obj file.";
        t.parms = {File("file", "Geometry File", "")};
        t.cook = [](CookContext& c) {
            const std::string& path = c.s("file");
            if (path.empty()) { c.warning("No file specified."); return; }
            std::string err;
            if (!loadObj(path, c.out, &err)) c.error(err);
        };
        add(t);
    }

    // ================================================================ MODIFIERS
    {
        NodeType t; t.name = "xform"; t.label = "Transform"; t.category = "Modifiers";
        t.description = "Translates, rotates and scales geometry (SRT, rotation order XYZ).";
        t.minInputs = 1; t.maxInputs = 1;
        t.parms = {V("t", "Translate", Vec3(0), -5, 5), V("r", "Rotate", Vec3(0), -180, 180), V("s", "Scale", Vec3(1), 0, 3),
                   V("p", "Pivot", Vec3(0), -5, 5), F("scale", "Uniform Scale", 1, 0, 3)};
        t.cook = [](CookContext& c) {
            c.out = c.input(0);
            c.out.transform(Mat4::srt(c.v("t"), c.v("r"), c.v("s") * c.f("scale"), c.v("p")));
        };
        add(t);
    }
    {
        NodeType t; t.name = "merge"; t.label = "Merge"; t.category = "Modifiers";
        t.description = "Combines all connected inputs into one geometry.";
        t.minInputs = 0; t.maxInputs = 8;
        t.cook = [](CookContext& c) {
            for (int i = 0; i < 8; ++i)
                if (c.hasInput(i)) c.out.merge(c.input(i));
        };
        add(t);
    }
    {
        NodeType t; t.name = "mountain"; t.label = "Mountain"; t.category = "Deformers";
        t.description = "Displaces points along their normals with fractal noise.";
        t.minInputs = 1; t.maxInputs = 1;
        t.parms = {F("height", "Height", 0.5f, -2, 2), F("elementsize", "Element Size", 1, 0.01f, 5), V("offset", "Offset", Vec3(0), -10, 10),
                   I("octaves", "Max Octaves", 4, 1, 10), F("roughness", "Roughness", 0.5f, 0, 1), F("lacunarity", "Lacunarity", 2, 1, 4),
                   M("dir", "Direction", {"Along Normal", "Along Y"}), T("recompN", "Recompute Normals", true)};
        t.cook = [](CookContext& c) {
            c.out = c.input(0);
            Geometry& g = c.out;
            float h = c.f("height"), es = std::max(c.f("elementsize"), 1e-4f), rough = c.f("roughness"), lac = c.f("lacunarity");
            int oct = clampi(c.i("octaves"), 1, 12);
            Vec3 off = c.v("offset");
            bool alongN = c.i("dir") == 0;
            const Attrib* N = g.findAttrib(AttribClass::Point, "N");
            Geometry tmp;
            if (alongN && !N) { tmp = g; tmp.computePointNormals(); N = tmp.findAttrib(AttribClass::Point, "N"); }
            for (size_t i = 0; i < g.numPoints(); ++i) {
                Vec3 dir = alongN ? N->getV(i) : Vec3(0, 1, 0);
                float n = fbm3(g.P[i] / es + off, oct, lac, rough);
                g.P[i] += dir * (n * h);
            }
            if (c.b("recompN") && (g.findAttrib(AttribClass::Point, "N") || g.numPrims() > 0)) g.computePointNormals();
        };
        add(t);
    }
    {
        NodeType t; t.name = "color"; t.label = "Color"; t.category = "Attributes";
        t.description = "Sets the Cd (color) attribute.";
        t.minInputs = 1; t.maxInputs = 1;
        t.parms = {M("class", "Class", {"Point", "Primitive"}), M("mode", "Color Type", {"Constant", "Random", "Ramp by Height (Y)"}),
                   V("color", "Color", Vec3(1, 1, 1), 0, 1), V("color2", "Ramp End Color", Vec3(0.1f, 0.3f, 1.f), 0, 1), I("seed", "Seed", 0, 0, 100)};
        t.parms[2].help = "color";
        t.parms[3].help = "color";
        t.cook = [](CookContext& c) {
            c.out = c.input(0);
            Geometry& g = c.out;
            AttribClass cls = c.i("class") == 0 ? AttribClass::Point : AttribClass::Prim;
            // A color on one class replaces color on the other, as in Houdini's viewport precedence.
            g.removeAttrib(cls == AttribClass::Point ? AttribClass::Prim : AttribClass::Point, "Cd");
            Attrib& Cd = g.addAttrib(cls, "Cd", 3, Vec3(1));
            size_t n = Cd.count();
            int mode = c.i("mode");
            Vec3 c1 = c.v("color"), c2 = c.v("color2");
            int seed = c.i("seed");
            Vec3 mn, mx;
            g.bounds(mn, mx);
            for (size_t i = 0; i < n; ++i) {
                Vec3 col = c1;
                if (mode == 1) {
                    float h = hashRandom(uint32_t(i * 7919u + uint32_t(seed) * 104729u));
                    float s = 0.55f + 0.45f * hashRandom(uint32_t(i * 3571u + uint32_t(seed) * 15485863u + 17u));
                    float v = 0.75f + 0.25f * hashRandom(uint32_t(i * 1301u + uint32_t(seed) * 32452843u + 91u));
                    std::string err;
                    // HSV -> RGB
                    float hh = h * 6.f; int k = int(hh) % 6; float f = hh - std::floor(hh);
                    float p = v * (1 - s), q = v * (1 - s * f), tt = v * (1 - s * (1 - f));
                    switch (k) {
                    case 0: col = {v, tt, p}; break; case 1: col = {q, v, p}; break; case 2: col = {p, v, tt}; break;
                    case 3: col = {p, q, v}; break; case 4: col = {tt, p, v}; break; default: col = {v, p, q};
                    }
                } else if (mode == 2) {
                    Vec3 pos = cls == AttribClass::Point ? g.P[i] : g.primCenter(int(i));
                    float tY = mx.y > mn.y ? (pos.y - mn.y) / (mx.y - mn.y) : 0.f;
                    col = lerp(c2, c1, tY);
                }
                Cd.setV(i, col);
            }
        };
        add(t);
    }
    {
        NodeType t; t.name = "normal"; t.label = "Normal"; t.category = "Attributes";
        t.description = "Computes smooth point normals (N), or per-primitive normals.";
        t.minInputs = 1; t.maxInputs = 1;
        t.parms = {M("class", "Add Normals To", {"Points", "Primitives"}), T("reverse", "Reverse", false)};
        t.cook = [](CookContext& c) {
            c.out = c.input(0);
            Geometry& g = c.out;
            float sgn = c.b("reverse") ? -1.f : 1.f;
            if (c.i("class") == 0) {
                g.computePointNormals();
                Attrib* N = g.findAttrib(AttribClass::Point, "N");
                for (size_t i = 0; i < g.numPoints(); ++i) N->setV(i, N->getV(i) * sgn);
            } else {
                g.removeAttrib(AttribClass::Point, "N");
                Attrib& N = g.addAttrib(AttribClass::Prim, "N", 3);
                for (size_t i = 0; i < g.numPrims(); ++i) N.setV(i, g.primNormal(int(i)) * sgn);
            }
        };
        add(t);
    }
    {
        NodeType t; t.name = "reverse"; t.label = "Reverse"; t.category = "Modifiers";
        t.description = "Reverses polygon winding (flips facing).";
        t.minInputs = 1; t.maxInputs = 1;
        t.cook = [](CookContext& c) {
            c.out = c.input(0);
            for (auto& pr : c.out.prims) std::reverse(c.out.vertices.begin() + pr.start, c.out.vertices.begin() + pr.start + pr.count);
            if (Attrib* N = c.out.findAttrib(AttribClass::Point, "N"))
                for (size_t i = 0; i < N->count(); ++i) N->setV(i, -N->getV(i));
        };
        add(t);
    }
    {
        NodeType t; t.name = "facet"; t.label = "Facet"; t.category = "Modifiers";
        t.description = "Unique points per primitive for a faceted look, or fuse/cusp.";
        t.minInputs = 1; t.maxInputs = 1;
        t.parms = {T("unique", "Unique Points", true), T("removeN", "Remove Normals", true)};
        t.cook = [](CookContext& c) {
            const Geometry& in = c.input(0);
            if (!c.b("unique")) { c.out = in; }
            else {
                Geometry& g = c.out;
                copyAttribLayout(in, g);
                for (size_t pi = 0; pi < in.numPrims(); ++pi) {
                    const Prim& pr = in.prims[pi];
                    std::vector<int> pts;
                    for (int k = 0; k < pr.count; ++k) {
                        int src = in.vertices[size_t(pr.start + k)];
                        int id = int(g.P.size());
                        g.P.push_back(in.P[size_t(src)]);
                        for (auto& [n, a] : in.pointAttribs) g.pointAttribs[n].appendFrom(a, size_t(src));
                        pts.push_back(id);
                    }
                    Prim np; np.start = int(g.vertices.size()); np.count = pr.count; np.closed = pr.closed;
                    g.vertices.insert(g.vertices.end(), pts.begin(), pts.end());
                    g.prims.push_back(np);
                    for (auto& [n, a] : in.primAttribs) g.primAttribs[n].appendFrom(a, pi);
                }
            }
            if (c.b("removeN")) c.out.removeAttrib(AttribClass::Point, "N");
        };
        add(t);
    }
    {
        NodeType t; t.name = "subdivide"; t.label = "Subdivide"; t.category = "Modifiers";
        t.description = "Catmull-Clark subdivision surfaces.";
        t.minInputs = 1; t.maxInputs = 1;
        t.parms = {I("iterations", "Depth", 1, 0, 5)};
        t.cook = [](CookContext& c) {
            c.out = c.input(0);
            int it = clampi(c.i("iterations"), 0, 7);
            for (int i = 0; i < it; ++i) {
                if (c.out.numVertices() * 4 > 20000000) { c.warning("Subdivision stopped early: too many polygons."); break; }
                c.out = catmullClark(c.out);
            }
        };
        add(t);
    }
    {
        NodeType t; t.name = "polyextrude"; t.label = "PolyExtrude"; t.category = "Modifiers";
        t.description = "Extrudes polygons along their normals, building side walls.";
        t.minInputs = 1; t.maxInputs = 1;
        t.parms = {S("group", "Group", "", "Primitive numbers (0-5 8) or expression (@P.y > 0)"), F("dist", "Distance", 0.2f, -2, 2),
                   F("inset", "Inset", 0, 0, 1), T("together", "Keep Points Together", true), T("outputfront", "Output Front", true),
                   T("outputside", "Output Side", true)};
        t.cook = [](CookContext& c) {
            const Geometry& in = c.input(0);
            std::vector<char> sel;
            std::string err;
            if (!selectElements(in, AttribClass::Prim, c.s("group"), c.frame(), c.fps(), sel, err)) { c.error(err); return; }
            Geometry g = in;
            float dist = c.f("dist"), inset = c.f("inset");
            bool together = c.b("together"), front = c.b("outputfront"), side = c.b("outputside");
            std::vector<int> dropPrims;
            auto addSide = [&](int a, int b, int a2, int b2, size_t srcPrim) {
                int id = g.addPrim({a, b, b2, a2});
                for (auto& [n, at] : g.primAttribs) at.setV(size_t(id), at.getV(srcPrim)), (void)n;
            };
            size_t origPrims = in.numPrims();
            if (together) {
                std::vector<Vec3> dirAcc(in.numPoints(), Vec3(0));
                std::map<std::pair<int, int>, int> edgeCount;
                for (size_t pi = 0; pi < origPrims; ++pi) {
                    if (!sel[pi] || !in.prims[pi].closed || in.prims[pi].count < 3) continue;
                    Vec3 n = in.primNormal(int(pi));
                    const Prim& pr = in.prims[pi];
                    for (int k = 0; k < pr.count; ++k) {
                        int a = in.vertices[size_t(pr.start + k)], b = in.vertices[size_t(pr.start + (k + 1) % pr.count)];
                        dirAcc[size_t(a)] += n;
                        edgeCount[{std::min(a, b), std::max(a, b)}]++;
                    }
                }
                std::vector<int> newPt(in.numPoints(), -1);
                for (size_t i = 0; i < in.numPoints(); ++i) {
                    if (length(dirAcc[i]) < 1e-12f) continue;
                    int id = g.addPoint(in.P[i] + normalize(dirAcc[i]) * dist);
                    g.copyPointAttribs(in, int(i), id);
                    newPt[i] = id;
                }
                for (size_t pi = 0; pi < origPrims; ++pi) {
                    if (!sel[pi] || !in.prims[pi].closed || in.prims[pi].count < 3) continue;
                    const Prim pr = g.prims[pi];
                    if (side)
                        for (int k = 0; k < pr.count; ++k) {
                            int a = in.vertices[size_t(pr.start + k)], b = in.vertices[size_t(pr.start + (k + 1) % pr.count)];
                            if (edgeCount[{std::min(a, b), std::max(a, b)}] == 1) addSide(a, b, newPt[size_t(a)], newPt[size_t(b)], pi);
                        }
                    for (int k = 0; k < pr.count; ++k) g.vertices[size_t(pr.start + k)] = newPt[size_t(in.vertices[size_t(pr.start + k)])];
                    if (!front) dropPrims.push_back(int(pi));
                }
            } else {
                for (size_t pi = 0; pi < origPrims; ++pi) {
                    if (!sel[pi] || !in.prims[pi].closed || in.prims[pi].count < 3) continue;
                    const Prim pr = in.prims[pi];
                    Vec3 n = in.primNormal(int(pi)), ctr = in.primCenter(int(pi));
                    std::vector<int> base, top;
                    for (int k = 0; k < pr.count; ++k) {
                        int src = in.vertices[size_t(pr.start + k)];
                        Vec3 p = in.P[size_t(src)];
                        int b = g.addPoint(p); g.copyPointAttribs(in, src, b);
                        int tp = g.addPoint(p + n * dist + (ctr - p) * inset); g.copyPointAttribs(in, src, tp);
                        base.push_back(b); top.push_back(tp);
                    }
                    if (side)
                        for (int k = 0; k < pr.count; ++k) {
                            int kn = (k + 1) % pr.count;
                            addSide(base[size_t(k)], base[size_t(kn)], top[size_t(k)], top[size_t(kn)], pi);
                        }
                    for (int k = 0; k < pr.count; ++k) g.vertices[size_t(pr.start + k)] = top[size_t(k)];
                    if (!front) dropPrims.push_back(int(pi));
                }
            }
            if (!dropPrims.empty()) {
                std::vector<char> drop(g.numPrims(), 0);
                for (int d : dropPrims) drop[size_t(d)] = 1;
                std::vector<int> keep;
                for (size_t i = 0; i < g.numPrims(); ++i) if (!drop[i]) keep.push_back(int(i));
                g = g.extractPrims(keep);
            } else {
                g.removeUnusedPoints();
            }
            if (g.findAttrib(AttribClass::Point, "N")) g.removeAttrib(AttribClass::Point, "N");
            c.out = std::move(g);
        };
        add(t);
    }
    {
        NodeType t; t.name = "fuse"; t.label = "Fuse"; t.category = "Modifiers";
        t.description = "Merges points that are within a distance of each other.";
        t.minInputs = 1; t.maxInputs = 1;
        t.parms = {F("dist", "Snap Distance", 0.001f, 0, 1)};
        t.cook = [](CookContext& c) { c.out = fusePoints(c.input(0), c.f("dist")); };
        add(t);
    }
    {
        NodeType t; t.name = "smooth"; t.label = "Smooth"; t.category = "Deformers";
        t.description = "Laplacian smoothing of point positions.";
        t.minInputs = 1; t.maxInputs = 1;
        t.parms = {F("strength", "Strength", 0.5f, 0, 1), I("iterations", "Iterations", 10, 0, 100)};
        t.cook = [](CookContext& c) {
            c.out = c.input(0);
            Geometry& g = c.out;
            std::vector<std::vector<int>> nbr(g.numPoints());
            for (auto& pr : g.prims) {
                for (int k = 0; k < pr.count; ++k) {
                    if (!pr.closed && k == pr.count - 1) break;
                    int a = g.vertices[size_t(pr.start + k)], b = g.vertices[size_t(pr.start + (k + 1) % pr.count)];
                    nbr[size_t(a)].push_back(b);
                    nbr[size_t(b)].push_back(a);
                }
            }
            float s = c.f("strength");
            int it = clampi(c.i("iterations"), 0, 10000);
            std::vector<Vec3> tmp(g.numPoints());
            for (int k = 0; k < it; ++k) {
                for (size_t i = 0; i < g.numPoints(); ++i) {
                    if (nbr[i].empty()) { tmp[i] = g.P[i]; continue; }
                    Vec3 avg(0);
                    for (int n : nbr[i]) avg += g.P[size_t(n)];
                    avg /= float(nbr[i].size());
                    tmp[i] = lerp(g.P[i], avg, s);
                }
                g.P.swap(tmp);
            }
            if (g.findAttrib(AttribClass::Point, "N")) g.computePointNormals();
        };
        add(t);
    }
    {
        NodeType t; t.name = "peak"; t.label = "Peak"; t.category = "Deformers";
        t.description = "Moves points along their normals.";
        t.minInputs = 1; t.maxInputs = 1;
        t.parms = {F("dist", "Distance", 0.1f, -1, 1)};
        t.cook = [](CookContext& c) {
            c.out = c.input(0);
            Geometry& g = c.out;
            bool had = g.findAttrib(AttribClass::Point, "N") != nullptr;
            if (!had) g.computePointNormals();
            const Attrib* N = g.findAttrib(AttribClass::Point, "N");
            float d = c.f("dist");
            for (size_t i = 0; i < g.numPoints(); ++i) g.P[i] += N->getV(i) * d;
            if (!had) g.removeAttrib(AttribClass::Point, "N");
        };
        add(t);
    }
    {
        NodeType t; t.name = "pointjitter"; t.label = "Point Jitter"; t.category = "Deformers";
        t.description = "Randomly offsets points.";
        t.minInputs = 1; t.maxInputs = 1;
        t.parms = {V("scale", "Scale", Vec3(0.05f), 0, 1), I("seed", "Seed", 0, 0, 100)};
        t.cook = [](CookContext& c) {
            c.out = c.input(0);
            Vec3 s = c.v("scale");
            uint32_t seed = uint32_t(c.i("seed")) * 2654435761u;
            for (size_t i = 0; i < c.out.numPoints(); ++i) {
                uint32_t b = uint32_t(i) * 3u + seed;
                Vec3 r(hashRandom(b) - 0.5f, hashRandom(b + 1) - 0.5f, hashRandom(b + 2) - 0.5f);
                c.out.P[i] += r * s * 2.f;
            }
        };
        add(t);
    }
    {
        NodeType t; t.name = "blast"; t.label = "Blast"; t.category = "Modifiers";
        t.description = "Deletes primitives or points matching a group pattern.";
        t.minInputs = 1; t.maxInputs = 1;
        t.parms = {S("group", "Group", "0", "Numbers (0-5 8 10-20:2) or expression (@P.y > 0)"), M("class", "Group Type", {"Primitives", "Points"}),
                   T("negate", "Delete Non Selected", false)};
        t.cook = [](CookContext& c) {
            const Geometry& in = c.input(0);
            AttribClass cls = c.i("class") == 0 ? AttribClass::Prim : AttribClass::Point;
            std::vector<char> sel;
            std::string err;
            if (!selectElements(in, cls, c.s("group"), c.frame(), c.fps(), sel, err)) { c.error(err); return; }
            bool neg = c.b("negate");
            if (cls == AttribClass::Prim) {
                std::vector<int> keep;
                for (size_t i = 0; i < sel.size(); ++i) if (bool(sel[i]) == neg) keep.push_back(int(i));
                c.out = in.extractPrims(keep);
            } else {
                std::vector<char> keep(sel.size());
                for (size_t i = 0; i < sel.size(); ++i) keep[i] = bool(sel[i]) == neg;
                c.out = in.extractPoints(keep);
            }
        };
        add(t);
    }

    // ================================================================ POINTS / INSTANCING
    {
        NodeType t; t.name = "scatter"; t.label = "Scatter"; t.category = "Points";
        t.description = "Scatters random points over surfaces (area weighted).";
        t.minInputs = 1; t.maxInputs = 1;
        t.parms = {I("npts", "Force Total Count", 500, 0, 10000), I("seed", "Global Seed", 0, 0, 100), F("pscale", "Point Scale (pscale)", 0.05f, 0, 1),
                   F("pscalevar", "Random Scale Variance", 0, 0, 1)};
        t.cook = [](CookContext& c) {
            const Geometry& in = c.input(0);
            int npts = clampi(c.i("npts"), 0, 5000000);
            // triangles: prim, a, b, c
            struct Tri { int prim, a, b, c; };
            std::vector<Tri> tris;
            std::vector<double> cdf;
            double total = 0;
            for (size_t pi = 0; pi < in.numPrims(); ++pi) {
                const Prim& pr = in.prims[pi];
                if (!pr.closed || pr.count < 3) continue;
                for (int k = 1; k + 1 < pr.count; ++k) {
                    Tri tr{int(pi), in.vertices[size_t(pr.start)], in.vertices[size_t(pr.start + k)], in.vertices[size_t(pr.start + k + 1)]};
                    double area = 0.5 * length(cross(in.P[size_t(tr.b)] - in.P[size_t(tr.a)], in.P[size_t(tr.c)] - in.P[size_t(tr.a)]));
                    if (area <= 0) continue;
                    total += area;
                    tris.push_back(tr);
                    cdf.push_back(total);
                }
            }
            if (tris.empty()) { c.warning("No polygons to scatter on."); return; }
            Geometry& g = c.out;
            for (auto& [n, a] : in.pointAttribs) { Attrib na; na.tuple = a.tuple; std::copy(a.defaults, a.defaults + 3, na.defaults); g.pointAttribs[n] = na; }
            bool hasN = in.findAttrib(AttribClass::Point, "N") != nullptr;
            const Attrib* primCd = in.findAttrib(AttribClass::Prim, "Cd");
            uint32_t seed = uint32_t(c.i("seed")) * 7919u + 13u;
            g.P.reserve(size_t(npts));
            std::vector<int> fromPrim;
            for (int i = 0; i < npts; ++i) {
                uint32_t b = uint32_t(i) * 4u + seed * 1000003u;
                double r = double(hashRandom(b)) * total;
                size_t ti = size_t(std::lower_bound(cdf.begin(), cdf.end(), r) - cdf.begin());
                if (ti >= tris.size()) ti = tris.size() - 1;
                const Tri& tr = tris[ti];
                float u = hashRandom(b + 1), v = hashRandom(b + 2);
                if (u + v > 1) { u = 1 - u; v = 1 - v; }
                float w = 1 - u - v;
                Vec3 p = in.P[size_t(tr.a)] * w + in.P[size_t(tr.b)] * u + in.P[size_t(tr.c)] * v;
                g.P.push_back(p);
                for (auto& [n, a] : in.pointAttribs) {
                    Attrib& d = g.pointAttribs[n];
                    for (int k = 0; k < a.tuple; ++k)
                        d.data.push_back(a.data[size_t(tr.a) * a.tuple + k] * w + a.data[size_t(tr.b) * a.tuple + k] * u + a.data[size_t(tr.c) * a.tuple + k] * v);
                }
                fromPrim.push_back(tr.prim);
            }
            if (hasN) {
                Attrib* N = g.findAttrib(AttribClass::Point, "N");
                for (size_t i = 0; i < g.numPoints(); ++i) N->setV(i, normalize(N->getV(i)));
            } else {
                Attrib& N = g.addAttrib(AttribClass::Point, "N", 3);
                for (size_t i = 0; i < g.numPoints(); ++i) N.setV(i, in.primNormal(fromPrim[i]));
            }
            if (primCd && !g.findAttrib(AttribClass::Point, "Cd")) {
                Attrib& Cd = g.addAttrib(AttribClass::Point, "Cd", 3, Vec3(1));
                for (size_t i = 0; i < g.numPoints(); ++i) Cd.setV(i, primCd->getV(size_t(fromPrim[i])));
            }
            float ps = c.f("pscale"), var = c.f("pscalevar");
            Attrib& PS = g.addAttrib(AttribClass::Point, "pscale", 1, Vec3(1));
            for (size_t i = 0; i < g.numPoints(); ++i)
                PS.setF(i, ps * (1.f - var * hashRandom(uint32_t(i) * 31u + seed + 7u)));
        };
        add(t);
    }
    {
        NodeType t; t.name = "copytopoints"; t.label = "Copy to Points"; t.category = "Points";
        t.description = "Copies the first input onto every point of the second input (uses N, up, pscale, scale, Cd).";
        t.minInputs = 2; t.maxInputs = 2;
        t.inputLabels = {"Geometry to copy", "Target points"};
        t.parms = {T("useN", "Orient to N", true), T("usepscale", "Use pscale", true), T("useCd", "Transfer Cd", true), F("scale", "Uniform Scale", 1, 0, 3)};
        t.cook = [](CookContext& c) {
            const Geometry& src = c.input(0);
            const Geometry& pts = c.input(1);
            const Attrib* N = c.b("useN") ? pts.findAttrib(AttribClass::Point, "N") : nullptr;
            const Attrib* up = pts.findAttrib(AttribClass::Point, "up");
            const Attrib* ps = c.b("usepscale") ? pts.findAttrib(AttribClass::Point, "pscale") : nullptr;
            const Attrib* sc = pts.findAttrib(AttribClass::Point, "scale");
            const Attrib* Cd = c.b("useCd") ? pts.findAttrib(AttribClass::Point, "Cd") : nullptr;
            float us = c.f("scale");
            if (src.numPoints() * pts.numPoints() > 50000000) { c.error("Too many copies (over 50M points)."); return; }
            Geometry& g = c.out;
            g.P.reserve(src.numPoints() * pts.numPoints());
            for (size_t i = 0; i < pts.numPoints(); ++i) {
                Geometry copy = src;
                Vec3 s(us);
                if (ps) s = s * ps->getF(i);
                if (sc) s = s * sc->getV(i);
                Mat4 R = N ? Mat4::lookAlong(N->getV(i), up ? up->getV(i) : Vec3(0, 1, 0)) : Mat4();
                copy.transform(Mat4::translate(pts.P[i]) * R * Mat4::scale(s));
                if (Cd) {
                    copy.removeAttrib(AttribClass::Prim, "Cd");
                    Attrib& cd = copy.addAttrib(AttribClass::Point, "Cd", 3, Vec3(1));
                    for (size_t k = 0; k < copy.numPoints(); ++k) cd.setV(k, Cd->getV(i));
                }
                Attrib& cn = copy.addAttrib(AttribClass::Prim, "copynum", 1);
                for (size_t k = 0; k < copy.numPrims(); ++k) cn.setF(k, float(i));
                g.merge(copy);
            }
        };
        add(t);
    }
    {
        NodeType t; t.name = "polywire"; t.label = "PolyWire"; t.category = "Modifiers";
        t.description = "Builds tubes along every edge (wireframe geometry).";
        t.minInputs = 1; t.maxInputs = 1;
        t.parms = {F("radius", "Wire Radius", 0.03f, 0, 0.5f), I("divs", "Divisions", 6, 3, 32)};
        t.cook = [](CookContext& c) {
            const Geometry& in = c.input(0);
            Geometry& g = c.out;
            float r = c.f("radius");
            int divs = clampi(c.i("divs"), 3, 64);
            std::map<std::pair<int, int>, bool> seen;
            const Attrib* Cd = in.findAttrib(AttribClass::Point, "Cd");
            Attrib* oCd = Cd ? &g.addAttrib(AttribClass::Point, "Cd", 3, Vec3(1)) : nullptr;
            for (auto& pr : in.prims) {
                int ne = pr.closed ? pr.count : pr.count - 1;
                for (int k = 0; k < ne; ++k) {
                    int a = in.vertices[size_t(pr.start + k)], b = in.vertices[size_t(pr.start + (k + 1) % pr.count)];
                    auto key = std::make_pair(std::min(a, b), std::max(a, b));
                    if (a == b || seen[key]) continue;
                    seen[key] = true;
                    Vec3 pa = in.P[size_t(a)], pb = in.P[size_t(b)];
                    Mat4 R = Mat4::lookAlong(pb - pa, Vec3(0, 1, 0));
                    int base = int(g.numPoints());
                    for (int end = 0; end < 2; ++end)
                        for (int j = 0; j < divs; ++j) {
                            float ph = 2 * kPi * float(j) / float(divs);
                            Vec3 off = R.transformDir(Vec3(std::cos(ph), std::sin(ph), 0) * r);
                            int id = g.addPoint((end ? pb : pa) + off);
                            if (oCd) { oCd = g.findAttrib(AttribClass::Point, "Cd"); oCd->setV(size_t(id), Cd->getV(size_t(end ? b : a))); }
                        }
                    for (int j = 0; j < divs; ++j) {
                        int jn = (j + 1) % divs;
                        g.addPrim({base + j, base + jn, base + divs + jn, base + divs + j});
                    }
                }
            }
            g.computePointNormals();
        };
        add(t);
    }

    // ================================================================ ATTRIBUTES / VEX
    {
        NodeType t; t.name = "attribwrangle"; t.label = "Attribute Wrangle"; t.category = "Attributes";
        t.description = "Runs a VEX-like snippet over every point or primitive.";
        t.minInputs = 1; t.maxInputs = 1;
        t.parms = {M("class", "Run Over", {"Points", "Primitives"}),
                   C("snippet", "VEXpression",
                     "// Runs once per point. Try: @P, @N, @Cd, @ptnum, @numpt, @Time, @Frame\n"
                     "float d = length(@P);\n"
                     "@P.y += sin(d * 4 - @Time * 3) * 0.15;\n"
                     "@Cd = hsvtorgb(set(fit(d, 0, 5, 0.55, 0.95), 0.7, 1));\n")};
        t.cook = [](CookContext& c) {
            c.out = c.input(0);
            std::string err;
            auto prog = Program::compile(c.s("snippet"), false, err);
            if (!prog) { c.error(err); return; }
            AttribClass cls = c.i("class") == 0 ? AttribClass::Point : AttribClass::Prim;
            if (!runOverElements(c.out, cls, *prog, c.frame(), c.fps(), nullptr, err)) { c.error(err); return; }
            for (auto& b : prog->attribs())
                if (b.name == "P" && b.written && c.out.findAttrib(AttribClass::Point, "N") && cls == AttribClass::Point) {
                    bool writesN = false;
                    for (auto& b2 : prog->attribs()) if (b2.name == "N" && b2.written) writesN = true;
                    if (!writesN && c.out.numPrims() > 0) c.out.computePointNormals();
                }
        };
        add(t);
    }
    {
        NodeType t; t.name = "attribcreate"; t.label = "Attribute Create"; t.category = "Attributes";
        t.description = "Creates or sets an attribute to a constant value.";
        t.minInputs = 1; t.maxInputs = 1;
        t.parms = {S("name", "Name", "foo"), M("class", "Class", {"Point", "Primitive"}), M("type", "Type", {"Float", "Vector"}), V("value", "Value", Vec3(0), -1, 1)};
        t.cook = [](CookContext& c) {
            c.out = c.input(0);
            std::string name = c.s("name");
            if (name.empty() || name == "P") { c.error("Invalid attribute name."); return; }
            for (char ch : name) if (!(std::isalnum((unsigned char)ch) || ch == '_')) { c.error("Invalid attribute name."); return; }
            AttribClass cls = c.i("class") == 0 ? AttribClass::Point : AttribClass::Prim;
            int tuple = c.i("type") == 0 ? 1 : 3;
            Attrib& a = c.out.addAttrib(cls, name, tuple);
            Vec3 v = c.v("value");
            for (size_t i = 0; i < a.count(); ++i) a.setV(i, v);
        };
        add(t);
    }

    // ================================================================ UTILITY
    {
        NodeType t; t.name = "null"; t.label = "Null"; t.category = "Utility";
        t.description = "Passes geometry through. Use as a named output (e.g. OUT).";
        t.minInputs = 0; t.maxInputs = 1;
        t.cook = [](CookContext& c) { if (c.hasInput(0)) c.out = c.input(0); };
        add(t);
    }
    {
        NodeType t; t.name = "switch"; t.label = "Switch"; t.category = "Utility";
        t.description = "Outputs one of its inputs, chosen by index (animatable).";
        t.minInputs = 0; t.maxInputs = 8;
        t.parms = {I("input", "Select Input", 0, 0, 7)};
        t.cook = [](CookContext& c) {
            int i = clampi(c.i("input"), 0, 7);
            if (c.hasInput(i)) c.out = c.input(i);
            else c.warning("Selected input is not connected.");
        };
        add(t);
    }
    registerSimNodes(types);
    return types;
}

} // namespace

const std::vector<NodeType>& nodeTypes() {
    static const std::vector<NodeType> types = buildTypes();
    return types;
}

const NodeType* findNodeType(const std::string& name) {
    for (auto& t : nodeTypes()) if (t.name == name) return &t;
    return nullptr;
}

} // namespace hd
