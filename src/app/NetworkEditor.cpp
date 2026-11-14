#include "App.h"
#include <imgui.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

namespace hd {

namespace {
constexpr float kNodeW = 150.f;
constexpr float kNodeH = 30.f;
constexpr float kFlagW = 16.f;

ImU32 categoryColor(const std::string& cat) {
    if (cat == "Generators") return IM_COL32(64, 112, 120, 255);
    if (cat == "Deformers") return IM_COL32(102, 84, 132, 255);
    if (cat == "Attributes") return IM_COL32(140, 104, 58, 255);
    if (cat == "Points") return IM_COL32(72, 118, 78, 255);
    if (cat == "Utility") return IM_COL32(88, 90, 96, 255);
    if (cat == "Simulation") return IM_COL32(150, 70, 78, 255);
    return IM_COL32(78, 92, 112, 255); // Modifiers
}

bool icontains(const std::string& hay, const char* needle) {
    if (!*needle) return true;
    std::string h = hay, n = needle;
    std::transform(h.begin(), h.end(), h.begin(), ::tolower);
    std::transform(n.begin(), n.end(), n.begin(), ::tolower);
    return h.find(n) != std::string::npos;
}

struct Hit {
    enum Kind { None, Body, Input, Output, Display, Bypass } kind = None;
    Node* node = nullptr;
    int input = -1;
};
} // namespace

void NetworkEditor::frameAll(App& app) {
    const auto& nodes = app.graph.nodes();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (nodes.empty() || avail.x < 10 || avail.y < 10) { offset_ = {avail.x * 0.5f, 60}; zoom_ = 1; return; }
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (auto& n : nodes) {
        x0 = std::min(x0, n->x); y0 = std::min(y0, n->y);
        x1 = std::max(x1, n->x + kNodeW + 110); y1 = std::max(y1, n->y + kNodeH);
    }
    float w = x1 - x0 + 80, h = y1 - y0 + 80;
    zoom_ = std::clamp(std::min(avail.x / w, avail.y / h), 0.3f, 1.4f);
    offset_.x = avail.x * 0.5f - ((x0 + x1) * 0.5f) * zoom_;
    offset_.y = avail.y * 0.5f - ((y0 + y1) * 0.5f) * zoom_;
}

void NetworkEditor::draw(App& app) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    bool open = ImGui::Begin("Network", nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    if (!open) { ImGui::End(); return; }

    Graph& graph = app.graph;
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImVec2 size = ImGui::GetContentRegionAvail();
    if (size.x < 4 || size.y < 4) { ImGui::End(); return; }
    // Wait a couple of frames so docking has settled the window size before framing.
    if (wantFrameAll_ && ++frameAllDelay_ > 2) { frameAll(app); wantFrameAll_ = false; frameAllDelay_ = 0; }

    ImGui::InvisibleButton("##canvas", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    bool hovered = ImGui::IsItemHovered();
    bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

    auto toScreen = [&](float x, float y) { return ImVec2(origin.x + offset_.x + x * zoom_, origin.y + offset_.y + y * zoom_); };
    auto toWorld = [&](ImVec2 s) { return Vec2{(s.x - origin.x - offset_.x) / zoom_, (s.y - origin.y - offset_.y) / zoom_}; };
    auto inputPos = [&](const Node* n, int i) {
        int cnt = int(n->inputs.size());
        return toScreen(n->x + kNodeW * float(i + 1) / float(cnt + 1), n->y);
    };
    auto outputPos = [&](const Node* n) { return toScreen(n->x + kNodeW * 0.5f, n->y + kNodeH); };

    auto hitTest = [&](ImVec2 m) {
        Hit h;
        const auto& nodes = graph.nodes();
        float pr = std::max(7.f, 9.f * zoom_);
        for (auto it = nodes.rbegin(); it != nodes.rend(); ++it) {
            Node* n = it->get();
            ImVec2 o = outputPos(n);
            if (std::hypot(m.x - o.x, m.y - o.y) < pr) { h.kind = Hit::Output; h.node = n; return h; }
            for (int i = 0; i < int(n->inputs.size()); ++i) {
                ImVec2 p = inputPos(n, i);
                if (std::hypot(m.x - p.x, m.y - p.y) < pr) { h.kind = Hit::Input; h.node = n; h.input = i; return h; }
            }
            ImVec2 a = toScreen(n->x, n->y), b = toScreen(n->x + kNodeW, n->y + kNodeH);
            if (m.x >= a.x && m.x <= b.x && m.y >= a.y && m.y <= b.y) {
                h.node = n;
                if (m.x < a.x + kFlagW * zoom_) h.kind = Hit::Bypass;
                else if (m.x > b.x - kFlagW * zoom_) h.kind = Hit::Display;
                else h.kind = Hit::Body;
                return h;
            }
        }
        return h;
    };

    // ---------------------------------------------------------- background grid
    dl->PushClipRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), true);
    dl->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), IM_COL32(38, 39, 42, 255));
    float step = 40.f * zoom_;
    if (step > 8) {
        for (float x = std::fmod(offset_.x, step); x < size.x; x += step)
            dl->AddLine(ImVec2(origin.x + x, origin.y), ImVec2(origin.x + x, origin.y + size.y), IM_COL32(48, 49, 53, 255));
        for (float y = std::fmod(offset_.y, step); y < size.y; y += step)
            dl->AddLine(ImVec2(origin.x, origin.y + y), ImVec2(origin.x + size.x, origin.y + y), IM_COL32(48, 49, 53, 255));
    }

    // ---------------------------------------------------------- input handling
    bool popupOpen = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
    ImVec2 mouse = io.MousePos;
    if (hovered && io.MouseWheel != 0 && !popupOpen) {
        Vec2 before = toWorld(mouse);
        zoom_ = std::clamp(zoom_ * std::pow(1.15f, io.MouseWheel), 0.2f, 2.5f);
        offset_.x = mouse.x - origin.x - before.x * zoom_;
        offset_.y = mouse.y - origin.y - before.y * zoom_;
    }
    if (hovered && !popupOpen) {
        bool panClick = ImGui::IsMouseClicked(ImGuiMouseButton_Middle) || (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && io.KeyAlt);
        if (panClick) {
            drag_ = Drag::Pan;
        } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            Hit h = hitTest(mouse);
            switch (h.kind) {
            case Hit::Output:
                drag_ = Drag::Wire;
                wireSrc_ = h.node->id();
                wireDetachedDst_ = -1;
                break;
            case Hit::Input:
                if (Node* src = h.node->inputs[size_t(h.input)]) {
                    app.checkpoint();
                    graph.disconnect(h.node, h.input);
                    drag_ = Drag::Wire;
                    wireSrc_ = src->id();
                    wireDetachedDst_ = h.node->id();
                    wireDetachedIdx_ = h.input;
                } else {
                    app.select(h.node);
                    drag_ = Drag::Move;
                    moveCheckpointed_ = false;
                }
                break;
            case Hit::Display:
                app.checkpoint();
                app.setDisplay(h.node);
                app.select(h.node);
                break;
            case Hit::Bypass:
                app.checkpoint();
                h.node->bypass = !h.node->bypass;
                break;
            case Hit::Body:
                if (io.KeyShift || io.KeyCtrl) app.select(h.node, true);
                else if (!app.selection.count(h.node->id())) app.select(h.node);
                else app.currentId = h.node->id();
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    renameId_ = h.node->id();
                    std::snprintf(renameBuf_, sizeof renameBuf_, "%s", h.node->name().c_str());
                    ImGui::OpenPopup("Rename Node");
                } else {
                    drag_ = Drag::Move;
                    moveCheckpointed_ = false;
                }
                break;
            case Hit::None:
                if (!io.KeyShift && !io.KeyCtrl) app.clearSelection();
                drag_ = Drag::Box;
                boxStart_ = toWorld(mouse);
                break;
            }
        } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            Hit h = hitTest(mouse);
            if (h.node) {
                if (!app.selection.count(h.node->id())) app.select(h.node);
                ImGui::OpenPopup("Node Context");
            } else {
                openMenu_ = true;
                menuWorld_ = toWorld(mouse);
                menuConnectFrom_ = -1;
            }
        }
    }

    // drag updates
    if (drag_ == Drag::Pan) {
        offset_.x += io.MouseDelta.x;
        offset_.y += io.MouseDelta.y;
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Middle) && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) drag_ = Drag::None;
    } else if (drag_ == Drag::Move) {
        if (io.MouseDelta.x != 0 || io.MouseDelta.y != 0) {
            if (!moveCheckpointed_) { app.checkpoint(); moveCheckpointed_ = true; }
            for (int id : app.selection)
                if (Node* n = graph.findNode(id)) { n->x += io.MouseDelta.x / zoom_; n->y += io.MouseDelta.y / zoom_; }
        }
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) drag_ = Drag::None;
    } else if (drag_ == Drag::Box) {
        Vec2 a = boxStart_, b = toWorld(mouse);
        ImVec2 sa = toScreen(a.x, a.y), sb = mouse;
        dl->AddRectFilled(ImVec2(std::min(sa.x, sb.x), std::min(sa.y, sb.y)), ImVec2(std::max(sa.x, sb.x), std::max(sa.y, sb.y)), IM_COL32(230, 200, 60, 30));
        dl->AddRect(ImVec2(std::min(sa.x, sb.x), std::min(sa.y, sb.y)), ImVec2(std::max(sa.x, sb.x), std::max(sa.y, sb.y)), IM_COL32(230, 200, 60, 200));
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            float x0 = std::min(a.x, b.x), x1 = std::max(a.x, b.x), y0 = std::min(a.y, b.y), y1 = std::max(a.y, b.y);
            if (x1 - x0 > 2 || y1 - y0 > 2) {
                for (auto& n : graph.nodes())
                    if (n->x + kNodeW >= x0 && n->x <= x1 && n->y + kNodeH >= y0 && n->y <= y1) {
                        app.selection.insert(n->id());
                        app.currentId = n->id();
                    }
            }
            drag_ = Drag::None;
        }
    } else if (drag_ == Drag::Wire) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            drag_ = Drag::None;
            Node* src = graph.findNode(wireSrc_);
            Hit h = hitTest(mouse);
            bool detached = wireDetachedDst_ >= 0;
            if (src && h.node && h.node != src && (h.kind == Hit::Input || h.kind == Hit::Body || h.kind == Hit::Display || h.kind == Hit::Bypass)) {
                int idx = h.input;
                if (idx < 0) {
                    for (int i = 0; i < int(h.node->inputs.size()); ++i)
                        if (!h.node->inputs[size_t(i)]) { idx = i; break; }
                    if (idx < 0 && !h.node->inputs.empty()) idx = 0;
                }
                if (idx >= 0) {
                    if (!detached) app.checkpoint();
                    std::string err;
                    if (!graph.connect(src, h.node, idx, &err)) app.status("Cannot connect: " + err, true);
                    else if (graph.displayNode() == src) app.setDisplay(h.node);
                } else {
                    app.status(h.node->type().label + " has no inputs", true);
                }
            } else if (src && h.kind == Hit::None && hovered && !detached) {
                openMenu_ = true;
                menuWorld_ = toWorld(mouse);
                menuConnectFrom_ = src->id();
            }
            wireDetachedDst_ = -1;
        }
    }

    // keyboard
    if (focused && !io.WantTextInput && !popupOpen) {
        if (ImGui::IsKeyPressed(ImGuiKey_Tab, false) && hovered) {
            openMenu_ = true;
            menuWorld_ = toWorld(mouse);
            menuConnectFrom_ = -1;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) app.deleteSelected();
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_D)) app.duplicateSelected();
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_A)) {
            for (auto& n : graph.nodes()) app.selection.insert(n->id());
        }
        if (ImGui::IsKeyPressed(ImGuiKey_H, false) && !io.KeyCtrl) frameAll(app);
        if (ImGui::IsKeyPressed(ImGuiKey_F2, false))
            if (Node* c = app.current()) {
                renameId_ = c->id();
                std::snprintf(renameBuf_, sizeof renameBuf_, "%s", c->name().c_str());
                ImGui::OpenPopup("Rename Node");
            }
    }

    // ---------------------------------------------------------- wires
    for (auto& up : graph.nodes()) {
        Node* n = up.get();
        for (int i = 0; i < int(n->inputs.size()); ++i) {
            Node* src = n->inputs[size_t(i)];
            if (!src) continue;
            ImVec2 a = outputPos(src), b = inputPos(n, i);
            float d = std::max(30.f * zoom_, std::fabs(b.y - a.y) * 0.5f);
            bool hi = app.selection.count(n->id()) || app.selection.count(src->id());
            ImU32 col = hi ? IM_COL32(235, 210, 120, 255) : IM_COL32(150, 152, 158, 255);
            dl->AddBezierCubic(a, ImVec2(a.x, a.y + d), ImVec2(b.x, b.y - d), b, col, std::max(1.f, 2.f * zoom_));
        }
    }
    if (drag_ == Drag::Wire)
        if (Node* src = graph.findNode(wireSrc_)) {
            ImVec2 a = outputPos(src), b = mouse;
            float d = std::max(30.f * zoom_, std::fabs(b.y - a.y) * 0.5f);
            dl->AddBezierCubic(a, ImVec2(a.x, a.y + d), ImVec2(b.x, b.y - d), b, IM_COL32(255, 220, 90, 255), 2.f * zoom_);
        }

    // ---------------------------------------------------------- nodes
    ImFont* font = ImGui::GetFont();
    float fs = ImGui::GetFontSize() * zoom_;
    Node* hoverNode = nullptr;
    if (hovered && drag_ == Drag::None && !popupOpen) hoverNode = hitTest(mouse).node;
    for (auto& up : graph.nodes()) {
        Node* n = up.get();
        ImVec2 a = toScreen(n->x, n->y), b = toScreen(n->x + kNodeW, n->y + kNodeH);
        if (b.x < origin.x - 200 || a.x > origin.x + size.x + 50 || b.y < origin.y - 50 || a.y > origin.y + size.y + 50) continue;
        bool sel = app.selection.count(n->id()) > 0;
        bool isDisplay = graph.displayNode() == n;
        float r = 5.f * zoom_;
        ImU32 body = categoryColor(n->type().category);
        if (n->bypass) body = IM_COL32(110, 100, 50, 255);
        dl->AddRectFilled(ImVec2(a.x + 3 * zoom_, a.y + 4 * zoom_), ImVec2(b.x + 3 * zoom_, b.y + 4 * zoom_), IM_COL32(0, 0, 0, 70), r);
        dl->AddRectFilled(a, b, body, r);
        // flags
        ImU32 bypassCol = n->bypass ? IM_COL32(240, 205, 50, 255) : IM_COL32(0, 0, 0, 50);
        ImU32 displayCol = isDisplay ? IM_COL32(70, 140, 255, 255) : IM_COL32(0, 0, 0, 50);
        dl->AddRectFilled(a, ImVec2(a.x + kFlagW * zoom_, b.y), bypassCol, r, ImDrawFlags_RoundCornersLeft);
        dl->AddRectFilled(ImVec2(b.x - kFlagW * zoom_, a.y), b, displayCol, r, ImDrawFlags_RoundCornersRight);
        // border
        ImU32 border = IM_COL32(20, 20, 22, 255);
        float bw = 1.f;
        if (!n->error().empty()) { border = IM_COL32(235, 60, 60, 255); bw = 2.f; }
        else if (!n->warning().empty()) { border = IM_COL32(240, 160, 40, 255); bw = 1.5f; }
        if (sel) { border = IM_COL32(255, 222, 80, 255); bw = 2.5f; }
        dl->AddRect(a, b, border, r, 0, bw * std::max(0.6f, zoom_));
        // label
        if (zoom_ > 0.35f) {
            const std::string& label = n->type().label;
            ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0, label.c_str());
            float maxW = (kNodeW - 2 * kFlagW - 6) * zoom_;
            ImVec2 tp(a.x + (b.x - a.x - std::min(ts.x, maxW)) * 0.5f, a.y + (b.y - a.y - ts.y) * 0.5f);
            dl->PushClipRect(ImVec2(a.x + kFlagW * zoom_, a.y), ImVec2(b.x - kFlagW * zoom_, b.y), true);
            dl->AddText(font, fs, tp, IM_COL32(235, 235, 238, 255), label.c_str());
            dl->PopClipRect();
            // name (and info) to the right
            ImVec2 np(b.x + 8 * zoom_, a.y + (kNodeH * zoom_ - fs) * 0.5f - (zoom_ > 0.7f ? fs * 0.4f : 0));
            dl->AddText(font, fs, np, isDisplay ? IM_COL32(150, 195, 255, 255) : IM_COL32(225, 225, 228, 255), n->name().c_str());
            if (zoom_ > 0.7f) {
                char info[96];
                if (!n->error().empty()) std::snprintf(info, sizeof info, "error");
                else if (n->cookStamp()) std::snprintf(info, sizeof info, "%zu pts  %.1f ms", n->geometry().numPoints(), n->cookTimeMs());
                else info[0] = 0;
                dl->AddText(font, fs * 0.82f, ImVec2(np.x, np.y + fs * 1.05f), n->error().empty() ? IM_COL32(140, 142, 148, 255) : IM_COL32(240, 90, 90, 255), info);
            }
            if (!n->error().empty()) {
                ImVec2 c(a.x - 10 * zoom_, a.y + kNodeH * zoom_ * 0.5f);
                dl->AddCircleFilled(c, 7 * zoom_, IM_COL32(220, 50, 50, 255));
                dl->AddText(font, fs * 0.9f, ImVec2(c.x - fs * 0.15f, c.y - fs * 0.45f), IM_COL32_WHITE, "!");
            }
        }
        // ports
        float pr = std::max(2.5f, 4.5f * zoom_);
        for (int i = 0; i < int(n->inputs.size()); ++i) {
            ImVec2 p = inputPos(n, i);
            dl->AddCircleFilled(p, pr, n->inputs[size_t(i)] ? IM_COL32(200, 200, 205, 255) : IM_COL32(90, 92, 98, 255));
            dl->AddCircle(p, pr, IM_COL32(20, 20, 22, 255));
        }
        ImVec2 o = outputPos(n);
        dl->AddCircleFilled(o, pr, IM_COL32(200, 200, 205, 255));
        dl->AddCircle(o, pr, IM_COL32(20, 20, 22, 255));
    }
    dl->PopClipRect();

    // tooltip
    if (hoverNode && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        Hit h = hitTest(mouse);
        ImGui::BeginTooltip();
        if (h.kind == Hit::Display) ImGui::TextUnformatted("Display flag: show this node in the viewport");
        else if (h.kind == Hit::Bypass) ImGui::TextUnformatted("Bypass flag: pass input through unchanged");
        else if (h.kind == Hit::Input) {
            const auto& labels = h.node->type().inputLabels;
            ImGui::Text("Input %d%s%s", h.input + 1, h.input < int(labels.size()) ? ": " : "", h.input < int(labels.size()) ? labels[size_t(h.input)].c_str() : "");
        } else {
            ImGui::Text("%s  (%s)", hoverNode->name().c_str(), hoverNode->type().label.c_str());
            ImGui::TextDisabled("%s", hoverNode->type().description.c_str());
            if (hoverNode->cookStamp()) {
                ImGui::Text("%zu points, %zu primitives, %.2f ms", hoverNode->geometry().numPoints(), hoverNode->geometry().numPrims(), hoverNode->cookTimeMs());
                if (hoverNode->isTimeDependent()) ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1), "Time dependent");
            }
            if (!hoverNode->error().empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Error: %s", hoverNode->error().c_str());
            if (!hoverNode->warning().empty()) ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "Warning: %s", hoverNode->warning().c_str());
        }
        ImGui::EndTooltip();
    }

    // hint when empty
    if (graph.nodes().empty()) {
        const char* hint = "Press Tab or right-click to add a node";
        ImVec2 ts = ImGui::CalcTextSize(hint);
        dl->AddText(ImVec2(origin.x + (size.x - ts.x) * 0.5f, origin.y + size.y * 0.5f), IM_COL32(140, 140, 145, 255), hint);
    }

    // ---------------------------------------------------------- popups
    bool openRename = false;
    if (ImGui::BeginPopup("Node Context")) {
        Node* c = app.current();
        if (c) {
            ImGui::TextDisabled("%s", c->name().c_str());
            ImGui::Separator();
            if (ImGui::MenuItem("Display", nullptr, graph.displayNode() == c)) { app.checkpoint(); app.setDisplay(c); }
            if (ImGui::MenuItem("Bypass", nullptr, c->bypass)) { app.checkpoint(); c->bypass = !c->bypass; }
            if (ImGui::MenuItem("Rename...", "F2")) {
                renameId_ = c->id();
                std::snprintf(renameBuf_, sizeof renameBuf_, "%s", c->name().c_str());
                openRename = true;
            }
            if (ImGui::MenuItem("Duplicate", "Ctrl+D")) app.duplicateSelected();
            if (ImGui::MenuItem("Delete", "Del")) app.deleteSelected();
        }
        ImGui::EndPopup();
    }
    if (openRename) ImGui::OpenPopup("Rename Node");
    if (ImGui::BeginPopupModal("Rename Node", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        bool enter = ImGui::InputText("##rename", renameBuf_, sizeof renameBuf_, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        ImGui::TextDisabled("Letters, digits and _ only");
        if (enter || ImGui::Button("OK", ImVec2(90, 0))) {
            if (Node* n = graph.findNode(renameId_)) {
                app.checkpoint();
                if (!graph.rename(n, renameBuf_)) app.status("Invalid or duplicate name: " + std::string(renameBuf_), true);
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(90, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    drawNodeMenu(app);
    ImGui::End();
}

void NetworkEditor::drawNodeMenu(App& app) {
    if (openMenu_) {
        ImGui::OpenPopup("Add Node");
        openMenu_ = false;
        search_[0] = 0;
        menuHighlight_ = 0;
    }
    ImGui::SetNextWindowSizeConstraints(ImVec2(240, 0), ImVec2(320, 520));
    if (!ImGui::BeginPopup("Add Node")) return;
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-1);
    bool enter = ImGui::InputTextWithHint("##search", "Search nodes...", search_, sizeof search_, ImGuiInputTextFlags_EnterReturnsTrue);
    if (ImGui::IsItemEdited()) menuHighlight_ = 0;

    std::vector<const NodeType*> matches;
    static const char* cats[] = {"Generators", "Modifiers", "Deformers", "Attributes", "Points", "Simulation", "Utility"};
    for (const char* cat : cats)
        for (auto& t : nodeTypes())
            if (t.category == cat && (icontains(t.label, search_) || icontains(t.name, search_))) matches.push_back(&t);
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) menuHighlight_ = std::min(menuHighlight_ + 1, int(matches.size()) - 1);
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) menuHighlight_ = std::max(menuHighlight_ - 1, 0);

    const NodeType* chosen = nullptr;
    if (enter && !matches.empty()) chosen = matches[size_t(std::clamp(menuHighlight_, 0, int(matches.size()) - 1))];
    int ncat = 0;
    { std::string lc; for (auto* t : matches) if (t->category != lc) { lc = t->category; ++ncat; } }
    float rowH = ImGui::GetTextLineHeightWithSpacing();
    ImGui::BeginChild("##list", ImVec2(0, std::min(440.f, rowH * float(matches.size() + size_t(ncat) * 1.4f + 1.f))), ImGuiChildFlags_None);
    std::string lastCat;
    for (int i = 0; i < int(matches.size()); ++i) {
        const NodeType* t = matches[size_t(i)];
        if (t->category != lastCat) {
            lastCat = t->category;
            ImGui::SeparatorText(lastCat.c_str());
        }
        bool hi = i == menuHighlight_;
        if (ImGui::Selectable(t->label.c_str(), hi)) chosen = t;
        if (hi && ImGui::IsKeyPressed(ImGuiKey_DownArrow, true)) ImGui::SetScrollHereY();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", t->description.c_str());
    }
    if (matches.empty()) ImGui::TextDisabled("No matching nodes");
    ImGui::EndChild();
    if (chosen) {
        app.checkpoint();
        Node* src = app.graph.findNode(menuConnectFrom_);
        float x = menuWorld_.x - kNodeW * 0.5f, y = menuWorld_.y;
        if (src && menuConnectFrom_ >= 0 && std::fabs(menuWorld_.y - src->y) < 1) y = src->y + 70;
        app.createNode(chosen->name, x, y, menuConnectFrom_);
        ImGui::CloseCurrentPopup();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

} // namespace hd
