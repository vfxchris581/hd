#include "App.h"
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <cmath>
#include <cstdint>
#include <cstdio>

namespace hd {

namespace {
bool toggleButton(const char* label, bool* v, const char* tip) {
    ImVec4 on(0.26f, 0.46f, 0.78f, 1.f);
    if (*v) ImGui::PushStyleColor(ImGuiCol_Button, on);
    bool pressed = ImGui::SmallButton(label);
    if (*v) ImGui::PopStyleColor();
    if (pressed) *v = !*v;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
    return pressed;
}
int g_vpDrag = 0; // 0 none, 1 tumble, 2 pan, 3 dolly
} // namespace

// ---------------------------------------------------------------- viewport
void App::drawViewport() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    bool open = ImGui::Begin("Scene View", nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    if (!open) { ImGui::End(); return; }

    // toolbar
    ImGui::SetCursorPos(ImVec2(6, 4));
    toggleButton("Shaded", &viewOpts.shaded, "Smooth shaded surfaces");
    ImGui::SameLine(); toggleButton("Wire", &viewOpts.wireframe, "Wireframe over shaded (W)");
    ImGui::SameLine(); toggleButton("Points", &viewOpts.points, "Display points");
    ImGui::SameLine(); toggleButton("#Pt", &showPointNumbers, "Point numbers");
    ImGui::SameLine(); toggleButton("#Prim", &showPrimNumbers, "Primitive numbers");
    ImGui::SameLine(); toggleButton("N", &viewOpts.normals, "Point normals");
    ImGui::SameLine(); toggleButton("Grid", &viewOpts.grid, "Ground grid");
    ImGui::SameLine(); toggleButton("Backface", &viewOpts.backfaceTint, "Tint back-facing polygons blue");
    ImGui::SameLine(); ImGui::TextDisabled("|");
    ImGui::SameLine(); if (ImGui::SmallButton("Frame")) frameGeometry();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Frame geometry (F)");
    ImGui::SameLine(); if (ImGui::SmallButton("Home")) camera.home();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Home camera (H)");
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 2);

    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (avail.x < 8 || avail.y < 8) { ImGui::End(); return; }
    ImVec2 scale = ImGui::GetIO().DisplayFramebufferScale;
    int w = int(avail.x * scale.x), h = int(avail.y * scale.y);
    viewportW_ = avail.x; viewportH_ = avail.y;
    unsigned tex = renderer.render(w, h, camera, viewOpts);
    ImVec2 imgPos = ImGui::GetCursorScreenPos();
    ImGui::Image((ImTextureID)(intptr_t)tex, avail, ImVec2(0, 1), ImVec2(1, 0));
    bool hovered = ImGui::IsItemHovered();
    ImGuiIO& io = ImGui::GetIO();

    // camera navigation (Houdini: LMB tumble, MMB pan, RMB dolly; Alt also works)
    if (hovered && g_vpDrag == 0) {
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) g_vpDrag = 1;
        else if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) g_vpDrag = 2;
        else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) g_vpDrag = 3;
        if (io.MouseWheel != 0) camera.dolly(io.MouseWheel * 0.12f);
        if (g_vpDrag) ImGui::SetWindowFocus();
    }
    if (g_vpDrag) {
        ImVec2 d = io.MouseDelta;
        if (g_vpDrag == 1) {
            if (io.KeyShift) camera.pan(d.x, d.y, avail.y);
            else camera.tumble(d.x, d.y);
        } else if (g_vpDrag == 2) camera.pan(d.x, d.y, avail.y);
        else if (g_vpDrag == 3) camera.dolly((d.x - d.y) * 0.01f);
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) && !ImGui::IsMouseDown(ImGuiMouseButton_Middle) && !ImGui::IsMouseDown(ImGuiMouseButton_Right))
            g_vpDrag = 0;
    }
    if ((hovered || ImGui::IsWindowFocused()) && !io.WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_F, false)) frameGeometry();
        if (ImGui::IsKeyPressed(ImGuiKey_H, false)) camera.home();
        if (ImGui::IsKeyPressed(ImGuiKey_W, false) && !io.KeyCtrl) viewOpts.wireframe = !viewOpts.wireframe;
    }

    // overlays
    ImDrawList* dl = ImGui::GetWindowDrawList();
    Node* d = graph.displayNode();
    ImVec2 tp(imgPos.x + 10, imgPos.y + 8);
    if (d) {
        const Geometry& g = d->geometry();
        char buf[256];
        std::snprintf(buf, sizeof buf, "%s  (%s)", d->name().c_str(), d->type().label.c_str());
        dl->AddText(tp, IM_COL32(200, 215, 255, 255), buf);
        std::snprintf(buf, sizeof buf, "%zu points  %zu primitives  %zu tris", g.numPoints(), g.numPrims(), renderer.stats().triangles);
        dl->AddText(ImVec2(tp.x, tp.y + ImGui::GetTextLineHeight()), IM_COL32(190, 190, 195, 220), buf);
        if (!d->error().empty()) {
            std::snprintf(buf, sizeof buf, "Error in %s: %s", d->name().c_str(), d->error().c_str());
            ImVec2 ts = ImGui::CalcTextSize(buf);
            ImVec2 a(imgPos.x + (avail.x - ts.x) * 0.5f - 10, imgPos.y + avail.y * 0.5f - 14);
            dl->AddRectFilled(a, ImVec2(a.x + ts.x + 20, a.y + ts.y + 12), IM_COL32(120, 20, 20, 220), 4);
            dl->AddText(ImVec2(a.x + 10, a.y + 6), IM_COL32(255, 220, 220, 255), buf);
        }
        // element numbers
        if (showPointNumbers || showPrimNumbers) {
            Mat4 VP = camera.proj(avail.x / avail.y) * camera.view();
            auto project = [&](const Vec3& p, ImVec2& out) {
                float x = VP.at(0, 0) * p.x + VP.at(0, 1) * p.y + VP.at(0, 2) * p.z + VP.at(0, 3);
                float y = VP.at(1, 0) * p.x + VP.at(1, 1) * p.y + VP.at(1, 2) * p.z + VP.at(1, 3);
                float ww = VP.at(3, 0) * p.x + VP.at(3, 1) * p.y + VP.at(3, 2) * p.z + VP.at(3, 3);
                if (ww <= 1e-5f) return false;
                out = ImVec2(imgPos.x + (x / ww * 0.5f + 0.5f) * avail.x, imgPos.y + (1 - (y / ww * 0.5f + 0.5f)) * avail.y);
                return out.x > imgPos.x && out.y > imgPos.y && out.x < imgPos.x + avail.x && out.y < imgPos.y + avail.y;
            };
            dl->PushClipRect(imgPos, ImVec2(imgPos.x + avail.x, imgPos.y + avail.y), true);
            char num[16];
            ImVec2 s;
            if (showPointNumbers)
                for (size_t i = 0; i < g.numPoints() && i < 4000; ++i)
                    if (project(g.P[i], s)) { std::snprintf(num, sizeof num, "%zu", i); dl->AddText(s, IM_COL32(255, 235, 120, 255), num); }
            if (showPrimNumbers)
                for (size_t i = 0; i < g.numPrims() && i < 4000; ++i)
                    if (project(g.primCenter(int(i)), s)) { std::snprintf(num, sizeof num, "%zu", i); dl->AddText(s, IM_COL32(140, 230, 255, 255), num); }
            dl->PopClipRect();
        }
    } else {
        dl->AddText(tp, IM_COL32(200, 200, 200, 255), "No display node — click a node's blue flag in the Network");
    }
    // axis gizmo (bottom-left)
    {
        Mat4 V = camera.view();
        ImVec2 c(imgPos.x + 40, imgPos.y + avail.y - 40);
        struct Ax { Vec3 d; ImU32 col; const char* n; } axes[3] = {
            {Vec3(1, 0, 0), IM_COL32(230, 80, 80, 255), "X"}, {Vec3(0, 1, 0), IM_COL32(110, 210, 90, 255), "Y"}, {Vec3(0, 0, 1), IM_COL32(90, 140, 255, 255), "Z"}};
        for (auto& a : axes) {
            Vec3 v = V.transformDir(a.d);
            ImVec2 e(c.x + v.x * 24, c.y - v.y * 24);
            dl->AddLine(c, e, a.col, 2.f);
            dl->AddText(ImVec2(e.x - 3, e.y - 7), a.col, a.n);
        }
    }
    ImGui::End();
}

// ---------------------------------------------------------------- spreadsheet
void App::drawSpreadsheet() {
    if (!ImGui::Begin("Geometry Spreadsheet")) { ImGui::End(); return; }
    Node* n = current();
    if (!n) n = graph.displayNode();
    if (!n) { ImGui::TextDisabled("No node selected."); ImGui::End(); return; }
    graph.cook(n);
    const Geometry& g = n->geometry();
    ImGui::Text("Node: %s", n->name().c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("(%zu points, %zu primitives)", g.numPoints(), g.numPrims());
    if (ImGui::BeginTabBar("##sheet")) {
        ImGuiTableFlags tf = ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                             ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingFixedFit;
        if (ImGui::BeginTabItem("Points")) {
            int cols = 4;
            for (auto& [k, a] : g.pointAttribs) cols += a.tuple;
            if (cols < 512 && ImGui::BeginTable("##pts", cols, tf)) {
                ImGui::TableSetupScrollFreeze(1, 1);
                ImGui::TableSetupColumn("#");
                ImGui::TableSetupColumn("P[x]"); ImGui::TableSetupColumn("P[y]"); ImGui::TableSetupColumn("P[z]");
                for (auto& [k, a] : g.pointAttribs) {
                    if (a.tuple == 1) ImGui::TableSetupColumn(k.c_str());
                    else for (int c = 0; c < a.tuple; ++c) ImGui::TableSetupColumn((k + "[" + "xyz"[c] + "]").c_str());
                }
                ImGui::TableHeadersRow();
                ImGuiListClipper clip;
                clip.Begin(int(g.numPoints()));
                while (clip.Step()) {
                    for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn(); ImGui::TextDisabled("%d", i);
                        for (int c = 0; c < 3; ++c) { ImGui::TableNextColumn(); ImGui::Text("%.4f", double(g.P[size_t(i)][c])); }
                        for (auto& [k, a] : g.pointAttribs)
                            for (int c = 0; c < a.tuple; ++c) { ImGui::TableNextColumn(); ImGui::Text("%.4f", double(a.data[size_t(i) * size_t(a.tuple) + size_t(c)])); }
                    }
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Primitives")) {
            int cols = 3;
            for (auto& [k, a] : g.primAttribs) cols += a.tuple;
            if (cols < 512 && ImGui::BeginTable("##prims", cols, tf)) {
                ImGui::TableSetupScrollFreeze(1, 1);
                ImGui::TableSetupColumn("#");
                ImGui::TableSetupColumn("Type");
                ImGui::TableSetupColumn("Points");
                for (auto& [k, a] : g.primAttribs) {
                    if (a.tuple == 1) ImGui::TableSetupColumn(k.c_str());
                    else for (int c = 0; c < a.tuple; ++c) ImGui::TableSetupColumn((k + "[" + "xyz"[c] + "]").c_str());
                }
                ImGui::TableHeadersRow();
                ImGuiListClipper clip;
                clip.Begin(int(g.numPrims()));
                while (clip.Step()) {
                    for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
                        const Prim& pr = g.prims[size_t(i)];
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn(); ImGui::TextDisabled("%d", i);
                        ImGui::TableNextColumn(); ImGui::TextUnformatted(pr.closed ? "Polygon" : "Polyline");
                        ImGui::TableNextColumn();
                        std::string pts;
                        for (int k = 0; k < pr.count && k < 12; ++k) pts += std::to_string(g.vertices[size_t(pr.start + k)]) + " ";
                        if (pr.count > 12) pts += "...";
                        ImGui::TextUnformatted(pts.c_str());
                        for (auto& [k, a] : g.primAttribs)
                            for (int c = 0; c < a.tuple; ++c) { ImGui::TableNextColumn(); ImGui::Text("%.4f", double(a.data[size_t(i) * size_t(a.tuple) + size_t(c)])); }
                    }
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

// ---------------------------------------------------------------- timeline
void App::drawTimeline() {
    if (!ImGui::Begin("Timeline", nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) { ImGui::End(); return; }
    if (ImGui::Button("|<")) graph.frame = float(graph.frameStart);
    ImGui::SameLine();
    if (ImGui::Button("<")) graph.frame = std::max(float(graph.frameStart), graph.frame - 1);
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, playing ? ImVec4(0.2f, 0.55f, 0.25f, 1) : ImGui::GetStyle().Colors[ImGuiCol_Button]);
    if (ImGui::Button(playing ? " Stop " : " Play ")) playing = !playing;
    ImGui::PopStyleColor();
    ImGui::SameLine();
    if (ImGui::Button(">")) graph.frame = std::min(float(graph.frameEnd), graph.frame + 1);
    ImGui::SameLine();
    if (ImGui::Button(">|")) graph.frame = float(graph.frameEnd);
    ImGui::SameLine();
    int f = int(graph.frame);
    float rightW = 470;
    ImGui::SetNextItemWidth(std::max(120.f, ImGui::GetContentRegionAvail().x - rightW));
    if (ImGui::SliderInt("##frame", &f, graph.frameStart, std::max(graph.frameStart + 1, graph.frameEnd), "Frame %d")) graph.frame = float(f);
    {
        // blue bar under the slider: frames cached by simulation nodes
        ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        float span = float(std::max(1, graph.frameEnd - graph.frameStart));
        for (auto& n : graph.nodes()) {
            if (n->simCacheEnd < n->simCacheStart) continue;
            float t0 = std::clamp(float(n->simCacheStart - graph.frameStart) / span, 0.f, 1.f);
            float t1 = std::clamp(float(n->simCacheEnd - graph.frameStart) / span, 0.f, 1.f);
            ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(a.x + (b.x - a.x) * t0, b.y - 3), ImVec2(a.x + (b.x - a.x) * t1 + 2, b.y), IM_COL32(70, 150, 255, 220));
        }
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(55);
    if (ImGui::DragInt("##start", &graph.frameStart, 1, -100000, graph.frameEnd - 1)) markDirty();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Start frame");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(55);
    if (ImGui::DragInt("##end", &graph.frameEnd, 1, graph.frameStart + 1, 1000000)) markDirty();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("End frame");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(60);
    if (ImGui::DragFloat("##fps", &graph.fps, 0.1f, 1, 240, "%.0f fps")) { graph.fps = std::max(1.f, graph.fps); markDirty(); }
    ImGui::SameLine();
    ImGui::Checkbox("Realtime", &realtime);
    ImGui::SameLine();
    // status line
    double age = glfwGetTime() - statusTime_;
    if (!statusMsg_.empty() && age < 8) {
        ImVec4 c = statusErr_ ? ImVec4(1, 0.45f, 0.45f, 1) : ImVec4(0.7f, 0.85f, 0.7f, 1);
        c.w = float(std::min(1.0, 8 - age));
        ImGui::TextColored(c, "%s", statusMsg_.c_str());
    } else {
        ImGui::TextDisabled("T = %.2fs", double((graph.frame - 1) / graph.fps));
    }
    ImGui::End();
}

} // namespace hd
