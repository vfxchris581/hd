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
    if (!startupFile.empty() && loadScene(startupFile)) {
        // loaded
    } else {
        newScene(true);
    }
    lastTime_ = glfwGetTime();
    return true;
}

void App::shutdown() { renderer.shutdown(); }

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

App::Snapshot App::snapshot() const {
    Snapshot s;
    s.graph = graph.toJson().dump(0);
    for (int id : selection)
        if (Node* n = graph.findNode(id)) s.selected.push_back(n->name());
    if (Node* c = current()) s.current = c->name();
    return s;
}

void App::restore(const Snapshot& s) {
    float frame = graph.frame;
    graph.fromJson(Json::parse(s.graph));
    graph.frame = frame;
    selection.clear();
    currentId = -1;
    for (auto& name : s.selected)
        if (Node* n = graph.findNode(name)) selection.insert(n->id());
    if (Node* c = graph.findNode(s.current)) currentId = c->id();
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
    Node* n = graph.createNode(type, x, y);
    if (!n) return nullptr;
    if (Node* src = graph.findNode(connectFrom))
        if (!n->inputs.empty()) graph.connect(src, n, 0);
    // Houdini behaviour: new nodes become the displayed node when they're downstream
    // of the display node, or when nothing is displayed.
    if (!graph.displayNode() || (connectFrom >= 0 && graph.displayNode() && graph.displayNode()->id() == connectFrom))
        setDisplay(n);
    select(n);
    return n;
}

void App::deleteSelected() {
    if (selection.empty()) return;
    checkpoint();
    for (int id : std::vector<int>(selection.begin(), selection.end())) {
        Node* n = graph.findNode(id);
        if (!n) continue;
        // reconnect around a single-input node (like Houdini's delete)
        Node* upstream = n->inputs.empty() ? nullptr : n->inputs[0];
        auto outs = graph.outputsOf(n);
        bool wasDisplay = graph.displayNode() == n;
        graph.removeNode(n);
        if (upstream && graph.findNode(upstream->id())) {
            for (auto& [dst, idx] : outs) graph.connect(upstream, dst, idx);
            if (wasDisplay) graph.setDisplayNode(upstream);
        }
    }
    clearSelection();
    if (!graph.displayNode() && !graph.nodes().empty()) graph.setDisplayNode(graph.nodes().back().get());
}

void App::duplicateSelected() {
    if (selection.empty()) return;
    checkpoint();
    std::set<int> newSel;
    for (int id : selection) {
        Node* n = graph.findNode(id);
        if (!n) continue;
        Node* c = graph.createNode(n->type().name, n->x + 40, n->y + 40, n->name());
        for (size_t i = 0; i < n->parms().size(); ++i) {
            Parm& d = c->parms()[i];
            const Parm& s = n->parms()[i];
            std::copy(s.val, s.val + 3, d.val);
            for (int k = 0; k < 3; ++k) d.expr[k] = s.expr[k];
            d.str = s.str;
        }
        for (size_t i = 0; i < n->inputs.size(); ++i)
            if (n->inputs[i]) graph.connect(n->inputs[i], c, int(i));
        newSel.insert(c->id());
    }
    selection = newSel;
    currentId = newSel.empty() ? -1 : *newSel.begin();
}

void App::setDisplay(Node* n) {
    graph.setDisplayNode(n);
}

void App::newScene(bool withDemo) {
    graph.clear();
    if (withDemo) buildDemoScene(graph, "terrain");
    undo_.clear();
    redo_.clear();
    clearSelection();
    if (Node* d = graph.displayNode()) select(d);
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
    graph.fromJson(tmp.toJson());
    undo_.clear();
    redo_.clear();
    clearSelection();
    if (Node* d = graph.displayNode()) select(d);
    currentFile_ = path;
    dirty_ = false;
    frameOnNextCook_ = true;
    network.requestFrameAll();
    status(err.empty() ? "Opened " + path : "Opened with warnings: " + err, !err.empty());
    return true;
}

bool App::saveScene(const std::string& path) {
    std::string err;
    if (!graph.save(path, &err)) {
        status("Save failed: " + err, true);
        return false;
    }
    currentFile_ = path;
    dirty_ = false;
    status("Saved " + path);
    return true;
}

void App::status(const std::string& msg, bool error) {
    statusMsg_ = msg;
    statusErr_ = error;
    statusTime_ = glfwGetTime();
}

void App::frameGeometry() {
    Node* d = graph.displayNode();
    if (!d || d->geometry().numPoints() == 0) { camera.home(); return; }
    Vec3 mn, mx;
    d->geometry().bounds(mn, mx);
    float yaw = camera.yaw, pitch = camera.pitch;
    camera.frame(mn, mx);
    camera.yaw = yaw; camera.pitch = pitch;
}

void App::cookDisplay() {
    Node* d = graph.displayNode();
    if (!d) { renderer.clearGeometry(); return; }
    graph.cook(d);
    renderer.setGeometry(d->geometry(), d->cookStamp());
    // also cook the selected node so its info (errors, counts) is fresh in the UI
    if (Node* c = current()) graph.cook(c);
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
        if (forceLayout_ || !node || node->IsLeafNode()) buildDefaultLayout(dockId);
        forceLayout_ = false;
    }

    handleGlobalShortcuts();
    cookDisplay();
    drawMenuBar();
    drawViewport();
    network.draw(*this);
    drawParameters();
    drawSpreadsheet();
    drawTimeline();
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
    ImGuiID bottom = ImGui::DockBuilderSplitNode(dockId, ImGuiDir_Down, 0.05f, nullptr, &dockId);
    ImGui::DockBuilderSetNodeSize(bottom, ImVec2(ImGui::GetMainViewport()->WorkSize.x, ImGui::GetFrameHeight() + ImGui::GetStyle().WindowPadding.y * 2 + 2));
    ImGuiID right = ImGui::DockBuilderSplitNode(dockId, ImGuiDir_Right, 0.42f, nullptr, &dockId);
    ImGuiID rightBottom = ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.55f, nullptr, &right);
    ImGui::DockBuilderDockWindow("Scene View", dockId);
    ImGui::DockBuilderDockWindow("Geometry Spreadsheet", dockId);
    ImGui::DockBuilderDockWindow("Parameters", right);
    ImGui::DockBuilderDockWindow("Network", rightBottom);
    ImGui::DockBuilderDockWindow("Timeline", bottom);
    if (ImGuiDockNode* b = ImGui::DockBuilderGetNode(bottom)) b->LocalFlags |= ImGuiDockNodeFlags_NoTabBar;
    ImGui::DockBuilderFinish(dockIdIn);
    ImGui::SetWindowFocus("Scene View");
}

// ---------------------------------------------------------------- menu & status
void App::drawMenuBar() {
    if (!ImGui::BeginMainMenuBar()) return;
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New", "Ctrl+N")) { checkpoint(); newScene(false); }
        if (ImGui::MenuItem("Open...", "Ctrl+O")) openFileDialog(FileDialogMode::Open);
        if (ImGui::BeginMenu("Open Example")) {
            for (auto& d : demoScenes()) {
                if (ImGui::MenuItem(d.title.c_str())) {
                    checkpoint();
                    buildDemoScene(graph, d.name);
                    clearSelection();
                    if (Node* n = graph.displayNode()) select(n);
                    currentFile_.clear();
                    frameOnNextCook_ = true;
                    network.requestFrameAll();
                    status(d.description);
                }
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
        ImGui::Separator();
        if (ImGui::MenuItem("Import OBJ...")) openFileDialog(FileDialogMode::ImportObj);
        if (ImGui::MenuItem("Export Display Geometry...", nullptr, false, graph.displayNode() != nullptr)) openFileDialog(FileDialogMode::ExportObj);
        ImGui::Separator();
        if (ImGui::MenuItem("Quit", "Ctrl+Q")) glfwSetWindowShouldClose(window_, 1);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        if (ImGui::MenuItem("Undo", "Ctrl+Z", false, canUndo())) undo();
        if (ImGui::MenuItem("Redo", "Ctrl+Shift+Z", false, canRedo())) redo();
        ImGui::Separator();
        if (ImGui::MenuItem("Duplicate Nodes", "Ctrl+D", false, !selection.empty())) duplicateSelected();
        if (ImGui::MenuItem("Delete Nodes", "Del", false, !selection.empty())) deleteSelected();
        if (ImGui::MenuItem("Select All", "Ctrl+A")) {
            selection.clear();
            for (auto& n : graph.nodes()) selection.insert(n->id());
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Display")) {
        ImGui::MenuItem("Shaded", "", &viewOpts.shaded);
        ImGui::MenuItem("Wireframe Overlay", "W", &viewOpts.wireframe);
        ImGui::MenuItem("Points", "", &viewOpts.points);
        ImGui::MenuItem("Point Numbers", "", &showPointNumbers);
        ImGui::MenuItem("Primitive Numbers", "", &showPrimNumbers);
        ImGui::MenuItem("Point Normals", "", &viewOpts.normals);
        ImGui::MenuItem("Ground Grid", "", &viewOpts.grid);
        ImGui::MenuItem("Tint Backfaces", "", &viewOpts.backfaceTint);
        ImGui::Separator();
        if (ImGui::MenuItem("Frame Geometry", "F")) frameGeometry();
        if (ImGui::MenuItem("Home Camera", "H")) camera.home();
        if (ImGui::MenuItem("Reset Layout")) { layoutBuilt_ = false; forceLayout_ = true; }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
        ImGui::MenuItem("Shortcuts", "", &showShortcuts);
        ImGui::MenuItem("VEX-lite Reference", "", &showVexHelp);
        ImGui::MenuItem("UI Toolkit Demo", "", &showImGuiDemo);
        ImGui::Separator();
        ImGui::MenuItem("About HD", "", &showAbout);
        ImGui::EndMenu();
    }
    // right-aligned info
    char info[128];
    std::snprintf(info, sizeof info, "%s   %.0f fps", currentFile_.empty() ? "untitled" : fs::path(currentFile_).filename().string().c_str(),
                  double(ImGui::GetIO().Framerate));
    float w = ImGui::CalcTextSize(info).x;
    ImGui::SameLine(ImGui::GetWindowWidth() - w - 16);
    ImGui::TextDisabled("%s", info);
    ImGui::EndMainMenuBar();
}

// ---------------------------------------------------------------- file dialog
void App::openFileDialog(FileDialogMode mode) {
    fileMode_ = mode;
    fileDialogOpenRequest_ = true;
    std::memset(fileName_, 0, sizeof fileName_);
    if (mode == FileDialogMode::SaveAs) {
        std::string n = currentFile_.empty() ? "untitled.hdg" : fs::path(currentFile_).filename().string();
        std::snprintf(fileName_, sizeof fileName_, "%s", n.c_str());
        if (!currentFile_.empty()) fileDir_ = fs::path(currentFile_).parent_path().string();
    } else if (mode == FileDialogMode::ExportObj) {
        std::snprintf(fileName_, sizeof fileName_, "%s.obj", graph.displayNode() ? graph.displayNode()->name().c_str() : "export");
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
    const char* titles[] = {"", "Open Scene", "Save Scene As", "Import OBJ", "Export Geometry (.obj / .ply)", "Choose File"};
    ImGui::TextUnformatted(titles[int(fileMode_)]);
    ImGui::Separator();
    bool saving = fileMode_ == FileDialogMode::SaveAs || fileMode_ == FileDialogMode::ExportObj;
    std::vector<std::string> exts;
    if (fileMode_ == FileDialogMode::Open || fileMode_ == FileDialogMode::SaveAs) exts = {".hdg"};
    else if (fileMode_ == FileDialogMode::ExportObj) exts = {".obj", ".ply"};
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
            Node* d = graph.displayNode();
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
            Node* n = graph.findNode(fileParmTarget.substr(0, slash));
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
                               "Writing to an attribute that doesn't exist creates it. Read-only: @ptnum @numpt @primnum @numprim @Time @Frame.");
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
