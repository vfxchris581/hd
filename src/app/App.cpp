#include "App.h"
#include "core/Demo.h"
#include "core/GeoIO.h"
#include "render/GL.h"
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>

namespace fs = std::filesystem;

namespace hd {

// ---------------------------------------------------------------- lifecycle
bool App::init(GLFWwindow* window, const std::string& startupFile) {
    window_ = window;
    std::string err;
    if (!renderer.init(err)) {
        status("Renderer failed: " + err, true);
        return false;
    }
    fileDir_ = fs::current_path().string();
    loadPrefs();
    refreshAssets();
    if (!startupFile.empty() && loadScene(startupFile)) {
        // loaded
    } else {
        newScene(true);
    }
    lastTime_ = glfwGetTime();
    return true;
}

void App::shutdown() {
    savePrefs();
    tracer_.stop();
    renderer.shutdown();
}

bool App::handleCloseRequest() {
    if (!dirty_ || allowClose_) return true;
    confirmClose_ = true;
    return false;
}

void App::openPath(const std::string& path) {
    std::string ext = fs::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    if (ext == ".obj") {
        checkpoint();
        Node* n = createNode("file", 0, 0);
        if (n) {
            n->parm("file")->str = path;
            n->touch();
            setDisplay(n);
            select(n);
            frameOnNextCook_ = true;
            status("Imported " + path);
        }
    } else {
        loadScene(path);
    }
}

// ---------------------------------------------------------------- selection & undo
void App::select(Node* n, bool additive) {
    if (!additive) selection.clear();
    if (n) {
        if (additive && selection.count(n->id())) {
            selection.erase(n->id());
            if (currentId == n->id()) currentId = selection.empty() ? -1 : *selection.begin();
            return;
        }
        selection.insert(n->id());
        currentId = n->id();
    } else if (!additive) {
        currentId = -1;
    }
}

Graph& App::net() {
    Graph* g = &graph;
    size_t i = 0;
    for (; i < netPath_.size(); ++i) {
        Node* s = g->findNode(netPath_[i]);
        if (!s || !s->subnet()) break;
        g = s->subnet();
    }
    if (i < netPath_.size()) { // the subnet we were in no longer exists (undo, delete)
        netPath_.resize(i);
        selection.clear();
        currentId = -1;
        network.requestFrameAll();
    }
    return *g;
}

void App::diveInto(Node* sn) {
    if (!sn || !sn->subnet()) return;
    netPath_.push_back(sn->name());
    clearSelection();
    if (Node* d = net().displayNode()) select(d);
    network.requestFrameAll();
}

void App::goToLevel(size_t depth) {
    if (depth >= netPath_.size()) return;
    std::string child = netPath_[depth];
    netPath_.resize(depth);
    clearSelection();
    if (Node* n = net().findNode(child)) select(n);
    network.requestFrameAll();
}

void App::collapseSelected() {
    std::vector<Node*> sel;
    for (int id : selection)
        if (Node* n = net().findNode(id)) sel.push_back(n);
    if (sel.empty()) { status("Select the nodes to collapse first", true); return; }
    checkpoint();
    std::string err;
    Node* sn = net().collapseToSubnet(sel, &err);
    if (!sn) { status("Cannot collapse: " + err, true); return; }
    select(sn);
    status("Collapsed " + std::to_string(sel.size()) + " nodes into " + sn->name() + " (double-click it to dive in)");
}

App::Snapshot App::snapshot() {
    Snapshot s;
    s.path = netPath_;
    s.graph = graph.toJson().dump(0);
    for (int id : selection)
        if (Node* n = net().findNode(id)) s.selected.push_back(n->name());
    if (Node* c = current()) s.current = c->name();
    return s;
}

void App::restore(const Snapshot& s) {
    float frame = graph.frame;
    graph.fromJson(Json::parse(s.graph));
    netPath_ = s.path;
    graph.frame = frame;
    selection.clear();
    currentId = -1;
    for (auto& name : s.selected)
        if (Node* n = net().findNode(name)) selection.insert(n->id());
    if (Node* c = net().findNode(s.current)) currentId = c->id();
}

void App::checkpoint() {
    Snapshot s = snapshot();
    dirty_ = true;
    redo_.clear();
    if (!undo_.empty() && undo_.back().graph == s.graph) return; // nothing changed since last checkpoint
    undo_.push_back(std::move(s));
    if (undo_.size() > 200) undo_.erase(undo_.begin());
}

void App::undo() {
    if (undo_.empty()) return;
    redo_.push_back(snapshot());
    restore(undo_.back());
    undo_.pop_back();
    dirty_ = true;
    status("Undo");
}

void App::redo() {
    if (redo_.empty()) return;
    undo_.push_back(snapshot());
    restore(redo_.back());
    redo_.pop_back();
    dirty_ = true;
    status("Redo");
}

// ---------------------------------------------------------------- actions
Node* App::createNode(const std::string& type, float x, float y, int connectFrom) {
    if (type.rfind("asset:", 0) == 0) return createAsset(type.substr(6), x, y, connectFrom);
    Node* n = net().createNode(type, x, y);
    if (!n) return nullptr;
    if (Node* src = net().findNode(connectFrom))
        if (!n->inputs.empty()) net().connect(src, n, 0);
    // Houdini behaviour: new nodes become the displayed node when they're downstream
    // of the display node, or when nothing is displayed.
    if (!net().displayNode() || (connectFrom >= 0 && net().displayNode() && net().displayNode()->id() == connectFrom))
        setDisplay(n);
    select(n);
    return n;
}

void App::deleteSelected() {
    if (selection.empty()) return;
    checkpoint();
    for (int id : std::vector<int>(selection.begin(), selection.end())) {
        Node* n = net().findNode(id);
        if (!n) continue;
        // reconnect around a single-input node (like Houdini's delete)
        Node* upstream = n->inputs.empty() ? nullptr : n->inputs[0];
        auto outs = net().outputsOf(n);
        bool wasDisplay = net().displayNode() == n;
        net().removeNode(n);
        if (upstream && net().findNode(upstream->id())) {
            for (auto& [dst, idx] : outs) net().connect(upstream, dst, idx);
            if (wasDisplay) net().setDisplayNode(upstream);
        }
    }
    clearSelection();
    if (!net().displayNode() && !net().nodes().empty()) net().setDisplayNode(net().nodes().back().get());
}

void App::duplicateSelected() {
    if (selection.empty()) return;
    checkpoint();
    std::set<int> newSel;
    for (int id : selection) {
        Node* n = net().findNode(id);
        if (!n) continue;
        Node* c = net().createNode(n->type().name, n->x + 40, n->y + 40, n->name());
        for (size_t i = 0; i < n->parms().size(); ++i) {
            Parm& d = c->parms()[i];
            const Parm& s = n->parms()[i];
            std::copy(s.val, s.val + 3, d.val);
            for (int k = 0; k < 3; ++k) { d.expr[k] = s.expr[k]; d.keys[k] = s.keys[k]; }
            d.str = s.str;
        }
        for (size_t i = 0; i < n->inputs.size(); ++i)
            if (n->inputs[i]) net().connect(n->inputs[i], c, int(i));
        if (n->subnet() && c->subnet()) c->subnet()->fromJson(n->subnet()->toJson());
        newSel.insert(c->id());
    }
    selection = newSel;
    currentId = newSel.empty() ? -1 : *newSel.begin();
}

void App::setDisplay(Node* n) {
    net().setDisplayNode(n);
}

void App::newScene(bool withDemo) {
    netPath_.clear();
    graph.clear();
    if (withDemo) buildDemoScene(graph, "terrain");
    undo_.clear();
    redo_.clear();
    clearSelection();
    if (Node* d = net().displayNode()) select(d);
    currentFile_.clear();
    dirty_ = false;
    frameOnNextCook_ = true;
    network.requestFrameAll();
}

bool App::loadScene(const std::string& path) {
    Graph tmp;
    std::string err;
    if (!tmp.load(path, &err)) {
        status("Open failed: " + err, true);
        return false;
    }
    netPath_.clear();
    graph.fromJson(tmp.toJson());
    undo_.clear();
    redo_.clear();
    clearSelection();
    if (Node* d = net().displayNode()) select(d);
    currentFile_ = path;
    dirty_ = false;
    frameOnNextCook_ = true;
    if (graph.render.hasCamera) { // restore the saved view
        const RenderSettings& r = graph.render;
        camera.target = r.camTarget; camera.yaw = r.camYaw; camera.pitch = r.camPitch; camera.dist = r.camDist; camera.fovY = r.camFov;
        frameOnNextCook_ = false;
    }
    network.requestFrameAll();
    pushRecent(path);
    status(err.empty() ? "Opened " + path : "Opened with warnings: " + err, !err.empty());
    return true;
}

void App::syncCameraToScene() {
    RenderSettings& r = graph.render;
    r.hasCamera = true;
    r.camTarget = camera.target; r.camYaw = camera.yaw; r.camPitch = camera.pitch; r.camDist = camera.dist; r.camFov = camera.fovY;
}

bool App::saveScene(const std::string& path) {
    syncCameraToScene();
    std::string err;
    if (!graph.save(path, &err)) {
        status("Save failed: " + err, true);
        return false;
    }
    currentFile_ = path;
    dirty_ = false;
    pushRecent(path);
    status("Saved " + path);
    return true;
}

void App::status(const std::string& msg, bool error) {
    statusMsg_ = msg;
    statusErr_ = error;
    statusTime_ = glfwGetTime();
}

void App::frameGeometry() {
    Node* d = net().displayNode();
    if (!d || d->geometry().empty()) { camera.home(); return; }
    Vec3 mn, mx;
    d->geometry().bounds(mn, mx);
    float yaw = camera.yaw, pitch = camera.pitch;
    camera.frame(mn, mx);
    camera.yaw = yaw; camera.pitch = pitch;
}

void App::cookDisplay() {
    // cook down the path we have dived into, so each level's Subnet Inputs and clock are up to date
    {
        Graph* g = &graph;
        for (auto& name : netPath_) {
            Node* sn = g->findNode(name);
            if (!sn || !sn->subnet()) break;
            g->cook(sn);
            Graph* sub = sn->subnet();
            sub->frame = graph.frame; sub->fps = graph.fps; sub->frameStart = graph.frameStart; sub->frameEnd = graph.frameEnd;
            g = sub;
        }
    }
    Node* d = net().displayNode();
    if (!d) { renderer.clearGeometry(); return; }
    net().cook(d);
    renderer.setGeometry(d->geometry(), d->cookStamp());
    // also cook the selected node so its info (errors, counts) is fresh in the UI
    if (Node* c = current()) net().cook(c);
    if (frameOnNextCook_) {
        frameOnNextCook_ = false;
        frameGeometry();
    }
}

void App::updateTitle() {
    std::string name = currentFile_.empty() ? "untitled.hdg" : fs::path(currentFile_).filename().string();
    std::string title = "HD — " + name + (dirty_ ? " *" : "");
    if (title != lastTitle_) {
        glfwSetWindowTitle(window_, title.c_str());
        lastTitle_ = title;
    }
}

// ---------------------------------------------------------------- per-frame
void App::handleGlobalShortcuts() {
    ImGuiIO& io = ImGui::GetIO();
    bool typing = io.WantTextInput;
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z) && !typing) undo();
    if ((ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z) || ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Y)) && !typing) redo();
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S)) {
        if (currentFile_.empty()) openFileDialog(FileDialogMode::SaveAs);
        else saveScene(currentFile_);
    }
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S)) openFileDialog(FileDialogMode::SaveAs);
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_O)) openFileDialog(FileDialogMode::Open);
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_N)) { checkpoint(); newScene(false); }
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Q)) glfwSetWindowShouldClose(window_, 1);
    if (!typing) {
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_C)) copySelected();
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_X)) cutSelected();
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_V)) pasteNodes();
        if (ImGui::IsKeyPressed(ImGuiKey_K, false) && !io.KeyCtrl && !io.KeyAlt) keySelected();
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_RightArrow)) jumpToKey(1);
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_LeftArrow)) jumpToKey(-1);
    }
    if (!typing) {
        // Houdini-style: up/down arrow steps frames, left/right too
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) && !io.KeyCtrl) graph.frame = std::min(graph.frame + 1, float(graph.frameEnd));
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) && !io.KeyCtrl) graph.frame = std::max(graph.frame - 1, float(graph.frameStart));
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, false) && io.KeyAlt) playing = !playing;
    }
}

void App::frame() {
    double now = glfwGetTime();
    double dt = now - lastTime_;
    lastTime_ = now;
    if (playing) {
        playAccum_ += realtime ? dt : 1.0 / graph.fps;
        double step = 1.0 / std::max(1.f, graph.fps);
        int steps = 0;
        while (playAccum_ >= step && steps < 4) {
            playAccum_ -= step;
            graph.frame += 1;
            if (graph.frame > graph.frameEnd) graph.frame = float(graph.frameStart);
            ++steps;
        }
        if (playAccum_ > step) playAccum_ = 0; // can't keep up: drop frames instead of spiralling
    }

    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGuiID dockId = ImGui::DockSpaceOverViewport(0, vp, ImGuiDockNodeFlags_PassthruCentralNode);
    if (!layoutBuilt_) {
        layoutBuilt_ = true;
        ImGuiDockNode* node = ImGui::DockBuilderGetNode(dockId);
        // layouts saved by older versions have no shelf: rebuild so it gets its place
        if (!ImGui::FindWindowSettingsByID(ImHashStr("Shelf"))) forceLayout_ = true;
        if (forceLayout_ || !node || node->IsLeafNode()) buildDefaultLayout(dockId);
        forceLayout_ = false;
        focusSceneView_ = 3;
    }

    // tabs only exist after their first frame, so select the Scene View a little later
    if (focusSceneView_ > 0 && --focusSceneView_ == 0) ImGui::SetWindowFocus("Scene View");
    handleGlobalShortcuts();
    cookDisplay();
    drawMenuBar();
    if (showShelf) drawShelf();
    updateGuides();
    if (showSceneView) drawViewport();
    if (showNetwork) network.draw(*this);
    if (showParms) drawParameters();
    if (showSpreadsheet) drawSpreadsheet();
    if (showTimeline) drawTimeline();
    stepSequence();
    if (showRenderView) drawRenderView();
    drawDisplayOptions();
    drawRenderSettings();
    drawAssetManager();
    drawAnimEditor();
    drawFileDialog();
    drawHelpWindows();
    if (showImGuiDemo) ImGui::ShowDemoWindow(&showImGuiDemo);

    if (confirmClose_) { ImGui::OpenPopup("Unsaved changes"); confirmClose_ = false; }
    if (ImGui::BeginPopupModal("Unsaved changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("The scene has unsaved changes. Quit anyway?");
        ImGui::Spacing();
        if (ImGui::Button("Save", ImVec2(110, 0))) {
            ImGui::CloseCurrentPopup();
            if (currentFile_.empty()) openFileDialog(FileDialogMode::SaveAs);
            else if (saveScene(currentFile_)) { allowClose_ = true; glfwSetWindowShouldClose(window_, 1); }
        }
        ImGui::SameLine();
        if (ImGui::Button("Don't Save", ImVec2(110, 0))) { allowClose_ = true; glfwSetWindowShouldClose(window_, 1); ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(110, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    updateTitle();
}

void App::buildDefaultLayout(unsigned dockIdIn) {
    ImGuiID dockId = dockIdIn;
    ImGui::DockBuilderRemoveNode(dockId);
    ImGui::DockBuilderAddNode(dockId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockId, ImGui::GetMainViewport()->WorkSize);
    ImGuiID shelf = ImGui::DockBuilderSplitNode(dockId, ImGuiDir_Up, 0.07f, nullptr, &dockId);
    ImGui::DockBuilderSetNodeSize(shelf, ImVec2(ImGui::GetMainViewport()->WorkSize.x, ImGui::GetFrameHeight() * 2.1f + ImGui::GetStyle().WindowPadding.y * 2));
    ImGui::DockBuilderDockWindow("Shelf", shelf);
    if (ImGuiDockNode* sh = ImGui::DockBuilderGetNode(shelf)) sh->LocalFlags |= ImGuiDockNodeFlags_NoTabBar;
    ImGuiID bottom = ImGui::DockBuilderSplitNode(dockId, ImGuiDir_Down, 0.05f, nullptr, &dockId);
    ImGui::DockBuilderSetNodeSize(bottom, ImVec2(ImGui::GetMainViewport()->WorkSize.x, ImGui::GetFrameHeight() + ImGui::GetStyle().WindowPadding.y * 2 + 2));
    ImGuiID right = ImGui::DockBuilderSplitNode(dockId, ImGuiDir_Right, 0.42f, nullptr, &dockId);
    ImGuiID rightBottom = ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.55f, nullptr, &right);
    ImGui::DockBuilderDockWindow("Scene View", dockId);
    ImGui::DockBuilderDockWindow("Render View", dockId);
    ImGui::DockBuilderDockWindow("Geometry Spreadsheet", dockId);
    ImGui::DockBuilderDockWindow("Parameters", right);
    ImGui::DockBuilderDockWindow("Network", rightBottom);
    ImGui::DockBuilderDockWindow("Timeline", bottom);
    if (ImGuiDockNode* b = ImGui::DockBuilderGetNode(bottom)) b->LocalFlags |= ImGuiDockNodeFlags_NoTabBar;
    ImGui::DockBuilderFinish(dockIdIn);
    ImGui::SetWindowFocus("Scene View");
}

// ---------------------------------------------------------------- menu & status
// ---------------------------------------------------------------- file dialog
void App::openFileDialog(FileDialogMode mode) {
    fileMode_ = mode;
    fileDialogOpenRequest_ = true;
    std::memset(fileName_, 0, sizeof fileName_);
    if (mode == FileDialogMode::SaveAs) {
        std::string n = currentFile_.empty() ? "untitled.hdg" : fs::path(currentFile_).filename().string();
        std::snprintf(fileName_, sizeof fileName_, "%s", n.c_str());
        if (!currentFile_.empty()) fileDir_ = fs::path(currentFile_).parent_path().string();
    } else if (mode == FileDialogMode::SaveImage || mode == FileDialogMode::SaveSequence) {
        std::string stem = currentFile_.empty() ? "render" : fs::path(currentFile_).stem().string();
        std::snprintf(fileName_, sizeof fileName_, "%s.png", stem.c_str());
    } else if (mode == FileDialogMode::ExportObj) {
        std::snprintf(fileName_, sizeof fileName_, "%s.obj", net().displayNode() ? net().displayNode()->name().c_str() : "export");
    }
    if (fileDir_.empty() || !fs::exists(fileDir_)) fileDir_ = fs::current_path().string();
}

void App::drawFileDialog() {
    if (fileDialogOpenRequest_) {
        ImGui::OpenPopup("File Browser");
        fileDialogOpenRequest_ = false;
    }
    ImGui::SetNextWindowSize(ImVec2(640, 460), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("File Browser", nullptr)) return;
    const char* titles[] = {"", "Open Scene", "Save Scene As", "Import OBJ", "Export Geometry (.obj / .ply)", "Choose File", "Save Rendered Image", "Render Sequence To (frame numbers are added)", "Merge Scene", "Install Asset"};
    ImGui::TextUnformatted(titles[int(fileMode_)]);
    ImGui::Separator();
    bool saving = fileMode_ == FileDialogMode::SaveAs || fileMode_ == FileDialogMode::ExportObj || fileMode_ == FileDialogMode::SaveImage || fileMode_ == FileDialogMode::SaveSequence;
    std::vector<std::string> exts;
    if (fileMode_ == FileDialogMode::Open || fileMode_ == FileDialogMode::SaveAs || fileMode_ == FileDialogMode::MergeScene) exts = {".hdg"};
    else if (fileMode_ == FileDialogMode::ExportObj) exts = {".obj", ".ply"};
    else if (fileMode_ == FileDialogMode::SaveImage || fileMode_ == FileDialogMode::SaveSequence) exts = {".png"};
    else if (fileMode_ == FileDialogMode::InstallAsset) exts = {".hda"};
    else exts = {".obj"};

    // path bar
    if (ImGui::Button("Up")) {
        fs::path p(fileDir_);
        if (p.has_parent_path()) fileDir_ = p.parent_path().string();
    }
    ImGui::SameLine();
    if (ImGui::Button("Home")) {
        const char* home = std::getenv("HOME");
        if (!home) home = std::getenv("USERPROFILE");
        if (home) fileDir_ = home;
    }
    ImGui::SameLine();
    if (ImGui::Button("Examples")) {
        for (auto cand : {fs::path(examplesDir), fs::current_path() / "examples", fs::current_path().parent_path() / "examples"})
            if (fs::exists(cand)) { fileDir_ = cand.string(); break; }
    }
    ImGui::SameLine();
    char dirBuf[1024];
    std::snprintf(dirBuf, sizeof dirBuf, "%s", fileDir_.c_str());
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputText("##dir", dirBuf, sizeof dirBuf, ImGuiInputTextFlags_EnterReturnsTrue) && fs::is_directory(dirBuf)) fileDir_ = dirBuf;

    std::string chosen;
    ImGui::BeginChild("##files", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 2.2f), ImGuiChildFlags_Borders);
    std::error_code ec;
    std::vector<fs::directory_entry> dirs, files;
    for (auto& e : fs::directory_iterator(fileDir_, fs::directory_options::skip_permission_denied, ec)) {
        std::string fn = e.path().filename().string();
        if (fn.empty() || fn[0] == '.') continue;
        std::error_code ec2;
        if (e.is_directory(ec2)) dirs.push_back(e);
        else {
            std::string ext = e.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (std::find(exts.begin(), exts.end(), ext) != exts.end()) files.push_back(e);
        }
    }
    auto byName = [](const fs::directory_entry& a, const fs::directory_entry& b) { return a.path().filename() < b.path().filename(); };
    std::sort(dirs.begin(), dirs.end(), byName);
    std::sort(files.begin(), files.end(), byName);
    for (auto& d : dirs) {
        std::string label = "[dir]  " + d.path().filename().string();
        if (ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick) && ImGui::IsMouseDoubleClicked(0))
            fileDir_ = d.path().string();
    }
    for (auto& f : files) {
        std::string fn = f.path().filename().string();
        bool sel = fn == fileName_;
        if (ImGui::Selectable(fn.c_str(), sel, ImGuiSelectableFlags_AllowDoubleClick)) {
            std::snprintf(fileName_, sizeof fileName_, "%s", fn.c_str());
            if (ImGui::IsMouseDoubleClicked(0)) chosen = f.path().string();
        }
    }
    if (ec) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", ec.message().c_str());
    ImGui::EndChild();

    ImGui::SetNextItemWidth(-220);
    bool enter = ImGui::InputText("##name", fileName_, sizeof fileName_, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button(saving ? "Save" : "Accept", ImVec2(100, 0)) || enter) {
        if (fileName_[0]) {
            fs::path p(fileName_);
            if (p.is_relative()) p = fs::path(fileDir_) / p;
            if (saving && p.extension().empty()) p += exts.front();
            chosen = p.string();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(100, 0))) { fileMode_ = FileDialogMode::None; ImGui::CloseCurrentPopup(); }

    if (!chosen.empty()) {
        std::string err;
        switch (fileMode_) {
        case FileDialogMode::Open: loadScene(chosen); break;
        case FileDialogMode::SaveAs: saveScene(chosen); break;
        case FileDialogMode::ImportObj: openPath(chosen); break;
        case FileDialogMode::ExportObj: {
            Node* d = net().displayNode();
            bool ok = false;
            if (d) {
                std::string ext = fs::path(chosen).extension().string();
                ok = ext == ".ply" ? savePly(chosen, d->geometry(), &err) : saveObj(chosen, d->geometry(), &err);
            }
            status(ok ? "Exported " + chosen : "Export failed: " + err, !ok);
            break;
        }
        case FileDialogMode::ChooseFileParm: {
            size_t slash = fileParmTarget.find('/');
            Node* n = net().findNode(fileParmTarget.substr(0, slash));
            if (n && slash != std::string::npos) {
                if (Parm* p = n->parm(fileParmTarget.substr(slash + 1))) {
                    checkpoint();
                    p->str = chosen;
                    n->touch();
                    frameOnNextCook_ = true;
                }
            }
            break;
        }
        case FileDialogMode::MergeScene: mergeScene(chosen); break;
        case FileDialogMode::InstallAsset: installAsset(chosen); break;
        case FileDialogMode::SaveImage: {
            bool ok = writePng(chosen, renderImg_, renderW_, renderH_, &err);
            status(ok ? "Saved " + chosen : "Save failed: " + err, !ok);
            break;
        }
        case FileDialogMode::SaveSequence:
            seqBase_ = chosen;
            seqFrame_ = graph.frameStart;
            seqEnd_ = graph.frameEnd;
            seqCount_ = 0;
            seqActive_ = true;
            seqWaiting_ = false;
            playing = false;
            break;
        default: break;
        }
        fileDir_ = fs::path(chosen).parent_path().string();
        fileMode_ = FileDialogMode::None;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// ---------------------------------------------------------------- help
void App::drawHelpWindows() {
    if (showShortcuts) {
        ImGui::SetNextWindowSize(ImVec2(520, 520), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Shortcuts", &showShortcuts)) {
            auto row = [](const char* k, const char* d) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(k);
                ImGui::TableNextColumn(); ImGui::TextDisabled("%s", d);
            };
            if (ImGui::BeginTable("keys", 2, ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed, 190);
                row("Network: Tab / Right-click", "Add node menu (type to search)");
                row("Network: drag output -> input", "Connect nodes");
                row("Network: drag wire to empty", "Add node connected to the wire");
                row("Network: drag from input", "Disconnect / re-wire");
                row("Network: blue flag (right)", "Set display node");
                row("Network: yellow flag (left)", "Bypass node");
                row("Network: Del / Backspace", "Delete selected nodes");
                row("Network: Ctrl+D", "Duplicate selected nodes");
                row("Network: H", "Frame all nodes");
                row("Network: MMB / Alt+LMB drag", "Pan");
                row("Network: wheel", "Zoom");
                row("Network: F2 / double-click", "Rename node");
                row("Network: Shift+C", "Collapse selected nodes into a subnet");
                row("Network: I / double-click subnet", "Dive into a subnet");
                row("Network: U", "Go up one level");
                row("Viewport: LMB / Alt+LMB", "Tumble");
                row("Viewport: MMB / Alt+MMB", "Pan");
                row("Viewport: RMB / wheel", "Dolly");
                row("Viewport: F", "Frame geometry");
                row("Viewport: H", "Home camera");
                row("Viewport: W", "Toggle wireframe overlay");
                row("Left / Right arrows", "Previous / next frame");
                row("Alt+Up", "Play / stop");
                row("Ctrl+Z / Ctrl+Shift+Z", "Undo / redo");
                row("Ctrl+S / Ctrl+O / Ctrl+N", "Save / open / new");
                row("Parameters: right-click label", "Expressions, revert to default");
                row("Viewport: D", "Display Options (ground plane, markers, background)");
                row("Ctrl+C / Ctrl+X / Ctrl+V", "Copy / cut / paste nodes");
                row("Parameters: Alt+click label", "Set a keyframe at the current frame");
                row("K", "Key the animated parameters of the selected nodes");
                row("Ctrl+Left / Ctrl+Right", "Previous / next keyframe");
                row("Render View: Render", "Path-trace the displayed node from the viewport camera");
                ImGui::EndTable();
            }
        }
        ImGui::End();
    }
    if (showVexHelp) {
        ImGui::SetNextWindowSize(ImVec2(600, 560), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("VEX-lite Reference", &showVexHelp)) {
            ImGui::TextWrapped("VEX-lite is HD's snippet language, used by Attribute Wrangle, expression groups (Blast / PolyExtrude) and parameter expressions.");
            ImGui::SeparatorText("Attributes");
            ImGui::TextWrapped("@P @N @Cd @v @up @uv @scale are vectors; other @name are floats. Force a type with v@name or f@name. "
                               "Writing to an attribute that doesn't exist creates it. Read-only: @ptnum @numpt @primnum @numprim @Time @Frame. "
                               "In a POP wrangle, particles also have @v @age @life @id @pscale, and @TimeInc is the substep length.");
            ImGui::SeparatorText("Syntax");
            if (monoFont) ImGui::PushFont(monoFont);
            ImGui::TextUnformatted(
                "float d = length(@P);            // declarations: float, int, vector\n"
                "vector c = {1, 0.5, 0};          // vector literal\n"
                "@P.y += sin(d * 4 - @Time) * 0.2;  // component access .x .y .z / .r .g .b\n"
                "if (@P.y > 0) @Cd = c; else @Cd = set(0, 0, 1);\n"
                "for (int i = 0; i < 3; i++) @P *= 0.9;\n"
                "@pscale = rand(@ptnum) * 0.1;   // new float attribute\n");
            if (monoFont) ImGui::PopFont();
            ImGui::SeparatorText("Functions");
            ImGui::TextWrapped("sin cos tan asin acos atan atan2 sqrt abs floor ceil frac rint pow exp log min max clamp "
                               "fit(v,omin,omax,nmin,nmax) fit01 lerp smooth(lo,hi,v) length normalize distance dot cross "
                               "set(x,y,z) noise(p) curlnoise(p) rand(seed) hsvtorgb(hsv) degrees radians sign mod");
            ImGui::SeparatorText("Parameter expressions");
            ImGui::TextWrapped("Right-click a parameter label > Add Expression. Variables: $F (frame), $T (seconds), $FPS, $PI. "
                               "Example: sin($T * 2) * 0.5. Animated parameters turn the node time-dependent.");
            ImGui::SeparatorText("Groups");
            ImGui::TextWrapped("Group fields accept numbers and ranges (0-10 15 20-40:2), an expression (@P.y > 0), and ! to invert.");
        }
        ImGui::End();
    }
    if (showAbout) {
        if (ImGui::Begin("About HD", &showAbout, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted("HD — procedural 3D VFX environment");
            ImGui::TextDisabled("Version 0.1  ·  node-based geometry (SOP-style) core");
            ImGui::Separator();
            ImGui::Text("%zu node types  ·  OpenGL %s", nodeTypes().size(), (const char*)glGetString(GL_VERSION));
            ImGui::TextDisabled("%s", (const char*)glGetString(GL_RENDERER));
        }
        ImGui::End();
    }
}

} // namespace hd
