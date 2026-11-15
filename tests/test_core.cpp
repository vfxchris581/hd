// Headless tests for the HD core engine.
#include "core/Demo.h"
#include "core/Expr.h"
#include "core/GeoIO.h"
#include "core/Lights.h"
#include "core/Node.h"
#include "core/Ops.h"
#include "core/PathTracer.h"
#include "core/Volume.h"
#include <cstdio>
#include <cmath>
#include <cstdlib>
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

TEST(copy_paste_and_merge_nodes) {
    Graph g;
    Node* box = g.createNode("box", 0, 0);
    Node* xf = g.createNode("xform", 0, 80);
    Node* out = g.createNode("null", 0, 160, "OUT");
    g.connect(box, xf, 0);
    g.connect(xf, out, 0);
    xf->parm("t")->val[1] = 3;
    xf->parm("r")->expr[1] = "$F";
    // copy two of the three nodes and paste them back
    Json nodes = g.nodesToJson({xf, out});
    CHECK(nodes.size() == 2);
    std::vector<Node*> made = g.addNodesFromJson(Json::parse(nodes.dump(0)), 40, 40);
    CHECK(made.size() == 2 && g.nodes().size() == 5);
    if (made.size() == 2) {
        Node* xf2 = made[0]->type().name == "xform" ? made[0] : made[1];
        Node* out2 = made[0] == xf2 ? made[1] : made[0];
        CHECK(xf2->name() != "xform1" && out2->name() != "OUT"); // renamed, not clashing
        NEAR(xf2->parm("t")->val[1], 3);
        CHECK(xf2->parm("r")->expr[1] == "$F");
        NEAR(xf2->x, 40); NEAR(xf2->y, 120);
        CHECK(out2->inputs[0] == xf2);    // wire inside the copied set is kept
        CHECK(xf2->inputs[0] == nullptr); // wire to a node outside the set is not
        CHECK(xf->inputs[0] == box && out->inputs[0] == xf); // originals untouched
    }
    // merging a whole scene keeps its wiring and leaves the display node alone
    Graph other;
    buildDemoScene(other, "sculpt");
    size_t before = g.nodes().size();
    Node* disp = g.displayNode();
    made = g.addNodesFromJson(other.toJson().get("nodes"), 500, 0);
    CHECK(made.size() == other.nodes().size() && g.nodes().size() == before + made.size());
    CHECK(g.displayNode() == disp);
    int wired = 0;
    for (Node* n : made) for (Node* in : n->inputs) wired += in != nullptr;
    int expect = 0;
    for (auto& n : other.nodes()) for (Node* in : n->inputs) expect += in != nullptr;
    CHECK(wired == expect);
    for (Node* n : made) { g.cook(n); CHECK(n->error().empty()); }
}

TEST(subnetworks) {
    Graph g;
    Node* box = g.createNode("box");
    Node* sn = g.createNode("subnet");
    CHECK(sn && sn->subnet() && sn->subnet()->parent() == &g && sn->subnet()->owner() == sn);
    if (!sn || !sn->subnet()) return;
    g.connect(box, sn, 0);
    g.cook(sn);
    CHECK(sn->error().empty() && sn->geometry().numPoints() == 8); // default: pass-through
    // edit inside: add a transform between Input and Output
    Graph* sub = sn->subnet();
    Node* in = sub->findNode("input1");
    Node* out = sub->outputNode();
    CHECK(in && out && out->type().name == "output");
    Node* xf = sub->createNode("xform");
    sub->connect(in, xf, 0);
    sub->connect(xf, out, 0);
    xf->parm("t")->val[1] = 5;
    g.cook(sn); // the outer node notices the inner change
    Vec3 mn, mx;
    sn->geometry().bounds(mn, mx);
    NEAR(mn.y, 4.5f);
    // a change outside reaches the inside
    box->parm("size")->val[0] = 4; box->touch();
    g.cook(sn);
    sn->geometry().bounds(mn, mx);
    NEAR(mx.x, 2);
    // nothing changed -> nothing recooks
    uint64_t c0 = g.totalCooks() + sub->totalCooks();
    g.cook(sn);
    CHECK(g.totalCooks() + sub->totalCooks() == c0);
    // animation inside makes the subnet time dependent
    CHECK(!sn->isTimeDependent());
    xf->parm("t")->expr[0] = "$F";
    xf->touch();
    CHECK(sn->isTimeDependent());
    g.frame = 7; g.cook(sn);
    sn->geometry().bounds(mn, mx);
    NEAR(mn.x, 5); // 7 - 2
    // unconnected input and error reporting
    Node* in2 = sub->createNode("input");
    in2->parm("index")->val[0] = 3;
    Node* m = sub->createNode("merge");
    sub->connect(xf, m, 0); sub->connect(in2, m, 1); sub->connect(m, out, 0);
    g.cook(sn);
    CHECK(sn->error().empty() && sn->geometry().numPoints() == 8);
    Node* bad = sub->createNode("attribwrangle");
    bad->parm("snippet")->str = "@P.y = ;";
    sub->connect(m, bad, 0); sub->connect(bad, out, 0);
    g.cook(sn);
    CHECK(!sn->error().empty());
    sub->connect(m, out, 0);
    g.cook(sn);
    CHECK(sn->error().empty());
    // save / load keeps the contents
    Graph g2;
    CHECK(g2.fromJson(Json::parse(g.toJson().dump())));
    Node* sn2 = g2.findNode(sn->name());
    CHECK(sn2 && sn2->subnet() && sn2->subnet()->nodes().size() == sub->nodes().size());
    if (sn2 && sn2->subnet()) {
        CHECK(sn2->subnet()->parent() == &g2 && sn2->subnet()->owner() == sn2);
        g2.frame = 7; g2.cook(sn2);
        Vec3 a, b; sn2->geometry().bounds(a, b);
        NEAR(a.x, 5); NEAR(a.y, 4.5f);
        CHECK(g2.toJson().dump() == g.toJson().dump());
    }
    // nested subnets
    Node* inner = sub->createNode("subnet");
    sub->connect(m, inner, 0); sub->connect(inner, out, 0);
    Node* pk = inner->subnet()->createNode("xform");
    pk->parm("scale")->val[0] = 2;
    inner->subnet()->connect(inner->subnet()->findNode("input1"), pk, 0);
    inner->subnet()->connect(pk, inner->subnet()->outputNode(), 0);
    g.frame = 1; g.cook(sn);
    sn->geometry().bounds(mn, mx);
    CHECK(sn->error().empty());
    NEAR(mx.x - mn.x, 8); // 4 wide, doubled
    // a simulation inside a subnet sees an animated outer emitter frame by frame
    Graph g3;
    Node* e = g3.createNode("sphere");
    for (int c = 0; c < 3; ++c) e->parm("rad")->val[c] = 0.01f;
    e->parm("t")->expr[0] = "$F";
    Node* s3 = g3.createNode("subnet");
    g3.connect(e, s3, 0);
    Graph* sub3 = s3->subnet();
    Node* pop = sub3->createNode("popsolver");
    for (const char* z : {"speedn", "velvar", "drag"}) pop->parm(z)->val[0] = 0;
    pop->parm("gravity")->val[1] = 0; pop->parm("life")->val[0] = 100; pop->parm("rate")->val[0] = 24;
    sub3->connect(sub3->findNode("input1"), pop, 0);
    sub3->connect(pop, sub3->outputNode(), 0);
    g3.frame = 30; g3.cook(s3);
    s3->geometry().bounds(mn, mx);
    CHECK(s3->error().empty() && s3->geometry().numPoints() == 30);
    CHECK(mn.x < 1.1f && mx.x > 29.9f);
    g3.frame = 31; g3.cook(s3); // continues from the cache instead of restarting
    CHECK(s3->geometry().numPoints() == 31 && pop->simCacheStart == 1);
}

TEST(collapse_to_subnet) {
    Graph g;
    buildDemoScene(g, "terrain");
    Node* disp = g.displayNode();
    g.cook(disp);
    const size_t pts = disp->geometry().numPoints(), prims = disp->geometry().numPrims();
    // rock branch: sphere -> mountain -> color, feeding copy_rocks
    std::vector<Node*> sel = {g.findNode("rock"), g.findNode("rock_noise"), g.findNode("rock_color")};
    std::string err;
    Node* sn = g.collapseToSubnet(sel, &err);
    CHECK(sn != nullptr);
    if (!sn) { std::printf("  %s\n", err.c_str()); return; }
    CHECK(!g.findNode("rock") && sn->subnet()->findNode("rock") && sn->subnet()->nodes().size() == 4);
    CHECK(g.findNode("copy_rocks")->inputs[0] == sn);
    g.cook(g.displayNode());
    CHECK(g.displayNode()->error().empty());
    CHECK(g.displayNode()->geometry().numPoints() == pts && g.displayNode()->geometry().numPrims() == prims);
    // a middle section with an outside input: mountain1 + height_ramp (reads ground, feeds scatter and merge)
    sel = {g.findNode("mountain1"), g.findNode("height_ramp")};
    Node* sn2 = g.collapseToSubnet(sel, &err);
    CHECK(sn2 != nullptr);
    if (sn2) {
        CHECK(sn2->inputs[0] == g.findNode("ground"));
        CHECK(g.findNode("scatter1")->inputs[0] == sn2 && g.findNode("merge1")->inputs[0] == sn2);
        g.cook(g.displayNode());
        CHECK(g.displayNode()->error().empty() && g.displayNode()->geometry().numPoints() == pts);
    }
    // two different outputs -> refused, graph untouched
    size_t count = g.nodes().size();
    sel = {g.findNode("ground"), g.findNode("scatter1")};
    CHECK(g.collapseToSubnet(sel, &err) == nullptr && !err.empty());
    CHECK(g.nodes().size() == count);
    // collapsing the displayed node keeps the display on the subnet
    sel = {g.findNode("merge1"), g.findNode("OUT")};
    Node* sn3 = g.collapseToSubnet(sel, &err);
    CHECK(sn3 && g.displayNode() == sn3);
    if (sn3) { g.cook(sn3); CHECK(sn3->error().empty() && sn3->geometry().numPoints() == pts); }
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
        if (t.name == "input") continue; // only meaningful inside a subnet (covered by the subnet tests)
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

TEST(volumes_and_pyro) {
    // voxelizer: a unit box fills ~1 unit^3
    Graph g;
    Node* box = g.createNode("box");
    g.cook(box);
    std::vector<unsigned char> mask;
    const float dx = 0.05f;
    size_t n = voxelizeGeometry(box->geometry(), 30, 30, 30, Vec3(-0.75f), dx, 0.1f, mask);
    CHECK(std::fabs(double(n) * dx * dx * dx - 1.0) < 0.05);
    // fog volume from a sphere: volume ~ 4/3 pi
    Node* sph = g.createNode("sphere");
    sph->parm("rows")->val[0] = 40; sph->parm("cols")->val[0] = 80;
    Node* fog = g.createNode("fogvolume");
    g.connect(sph, fog, 0);
    g.cook(fog);
    CHECK(fog->error().empty() && fog->geometry().volumes.size() == 1);
    if (fog->geometry().volumes.size() == 1) {
        const Volume& v = fog->geometry().volumes[0];
        double sum = 0;
        for (float d : *v.density) sum += d;
        CHECK(std::fabs(sum * v.dx * v.dx * v.dx - 4.18879) < 0.15);
        Vec3 mn, mx; fog->geometry().bounds(mn, mx);
        CHECK(mn.x < -1 && mx.x > 1);
        // transform moves and scales the volume
        Node* xf = g.createNode("xform");
        g.connect(fog, xf, 0);
        xf->parm("t")->val[0] = 5; xf->parm("scale")->val[0] = 2;
        g.cook(xf);
        xf->geometry().bounds(mn, mx);
        CHECK(mn.x > 2.5f && mx.x > 7 && std::fabs(xf->geometry().volumes[0].dx - v.dx * 2) < 1e-5f);
    }
    // smoke: rises, stays finite and bounded, avoids the collider, caches frames
    Node* src = g.createNode("sphere");
    for (int c = 0; c < 3; ++c) src->parm("rad")->val[c] = 0.3f;
    src->parm("t")->val[1] = 0.4f;
    Node* col = g.createNode("box");
    col->parm("t")->val[1] = 1.6f;
    for (int c = 0; c < 3; ++c) col->parm("size")->val[c] = 0.6f;
    Node* pyro = g.createNode("pyrosolver");
    pyro->parm("voxelsize")->val[0] = 0.12f;
    g.connect(src, pyro, 0);
    g.connect(col, pyro, 1);
    auto stats = [&](double& sum, double& comY, bool& ok, double& inSolid) {
        const Volume& v = pyro->geometry().volumes[0];
        sum = comY = inSolid = 0; ok = true;
        for (int k = 0; k < v.nz; ++k)
            for (int j = 0; j < v.ny; ++j)
                for (int i = 0; i < v.nx; ++i) {
                    float d = (*v.density)[size_t(i) + size_t(v.nx) * (size_t(j) + size_t(v.ny) * size_t(k))];
                    if (!(d >= 0.f && d <= 1.001f)) ok = false;
                    Vec3 p = v.origin + Vec3(i + 0.5f, j + 0.5f, k + 0.5f) * v.dx;
                    sum += d; comY += double(d) * p.y;
                    if (std::fabs(p.x) < 0.2f && std::fabs(p.z) < 0.2f && std::fabs(p.y - 1.6f) < 0.2f) inSolid += d;
                }
        comY /= std::max(sum, 1e-9);
    };
    double s1, y1, s2, y2, solid; bool ok;
    g.frame = 4; g.cook(pyro);
    CHECK(pyro->error().empty() && pyro->geometry().volumes.size() == 1);
    if (pyro->geometry().volumes.size() != 1) return;
    stats(s1, y1, ok, solid);
    CHECK(ok && s1 > 0);
    auto ptr4 = pyro->geometry().volumes[0].density;
    g.frame = 48; g.cook(pyro);
    stats(s2, y2, ok, solid);
    CHECK(ok);
    CHECK(s2 > s1 && y2 > y1 + 0.3);
    CHECK(solid == 0);
    CHECK(pyro->simCacheStart == 1 && pyro->simCacheEnd == 48);
    g.frame = 4; g.cook(pyro);
    CHECK(pyro->geometry().volumes[0].density == ptr4); // served from cache
    CHECK(pyro->geometry().volumes[0].heat != nullptr);
    // before the start frame: nothing; bad domain: error
    pyro->parm("startframe")->val[0] = 10; pyro->touch(); g.cook(pyro);
    CHECK(pyro->geometry().volumes.empty() && pyro->error().empty());
    pyro->parm("startframe")->val[0] = 1; pyro->parm("voxelsize")->val[0] = 0.001f; pyro->touch(); g.cook(pyro);
    CHECK(!pyro->error().empty());
    // merge keeps volumes alongside polygons
    pyro->parm("voxelsize")->val[0] = 0.2f; pyro->touch();
    Node* m = g.createNode("merge");
    g.connect(pyro, m, 0); g.connect(col, m, 1);
    g.cook(m);
    CHECK(m->geometry().volumes.size() == 1 && m->geometry().numPrims() == 6);
    // parallelFor covers every index exactly once
    std::vector<int> hits(1000, 0);
    parallelFor(1000, [&](int a, int b) { for (int i = a; i < b; ++i) hits[size_t(i)]++; });
    bool once = true;
    for (int h : hits) once &= h == 1;
    CHECK(once);
}

TEST(render_and_material) {
    Graph g;
    Node* box = g.createNode("box");
    Node* mat = g.createNode("material");
    g.connect(box, mat, 0);
    mat->parm("group")->str = "@P.y > 0.4";
    mat->parm("emit")->val[0] = 2; mat->parm("metal")->val[0] = 1;
    mat->parm("setcolor")->val[0] = 1;
    g.cook(mat);
    CHECK(mat->error().empty());
    const Attrib* e = mat->geometry().findAttrib(AttribClass::Prim, "emit");
    const Attrib* r = mat->geometry().findAttrib(AttribClass::Prim, "rough");
    CHECK(e && r && mat->geometry().findAttrib(AttribClass::Prim, "Cd"));
    if (e && r) {
        int lit = 0;
        for (size_t i = 0; i < e->count(); ++i) lit += e->getF(i) > 0;
        CHECK(lit == 1);            // only the top face
        NEAR(r->getF(0), 0.5f);     // others keep the default
    }
    // settings (and camera) survive a save/load round trip
    g.render.samples = 7; g.render.sunElevation = 33; g.render.ground = true;
    g.render.hasCamera = true; g.render.camYaw = 10; g.render.camDist = 5;
    Graph g2;
    CHECK(g2.fromJson(Json::parse(g.toJson().dump())));
    CHECK(g2.render.samples == 7 && g2.render.ground && g2.render.hasCamera);
    NEAR(g2.render.sunElevation, 33); NEAR(g2.render.camYaw, 10); NEAR(g2.render.camDist, 5);
    // render a small image: finishes, is deterministic, shows the object against the backdrop
    RenderSettings rs;
    rs.width = 64; rs.height = 48; rs.samples = 4; rs.camDist = 4;
    PathTracer pt;
    CHECK(!pt.running());
    pt.start(mat->geometry(), rs);
    pt.wait();
    CHECK(pt.samplesDone() == 4 && !pt.running());
    std::vector<uint8_t> a, b; int w = 0, h = 0; uint64_t ver = 0;
    CHECK(pt.fetch(a, w, h, ver));
    CHECK(w == 64 && h == 48 && a.size() == 64u * 48u * 4u);
    CHECK(!pt.fetch(b, w, h, ver)); // nothing newer
    if (a.size() == 64u * 48u * 4u) {
        auto px = [&](int x, int y) { return int(a[(size_t(y) * 64 + size_t(x)) * 4]) + a[(size_t(y) * 64 + size_t(x)) * 4 + 1] + a[(size_t(y) * 64 + size_t(x)) * 4 + 2]; };
        CHECK(std::abs(px(32, 24) - px(2, 2)) > 30);
        CHECK(px(2, 2) > px(2, 45)); // backdrop is lighter at the top
        pt.start(mat->geometry(), rs);
        pt.wait();
        ver = 0;
        CHECK(pt.fetch(b, w, h, ver) && a == b);
        // PNG file
        std::string path = "hd_test_render.png", err;
        CHECK(writePng(path, a, w, h, &err));
        FILE* f = std::fopen(path.c_str(), "rb");
        CHECK(f != nullptr);
        if (f) {
            unsigned char sig[8] = {0};
            size_t got = std::fread(sig, 1, 8, f);
            std::fseek(f, 0, SEEK_END);
            long size = std::ftell(f);
            std::fclose(f);
            CHECK(got == 8 && sig[1] == 'P' && sig[2] == 'N' && sig[3] == 'G');
            CHECK(size > 64 * 48 * 3);
            std::remove(path.c_str());
        }
    }
    // stopping mid-render returns promptly and leaves the tracer reusable
    rs.samples = 100000;
    pt.start(mat->geometry(), rs);
    pt.stop();
    CHECK(!pt.running());
    // volumes and loose points render without issue
    Node* sph = g.createNode("sphere");
    Node* fog = g.createNode("fogvolume");
    fog->parm("voxelsize")->val[0] = 0.1f;
    g.connect(sph, fog, 0);
    Node* sc = g.createNode("scatter");
    g.connect(sph, sc, 0);
    Node* m = g.createNode("merge");
    g.connect(fog, m, 0); g.connect(sc, m, 1);
    g.cook(m);
    rs.samples = 2;
    pt.start(m->geometry(), rs);
    pt.wait();
    CHECK(pt.samplesDone() == 2);
}

TEST(lights_and_cameras) {
    Graph g;
    Node* box = g.createNode("box");
    Node* floor = g.createNode("grid");
    floor->parm("t")->val[1] = -0.5f;
    Node* m = g.createNode("merge");
    g.connect(box, m, 0); g.connect(floor, m, 1);
    g.cook(m);
    CHECK(collectLights(g, 1).empty() && collectCameras(g, 1).empty());
    // light nodes are part of the scene without being wired in
    Node* spot = g.createNode("spotlight");
    Vec3 p(3, 4, 3);
    for (int c = 0; c < 3; ++c) spot->parm("t")->val[c] = p[c];
    Vec3 r = aimRotation(p, Vec3(0));
    spot->parm("r")->val[0] = r.x; spot->parm("r")->val[1] = r.y;
    spot->parm("light_exposure")->val[0] = 1;
    g.cook(spot);
    CHECK(spot->error().empty() && spot->geometry().empty());
    auto ls = collectLights(g, 1);
    CHECK(ls.size() == 1 && ls[0].type == Light::Spot);
    if (ls.size() == 1) {
        NEAR(ls[0].intensity, 80); // 40 doubled by +1 exposure
        Vec3 want = normalize(Vec3(0) - p);
        CHECK(length(ls[0].dir - want) < 1e-4f); // aimed at the origin
    }
    spot->bypass = true;
    CHECK(collectLights(g, 1).empty()); // bypass = off
    spot->bypass = false;
    // animated light
    Node* pt = g.createNode("pointlight");
    pt->parm("t")->expr[0] = "$F";
    CHECK(collectLights(g, 12).size() == 2);
    NEAR(collectLights(g, 12)[1].pos.x, 12);
    // camera: lens and aim
    Node* cam = g.createNode("camera");
    for (int c = 0; c < 3; ++c) cam->parm("t")->val[c] = Vec3(0, 1, 6)[c];
    Vec3 cr = aimRotation(Vec3(0, 1, 6), Vec3(0));
    cam->parm("r")->val[0] = cr.x; cam->parm("r")->val[1] = cr.y;
    auto cams = collectCameras(g, 1);
    CHECK(cams.size() == 1);
    if (cams.size() == 1) {
        CHECK(length(cams[0].fwd - normalize(Vec3(0, -1, -6))) < 1e-4f);
        NEAR(cams[0].fovYDegrees(), 2 * std::atan(41.4214 / 100.0 / (1280.0 / 720.0)) * 180 / 3.14159265);
    }
    // rendering: a spot light lights the box but not a far corner; no lights at all = default sun and sky
    RenderSettings rs;
    rs.width = 64; rs.height = 48; rs.samples = 8;
    rs.useEye = true; rs.camEye = Vec3(0, 1, 6); rs.camTarget = Vec3(0, 0, 0); rs.camFov = 40;
    auto render = [&](const std::vector<Light>& lights) {
        PathTracer ptr;
        ptr.start(m->geometry(), rs, lights);
        ptr.wait();
        std::vector<uint8_t> img; int w, h; uint64_t v = 0;
        ptr.fetch(img, w, h, v);
        return img;
    };
    auto lum = [](const std::vector<uint8_t>& img, int x, int y) { size_t i = (size_t(y) * 64 + size_t(x)) * 4; return int(img[i]) + img[i + 1] + img[i + 2]; };
    std::vector<Light> one = {collectLights(g, 1)[0]};
    one[0].coneAngle = 25; one[0].coneDelta = 5;
    auto lit = render(one);
    auto def = render({});
    CHECK(lit.size() == 64u * 48u * 4u && def.size() == lit.size());
    if (lit.size() == 64u * 48u * 4u) {
        CHECK(lum(lit, 32, 24) > 60);                       // the box, inside the cone
        CHECK(lum(lit, 32, 24) > lum(lit, 3, 36) + 40);     // floor far outside the cone is dark (no sky either)
        CHECK(lum(def, 3, 36) > lum(lit, 3, 36) + 40);      // the default sky lights that same floor
    }
    // an environment light alone lights everything softly
    Light env; env.type = Light::Env; env.sky = false; env.intensity = 1;
    auto amb = render({env});
    if (amb.size() == lit.size()) CHECK(lum(amb, 3, 36) > 60);
    // the active camera name is saved with the scene
    g.render.cameraName = cam->name();
    Graph g2;
    CHECK(g2.fromJson(Json::parse(g.toJson().dump())));
    CHECK(g2.render.cameraName == cam->name() && collectLights(g2, 1).size() == 2 && collectCameras(g2, 1).size() == 1);
}

TEST(keyframes) {
    Graph g;
    Node* box = g.createNode("box");
    Node* xf = g.createNode("xform");
    g.connect(box, xf, 0);
    Parm* t = xf->parm("t");
    CHECK(!t->anyKeys() && !xf->isTimeDependent());
    t->setKey(1, 10, 0);
    CHECK(t->animated(1) && !t->animated(0));
    CHECK(!xf->isTimeDependent()); // a single key does not move
    t->setKey(1, 20, 4);
    t->setKey(1, 30, 0);
    CHECK(xf->isTimeDependent() && !t->isDefault());
    // passes through the keys, holds outside them
    NEAR(t->evalKeys(1, 10), 0); NEAR(t->evalKeys(1, 20), 4); NEAR(t->evalKeys(1, 30), 0);
    NEAR(t->evalKeys(1, 1), 0); NEAR(t->evalKeys(1, 99), 0);
    // smooth: eases (slow near the keys), symmetric, never overshoots
    float q = t->evalKeys(1, 12.5f), h = t->evalKeys(1, 15);
    CHECK(q > 0 && q < 1.0f); // less than the 1.0 a straight line gives
    NEAR(h, 2);
    bool bounded = true;
    for (float f = 1; f <= 40; f += 0.25f) { float v = t->evalKeys(1, f); bounded &= v >= -1e-4f && v <= 4.0001f; }
    CHECK(bounded);
    // rising run of keys stays monotone
    Parm* s = xf->parm("scale");
    s->setKey(0, 1, 0); s->setKey(0, 2, 0.1f); s->setKey(0, 20, 10); s->setKey(0, 21, 10.1f);
    bool mono = true; float prev = -1;
    for (float f = 1; f <= 21; f += 0.1f) { float v = s->evalKeys(0, f); mono &= v >= prev - 1e-4f; prev = v; }
    CHECK(mono);
    s->keys[0].clear(); s->val[0] = 1;
    // linear and constant
    t->keys[1][0].interp = 1;
    NEAR(t->evalKeys(1, 12.5f), 1);
    t->keys[1][0].interp = 2;
    NEAR(t->evalKeys(1, 19.9f), 0);
    NEAR(t->evalKeys(1, 20), 4);
    t->keys[1][0].interp = 0;
    // replacing a key on the same frame, keys stay sorted, new keys inherit the curve type
    t->setKey(1, 20, 6);
    CHECK(t->keys[1].size() == 3);
    NEAR(t->evalKeys(1, 20), 6);
    t->setKey(1, 5, 1);
    CHECK(t->keys[1].size() == 4 && t->keys[1][0].frame == 5 && t->keyIndexAt(1, 20) == 2);
    // setValue keys the frame on an animated component and just sets the value otherwise
    t->setValue(1, 9, 25);
    CHECK(t->keyIndexAt(1, 25) >= 0);
    t->setValue(0, 7, 25);
    CHECK(!t->animated(0));
    NEAR(t->val[0], 7);
    // the node cooks differently per frame
    xf->touch();
    Vec3 mn, mx;
    g.frame = 20; g.cook(xf); xf->geometry().bounds(mn, mx); NEAR(mn.y, 5.5f);
    g.frame = 30; g.cook(xf); xf->geometry().bounds(mn, mx); NEAR(mn.y, -0.5f);
    // save / load
    Graph g2;
    CHECK(g2.fromJson(Json::parse(g.toJson().dump())));
    Parm* t2 = g2.findNode("xform1")->parm("t");
    CHECK(t2->keys[1].size() == t->keys[1].size());
    for (float f : {5.f, 12.f, 20.f, 27.f}) NEAR(t2->evalKeys(1, f), t->evalKeys(1, f));
    CHECK(g2.toJson().dump() == g.toJson().dump());
    // removing keys; the last one leaves its value in the parameter
    CHECK(t->removeKey(1, 25) && !t->removeKey(1, 26));
    while (!t->keys[1].empty()) t->removeKey(1, t->keys[1].back().frame);
    CHECK(!t->animated(1));
    NEAR(t->val[1], 1);
    // an expression wins over keys; lights and cameras animate through the same path
    Node* pl = g.createNode("pointlight");
    pl->parm("light_intensity")->setKey(0, 1, 0);
    pl->parm("light_intensity")->setKey(0, 11, 10, 1);
    pl->parm("light_intensity")->keys[0][0].interp = 1;
    NEAR(collectLights(g, 6)[0].intensity, 5);
    pl->parm("light_intensity")->expr[0] = "3";
    NEAR(collectLights(g, 6)[0].intensity, 3);
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
        CHECK(g.displayNode() && !g.displayNode()->geometry().empty());
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
