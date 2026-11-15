// Keyframe animation UI: setting keys, timeline marks, and the Animation Editor (dope sheet).
#include "App.h"
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>

namespace hd {

namespace {
bool keyable(const ParmDef& d) {
    return d.type == ParmType::Float || d.type == ParmType::Int || d.type == ParmType::Vec3 || d.type == ParmType::Toggle || d.type == ParmType::Menu;
}
int defaultInterp(const ParmDef& d) { return d.type == ParmType::Toggle || d.type == ParmType::Menu ? 2 : -1; } // switches hold
} // namespace

void App::setKeyframe(Node* n, Parm& p) {
    if (!n || !p.def || !keyable(*p.def)) return;
    checkpoint();
    const float fr = std::floor(graph.frame + 0.5f);
    for (int c = 0; c < p.def->components(); ++c) {
        if (!p.expr[c].empty()) continue;
        p.setKey(c, fr, p.animated(c) ? p.evalKeys(c, graph.frame) : p.val[c], p.animated(c) ? -1 : defaultInterp(*p.def));
    }
    n->touch();
    status("Keyframe set on " + n->name() + " / " + p.def->label + " at frame " + std::to_string(int(fr)));
}

void App::keySelected() {
    const float fr = std::floor(graph.frame + 0.5f);
    int count = 0;
    bool cp = false;
    for (int id : selection) {
        Node* n = net().findNode(id);
        if (!n) continue;
        for (auto& p : n->parms()) {
            if (!p.anyKeys()) continue;
            for (int c = 0; c < p.def->components(); ++c) {
                if (!p.animated(c) || !p.expr[c].empty()) continue;
                if (!cp) { checkpoint(); cp = true; }
                p.setKey(c, fr, p.evalKeys(c, graph.frame));
                ++count;
            }
            n->touch();
        }
    }
    if (count) status("Keyed " + std::to_string(count) + " animated channel" + (count == 1 ? "" : "s") + " at frame " + std::to_string(int(fr)));
    else status("Nothing animated on the selection yet. Alt+click a parameter name to set its first keyframe.", true);
}

void App::jumpToKey(int dir) {
    // keys of the selected nodes, or of every node at this level when nothing selected is animated
    std::set<int> frames;
    auto gather = [&](bool selectedOnly) {
        for (auto& n : net().nodes()) {
            if (selectedOnly && !selection.count(n->id())) continue;
            for (auto& p : n->parms())
                for (int c = 0; c < 3; ++c)
                    for (auto& k : p.keys[c]) frames.insert(int(std::lround(k.frame)));
        }
    };
    gather(true);
    if (frames.empty()) gather(false);
    if (frames.empty()) { status("There are no keyframes yet", true); return; }
    int cur = int(std::lround(graph.frame));
    if (dir > 0) {
        auto it = frames.upper_bound(cur);
        if (it != frames.end()) graph.frame = float(*it);
    } else {
        auto it = frames.lower_bound(cur);
        if (it != frames.begin()) graph.frame = float(*--it);
    }
}

void App::drawKeyMarks(float x0, float x1, float y0, float y1) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float span = float(std::max(1, graph.frameEnd - graph.frameStart));
    std::set<int> sel, other;
    for (auto& n : net().nodes()) {
        bool s = selection.count(n->id()) > 0;
        for (auto& p : n->parms())
            for (int c = 0; c < 3; ++c)
                for (auto& k : p.keys[c]) (s ? sel : other).insert(int(std::lround(k.frame)));
    }
    // light and camera keys live at the top level and matter wherever we are
    if (!netPath_.empty())
        for (auto& n : graph.nodes())
            for (auto& p : n->parms())
                for (int c = 0; c < 3; ++c)
                    for (auto& k : p.keys[c]) other.insert(int(std::lround(k.frame)));
    auto xOf = [&](int f) { return x0 + (x1 - x0) * std::clamp(float(f - graph.frameStart) / span, 0.f, 1.f); };
    for (int f : other)
        if (!sel.count(f)) dl->AddLine(ImVec2(xOf(f), y0 + 1), ImVec2(xOf(f), y0 + (y1 - y0) * 0.45f), IM_COL32(150, 150, 155, 200), 1.5f);
    for (int f : sel) dl->AddLine(ImVec2(xOf(f), y0 + 1), ImVec2(xOf(f), y1 - 4), IM_COL32(80, 235, 110, 255), 2.f);
}

// ---------------------------------------------------------------- animation editor
void App::drawAnimEditor() {
    if (!showAnimEditor) return;
    ImGui::SetNextWindowSize(ImVec2(760, 320), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Animation Editor", &showAnimEditor)) { ImGui::End(); return; }
    struct Row { Node* node; Parm* parm; int comp; std::string label; };
    std::vector<Row> rows;
    static const char* compName[3] = {"x", "y", "z"};
    for (auto& n : net().nodes())
        for (auto& p : n->parms())
            for (int c = 0; c < p.def->components(); ++c)
                if (p.animated(c))
                    rows.push_back({n.get(), &p, c, n->name() + " / " + p.def->label + (p.def->components() == 3 ? std::string(" ") + compName[c] : std::string())});
    ImGui::TextDisabled("Drag a key to move it in time. Right-click a key for its curve or to delete it. Click the ruler to change frame.");
    if (rows.empty()) {
        ImGui::Spacing();
        ImGui::TextUnformatted("Nothing is animated at this level yet.");
        ImGui::TextDisabled("Alt+click a parameter name in the Parameters pane to set its first keyframe,\nmove to another frame, change the value, and a second key is set for you.");
        ImGui::End();
        return;
    }
    const float labelW = 230, rowH = ImGui::GetTextLineHeight() + 6, rulerH = ImGui::GetTextLineHeight() + 4;
    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImGui::BeginChild("##dope", ImVec2(0, 0), ImGuiChildFlags_None);
    ImVec2 org = ImGui::GetCursorScreenPos();
    const float trackX = org.x + labelW, trackW = std::max(60.f, avail.x - labelW - 16);
    const float span = float(std::max(1, graph.frameEnd - graph.frameStart));
    auto xOf = [&](float f) { return trackX + trackW * (f - float(graph.frameStart)) / span; };
    auto frameAt = [&](float x) { return std::clamp(std::round(float(graph.frameStart) + (x - trackX) / trackW * span), float(graph.frameStart), float(graph.frameEnd)); };
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float totalH = rulerH + rowH * float(rows.size());
    ImGui::InvisibleButton("##area", ImVec2(labelW + trackW + 8, totalH), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 mouse = ImGui::GetIO().MousePos;

    // ruler
    dl->AddRectFilled(ImVec2(trackX, org.y), ImVec2(trackX + trackW, org.y + rulerH), IM_COL32(44, 44, 48, 255));
    int stepF = 1;
    while (trackW / span * float(stepF) < 46) stepF = stepF < 5 ? 5 : (stepF < 10 ? 10 : stepF * 2);
    for (int f = graph.frameStart; f <= graph.frameEnd; ++f) {
        if (f % stepF && f != graph.frameStart) continue;
        float x = xOf(float(f));
        dl->AddLine(ImVec2(x, org.y + rulerH - 5), ImVec2(x, org.y + totalH), IM_COL32(70, 70, 76, 120));
        char b[16]; std::snprintf(b, sizeof b, "%d", f);
        dl->AddText(ImVec2(x + 3, org.y + 1), IM_COL32(170, 170, 175, 255), b);
    }

    // rows and keys
    int hitRow = -1, hitKey = -1;
    for (size_t r = 0; r < rows.size(); ++r) {
        float y = org.y + rulerH + rowH * float(r);
        if (r % 2) dl->AddRectFilled(ImVec2(org.x, y), ImVec2(trackX + trackW, y + rowH), IM_COL32(255, 255, 255, 8));
        bool selRow = selection.count(rows[r].node->id()) > 0;
        dl->PushClipRect(ImVec2(org.x, y), ImVec2(trackX - 6, y + rowH), true);
        dl->AddText(ImVec2(org.x + 4, y + 3), selRow ? IM_COL32(120, 235, 140, 255) : IM_COL32(200, 200, 205, 255), rows[r].label.c_str());
        dl->PopClipRect();
        auto& keys = rows[r].parm->keys[rows[r].comp];
        float cy = y + rowH * 0.5f;
        for (size_t k = 0; k + 1 < keys.size(); ++k) // segment between keys, styled by curve type
            dl->AddLine(ImVec2(xOf(keys[k].frame), cy), ImVec2(xOf(keys[k + 1].frame), cy),
                        keys[k].interp == 2 ? IM_COL32(200, 150, 80, 160) : keys[k].interp == 1 ? IM_COL32(150, 150, 220, 160) : IM_COL32(90, 200, 120, 140), 2.f);
        for (size_t k = 0; k < keys.size(); ++k) {
            float x = xOf(keys[k].frame), q = 5.f;
            bool over = hovered && std::fabs(mouse.x - x) <= q + 2 && mouse.y >= y && mouse.y < y + rowH;
            if (over) { hitRow = int(r); hitKey = int(k); }
            ImVec2 pts[4] = {ImVec2(x, cy - q), ImVec2(x + q, cy), ImVec2(x, cy + q), ImVec2(x - q, cy)};
            dl->AddConvexPolyFilled(pts, 4, over ? IM_COL32(255, 255, 255, 255) : selRow ? IM_COL32(90, 235, 120, 255) : IM_COL32(210, 210, 215, 255));
            dl->AddPolyline(pts, 4, IM_COL32(20, 20, 22, 255), ImDrawFlags_Closed, 1.f);
        }
    }
    // current frame
    float cx = xOf(std::floor(graph.frame + 0.5f));
    dl->AddLine(ImVec2(cx, org.y), ImVec2(cx, org.y + totalH), IM_COL32(255, 205, 70, 230), 1.5f);

    // interaction
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (hitKey >= 0) {
            Row& r = rows[size_t(hitRow)];
            keyDrag_ = {true, r.node->name(), r.parm->def->name, r.comp, r.parm->keys[r.comp][size_t(hitKey)].frame};
            select(r.node);
            checkpoint();
        } else if (mouse.x >= trackX) {
            graph.frame = frameAt(mouse.x);
            keyDrag_.active = false;
        }
    }
    if (ImGui::IsItemActive() && !keyDrag_.active && ImGui::IsMouseDragging(ImGuiMouseButton_Left) && mouse.x >= trackX) graph.frame = frameAt(mouse.x); // scrub
    if (keyDrag_.active) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) keyDrag_.active = false;
        else if (Node* n = net().findNode(keyDrag_.node)) {
            if (Parm* p = n->parm(keyDrag_.parm)) {
                float nf = frameAt(mouse.x);
                int k = p->keyIndexAt(keyDrag_.comp, keyDrag_.frame);
                if (k >= 0 && nf != keyDrag_.frame && p->keyIndexAt(keyDrag_.comp, nf) < 0) { // never land on another key
                    Key moved = p->keys[keyDrag_.comp][size_t(k)];
                    p->keys[keyDrag_.comp].erase(p->keys[keyDrag_.comp].begin() + k);
                    p->setKey(keyDrag_.comp, nf, moved.value, moved.interp);
                    keyDrag_.frame = nf;
                    n->touch();
                    markDirty();
                }
            }
        }
    }
    static int ctxRow = -1; static float ctxFrame = 0;
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && hitKey >= 0) {
        ctxRow = hitRow;
        ctxFrame = rows[size_t(hitRow)].parm->keys[rows[size_t(hitRow)].comp][size_t(hitKey)].frame;
        ImGui::OpenPopup("##keyctx");
    }
    if (ImGui::BeginPopup("##keyctx")) {
        if (ctxRow >= 0 && ctxRow < int(rows.size())) {
            Row& r = rows[size_t(ctxRow)];
            int k = r.parm->keyIndexAt(r.comp, ctxFrame);
            ImGui::TextDisabled("%s  frame %d", r.label.c_str(), int(ctxFrame));
            ImGui::Separator();
            const char* names[3] = {"Smooth (ease in and out)", "Linear", "Constant (hold)"};
            for (int m = 0; m < 3 && k >= 0; ++m)
                if (ImGui::MenuItem(names[m], nullptr, r.parm->keys[r.comp][size_t(k)].interp == m)) {
                    checkpoint();
                    r.parm->keys[r.comp][size_t(k)].interp = m;
                    r.node->touch();
                }
            ImGui::Separator();
            if (ImGui::MenuItem("Go to This Frame")) graph.frame = ctxFrame;
            if (ImGui::MenuItem("Delete Key") && k >= 0) {
                checkpoint();
                r.parm->removeKey(r.comp, ctxFrame);
                r.node->touch();
            }
        }
        ImGui::EndPopup();
    }
    ImGui::EndChild();
    ImGui::End();
}

} // namespace hd
