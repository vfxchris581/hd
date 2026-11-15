#include "App.h"
#include "render/GL.h"
#include <imgui.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>

namespace hd {

void App::startRender() {
    Node* d = net().displayNode();
    if (!d) { status("Nothing to render: no display node", true); return; }
    net().cook(d);
    if (!d->error().empty()) { status("Cannot render: " + d->name() + " has an error", true); return; }
    syncCameraToScene();
    tracer_.start(d->geometry(), graph.render);
}

void App::stepSequence() {
    // ---- sequence rendering: one frame at a time
    if (seqActive_ && !tracer_.running()) {
        if (seqWaiting_) {
            uint64_t v = 0;
            std::vector<uint8_t> img; int w = 0, h = 0;
            tracer_.fetch(img, w, h, v);
            std::string path = seqBase_;
            char num[16];
            std::snprintf(num, sizeof num, ".%04d", seqFrame_);
            size_t dot = path.rfind('.');
            path.insert(dot == std::string::npos ? path.size() : dot, num);
            std::string err;
            if (!writePng(path, img, w, h, &err)) { status("Sequence stopped: " + err, true); seqActive_ = false; }
            else { ++seqCount_; status("Saved " + path); }
            seqWaiting_ = false;
            ++seqFrame_;
        }
        if (seqActive_) {
            if (seqFrame_ > seqEnd_) {
                seqActive_ = false;
                status("Sequence finished: " + std::to_string(seqCount_) + " frames");
            } else {
                graph.frame = float(seqFrame_);
                startRender();
                seqWaiting_ = tracer_.running();
                if (!seqWaiting_) seqActive_ = false;
            }
        }
    }

}

void App::drawRenderSettings() {
    if (!showRenderSettings) return;
    RenderSettings& rs = graph.render;
    ImGui::SetNextWindowSize(ImVec2(380, 0), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Render Settings", &showRenderSettings, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize)) {
        bool ch = false;
        ImGui::SeparatorText("Output");
        ch |= ImGui::InputInt("Width", &rs.width, 16, 160);
        ch |= ImGui::InputInt("Height", &rs.height, 16, 160);
        ch |= ImGui::InputInt("Samples", &rs.samples, 8, 64);
        rs.width = std::clamp(rs.width, 16, 4096); rs.height = std::clamp(rs.height, 16, 4096); rs.samples = std::clamp(rs.samples, 1, 100000);
        ImGui::SeparatorText("Light");
        ch |= ImGui::SliderFloat("Sun Direction", &rs.sunAzimuth, 0, 360, "%.0f deg");
        ch |= ImGui::SliderFloat("Sun Height", &rs.sunElevation, 2, 90, "%.0f deg");
        ch |= ImGui::SliderFloat("Sun Brightness", &rs.sunIntensity, 0, 10);
        ch |= ImGui::SliderFloat("Sky Brightness", &rs.skyIntensity, 0, 4);
        ch |= ImGui::SliderFloat("Exposure", &rs.exposure, 0.1f, 5);
        ImGui::SeparatorText("Scene");
        ch |= ImGui::Checkbox("Ground Plane", &rs.ground);
        ch |= ImGui::DragFloat("Ground Height", &rs.groundY, 0.01f);
        ch |= ImGui::DragFloat("Curve Thickness", &rs.curveWidth, 0.001f, 0.001f, 1.f, "%.3f");
        ch |= ImGui::SliderInt("Light Bounces", &rs.bounces, 0, 12);
        ImGui::TextDisabled("Changes apply to the next render. Use a Material node\nfor roughness, metallic and glow.");
        if (ch) markDirty();
    }
    ImGui::End();
}

void App::drawRenderView() {
    if (!ImGui::Begin("Render View", &showRenderView, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) { ImGui::End(); return; }
    RenderSettings& rs = graph.render;

    // ---- toolbar
    bool busy = tracer_.running();
    if (!busy) {
        if (ImGui::Button("Render")) startRender();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Path-trace the displayed node from the Scene View camera");
    } else if (ImGui::Button(seqActive_ ? "Stop Sequence" : "Stop")) {
        tracer_.stop();
        if (seqActive_) { seqActive_ = false; status("Sequence stopped after " + std::to_string(seqCount_) + " frames"); }
    }
    ImGui::SameLine();
    static const int sizes[][2] = {{640, 360}, {960, 540}, {1280, 720}, {1920, 1080}, {1080, 1080}, {1080, 1920}};
    char cur[32];
    std::snprintf(cur, sizeof cur, "%d x %d", rs.width, rs.height);
    ImGui::SetNextItemWidth(120);
    ImGui::BeginDisabled(busy);
    if (ImGui::BeginCombo("##size", cur)) {
        for (auto& sz : sizes) {
            char label[32];
            std::snprintf(label, sizeof label, "%d x %d", sz[0], sz[1]);
            if (ImGui::Selectable(label, rs.width == sz[0] && rs.height == sz[1])) { rs.width = sz[0]; rs.height = sz[1]; markDirty(); }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    if (ImGui::DragInt("##samples", &rs.samples, 1, 1, 4096, "%d samples")) { rs.samples = std::max(1, rs.samples); markDirty(); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("More samples = less noise, longer render");
    ImGui::SameLine();
    if (ImGui::Button("Settings")) showRenderSettings = true;
    ImGui::SameLine();
    if (ImGui::Button("Save Image...") && !renderImg_.empty()) openFileDialog(FileDialogMode::SaveImage);
    ImGui::SameLine();
    if (ImGui::Button("Render Sequence...")) openFileDialog(FileDialogMode::SaveSequence);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Render frames %d-%d (the timeline range) to numbered PNG files", graph.frameStart, graph.frameEnd);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (tracer_.samplesTarget() > 0) {
        if (seqActive_) ImGui::Text("Frame %d / %d   %d / %d samples", seqFrame_, seqEnd_, tracer_.samplesDone(), tracer_.samplesTarget());
        else ImGui::TextDisabled("%d / %d samples   %.1fs%s", tracer_.samplesDone(), tracer_.samplesTarget(), tracer_.secondsElapsed(), busy ? "" : "   done");
    }

    // ---- image
    std::vector<uint8_t> img;
    int w = 0, h = 0;
    if (tracer_.fetch(img, w, h, renderVersion_)) {
        if (!renderTex_) glGenTextures(1, &renderTex_);
        glBindTexture(GL_TEXTURE_2D, renderTex_);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, img.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glBindTexture(GL_TEXTURE_2D, 0);
        renderImg_.swap(img);
        renderW_ = w; renderH_ = h;
    }
    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (renderTex_ && renderW_ > 0 && avail.x > 8 && avail.y > 8) {
        float s = std::min(avail.x / float(renderW_), avail.y / float(renderH_));
        ImVec2 sz(float(renderW_) * s, float(renderH_) * s);
        ImVec2 pos = ImGui::GetCursorPos();
        ImGui::SetCursorPos(ImVec2(pos.x + (avail.x - sz.x) * 0.5f, pos.y + (avail.y - sz.y) * 0.5f));
        ImGui::Image((ImTextureID)(intptr_t)renderTex_, sz);
    } else {
        ImGui::Dummy(ImVec2(0, avail.y * 0.4f));
        const char* hint = "Press Render to path-trace the displayed node from the Scene View camera.";
        ImGui::SetCursorPosX(std::max(8.f, (avail.x - ImGui::CalcTextSize(hint).x) * 0.5f));
        ImGui::TextDisabled("%s", hint);
    }
    ImGui::End();
}

} // namespace hd
