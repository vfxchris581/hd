// Reusable assets: subnets saved to the user's asset folder, listed in the Tab menu.
#include "App.h"
#include <imgui.h>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace hd {

std::string App::assetDir() const {
    const char* home = std::getenv("HOME");
    if (!home) home = std::getenv("USERPROFILE");
    std::error_code ec;
    fs::path dir = home ? fs::path(home) : fs::temp_directory_path(ec);
    return (dir / ".hd_assets").string();
}

void App::refreshAssets() {
    assets_.clear();
    std::error_code ec;
    for (auto& e : fs::directory_iterator(assetDir(), fs::directory_options::skip_permission_denied, ec))
        if (e.path().extension() == ".hda") assets_.push_back(e.path().stem().string());
    std::sort(assets_.begin(), assets_.end());
}

void App::saveAsset(Node* sn) {
    if (!sn || !sn->subnet()) { status("Select a subnet to save as an asset", true); return; }
    std::error_code ec;
    fs::create_directories(assetDir(), ec);
    Json j = Json::object();
    j["format"] = "hd-asset";
    j["name"] = sn->name();
    j["graph"] = sn->subnet()->toJson();
    fs::path path = fs::path(assetDir()) / (sn->name() + ".hda");
    const bool existed = fs::exists(path, ec);
    std::ofstream f(path, std::ios::binary);
    if (f) f << j.dump(2) << "\n";
    if (!f) { status("Could not write " + path.string(), true); return; }
    refreshAssets();
    status(std::string(existed ? "Updated asset '" : "Saved asset '") + sn->name() + "'. Find it in the Tab menu under Assets.");
}

Node* App::createAsset(const std::string& name, float x, float y, int connectFrom) {
    std::ifstream f(fs::path(assetDir()) / (name + ".hda"), std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    Json j = Json::parse(ss.str());
    if (!f || j.get("format").str() != "hd-asset") { status("Asset '" + name + "' could not be read", true); return nullptr; }
    Node* n = createNode("subnet", x, y, connectFrom);
    if (!n) return nullptr;
    n->subnet()->fromJson(j.get("graph"));
    net().rename(n, net().uniqueName(name + "1"));
    return n;
}

void App::deleteAsset(const std::string& name) {
    std::error_code ec;
    fs::remove(fs::path(assetDir()) / (name + ".hda"), ec);
    refreshAssets();
    status("Deleted asset '" + name + "'");
}

void App::installAsset(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    if (!f || Json::parse(ss.str()).get("format").str() != "hd-asset") { status("Not an HD asset file: " + path, true); return; }
    std::error_code ec;
    fs::create_directories(assetDir(), ec);
    fs::copy_file(path, fs::path(assetDir()) / fs::path(path).filename(), fs::copy_options::overwrite_existing, ec);
    if (ec) { status("Install failed: " + ec.message(), true); return; }
    refreshAssets();
    status("Installed asset '" + fs::path(path).stem().string() + "'");
}

void App::drawAssetManager() {
    if (!showAssetManager) return;
    ImGui::SetNextWindowSize(ImVec2(460, 320), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Asset Manager", &showAssetManager, ImGuiWindowFlags_NoDocking)) {
        ImGui::TextDisabled("Assets are saved subnets. Add one from the Tab menu (Assets section).");
        ImGui::TextDisabled("Folder: %s", assetDir().c_str());
        if (ImGui::Button("Install Asset...")) openFileDialog(FileDialogMode::InstallAsset);
        ImGui::SameLine();
        if (ImGui::Button("Refresh")) refreshAssets();
        ImGui::Separator();
        std::string toDelete;
        if (assets_.empty()) ImGui::TextDisabled("No assets yet. Right-click a subnet and choose Save As Asset.");
        for (auto& a : assets_) {
            ImGui::PushID(a.c_str());
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(a.c_str());
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 150);
            if (ImGui::SmallButton("Add to Network")) {
                checkpoint();
                float x = 0, y = 0;
                for (auto& n : net().nodes()) { x = std::max(x, n->x + 220); y = std::min(y, n->y); }
                createAsset(a, x, y);
                network.requestFrameAll();
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Delete")) toDelete = a;
            ImGui::PopID();
        }
        if (!toDelete.empty()) deleteAsset(toDelete);
    }
    ImGui::End();
}

} // namespace hd
