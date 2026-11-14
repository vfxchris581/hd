#include "Demo.h"

namespace hd {

const std::vector<DemoInfo>& demoScenes() {
    static const std::vector<DemoInfo> d = {
        {"terrain", "Rocky Terrain", "Noise-displaced grid with a height color ramp and rocks copied onto scattered points."},
        {"ripples", "Animated Ripples", "Attribute Wrangle drives animated ripples and color. Press Play."},
        {"city", "Procedural City", "Boxes copied to grid points with random per-point scale and color."},
        {"sculpt", "Subdivided Sculpt", "Box -> PolyExtrude -> Catmull-Clark subdivision -> Mountain."},
        {"molecule", "Wire Molecule", "Torus edges turned into tubes with spheres on every point."},
    };
    return d;
}

namespace {
void setF(Node* n, const char* p, float v) { n->parm(p)->val[0] = v; }
void setV(Node* n, const char* p, Vec3 v) { auto* q = n->parm(p); q->val[0] = v.x; q->val[1] = v.y; q->val[2] = v.z; }
void setS(Node* n, const char* p, const std::string& s) { n->parm(p)->str = s; }
} // namespace

static bool buildDemoSceneImpl(Graph& g, const std::string& name) {
    g.clear();
    if (name == "terrain") {
        Node* grid = g.createNode("grid", 0, 0, "ground");
        setF(grid, "sizex", 12); setF(grid, "sizey", 12); setF(grid, "rows", 90); setF(grid, "cols", 90);
        Node* mtn = g.createNode("mountain", 0, 70, "mountain1");
        setF(mtn, "height", 1.4f); setF(mtn, "elementsize", 3.5f); setF(mtn, "octaves", 6);
        g.connect(grid, mtn, 0);
        Node* col = g.createNode("color", 0, 140, "height_ramp");
        setF(col, "mode", 2); setV(col, "color", Vec3(0.92f, 0.9f, 0.86f)); setV(col, "color2", Vec3(0.18f, 0.32f, 0.14f));
        g.connect(mtn, col, 0);
        Node* sc = g.createNode("scatter", 220, 210, "scatter1");
        setF(sc, "npts", 160); setF(sc, "pscale", 0.18f); setF(sc, "pscalevar", 0.7f); setF(sc, "seed", 3);
        g.connect(col, sc, 0);
        Node* rock = g.createNode("sphere", 420, 0, "rock");
        setF(rock, "rows", 8); setF(rock, "cols", 12);
        Node* rmtn = g.createNode("mountain", 420, 70, "rock_noise");
        setF(rmtn, "height", 0.35f); setF(rmtn, "elementsize", 0.6f);
        g.connect(rock, rmtn, 0);
        Node* rcol = g.createNode("color", 420, 140, "rock_color");
        setV(rcol, "color", Vec3(0.45f, 0.42f, 0.4f));
        g.connect(rmtn, rcol, 0);
        Node* cp = g.createNode("copytopoints", 330, 290, "copy_rocks");
        setF(cp, "useCd", 0);
        g.connect(rcol, cp, 0);
        g.connect(sc, cp, 1);
        Node* m = g.createNode("merge", 120, 370, "merge1");
        g.connect(col, m, 0);
        g.connect(cp, m, 1);
        Node* out = g.createNode("null", 120, 440, "OUT");
        g.connect(m, out, 0);
        g.setDisplayNode(out);
        return true;
    }
    if (name == "ripples") {
        Node* grid = g.createNode("grid", 0, 0, "grid1");
        setF(grid, "sizex", 10); setF(grid, "sizey", 10); setF(grid, "rows", 120); setF(grid, "cols", 120);
        Node* w = g.createNode("attribwrangle", 0, 80, "ripples");
        setS(w, "snippet",
             "// Concentric ripples from two moving sources\n"
             "vector c1 = set(sin(@Time) * 2, 0, cos(@Time * 0.7) * 2);\n"
             "vector c2 = set(-2, 0, 1.5);\n"
             "float d1 = distance(@P, c1);\n"
             "float d2 = distance(@P, c2);\n"
             "float h = sin(d1 * 5 - @Time * 6) * exp(-d1 * 0.35)\n"
             "        + sin(d2 * 4 - @Time * 5) * exp(-d2 * 0.4);\n"
             "@P.y = h * 0.35;\n"
             "@Cd = hsvtorgb(set(fit(h, -1, 1, 0.62, 0.48), 0.75, fit(h, -1, 1, 0.45, 1)));\n");
        g.connect(grid, w, 0);
        Node* n = g.createNode("normal", 0, 160, "normal1");
        g.connect(w, n, 0);
        Node* out = g.createNode("null", 0, 230, "OUT");
        g.connect(n, out, 0);
        g.setDisplayNode(out);
        g.frameEnd = 240;
        return true;
    }
    if (name == "city") {
        Node* grid = g.createNode("grid", 0, 0, "blocks");
        setF(grid, "sizex", 14); setF(grid, "sizey", 14); setF(grid, "rows", 16); setF(grid, "cols", 16);
        Node* w = g.createNode("attribwrangle", 0, 80, "building_attrs");
        setS(w, "snippet",
             "// Random building footprint, height and color per point\n"
             "float r = rand(@ptnum * 1.37);\n"
             "float centre = 1 - clamp(length(@P) / 8, 0, 1);\n"
             "float h = 0.3 + pow(r, 3) * 4 * centre + centre * 1.2;\n"
             "@scale = set(0.62, h, 0.62);\n"
             "@Cd = hsvtorgb(set(0.58 + rand(@ptnum + 7) * 0.08, 0.25, 0.55 + r * 0.4));\n");
        g.connect(grid, w, 0);
        Node* box = g.createNode("box", 250, 0, "building");
        setV(box, "t", Vec3(0, 0.5f, 0));
        Node* cp = g.createNode("copytopoints", 120, 160, "copy_buildings");
        setF(cp, "useN", 0);
        g.connect(box, cp, 0);
        g.connect(w, cp, 1);
        Node* ground = g.createNode("grid", -230, 160, "ground");
        setF(ground, "sizex", 16); setF(ground, "sizey", 16); setF(ground, "rows", 2); setF(ground, "cols", 2);
        Node* gcol = g.createNode("color", -230, 230, "asphalt");
        setV(gcol, "color", Vec3(0.2f, 0.21f, 0.23f));
        g.connect(ground, gcol, 0);
        Node* m = g.createNode("merge", 0, 300, "merge1");
        g.connect(gcol, m, 0);
        g.connect(cp, m, 1);
        Node* out = g.createNode("null", 0, 370, "OUT");
        g.connect(m, out, 0);
        g.setDisplayNode(out);
        return true;
    }
    if (name == "sculpt") {
        Node* box = g.createNode("box", 0, 0, "box1");
        Node* ex = g.createNode("polyextrude", 0, 70, "extrude_top");
        setS(ex, "group", "@P.y > 0.4"); setF(ex, "dist", 0.8f);
        g.connect(box, ex, 0);
        Node* ex2 = g.createNode("polyextrude", 0, 140, "extrude_sides");
        setS(ex2, "group", "abs(@P.x) > 0.45 && @P.y < 0.6"); setF(ex2, "dist", 0.5f);
        g.connect(ex, ex2, 0);
        Node* sub = g.createNode("subdivide", 0, 210, "subdivide1");
        setF(sub, "iterations", 3);
        g.connect(ex2, sub, 0);
        Node* mtn = g.createNode("mountain", 0, 280, "detail");
        setF(mtn, "height", 0.06f); setF(mtn, "elementsize", 0.25f);
        g.connect(sub, mtn, 0);
        Node* col = g.createNode("color", 0, 350, "clay");
        setV(col, "color", Vec3(0.78f, 0.55f, 0.42f));
        g.connect(mtn, col, 0);
        g.setDisplayNode(col);
        return true;
    }
    if (name == "molecule") {
        Node* tor = g.createNode("torus", 0, 0, "torus1");
        setF(tor, "rows", 16); setF(tor, "cols", 8);
        Node* wire = g.createNode("polywire", -120, 90, "bonds");
        setF(wire, "radius", 0.025f);
        g.connect(tor, wire, 0);
        Node* sph = g.createNode("sphere", 200, 0, "atom");
        setF(sph, "rows", 10); setF(sph, "cols", 16); setV(sph, "rad", Vec3(0.07f));
        Node* col = g.createNode("color", 120, 90, "random_color");
        setF(col, "class", 0); setF(col, "mode", 1); setF(col, "seed", 4);
        g.connect(tor, col, 0);
        Node* cp = g.createNode("copytopoints", 60, 180, "atoms");
        setF(cp, "useN", 0);
        g.connect(sph, cp, 0);
        g.connect(col, cp, 1);
        Node* m = g.createNode("merge", -40, 260, "merge1");
        g.connect(wire, m, 0);
        g.connect(cp, m, 1);
        Node* xf = g.createNode("xform", -40, 330, "spin");
        xf->parm("r")->expr[1] = "$F * 1.5";
        setV(xf, "r", Vec3(25, 0, 0));
        g.connect(m, xf, 0);
        g.setDisplayNode(xf);
        return true;
    }
    return false;
}

bool buildDemoScene(Graph& g, const std::string& name) {
    if (!buildDemoSceneImpl(g, name)) return false;
    for (auto& n : g.nodes()) { n->x *= 1.4f; n->y *= 1.15f; } // room for wide node labels
    return true;
}

} // namespace hd
