// Headless tests for the HD core engine.
#include "core/Demo.h"
#include "core/Expr.h"
#include "core/GeoIO.h"
#include "core/Node.h"
#include "core/Ops.h"
#include <cstdio>
#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace hd;

static int g_failed = 0, g_checks = 0;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failed; std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define NEAR(a, b) CHECK(std::fabs(double(a) - double(b)) < 1e-4)

struct TestCase { const char* name; std::function<void()> fn; };
static std::vector<TestCase>& tests() { static std::vector<TestCase> t; return t; }
struct Reg { Reg(const char* n, std::function<void()> f) { tests().push_back({n, std::move(f)}); } };
#define TEST(name) static void name(); static Reg reg_##name(#name, name); static void name()

static float evalE(const char* s, float frame = 1) {
    float v = 0;
    std::string err;
    bool ok = evalExpression(s, frame, 24, v, &err);
    if (!ok) std::printf("  expr error: %s\n", err.c_str());
    return v;
}

static bool closedManifold(const Geometry& g) {
    // every edge used exactly twice, in opposite directions => consistent outward winding
    std::map<std::pair<int, int>, int> dirCount;
    for (auto& pr : g.prims)
        for (int k = 0; k < pr.count; ++k) {
            int a = g.vertices[size_t(pr.start + k)], b = g.vertices[size_t(pr.start + (k + 1) % pr.count)];
            dirCount[{a, b}]++;
        }
    for (auto& [e, n] : dirCount) {
        if (n != 1) return false;
        if (!dirCount.count({e.second, e.first})) return false;
    }
    return true;
}

static float signedVolume(const Geometry& g) {
    double v = 0;
    for (size_t pi = 0; pi < g.numPrims(); ++pi) {
        const Prim& pr = g.prims[pi];
        Vec3 a = g.P[size_t(g.vertices[size_t(pr.start)])];
        for (int k = 1; k + 1 < pr.count; ++k) {
            Vec3 b = g.P[size_t(g.vertices[size_t(pr.start + k)])], c = g.P[size_t(g.vertices[size_t(pr.start + k + 1)])];
            v += dot(a, cross(b, c)) / 6.0;
        }
    }
    return float(v);
}

TEST(expressions) {
    NEAR(evalE("1 + 2 * 3"), 7);
    NEAR(evalE("(1 + 2) * 3"), 9);
    NEAR(evalE("-2 * -3"), 6);
    NEAR(evalE("$F * 0.5", 10), 5);
    NEAR(evalE("$T", 25), 1);
    NEAR(evalE("sin($PI / 2)"), 1);
    NEAR(evalE("fit(5, 0, 10, 0, 1)"), 0.5f);
    NEAR(evalE("clamp(5, 0, 2)"), 2);
    NEAR(evalE("1 < 2 ? 10 : 20"), 10);
    NEAR(evalE("length(set(3, 4, 0))"), 5);
    NEAR(evalE("dot({1,2,3}, {1,1,1})"), 6);
    NEAR(evalE("max(3, 7) % 4"), 3);
    float v; std::string err;
    CHECK(!evalExpression("1 +", 1, 24, v, &err) && !err.empty());
    CHECK(!evalExpression("foo(1)", 1, 24, v, &err));
    CHECK(isExpressionString("$F*2"));
    CHECK(!isExpressionString("1.5"));
}

TEST(snippets) {
    Geometry g;
    for (int i = 0; i < 5; ++i) g.addPoint(Vec3(float(i), 0, 0));
    std::string err;
    auto prog = Program::compile(
        "float s = @ptnum * 2;\n"
        "vector c = {0.5, 0.25, 1};\n"
        "if (@ptnum >= 3) { @P.y = s; } else @P.y = -1;\n"
        "@Cd = c;\n"
        "f@weight = @numpt;\n"
        "for (int i = 0; i < 3; i++) @P.z += 1;\n"
        "@N = normalize(set(0, 2, 0));", false, err);
    CHECK(prog != nullptr);
    if (!prog) { std::printf("  %s\n", err.c_str()); return; }
    CHECK(runOverElements(g, AttribClass::Point, *prog, 1, 24, nullptr, err));
    NEAR(g.P[0].y, -1);
    NEAR(g.P[4].y, 8);
    NEAR(g.P[2].z, 3);
    const Attrib* Cd = g.findAttrib(AttribClass::Point, "Cd");
    CHECK(Cd && Cd->tuple == 3);
    if (Cd) NEAR(Cd->getV(3).y, 0.25f);
    const Attrib* w = g.findAttrib(AttribClass::Point, "weight");
    CHECK(w && w->tuple == 1);
    if (w) NEAR(w->getF(0), 5);
    const Attrib* N = g.findAttrib(AttribClass::Point, "N");
    CHECK(N && std::fabs(N->getV(1).y - 1) < 1e-5);

    CHECK(Program::compile("@P.y = ;", false, err) == nullptr);
    CHECK(Program::compile("undefinedVar = 3;", false, err) == nullptr);
}

TEST(groups) {
    Geometry g;
    for (int i = 0; i < 10; ++i) g.addPoint(Vec3(float(i), float(i % 2), 0));
    std::vector<char> m;
    std::string err;
    CHECK(selectElements(g, AttribClass::Point, "0-3 7", 1, 24, m, err));
    CHECK(m[0] && m[3] && !m[4] && m[7] && !m[9]);
    CHECK(selectElements(g, AttribClass::Point, "0-9:3", 1, 24, m, err));
    CHECK(m[0] && m[3] && m[6] && m[9] && !m[1]);
    CHECK(selectElements(g, AttribClass::Point, "@P.y > 0.5", 1, 24, m, err));
    CHECK(!m[0] && m[1] && !m[2] && m[3]);
    CHECK(selectElements(g, AttribClass::Point, "!@ptnum < 8", 1, 24, m, err));
    CHECK(!m[0] && m[8] && m[9]);
    CHECK(!selectElements(g, AttribClass::Point, "abc", 1, 24, m, err));
}

TEST(generators) {
    Graph gr;
    for (const char* t : {"box", "sphere", "torus", "tube"}) {
        Node* n = gr.createNode(t);
        gr.cook(n);
        const Geometry& g = n->geometry();
        CHECK(n->error().empty());
        CHECK(g.numPrims() > 0);
        CHECK(closedManifold(g));
        CHECK(signedVolume(g) > 0); // outward-facing winding
        if (!closedManifold(g) || signedVolume(g) <= 0) std::printf("  generator %s bad topology (vol %f)\n", t, signedVolume(g));
    }
    Node* box = gr.findNode("box1");
    NEAR(signedVolume(box->geometry()), 1.0f);
    CHECK(box->geometry().numPoints() == 8);
    box->parm("divs")->val[0] = 3;
    box->touch();
    gr.cook(box);
    CHECK(box->geometry().numPoints() == 56); // 4^3 - 2^3
    CHECK(box->geometry().numPrims() == 54);
    NEAR(signedVolume(box->geometry()), 1.0f);

    Node* grid = gr.createNode("grid");
    gr.cook(grid);
    CHECK(grid->geometry().numPoints() == 100);
    CHECK(grid->geometry().numPrims() == 81);
    CHECK(grid->geometry().primNormal(0).y > 0.99f);
    Node* circ = gr.createNode("circle");
    gr.cook(circ);
    CHECK(circ->geometry().primNormal(0).y > 0.99f);
}

TEST(cook_caching_and_time) {
    Graph g;
    Node* box = g.createNode("box");
    Node* xf = g.createNode("xform");
    CHECK(g.connect(box, xf, 0));
    g.setDisplayNode(xf);
    g.cook(xf);
    uint64_t c0 = g.totalCooks();
    CHECK(c0 == 2);
    g.cook(xf);
    CHECK(g.totalCooks() == c0); // nothing dirty
    g.frame = 10;
    g.cook(xf);
    CHECK(g.totalCooks() == c0); // not time dependent
    xf->parm("t")->expr[1] = "$F * 0.1";
    xf->touch();
    g.cook(xf);
    CHECK(g.totalCooks() == c0 + 1);
    Vec3 mn, mx;
    xf->geometry().bounds(mn, mx);
    NEAR(mn.y, 0.5f);
    g.frame = 20;
    g.cook(xf);
    CHECK(g.totalCooks() == c0 + 2); // time dependent -> recook, box untouched
    xf->geometry().bounds(mn, mx);
    NEAR(mn.y, 1.5f);
    // cycles are refused
    Node* m = g.createNode("mountain");
    CHECK(g.connect(xf, m, 0));
    CHECK(!g.connect(m, xf, 0));
    // missing input -> error
    Node* xf2 = g.createNode("xform");
    g.cook(xf2);
    CHECK(!xf2->error().empty());
    // bypass passes input through
    xf->bypass = true;
    g.cook(xf);
    xf->geometry().bounds(mn, mx);
    NEAR(mn.y, -0.5f);
}

TEST(subdivide_and_extrude) {
    Graph g;
    Node* box = g.createNode("box");
    Node* sub = g.createNode("subdivide");
    g.connect(box, sub, 0);
    sub->parm("iterations")->val[0] = 2;
    g.cook(sub);
    const Geometry& s = sub->geometry();
    CHECK(s.numPrims() == 6 * 16);
    CHECK(closedManifold(s));
    float vol = signedVolume(s);
    CHECK(vol > 0.3f && vol < 1.0f); // shrinks toward a sphere-ish shape
    // all points roughly within the original box and away from corners
    Vec3 mn, mx; s.bounds(mn, mx);
    CHECK(mx.x < 0.5f && mx.x > 0.3f);

    Node* ex = g.createNode("polyextrude");
    g.connect(box, ex, 0);
    ex->parm("group")->str = "@P.y > 0.4";
    ex->parm("dist")->val[0] = 1;
    g.cook(ex);
    const Geometry& e = ex->geometry();
    CHECK(ex->error().empty());
    CHECK(e.numPrims() == 6 + 4);
    CHECK(closedManifold(e));
    NEAR(signedVolume(e), 2.0f);
    // individual mode on all faces
    ex->parm("together")->val[0] = 0;
    ex->parm("group")->str = "";
    ex->touch();
    g.cook(ex);
    CHECK(ex->geometry().numPrims() == 6 * 5);
}

TEST(scatter_copy_wrangle_blast) {
    Graph g;
    Node* sph = g.createNode("sphere");
    Node* sc = g.createNode("scatter");
    g.connect(sph, sc, 0);
    sc->parm("npts")->val[0] = 200;
    g.cook(sc);
    const Geometry& pts = sc->geometry();
    CHECK(pts.numPoints() == 200);
    CHECK(pts.numPrims() == 0);
    bool onSurface = true;
    for (auto& p : pts.P) onSurface &= std::fabs(length(p) - 1.f) < 0.06f;
    CHECK(onSurface);
    CHECK(pts.findAttrib(AttribClass::Point, "N") && pts.findAttrib(AttribClass::Point, "pscale"));

    Node* box = g.createNode("box");
    Node* cp = g.createNode("copytopoints");
    g.connect(box, cp, 0);
    g.connect(sc, cp, 1);
    g.cook(cp);
    CHECK(cp->geometry().numPrims() == 200 * 6);
    CHECK(cp->geometry().numPoints() == 200 * 8);

    Node* w = g.createNode("attribwrangle");
    g.connect(sph, w, 0);
    w->parm("snippet")->str = "@Cd = set(@P.y > 0, 0, 0);";
    g.cook(w);
    CHECK(w->error().empty());
    CHECK(w->geometry().findAttrib(AttribClass::Point, "Cd") != nullptr);
    CHECK(!w->isTimeDependent());
    w->parm("snippet")->str = "@P.y += @Time;";
    CHECK(w->isTimeDependent());
    w->parm("snippet")->str = "@P.y +=;";
    w->touch();
    g.cook(w);
    CHECK(!w->error().empty());

    Node* bl = g.createNode("blast");
    g.connect(sph, bl, 0);
    bl->parm("group")->str = "@P.y > 0";
    g.cook(bl);
    CHECK(bl->error().empty());
    CHECK(bl->geometry().numPrims() > 0 && bl->geometry().numPrims() < sph->geometry().numPrims());
    Vec3 mn, mx; bl->geometry().bounds(mn, mx);
    CHECK(mx.y < 0.3f);
}

TEST(fuse_and_merge) {
    Graph g;
    Node* a = g.createNode("box");
    Node* b = g.createNode("box");
    Node* m = g.createNode("merge");
    g.connect(a, m, 0);
    g.connect(b, m, 3);
    g.cook(m);
    CHECK(m->geometry().numPoints() == 16);
    Node* f = g.createNode("fuse");
    g.connect(m, f, 0);
    g.cook(f);
    CHECK(f->geometry().numPoints() == 8);
    CHECK(f->geometry().numPrims() == 12);
    // facet then fuse restores shared points
    Node* fc = g.createNode("facet");
    g.connect(a, fc, 0);
    g.cook(fc);
    CHECK(fc->geometry().numPoints() == 24);
}

TEST(serialization) {
    Graph g;
    Node* box = g.createNode("box", 10, 20);
    Node* w = g.createNode("attribwrangle", 10, 120);
    g.connect(box, w, 0);
    box->parm("size")->val[1] = 2.5f;
    box->parm("t")->expr[0] = "sin($T)";
    w->parm("snippet")->str = "@P *= 2; // \"quoted\"\n";
    w->bypass = true;
    g.setDisplayNode(w);
    std::string text = g.toJson().dump();
    Graph g2;
    std::string err;
    std::string perr;
    CHECK(g2.fromJson(Json::parse(text, &perr), &err));
    CHECK(perr.empty());
    Node* b2 = g2.findNode("box1");
    Node* w2 = g2.findNode("attribwrangle1");
    CHECK(b2 && w2);
    if (!b2 || !w2) return;
    NEAR(b2->parm("size")->val[1], 2.5f);
    CHECK(b2->parm("t")->expr[0] == "sin($T)");
    CHECK(w2->parm("snippet")->str == "@P *= 2; // \"quoted\"\n");
    CHECK(w2->bypass);
    CHECK(w2->inputs[0] == b2);
    CHECK(g2.displayNode() == w2);
    NEAR(b2->y, 20);
    CHECK(g2.toJson().dump() == text);
}

TEST(obj_roundtrip) {
    Graph g;
    Node* s = g.createNode("sphere");
    g.cook(s);
    std::string path = "hd_test_sphere.obj";
    std::string err;
    CHECK(saveObj(path, s->geometry(), &err));
    Geometry in;
    CHECK(loadObj(path, in, &err));
    CHECK(in.numPoints() == s->geometry().numPoints());
    CHECK(in.numPrims() == s->geometry().numPrims());
    CHECK(in.findAttrib(AttribClass::Point, "N") != nullptr);
    std::remove(path.c_str());
}

TEST(all_nodes_cook_on_defaults) {
    // Every node type must cook without crashing, with a sphere wired to each input.
    for (auto& t : nodeTypes()) {
        Graph g;
        Node* src = g.createNode("sphere");
        Node* src2 = g.createNode("scatter");
        g.connect(src, src2, 0);
        Node* n = g.createNode(t.name);
        CHECK(n != nullptr);
        if (t.maxInputs >= 1) g.connect(t.name == "trail" ? g.createNode("popsolver") : src, n, 0);
        if (t.maxInputs >= 2) g.connect(src2, n, 1);
        g.cook(n);
        if (t.name != "file" && t.name != "switch" && !n->error().empty())
            std::printf("  node %s error: %s\n", t.name.c_str(), n->error().c_str());
        CHECK(t.name == "file" || n->error().empty());
        for (auto& [nm, a] : n->geometry().pointAttribs) CHECK(a.count() == n->geometry().numPoints());
        for (auto& [nm, a] : n->geometry().primAttribs) CHECK(a.count() == n->geometry().numPrims());
        for (int v : n->geometry().vertices) CHECK(v >= 0 && v < int(n->geometry().numPoints()));
    }
}

TEST(particle_solver) {
    Graph g;
    Node* src = g.createNode("sphere");
    Node* pop = g.createNode("popsolver");
    g.connect(src, pop, 0);
    pop->parm("rate")->val[0] = 240; // 10 per frame at 24 fps
    pop->parm("lifevar")->val[0] = 0;
    pop->parm("life")->val[0] = 1;   // 24 frames
    g.frame = 1; g.cook(pop);
    CHECK(pop->error().empty());
    CHECK(pop->geometry().numPoints() == 10);
    g.frame = 12; g.cook(pop);
    CHECK(pop->geometry().numPoints() == 120);
    CHECK(pop->simCacheStart == 1 && pop->simCacheEnd == 12);
    const Geometry snap = pop->geometry();
    // steady state: births balance deaths
    g.frame = 60; g.cook(pop);
    size_t steady = pop->geometry().numPoints();
    CHECK(steady >= 230 && steady <= 250);
    // ground collision keeps everything above the floor; ids are unique
    Vec3 mn, mx; pop->geometry().bounds(mn, mx);
    CHECK(mn.y >= -1e-4f);
    const Attrib* id = pop->geometry().findAttrib(AttribClass::Point, "id");
    CHECK(id != nullptr);
    if (id) { std::set<int> ids; for (size_t i = 0; i < id->count(); ++i) ids.insert(int(id->getF(i))); CHECK(ids.size() == steady); }
    // scrubbing back uses the cache (no re-simulation) and is identical
    g.frame = 12; g.cook(pop);
    CHECK(pop->geometry().numPoints() == snap.numPoints());
    bool same = true;
    for (size_t i = 0; i < snap.numPoints() && same; ++i) same = snap.P[i] == pop->geometry().P[i];
    CHECK(same);
    CHECK(pop->simCacheEnd == 60);
    // editing a parameter (or anything upstream) invalidates the cache and re-simulates deterministically
    src->parm("rows")->val[0] = 9; src->touch();
    g.cook(pop);
    CHECK(pop->simCacheEnd == 12);
    src->parm("rows")->val[0] = 13; src->touch();
    g.cook(pop);
    same = pop->geometry().numPoints() == snap.numPoints();
    for (size_t i = 0; i < snap.numPoints() && same; ++i) same = length(snap.P[i] - pop->geometry().P[i]) < 1e-5f;
    CHECK(same);
    // before the start frame there are no particles
    pop->parm("startframe")->val[0] = 20; pop->touch();
    g.cook(pop);
    CHECK(pop->geometry().numPoints() == 0);
    // POP wrangle + burst emission + die on collision
    pop->parm("startframe")->val[0] = 1;
    pop->parm("emission")->val[0] = 1;
    pop->parm("rate")->val[0] = 500;
    pop->parm("snippet")->str = "@v.y = 0; f@marked = 1;";
    pop->touch();
    g.frame = 5; g.cook(pop);
    CHECK(pop->error().empty());
    CHECK(pop->geometry().numPoints() == 500);
    CHECK(pop->geometry().findAttrib(AttribClass::Point, "marked") != nullptr);
    pop->parm("snippet")->str = "@v.y = ;"; pop->touch(); g.cook(pop);
    CHECK(!pop->error().empty());
    // trail makes one 2-point polyline per particle
    pop->parm("snippet")->str = ""; pop->touch();
    Node* tr = g.createNode("trail");
    g.connect(pop, tr, 0);
    g.cook(tr);
    CHECK(tr->error().empty());
    CHECK(tr->geometry().numPrims() == 500 && tr->geometry().numPoints() == 1000);
    // animated emitter: particles are born where the emitter was at each frame
    Graph g2;
    Node* e = g2.createNode("sphere");
    e->parm("rad")->val[0] = e->parm("rad")->val[1] = e->parm("rad")->val[2] = 0.01f;
    e->parm("t")->expr[0] = "$F";
    Node* p2 = g2.createNode("popsolver");
    g2.connect(e, p2, 0);
    for (const char* z : {"speedn", "velvar", "drag"}) p2->parm(z)->val[0] = 0;
    p2->parm("gravity")->val[1] = 0;
    p2->parm("life")->val[0] = 100; p2->parm("rate")->val[0] = 24;
    g2.frame = 30; g2.cook(p2); // simulates 1..30 in one go
    p2->geometry().bounds(mn, mx);
    CHECK(p2->geometry().numPoints() == 30);
    CHECK(mn.x < 1.1f && mx.x > 29.9f);
}

TEST(demo_scenes_cook_cleanly) {
    for (auto& d : demoScenes()) {
        Graph g;
        CHECK(buildDemoScene(g, d.name));
        for (float f : {1.f, 37.f}) {
            g.frame = f;
            for (auto& n : g.nodes()) {
                g.cook(n.get());
                if (!n->error().empty()) std::printf("  demo %s node %s: %s\n", d.name.c_str(), n->name().c_str(), n->error().c_str());
                CHECK(n->error().empty());
            }
        }
        CHECK(g.displayNode() && g.displayNode()->geometry().numPoints() > 0);
        // round-trip through the file format
        Graph g2;
        CHECK(g2.fromJson(Json::parse(g.toJson().dump())));
        CHECK(g2.nodes().size() == g.nodes().size());
    }
}

int main() {
    for (auto& t : tests()) {
        int before = g_failed;
        t.fn();
        std::printf("%s %s\n", g_failed == before ? "[ ok ]" : "[FAIL]", t.name);
    }
    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
