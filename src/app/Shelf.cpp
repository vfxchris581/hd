// Shelf (tool tabs across the top, like Houdini's), light and camera tools,
// viewport icons for lights and cameras, and looking through a camera.
#include "App.h"
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <iterator>

namespace hd {

namespace {
struct Tool { const char* label; const char* id; const char* tip; };

void line(std::vector<float>& v, const Vec3& a, const Vec3& b, const Vec3& c) {
    for (const Vec3* p : {&a, &b}) { v.push_back(p->x); v.push_back(p->y); v.push_back(p->z); v.push_back(c.x); v.push_back(c.y); v.push_back(c.z); }
}
void circle(std::vector<float>& v, const Vec3& c, const Vec3& ux, const Vec3& uy, float r, const Vec3& col, int seg = 20) {
    for (int i = 0; i < seg; ++i) {
        float a0 = 2 * kPi * float(i) / float(seg), a1 = 2 * kPi * float(i + 1) / float(seg);
        line(v, c + (ux * std::cos(a0) + uy * std::sin(a0)) * r, c + (ux * std::cos(a1) + uy * std::sin(a1)) * r, col);
    }
}
void axes(const Vec3& dir, Vec3& right, Vec3& up) {
    right = normalize(cross(dir, Vec3(0, 1, 0)));
    if (length(right) < 0.5f) right = Vec3(1, 0, 0);
    up = cross(right, dir);
}
} // namespace

Node* App::activeCameraNode() {
    if (graph.render.cameraName.empty()) return nullptr;
    Node* n = graph.findNode(graph.render.cameraName);
    return n && n->type().name == "camera" ? n : nullptr;
}

// ---------------------------------------------------------------- look through / lock
void App::updateSceneCamera(bool navigated, float viewAspect) {
    Node* cn = activeCameraNode();
    if (!cn) {
        graph.render.cameraName.clear();
        activeCamAspect = 0;
        camEditActive_ = false;
        return;
    }
    if (!navigated) { // the camera node drives the view
        if (!ImGui::IsAnyMouseDown()) camEditActive_ = false;
        for (auto& c : collectCameras(graph, graph.frame)) {
            if (c.name != cn->name()) continue;
            float camAspect = float(c.resx) / float(c.resy);
            activeCamAspect = camAspect;
            float tanx = c.aperture * 0.5f / c.focal;
            // keep the whole camera frame visible whatever the pane's shape
            camera.fovY = 2.f * std::atan(tanx / std::min(viewAspect, camAspect)) * 180.f / kPi;
            camera.target = c.eye + c.fwd * camera.dist;
            Vec3 d = -c.fwd;
            camera.pitch = std::asin(std::clamp(d.y, -1.f, 1.f)) * 180.f / kPi;
            camera.yaw = std::atan2(d.x, d.z) * 180.f / kPi;
        }
        return;
    }
    if (!lockCamera) { // moving the view leaves the camera
        graph.render.cameraName.clear();
        activeCamAspect = 0;
        camera.fovY = Camera().fovY;
        return;
    }
    // locked: the view moves the camera node
    if (!camEditActive_) { checkpoint(); camEditActive_ = true; }
    Vec3 eye = camera.eye();
    Vec3 r = aimRotation(eye, camera.target);
    Parm* pt = cn->parm("t");
    Parm* pr = cn->parm("r");
    const float fr = std::floor(graph.frame + 0.5f); // on an animated camera this keys the current frame
    pt->setValue(0, eye.x, fr); pt->setValue(1, eye.y, fr); pt->setValue(2, eye.z, fr);
    pr->setValue(0, r.x, fr); pr->setValue(1, r.y, fr);
    cn->touch();
    markDirty();
}

// ---------------------------------------------------------------- viewport icons
void App::updateGuides() {
    std::vector<float> v;
    const Node* cur = netPath_.empty() ? current() : nullptr;
    auto colorFor = [&](const std::string& name, const Vec3& base) { return cur && cur->name() == name ? Vec3(1, 1, 1) : base; };
    const Vec3 lightCol(1.f, 0.85f, 0.2f), camCol(0.5f, 0.78f, 1.f);
    for (auto& l : collectLights(graph, graph.frame)) {
        Vec3 c = colorFor(l.name, lightCol);
        const Vec3 X(1, 0, 0), Y(0, 1, 0), Z(0, 0, 1);
        switch (l.type) {
        case Light::Point: {
            const float s = 0.28f;
            for (const Vec3& d : {X, Y, Z, normalize(Vec3(1, 1, 0)), normalize(Vec3(1, -1, 0)), normalize(Vec3(0, 1, 1)), normalize(Vec3(0, 1, -1)), normalize(Vec3(1, 0, 1)), normalize(Vec3(1, 0, -1))})
                line(v, l.pos - d * s, l.pos + d * s, c);
            break;
        }
        case Light::Spot: {
            Vec3 rt, up;
            axes(l.dir, rt, up);
            const float len = 1.1f, rad = std::min(1.6f, std::tan(radians(std::min(160.f, l.coneAngle) * 0.5f)) * len);
            Vec3 base = l.pos + l.dir * len;
            circle(v, base, rt, up, rad, c);
            for (int i = 0; i < 4; ++i) {
                float a = kPi * 0.5f * float(i);
                line(v, l.pos, base + (rt * std::cos(a) + up * std::sin(a)) * rad, c);
            }
            break;
        }
        case Light::Area: {
            Vec3 hx = l.ux * (l.sizeX * 0.5f), hy = l.uy * (l.sizeY * 0.5f);
            Vec3 p[4] = {l.pos - hx - hy, l.pos + hx - hy, l.pos + hx + hy, l.pos - hx + hy};
            for (int i = 0; i < 4; ++i) line(v, p[i], p[(i + 1) % 4], c);
            line(v, p[0], p[2], c);
            line(v, l.pos, l.pos + l.dir * 0.6f, c);
            break;
        }
        case Light::Distant: {
            Vec3 rt, up;
            axes(l.dir, rt, up);
            circle(v, l.pos, rt, up, 0.25f, c);
            for (const Vec3& o : {Vec3(0), rt * 0.18f, rt * -0.18f, up * 0.18f, up * -0.18f}) {
                Vec3 a = l.pos + o, b = a + l.dir * 0.9f;
                line(v, a, b, c);
                line(v, b, b - l.dir * 0.12f + rt * 0.05f, c);
                line(v, b, b - l.dir * 0.12f - rt * 0.05f, c);
            }
            break;
        }
        case Light::Env:
            circle(v, l.pos, X, Y, 0.3f, c);
            circle(v, l.pos, X, Z, 0.3f, c);
            circle(v, l.pos, Y, Z, 0.3f, c);
            break;
        }
    }
    for (auto& cam : collectCameras(graph, graph.frame)) {
        if (cam.name == graph.render.cameraName) continue; // we are looking through it
        Vec3 c = colorFor(cam.name, camCol);
        Vec3 rt, up;
        axes(cam.fwd, rt, up);
        const float d = 0.8f, hw = cam.aperture * 0.5f / cam.focal * d, hh = hw * float(cam.resy) / float(cam.resx);
        Vec3 ctr = cam.eye + cam.fwd * d;
        Vec3 p[4] = {ctr - rt * hw - up * hh, ctr + rt * hw - up * hh, ctr + rt * hw + up * hh, ctr - rt * hw + up * hh};
        for (int i = 0; i < 4; ++i) { line(v, cam.eye, p[i], c); line(v, p[i], p[(i + 1) % 4], c); }
        Vec3 tip = ctr + up * (hh + hw * 0.35f); // "this way up" marker
        line(v, p[3] + rt * (hw * 0.6f), tip, c);
        line(v, p[2] - rt * (hw * 0.6f), tip, c);
    }
    renderer.setGuides(v);
}

// ---------------------------------------------------------------- shelf tools
void App::shelfTool(const std::string& tool) {
    const bool sceneLevel = tool == "camera" || tool == "skylight" || tool == "ambientlight" || isLightType(tool);
    checkpoint();
    if (sceneLevel) {
        // lights and cameras belong to the scene, so they always go in the top-level network
        if (!netPath_.empty()) goToLevel(0);
        // aim and size them from what is displayed
        Vec3 mn(-1), mx(1);
        if (Node* d = graph.displayNode()) { graph.cook(d); if (!d->geometry().empty()) d->geometry().bounds(mn, mx); }
        const Vec3 ctr = (mn + mx) * 0.5f;
        const float R = std::max(1.f, length(mx - mn) * 0.5f);
        float left = 0, top = 0;
        int stacked = 0;
        bool any = false;
        for (auto& n : graph.nodes()) {
            left = any ? std::min(left, n->x) : n->x; top = any ? std::min(top, n->y) : n->y; any = true;
            if (n->type().category == "Lights and Cameras") ++stacked;
        }
        auto make = [&](const char* type) {
            Node* n = graph.createNode(type, left - 280, top + float(stacked++) * 70.f);
            return n;
        };
        auto setV = [](Node* n, const char* p, const Vec3& val) { Parm* q = n->parm(p); q->val[0] = val.x; q->val[1] = val.y; q->val[2] = val.z; };
        // local lights fall off with distance squared, so start them bright enough for their distance
        auto forDistance = [](float dist, float level) { return level * dist * dist; };
        Node* made = nullptr;
        if (tool == "camera") {
            made = make("camera");
            Vec3 eye = camera.eye();
            setV(made, "t", eye);
            setV(made, "r", aimRotation(eye, camera.target));
            const float aspect = 1280.f / 720.f, tanx = std::tan(radians(camera.fovY) * 0.5f) * aspect;
            made->parm("focal")->val[0] = made->parm("aperture")->val[0] * 0.5f / tanx;
            graph.render.cameraName = made->name(); // look through the new camera
        } else if (tool == "pointlight") {
            made = make("pointlight");
            setV(made, "t", ctr + Vec3(0.8f, 1.2f, 0.8f) * R);
            made->parm("light_intensity")->val[0] = forDistance(length(Vec3(0.8f, 1.2f, 0.8f)) * R, 1.2f);
        } else if (tool == "spotlight") {
            made = make("spotlight");
            Vec3 p = ctr + Vec3(1.2f, 1.6f, 1.2f) * R;
            setV(made, "t", p);
            setV(made, "r", aimRotation(p, ctr));
            made->parm("light_intensity")->val[0] = forDistance(length(Vec3(1.2f, 1.6f, 1.2f)) * R, 2.4f);
        } else if (tool == "arealight") {
            made = make("arealight");
            Vec3 p = ctr + Vec3(0, 1.8f, 0) * R;
            setV(made, "t", p);
            setV(made, "r", Vec3(-90, 0, 0));
            setV(made, "areasize", Vec3(R, R, 0));
            made->parm("light_intensity")->val[0] = forDistance(1.8f * R, 2.2f);
        } else if (tool == "distantlight" || tool == "skylight") {
            if (tool == "skylight") { Node* e = make("envlight"); setV(e, "t", ctr + Vec3(-1.2f, 1.6f, 0) * R); }
            made = make("distantlight");
            setV(made, "t", ctr + Vec3(0, 1.9f, 0) * R);
            setV(made, "r", Vec3(-50, 40, 0));
        } else if (tool == "envlight" || tool == "ambientlight") {
            made = make("envlight");
            setV(made, "t", ctr + Vec3(-1.2f, 1.6f, 0) * R);
            if (tool == "ambientlight") { made->parm("env_mode")->val[0] = 1; made->parm("light_intensity")->val[0] = 0.4f; }
        }
        if (made) {
            select(made);
            status(tool == "camera" ? "Created " + made->name() + " from the current view. Turn on Lock to move it with the view."
                                    : "Created " + made->name() + ". Lights replace the default sun and sky in the Render View.");
        }
        return;
    }
    // ordinary node tools: continue from the selected node when the tool takes an input
    const NodeType* t = findNodeType(tool);
    if (!t) return;
    Node* cur = current();
    float x = 0, y = 0;
    int from = -1;
    if (cur && t->maxInputs > 0) { x = cur->x; y = cur->y + 80; from = cur->id(); }
    else {
        bool any = false;
        for (auto& n : net().nodes()) { x = any ? std::max(x, n->x + 240) : n->x + 240; y = any ? std::min(y, n->y) : n->y; any = true; }
    }
    Node* n = createNode(tool, x, y, from);
    if (n && t->maxInputs == 0) setDisplay(n);
}

void App::drawShelf() {
    ImGuiWindowFlags fl = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 4));
    bool open = ImGui::Begin("Shelf", nullptr, fl);
    ImGui::PopStyleVar();
    if (!open) { ImGui::End(); return; }
    static const Tool create[] = {{"Box", "box", "Box"}, {"Sphere", "sphere", "Sphere"}, {"Tube", "tube", "Tube or cone"}, {"Torus", "torus", "Torus"},
                                  {"Grid", "grid", "Flat grid (a floor)"}, {"Line", "line", "Line"}, {"Circle", "circle", "Circle"}, {"File", "file", "Load an .obj file"}};
    static const Tool modify[] = {{"Transform", "xform", "Move, rotate, scale"}, {"Subdivide", "subdivide", "Smooth by subdividing"}, {"PolyExtrude", "polyextrude", "Extrude faces"},
                                  {"Mountain", "mountain", "Noise displacement"}, {"Smooth", "smooth", "Relax points"}, {"Scatter", "scatter", "Scatter points on a surface"},
                                  {"Copy to Points", "copytopoints", "Copy geometry onto points"}, {"Color", "color", "Set color"}, {"Material", "material", "Roughness, metallic, glow"},
                                  {"Merge", "merge", "Combine geometry"}, {"Wrangle", "attribwrangle", "Run VEX-lite code"}};
    static const Tool lights[] = {{"Camera", "camera", "New camera from the current view"}, {"Point Light", "pointlight", "Light in all directions from a point"},
                                  {"Spot Light", "spotlight", "Cone of light aimed at the scene"}, {"Area Light", "arealight", "Soft light from a rectangle above the scene"},
                                  {"Distant Light", "distantlight", "Sun-like parallel light"}, {"Environment Light", "envlight", "Light from a sky all around"},
                                  {"Sky Light", "skylight", "Sun plus sky"}, {"Ambient Light", "ambientlight", "Even light from everywhere"}};
    static const char* lightsUnavailable[] = {"Geometry Light", "Volume Light", "GI Light", "Caustic Light", "Portal Light", "Stereo Camera", "VR Camera", "Switcher"};
    static const Tool particles[] = {{"POP Solver", "popsolver", "Particles emitted from the selected node"}, {"Trail", "trail", "Streaks behind particles"}};
    static const Tool pyro[] = {{"Pyro Solver", "pyrosolver", "Smoke / fire from the selected node"}, {"Fog Volume", "fogvolume", "Fill the selected mesh with fog"}};

    auto tools = [&](const Tool* list, size_t n) {
        for (size_t i = 0; i < n; ++i) {
            if (i) ImGui::SameLine();
            if (ImGui::Button(list[i].label)) shelfTool(list[i].id);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", list[i].tip);
        }
    };
    if (ImGui::BeginTabBar("##shelf")) {
        if (ImGui::BeginTabItem("Create")) { tools(create, std::size(create)); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Modify")) { tools(modify, std::size(modify)); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Lights and Cameras")) {
            tools(lights, std::size(lights));
            ImGui::BeginDisabled();
            for (const char* u : lightsUnavailable) { ImGui::SameLine(); ImGui::Button(u); }
            ImGui::EndDisabled();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Particles")) { tools(particles, std::size(particles)); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Pyro FX")) { tools(pyro, std::size(pyro)); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

} // namespace hd
