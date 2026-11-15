// Main menu bar (laid out like Houdini's: File, Edit, Render, Assets, Windows, Help),
// Display Options, preferences, and the node clipboard.
#include "App.h"
#include "core/Demo.h"
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace hd {

namespace {
std::string prefsPath() {
    const char* home = std::getenv("HOME");
    if (!home) home = std::getenv("USERPROFILE");
    std::error_code ec;
    fs::path dir = home ? fs::path(home) : fs::temp_directory_path(ec);
    return (dir / ".hd_prefs.json").string();
}
Json vec(const Vec3& v) {
    Json a = Json::array();
    a.push(double(v.x)); a.push(double(v.y)); a.push(double(v.z));
    return a;
}
Vec3 toVec(const Json& j, const Vec3& def) {
    return j.size() == 3 ? Vec3(float(j[0].num()), float(j[1].num()), float(j[2].num())) : def;
}
// A greyed-out entry for something HD does not have yet.
void unavailable(const char* label) {
    ImGui::MenuItem(label, nullptr, false, false);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Not available in HD yet");
}
} // namespace

// ---------------------------------------------------------------- preferences
void App::loadPrefs() {
    std::ifstream f(prefsPath(), std::ios::binary);
    if (!f) return;
    std::stringstream ss;
    ss << f.rdbuf();
    Json j = Json::parse(ss.str());
    if (!j.isObject()) return;
    recent_.clear();
    for (auto& r : j.get("recent").items())
        if (!r.str().empty() && recent_.size() < 10) recent_.push_back(r.str());
    const Json& d = j.get("display");
    if (d.isObject()) {
        ViewOptions def;
        ViewOptions& o = viewOpts;
        o.shaded = d.get("shaded").boolean(def.shaded);
        o.wireframe = d.get("wireframe").boolean(def.wireframe);
        o.points = d.get("points").boolean(def.points);
        o.normals = d.get("normals").boolean(def.normals);
        o.backfaceTint = d.get("backfaceTint").boolean(def.backfaceTint);
        o.pointSize = float(d.get("pointSize").num(def.pointSize));
        o.grid = d.get("grid").boolean(def.grid);
        o.gridPlane = std::clamp(int(d.get("gridPlane").num(def.gridPlane)), 0, 2);
        o.gridOrigin = toVec(d.get("gridOrigin"), def.gridOrigin);
        o.gridSpacing = float(d.get("gridSpacing").num(def.gridSpacing));
        o.gridCells = int(d.get("gridCells").num(def.gridCells));
        o.gridRuler = int(d.get("gridRuler").num(def.gridRuler));
        o.gridOpacity = float(d.get("gridOpacity").num(def.gridOpacity));
        o.bgTop = toVec(d.get("bgTop"), def.bgTop);
        o.bgBottom = toVec(d.get("bgBottom"), def.bgBottom);
        showPointNumbers = d.get("pointNumbers").boolean(false);
        showPrimNumbers = d.get("primNumbers").boolean(false);
    }
}

void App::savePrefs() {
    Json j = Json::object();
    Json rec = Json::array();
    for (auto& r : recent_) rec.push(r);
    j["recent"] = rec;
    const ViewOptions& o = viewOpts;
    Json d = Json::object();
    d["shaded"] = o.shaded; d["wireframe"] = o.wireframe; d["points"] = o.points; d["normals"] = o.normals;
    d["backfaceTint"] = o.backfaceTint; d["pointSize"] = double(o.pointSize);
    d["grid"] = o.grid; d["gridPlane"] = o.gridPlane; d["gridOrigin"] = vec(o.gridOrigin);
    d["gridSpacing"] = double(o.gridSpacing); d["gridCells"] = o.gridCells; d["gridRuler"] = o.gridRuler;
    d["gridOpacity"] = double(o.gridOpacity);
    d["bgTop"] = vec(o.bgTop); d["bgBottom"] = vec(o.bgBottom);
    d["pointNumbers"] = showPointNumbers; d["primNumbers"] = showPrimNumbers;
    j["display"] = d;
    std::ofstream f(prefsPath(), std::ios::binary);
    if (f) f << j.dump(2) << "\n";
}

void App::pushRecent(const std::string& path) {
    std::error_code ec;
    std::string abs = fs::absolute(fs::path(path), ec).string();
    if (ec || abs.empty()) abs = path;
    recent_.erase(std::remove(recent_.begin(), recent_.end(), abs), recent_.end());
    recent_.insert(recent_.begin(), abs);
    if (recent_.size() > 10) recent_.resize(10);
    savePrefs();
}

// ---------------------------------------------------------------- clipboard / file actions
void App::copySelected() {
    std::vector<const Node*> sel;
    for (int id : selection)
        if (Node* n = net().findNode(id)) sel.push_back(n);
    if (sel.empty()) return;
    Json j = Json::object();
    j["format"] = "hd-nodes";
    j["nodes"] = net().nodesToJson(sel);
    clipboard_ = j.dump(0);
    ImGui::SetClipboardText(clipboard_.c_str()); // lets nodes be pasted into another HD window
    status("Copied " + std::to_string(sel.size()) + (sel.size() == 1 ? " node" : " nodes"));
}

void App::cutSelected() {
    if (selection.empty()) return;
    copySelected();
    deleteSelected();
}

void App::pasteNodes() {
    std::string text = clipboard_;
    if (const char* sys = ImGui::GetClipboardText())
        if (std::string(sys).find("\"hd-nodes\"") != std::string::npos) text = sys;
    Json j = Json::parse(text);
    if (!j.isObject() || j.get("format").str() != "hd-nodes" || j.get("nodes").size() == 0) { status("Nothing to paste", true); return; }
    checkpoint();
    std::vector<Node*> made = net().addNodesFromJson(j.get("nodes"), 40, 40);
    selection.clear();
    for (Node* n : made) selection.insert(n->id());
    currentId = made.empty() ? -1 : made.back()->id();
    status("Pasted " + std::to_string(made.size()) + (made.size() == 1 ? " node" : " nodes"));
}

void App::mergeScene(const std::string& path) {
    Graph other;
    std::string err;
    if (!other.load(path, &err)) { status("Merge failed: " + err, true); return; }
    checkpoint();
    // place the merged network to the right of what is already here
    float right = 0, top = 0, left = 1e9f;
    bool any = false;
    for (auto& n : net().nodes()) { right = any ? std::max(right, n->x) : n->x; top = any ? std::min(top, n->y) : n->y; any = true; }
    for (auto& n : other.nodes()) left = std::min(left, n->x);
    float dx = any ? right + 300 - (left < 1e8f ? left : 0) : 0;
    float otherTop = 1e9f;
    for (auto& n : other.nodes()) otherTop = std::min(otherTop, n->y);
    float dy = any && otherTop < 1e8f ? top - otherTop : 0;
    std::vector<Node*> made = net().addNodesFromJson(other.toJson().get("nodes"), dx, dy);
    selection.clear();
    for (Node* n : made) selection.insert(n->id());
    currentId = made.empty() ? -1 : made.back()->id();
    network.requestFrameAll();
    status("Merged " + std::to_string(made.size()) + " nodes from " + fs::path(path).filename().string());
}

void App::saveIncrement() {
    if (currentFile_.empty()) { openFileDialog(FileDialogMode::SaveAs); return; }
    fs::path p(currentFile_);
    std::string stem = p.stem().string();
    size_t end = stem.size(), start = end;
    while (start > 0 && std::isdigit((unsigned char)stem[start - 1])) --start;
    std::error_code ec;
    fs::path next;
    for (int tries = 0; tries < 10000; ++tries) {
        if (start < end) {
            unsigned long long n = std::strtoull(stem.substr(start).c_str(), nullptr, 10) + 1;
            char buf[32];
            std::snprintf(buf, sizeof buf, "%0*llu", int(end - start), n);
            stem = stem.substr(0, start) + buf;
            end = stem.size();
            start = end;
            while (start > 0 && std::isdigit((unsigned char)stem[start - 1])) --start;
        } else {
            stem += "_2";
            end = stem.size();
            start = end - 1;
        }
        next = p.parent_path() / (stem + p.extension().string());
        if (!fs::exists(next, ec)) break; // never overwrite an existing version
    }
    saveScene(next.string());
}

void App::openExample(const std::string& name) {
    for (auto& d : demoScenes()) {
        if (d.name != name) continue;
        checkpoint();
        buildDemoScene(graph, d.name);
        clearSelection();
        if (Node* n = net().displayNode()) select(n);
        currentFile_.clear();
        frameOnNextCook_ = true;
        network.requestFrameAll();
        status(d.description);
    }
}

// ---------------------------------------------------------------- menu bar
void App::drawMenuBar() {
    if (!ImGui::BeginMainMenuBar()) return;
    const bool hasSel = !selection.empty();

    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New", "Ctrl+N")) { checkpoint(); newScene(false); }
        if (ImGui::MenuItem("Open...", "Ctrl+O")) openFileDialog(FileDialogMode::Open);
        if (ImGui::BeginMenu("Open Recent File", !recent_.empty())) {
            std::string pick;
            for (auto& r : recent_) {
                if (ImGui::MenuItem(fs::path(r).filename().string().c_str())) pick = r;
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", r.c_str());
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Clear List")) { recent_.clear(); savePrefs(); }
            ImGui::EndMenu();
            if (!pick.empty() && !loadScene(pick)) { // file is gone: drop it from the list
                recent_.erase(std::remove(recent_.begin(), recent_.end(), pick), recent_.end());
                savePrefs();
            }
        }
        if (ImGui::BeginMenu("Open Example")) {
            for (auto& d : demoScenes()) {
                if (ImGui::MenuItem(d.title.c_str())) openExample(d.name);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", d.description.c_str());
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Save", "Ctrl+S")) {
            if (currentFile_.empty()) openFileDialog(FileDialogMode::SaveAs);
            else saveScene(currentFile_);
        }
        if (ImGui::MenuItem("Save As...", "Ctrl+Shift+S")) openFileDialog(FileDialogMode::SaveAs);
        if (ImGui::MenuItem("Save and Increment")) saveIncrement();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Save under the next version number (scene_01 -> scene_02), keeping the old file");
        if (ImGui::MenuItem("Revert", nullptr, false, !currentFile_.empty())) loadScene(std::string(currentFile_));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Reload the scene from disk, discarding unsaved changes");
        ImGui::Separator();
        if (ImGui::MenuItem("Merge...")) openFileDialog(FileDialogMode::MergeScene);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Add the nodes of another scene file to this one");
        if (ImGui::BeginMenu("Import")) {
            if (ImGui::MenuItem("Geometry (.obj)...")) openFileDialog(FileDialogMode::ImportObj);
            unavailable("Alembic Scene...");
            unavailable("Filmbox FBX...");
            unavailable("USD...");
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Export")) {
            if (ImGui::MenuItem("Geometry (.obj / .ply)...", nullptr, false, net().displayNode() != nullptr)) openFileDialog(FileDialogMode::ExportObj);
            unavailable("Alembic Scene...");
            unavailable("Filmbox FBX...");
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Quit", "Ctrl+Q")) glfwSetWindowShouldClose(window_, 1);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Edit")) {
        if (ImGui::MenuItem("Undo", "Ctrl+Z", false, canUndo())) undo();
        if (ImGui::MenuItem("Redo", "Ctrl+Shift+Z", false, canRedo())) redo();
        ImGui::Separator();
        if (ImGui::MenuItem("Cut", "Ctrl+X", false, hasSel)) cutSelected();
        if (ImGui::MenuItem("Copy", "Ctrl+C", false, hasSel)) copySelected();
        if (ImGui::MenuItem("Paste", "Ctrl+V")) pasteNodes();
        if (ImGui::MenuItem("Delete", "Del", false, hasSel)) deleteSelected();
        if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, hasSel)) duplicateSelected();
        ImGui::Separator();
        if (ImGui::MenuItem("Select All", "Ctrl+A")) {
            selection.clear();
            for (auto& n : net().nodes()) selection.insert(n->id());
        }
        if (ImGui::MenuItem("Select None", nullptr, false, hasSel)) clearSelection();
        ImGui::Separator();
        if (ImGui::MenuItem("Collapse Selected Into Subnet", "Shift+C", false, hasSel)) collapseSelected();
        ImGui::Separator();
        if (ImGui::BeginMenu("Preferences")) {
            if (ImGui::MenuItem("3D Viewports...", "D")) showDisplayOptions = true;
            if (ImGui::MenuItem("Rendering...")) showRenderSettings = true;
            unavailable("General User Interface...");
            unavailable("Network Editor, Nodes and Trees...");
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Hotkeys...")) showShortcuts = true;
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Render")) {
        const bool busy = rendering();
        if (ImGui::MenuItem("Render Current Frame", nullptr, false, !busy)) { showRenderView = true; ImGui::SetWindowFocus("Render View"); startRender(); }
        if (ImGui::MenuItem("Render Sequence...", nullptr, false, !busy)) { showRenderView = true; ImGui::SetWindowFocus("Render View"); openFileDialog(FileDialogMode::SaveSequence); }
        if (ImGui::MenuItem("Stop Render", nullptr, false, busy)) {
            tracer_.stop();
            if (seqActive_) { seqActive_ = false; status("Sequence stopped after " + std::to_string(seqCount_) + " frames"); }
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Save Rendered Image...", nullptr, false, !renderImg_.empty())) openFileDialog(FileDialogMode::SaveImage);
        if (ImGui::MenuItem("Render Settings...")) showRenderSettings = true;
        ImGui::Separator();
        unavailable("Create Render Node");
        unavailable("Edit Render Node");
        unavailable("Pre-Flight Scene...");
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Assets")) {
        if (ImGui::MenuItem("New Digital Asset From Selection", nullptr, false, hasSel)) {
            Node* c = current();
            if (!(selection.size() == 1 && c && c->subnet())) { collapseSelected(); c = current(); } // wrap the selection first
            if (c && c->subnet()) saveAsset(c);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Saves the selected subnet (or collapses the selected nodes into one) as a reusable asset");
        if (ImGui::MenuItem("Install Asset Library...")) openFileDialog(FileDialogMode::InstallAsset);
        ImGui::MenuItem("Asset Manager...", nullptr, &showAssetManager);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Windows")) {
        if (ImGui::BeginMenu("Desktop")) {
            if (ImGui::MenuItem("Build (Default Layout)")) {
                showShelf = showSceneView = showNetwork = showParms = showSpreadsheet = showRenderView = showTimeline = true;
                layoutBuilt_ = false;
                forceLayout_ = true;
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        ImGui::MenuItem("Shelf", nullptr, &showShelf);
        ImGui::MenuItem("Scene View", nullptr, &showSceneView);
        ImGui::MenuItem("Render View", nullptr, &showRenderView);
        ImGui::MenuItem("Network View", nullptr, &showNetwork);
        ImGui::MenuItem("Parameters", nullptr, &showParms);
        ImGui::MenuItem("Geometry Spreadsheet", nullptr, &showSpreadsheet);
        ImGui::MenuItem("Playbar", nullptr, &showTimeline);
        ImGui::Separator();
        ImGui::MenuItem("Display Options", "D", &showDisplayOptions);
        ImGui::MenuItem("Render Settings", nullptr, &showRenderSettings);
        ImGui::Separator();
        unavailable("Python Shell");
        unavailable("Hscript Textport");
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Help")) {
        ImGui::MenuItem("Shortcuts", nullptr, &showShortcuts);
        ImGui::MenuItem("VEX-lite Reference", nullptr, &showVexHelp);
        ImGui::MenuItem("UI Toolkit Demo", nullptr, &showImGuiDemo);
        ImGui::Separator();
        ImGui::MenuItem("About HD", nullptr, &showAbout);
        ImGui::EndMenu();
    }

    char info[128];
    std::snprintf(info, sizeof info, "%s   %.0f fps", currentFile_.empty() ? "untitled" : fs::path(currentFile_).filename().string().c_str(),
                  double(ImGui::GetIO().Framerate));
    float w = ImGui::CalcTextSize(info).x;
    ImGui::SameLine(ImGui::GetWindowWidth() - w - 16);
    ImGui::TextDisabled("%s", info);
    ImGui::EndMainMenuBar();
}

// ---------------------------------------------------------------- display options (D)
void App::drawDisplayOptions() {
    if (!showDisplayOptions) return;
    ImGui::SetNextWindowSize(ImVec2(430, 360), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Display Options", &showDisplayOptions, ImGuiWindowFlags_NoDocking)) { ImGui::End(); return; }
    ViewOptions& o = viewOpts;
    bool ch = false;
    if (ImGui::BeginTabBar("##dopts")) {
        if (ImGui::BeginTabItem("Grid")) {
            ImGui::SeparatorText("Reference Plane (Ground)");
            ch |= ImGui::Checkbox("Display Reference Plane", &o.grid);
            ImGui::BeginDisabled(!o.grid);
            const char* planes[] = {"XZ Plane (floor)", "XY Plane", "YZ Plane"};
            ch |= ImGui::Combo("Plane", &o.gridPlane, planes, 3);
            ch |= ImGui::DragFloat3("Origin", &o.gridOrigin.x, 0.05f);
            ch |= ImGui::DragFloat("Grid Spacing", &o.gridSpacing, 0.01f, 0.001f, 1000.f, "%.3f");
            ch |= ImGui::DragInt("Grid Size (cells)", &o.gridCells, 1, 2, 400);
            ch |= ImGui::DragInt("Ruler Every", &o.gridRuler, 0.1f, 1, 100, "%d cells");
            ch |= ImGui::SliderFloat("Opacity", &o.gridOpacity, 0.05f, 1.f);
            o.gridSpacing = std::max(o.gridSpacing, 0.001f);
            o.gridCells = std::clamp(o.gridCells, 2, 400);
            o.gridRuler = std::max(o.gridRuler, 1);
            ImGui::EndDisabled();
            ImGui::TextDisabled("The reference plane is a guide only. For a floor that renders,\nadd a Grid node, or turn on Ground Plane in Render Settings.");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Markers")) {
            ch |= ImGui::Checkbox("Points", &o.points);
            ch |= ImGui::SliderFloat("Point Size", &o.pointSize, 1.f, 16.f, "%.0f px");
            ch |= ImGui::Checkbox("Point Numbers", &showPointNumbers);
            ch |= ImGui::Checkbox("Primitive Numbers", &showPrimNumbers);
            ch |= ImGui::Checkbox("Point Normals", &o.normals);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Geometry")) {
            ch |= ImGui::Checkbox("Smooth Shaded", &o.shaded);
            ch |= ImGui::Checkbox("Wireframe Over Shaded", &o.wireframe);
            ch |= ImGui::Checkbox("Tint Backfaces", &o.backfaceTint);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Background")) {
            ch |= ImGui::ColorEdit3("Top Color", &o.bgTop.x);
            ch |= ImGui::ColorEdit3("Bottom Color", &o.bgBottom.x);
            const ViewOptions def;
            if (ImGui::Button("Dark (default)")) { o.bgTop = def.bgTop; o.bgBottom = def.bgBottom; ch = true; }
            ImGui::SameLine();
            if (ImGui::Button("Light")) { o.bgTop = Vec3(0.78f, 0.80f, 0.84f); o.bgBottom = Vec3(0.55f, 0.57f, 0.60f); ch = true; }
            ImGui::SameLine();
            if (ImGui::Button("Black")) { o.bgTop = o.bgBottom = Vec3(0.f); ch = true; }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::Separator();
    if (ImGui::Button("Reset All to Defaults")) {
        viewOpts = ViewOptions();
        showPointNumbers = showPrimNumbers = false;
        ch = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Saved automatically");
    // write once an edit is finished, not on every drag step
    static bool pending = false;
    pending |= ch;
    if (pending && !ImGui::IsAnyItemActive()) { savePrefs(); pending = false; }
    ImGui::End();
}

} // namespace hd
