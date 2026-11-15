#include "App.h"
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>
#include <cmath>
#include <cstdio>

namespace hd {

namespace {
std::string fmtFloat(float v) {
    char b[32];
    std::snprintf(b, sizeof b, "%g", double(v));
    return b;
}
ImVec4 kExprBg(0.16f, 0.36f, 0.18f, 1.f);
} // namespace

void App::drawParameters() {
    if (!ImGui::Begin("Parameters", &showParms)) { ImGui::End(); return; }
    Node* n = current();
    if (!n) {
        ImGui::TextDisabled("Select a node in the Network to edit its parameters.");
        ImGui::Spacing();
        ImGui::TextDisabled("Tip: press Tab in the Network to add nodes.");
        ImGui::End();
        return;
    }
    const NodeType& t = n->type();

    // ---- header
    ImGui::TextColored(ImVec4(0.65f, 0.8f, 1.f, 1), "%s", t.label.c_str());
    ImGui::SameLine();
    std::string name = n->name();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 150);
    if (ImGui::InputText("##nodename", &name, ImGuiInputTextFlags_EnterReturnsTrue)) {
        checkpoint();
        if (!net().rename(n, name)) status("Invalid or duplicate name: " + name, true);
    }
    ImGui::SameLine();
    bool isDisplay = net().displayNode() == n;
    ImGui::PushStyleColor(ImGuiCol_Button, isDisplay ? ImVec4(0.25f, 0.5f, 1.f, 1) : ImGui::GetStyle().Colors[ImGuiCol_Button]);
    if (ImGui::Button("Display")) { checkpoint(); setDisplay(n); }
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, n->bypass ? ImVec4(0.8f, 0.68f, 0.15f, 1) : ImGui::GetStyle().Colors[ImGuiCol_Button]);
    if (ImGui::Button("Bypass")) { checkpoint(); n->bypass = !n->bypass; }
    ImGui::PopStyleColor();
    ImGui::TextDisabled("%s", t.description.c_str());
    if (!n->error().empty()) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.35f, 0.08f, 0.08f, 1));
        ImGui::BeginChild("##err", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders);
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(ImVec4(1, 0.55f, 0.55f, 1), "Error: %s", n->error().c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndChild();
        ImGui::PopStyleColor();
    } else if (!n->warning().empty()) {
        ImGui::TextColored(ImVec4(1, 0.75f, 0.35f, 1), "Warning: %s", n->warning().c_str());
    }
    if (n->subnet()) {
        if (ImGui::Button("Dive In")) { diveInto(n); ImGui::End(); return; }
        ImGui::SameLine();
        if (ImGui::Button("Save As Asset")) saveAsset(n);
        ImGui::SameLine();
        ImGui::TextDisabled("%zu nodes inside", n->subnet()->nodes().size());
    }
    ImGui::Separator();

    // ---- parameters
    bool any = false;
    if (ImGui::BeginTable("##parms", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, 130);
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
        for (size_t pi = 0; pi < n->parms().size(); ++pi) {
            Parm& p = n->parms()[pi];
            const ParmDef& d = *p.def;
            any = true;
            if (d.type == ParmType::Code) continue; // drawn full width below
            ImGui::PushID(int(pi));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            bool hasExpr = !p.expr[0].empty() || !p.expr[1].empty() || !p.expr[2].empty();
            const bool keyed = p.anyKeys() && !hasExpr;
            const float curFrame = std::floor(graph.frame + 0.5f);
            if (hasExpr) ImGui::TextColored(ImVec4(0.55f, 0.95f, 0.55f, 1), "%s", d.label.c_str());
            else if (keyed) ImGui::TextColored(ImVec4(0.45f, 0.9f, 0.95f, 1), "%s", d.label.c_str());
            else if (!p.isDefault()) ImGui::TextUnformatted(d.label.c_str());
            else ImGui::TextColored(ImVec4(0.75f, 0.75f, 0.78f, 1), "%s", d.label.c_str());
            if (ImGui::IsItemHovered() && !d.help.empty() && d.help != "color") ImGui::SetTooltip("%s", d.help.c_str());
            bool numeric = d.type == ParmType::Float || d.type == ParmType::Int || d.type == ParmType::Vec3 || d.type == ParmType::Toggle || d.type == ParmType::Menu;
            // Alt+click the label: set a keyframe here (as in Houdini)
            if (numeric && !hasExpr && ImGui::IsItemClicked(ImGuiMouseButton_Left) && ImGui::GetIO().KeyAlt) setKeyframe(n, p);
            if (ImGui::IsItemHovered() && numeric && !hasExpr && d.help.empty()) ImGui::SetTooltip("Alt+click to set a keyframe. Right-click for more.");
            if (ImGui::BeginPopupContextItem("##parmctx")) {
                ImGui::TextDisabled("%s  (%s)", d.label.c_str(), d.name.c_str());
                ImGui::Separator();
                if (ImGui::MenuItem("Revert to Default", nullptr, false, !p.isDefault())) {
                    checkpoint();
                    for (int c = 0; c < 3; ++c) { p.val[c] = d.def[c]; p.expr[c].clear(); p.keys[c].clear(); }
                    p.str = d.defStr;
                    n->touch();
                }
                if (numeric && !hasExpr) {
                    ImGui::Separator();
                    bool onKey = false;
                    for (int c = 0; c < d.components(); ++c) onKey |= p.keyIndexAt(c, curFrame) >= 0;
                    if (ImGui::MenuItem("Set Keyframe", "Alt+click")) setKeyframe(n, p);
                    if (ImGui::MenuItem("Remove Keyframe", nullptr, false, onKey)) {
                        checkpoint();
                        for (int c = 0; c < d.components(); ++c) p.removeKey(c, curFrame);
                        n->touch();
                    }
                    if (ImGui::BeginMenu("Keyframe Curve", onKey)) {
                        const char* names[3] = {"Smooth (ease in and out)", "Linear", "Constant (hold)"};
                        for (int m = 0; m < 3; ++m)
                            if (ImGui::MenuItem(names[m])) {
                                checkpoint();
                                for (int c = 0; c < d.components(); ++c) { int k = p.keyIndexAt(c, curFrame); if (k >= 0) p.keys[c][size_t(k)].interp = m; }
                                n->touch();
                            }
                        ImGui::EndMenu();
                    }
                    if (ImGui::MenuItem("Delete Animation", nullptr, false, keyed)) {
                        checkpoint();
                        for (int c = 0; c < d.components(); ++c) {
                            if (p.animated(c)) p.val[c] = p.evalKeys(c, graph.frame);
                            p.keys[c].clear();
                        }
                        n->touch();
                    }
                }
                if (numeric && (d.type == ParmType::Float || d.type == ParmType::Int || d.type == ParmType::Vec3)) {
                    const char* comp[3] = {"X", "Y", "Z"};
                    for (int c = 0; c < d.components(); ++c) {
                        std::string label = d.components() == 3 ? std::string("Add Expression (") + comp[c] + ")" : "Add Expression";
                        if (p.expr[c].empty() && ImGui::MenuItem(label.c_str())) {
                            checkpoint();
                            p.expr[c] = fmtFloat(p.val[c]);
                            n->touch();
                        }
                    }
                    if (hasExpr && ImGui::MenuItem("Delete Expressions")) {
                        checkpoint();
                        for (int c = 0; c < 3; ++c) {
                            if (!p.expr[c].empty()) p.val[c] = n->evalFloat(p, c, graph.frame, graph.fps);
                            p.expr[c].clear();
                        }
                        n->touch();
                    }
                    ImGui::Separator();
                    ImGui::TextDisabled("Expressions: $F $T $FPS $PI, sin(), fit(), rand()...");
                }
                ImGui::EndPopup();
            }

            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1);
            bool changed = false;
            auto exprField = [&](int c, float width) {
                ImGui::SetNextItemWidth(width);
                ImGui::PushStyleColor(ImGuiCol_FrameBg, kExprBg);
                ImGui::PushID(c);
                bool ch = ImGui::InputText("##expr", &p.expr[c]);
                if (ImGui::IsItemActivated()) checkpoint();
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("= %g", double(n->evalFloat(p, c, graph.frame, graph.fps)));
                ImGui::PopID();
                ImGui::PopStyleColor();
                return ch;
            };
            float speed = (d.max - d.min) / 300.f;
            if (speed <= 0) speed = 0.01f;
            bool drewKeyed = false;
            if (keyed && numeric) {
                // Animated parameter: shows the value at this frame; editing it keys this frame.
                // Green = a key sits on this frame, blue = animated but between keys.
                drewKeyed = true;
                const ImVec4 onKeyCol(0.13f, 0.42f, 0.2f, 1), betweenCol(0.12f, 0.33f, 0.45f, 1);
                auto frameCol = [&](int c) { return p.keyIndexAt(c, curFrame) >= 0 ? onKeyCol : betweenCol; };
                if (d.type == ParmType::Toggle) {
                    bool v = p.evalKeys(0, graph.frame) != 0;
                    ImGui::PushStyleColor(ImGuiCol_FrameBg, frameCol(0));
                    if (ImGui::Checkbox("##v", &v)) { checkpoint(); p.setKey(0, curFrame, v ? 1.f : 0.f); changed = true; }
                    ImGui::PopStyleColor();
                } else if (d.type == ParmType::Menu) {
                    int v = std::clamp(int(std::lround(p.evalKeys(0, graph.frame))), 0, int(d.menu.size()) - 1);
                    ImGui::PushStyleColor(ImGuiCol_FrameBg, frameCol(0));
                    if (ImGui::BeginCombo("##v", d.menu[size_t(v)].c_str())) {
                        for (int i = 0; i < int(d.menu.size()); ++i)
                            if (ImGui::Selectable(d.menu[size_t(i)].c_str(), i == v) && i != v) { checkpoint(); p.setKey(0, curFrame, float(i)); changed = true; }
                        ImGui::EndCombo();
                    }
                    ImGui::PopStyleColor();
                } else {
                    int nc = d.components();
                    float w = (ImGui::GetContentRegionAvail().x - float(nc - 1) * ImGui::GetStyle().ItemInnerSpacing.x) / float(nc);
                    for (int c = 0; c < nc; ++c) {
                        if (c) ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
                        ImGui::PushID(c + 20);
                        ImGui::SetNextItemWidth(w);
                        bool an = p.animated(c);
                        if (an) ImGui::PushStyleColor(ImGuiCol_FrameBg, frameCol(c));
                        float v = an ? p.evalKeys(c, graph.frame) : p.val[c];
                        bool ch = ImGui::DragFloat("##k", &v, d.type == ParmType::Int ? std::max(0.05f, (d.max - d.min) / 200.f) : speed, 0, 0, d.type == ParmType::Int ? "%.0f" : "%.4g");
                        if (ImGui::IsItemActivated()) checkpoint();
                        if (an) ImGui::PopStyleColor();
                        if (ch) { p.setValue(c, d.type == ParmType::Int ? std::round(v) : v, curFrame); changed = true; }
                        ImGui::PopID();
                    }
                }
            }
            if (!drewKeyed) switch (d.type) {
            case ParmType::Float:
                if (!p.expr[0].empty()) changed = exprField(0, -1);
                else {
                    changed = ImGui::DragFloat("##v", &p.val[0], speed, 0, 0, "%.4g");
                    if (ImGui::IsItemActivated()) checkpoint();
                }
                break;
            case ParmType::Int:
                if (!p.expr[0].empty()) changed = exprField(0, -1);
                else {
                    int v = int(p.val[0]);
                    changed = ImGui::DragInt("##v", &v, std::max(0.05f, (d.max - d.min) / 200.f), int(d.min) < 0 ? 0 : int(d.min), 0);
                    if (ImGui::IsItemActivated()) checkpoint();
                    if (changed) p.val[0] = float(v);
                }
                break;
            case ParmType::Toggle: {
                bool v = p.val[0] != 0;
                changed = ImGui::Checkbox("##v", &v);
                if (ImGui::IsItemActivated()) checkpoint();
                if (changed) p.val[0] = v ? 1.f : 0.f;
                break;
            }
            case ParmType::Vec3: {
                bool exprAny = hasExpr;
                if (d.help == "color" && !exprAny) {
                    changed = ImGui::ColorEdit3("##v", p.val, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
                    if (ImGui::IsItemActivated()) checkpoint();
                } else if (!exprAny) {
                    changed = ImGui::DragFloat3("##v", p.val, speed, 0, 0, "%.4g");
                    if (ImGui::IsItemActivated()) checkpoint();
                } else {
                    float w = (ImGui::GetContentRegionAvail().x - 2 * ImGui::GetStyle().ItemInnerSpacing.x) / 3.f;
                    for (int c = 0; c < 3; ++c) {
                        if (c) ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
                        if (!p.expr[c].empty()) changed |= exprField(c, w);
                        else {
                            ImGui::PushID(c + 10);
                            ImGui::SetNextItemWidth(w);
                            changed |= ImGui::DragFloat("##c", &p.val[c], speed, 0, 0, "%.4g");
                            if (ImGui::IsItemActivated()) checkpoint();
                            ImGui::PopID();
                        }
                    }
                }
                break;
            }
            case ParmType::Menu: {
                int v = std::clamp(int(p.val[0]), 0, int(d.menu.size()) - 1);
                if (ImGui::BeginCombo("##v", d.menu[size_t(v)].c_str())) {
                    for (int i = 0; i < int(d.menu.size()); ++i)
                        if (ImGui::Selectable(d.menu[size_t(i)].c_str(), i == v) && i != v) {
                            checkpoint();
                            p.val[0] = float(i);
                            changed = true;
                        }
                    ImGui::EndCombo();
                }
                break;
            }
            case ParmType::String:
                changed = ImGui::InputText("##v", &p.str);
                if (ImGui::IsItemActivated()) checkpoint();
                break;
            case ParmType::File:
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 40);
                changed = ImGui::InputText("##v", &p.str);
                if (ImGui::IsItemActivated()) checkpoint();
                ImGui::SameLine();
                if (ImGui::Button("...")) {
                    fileParmTarget = n->name() + "/" + d.name;
                    openFileDialog(FileDialogMode::ChooseFileParm);
                }
                break;
            case ParmType::Code: break;
            }
            if (changed) { n->touch(); markDirty(); }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    // code parameters: full-width editors
    for (size_t pi = 0; pi < n->parms().size(); ++pi) {
        Parm& p = n->parms()[pi];
        if (p.def->type != ParmType::Code) continue;
        ImGui::PushID(int(pi) + 5000);
        ImGui::TextUnformatted(p.def->label.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("(Help > VEX-lite Reference)");
        if (monoFont) ImGui::PushFont(monoFont);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.11f, 0.115f, 0.13f, 1));
        float lines = std::max(10.f, std::min(28.f, ImGui::GetContentRegionAvail().y / ImGui::GetTextLineHeight() - 10));
        bool changed = ImGui::InputTextMultiline("##code", &p.str, ImVec2(-1, ImGui::GetTextLineHeight() * lines), ImGuiInputTextFlags_AllowTabInput);
        if (ImGui::IsItemActivated()) checkpoint();
        ImGui::PopStyleColor();
        if (monoFont) ImGui::PopFont();
        if (changed) { n->touch(); markDirty(); }
        ImGui::PopID();
    }
    if (!any) ImGui::TextDisabled("This node has no parameters.");

    // ---- node info
    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Node Info", ImGuiTreeNodeFlags_DefaultOpen)) {
        const Geometry& g = n->geometry();
        ImGui::Text("%zu points   %zu primitives   %zu vertices", g.numPoints(), g.numPrims(), g.numVertices());
        ImGui::TextDisabled("Cook: %.2f ms%s", n->cookTimeMs(), n->isTimeDependent() ? "   (time dependent)" : "");
        if (n->cookStamp()) {
            Vec3 mn, mx;
            g.bounds(mn, mx);
            ImGui::TextDisabled("Bounds  min (%.3g, %.3g, %.3g)  max (%.3g, %.3g, %.3g)", mn.x, mn.y, mn.z, mx.x, mx.y, mx.z);
        }
        for (auto& vol : g.volumes)
            ImGui::Text("Volume  %d x %d x %d voxels  (voxel size %.3g)", vol.nx, vol.ny, vol.nz, double(vol.dx));
        std::string pa, qa;
        for (auto& [k, a] : g.pointAttribs) pa += k + (a.tuple == 3 ? "[3] " : " ");
        for (auto& [k, a] : g.primAttribs) qa += k + (a.tuple == 3 ? "[3] " : " ");
        ImGui::TextDisabled("Point attributes: P[3] %s", pa.c_str());
        if (!qa.empty()) ImGui::TextDisabled("Primitive attributes: %s", qa.c_str());
        int nin = 0;
        for (Node* in : n->inputs) if (in) ++nin;
        if (!n->inputs.empty()) ImGui::TextDisabled("Inputs connected: %d / %zu (min %d)", nin, n->inputs.size(), t.minInputs);
    }
    ImGui::End();
}

} // namespace hd
