#include "App.h"
#include <imgui.h>

namespace hd {

// Dark, neutral-grey theme in the spirit of Houdini's UI.
void applyHoudiniTheme(float scale) {
    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();
    s.WindowPadding = ImVec2(8, 8);
    s.FramePadding = ImVec2(6, 4);
    s.ItemSpacing = ImVec2(6, 5);
    s.ItemInnerSpacing = ImVec2(5, 4);
    s.ScrollbarSize = 13;
    s.GrabMinSize = 10;
    s.WindowRounding = 3;
    s.FrameRounding = 3;
    s.PopupRounding = 3;
    s.GrabRounding = 2;
    s.TabRounding = 3;
    s.ChildRounding = 3;
    s.WindowBorderSize = 1;
    s.FrameBorderSize = 0;
    s.TabBarBorderSize = 1;
    s.DockingSeparatorSize = 3;

    ImVec4* c = s.Colors;
    auto rgb = [](int r, int g, int b, float a = 1.f) { return ImVec4(r / 255.f, g / 255.f, b / 255.f, a); };
    c[ImGuiCol_Text] = rgb(214, 214, 216);
    c[ImGuiCol_TextDisabled] = rgb(132, 134, 140);
    c[ImGuiCol_WindowBg] = rgb(58, 58, 60);
    c[ImGuiCol_ChildBg] = rgb(52, 52, 54, 0.f);
    c[ImGuiCol_PopupBg] = rgb(46, 46, 48, 0.98f);
    c[ImGuiCol_Border] = rgb(30, 30, 32);
    c[ImGuiCol_BorderShadow] = rgb(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = rgb(40, 40, 42);
    c[ImGuiCol_FrameBgHovered] = rgb(48, 48, 52);
    c[ImGuiCol_FrameBgActive] = rgb(54, 56, 62);
    c[ImGuiCol_TitleBg] = rgb(44, 44, 46);
    c[ImGuiCol_TitleBgActive] = rgb(50, 50, 54);
    c[ImGuiCol_TitleBgCollapsed] = rgb(44, 44, 46);
    c[ImGuiCol_MenuBarBg] = rgb(48, 48, 50);
    c[ImGuiCol_ScrollbarBg] = rgb(44, 44, 46);
    c[ImGuiCol_ScrollbarGrab] = rgb(84, 84, 88);
    c[ImGuiCol_ScrollbarGrabHovered] = rgb(100, 100, 106);
    c[ImGuiCol_ScrollbarGrabActive] = rgb(120, 120, 126);
    c[ImGuiCol_CheckMark] = rgb(255, 205, 70);
    c[ImGuiCol_SliderGrab] = rgb(120, 140, 175);
    c[ImGuiCol_SliderGrabActive] = rgb(150, 175, 215);
    c[ImGuiCol_Button] = rgb(76, 76, 80);
    c[ImGuiCol_ButtonHovered] = rgb(92, 92, 98);
    c[ImGuiCol_ButtonActive] = rgb(108, 110, 120);
    c[ImGuiCol_Header] = rgb(78, 84, 98);
    c[ImGuiCol_HeaderHovered] = rgb(90, 98, 116);
    c[ImGuiCol_HeaderActive] = rgb(104, 114, 136);
    c[ImGuiCol_Separator] = rgb(36, 36, 38);
    c[ImGuiCol_SeparatorHovered] = rgb(255, 205, 70, 0.6f);
    c[ImGuiCol_SeparatorActive] = rgb(255, 205, 70);
    c[ImGuiCol_ResizeGrip] = rgb(90, 90, 96, 0.4f);
    c[ImGuiCol_ResizeGripHovered] = rgb(255, 205, 70, 0.6f);
    c[ImGuiCol_ResizeGripActive] = rgb(255, 205, 70);
    c[ImGuiCol_Tab] = rgb(48, 48, 50);
    c[ImGuiCol_TabHovered] = rgb(84, 84, 90);
    c[ImGuiCol_TabSelected] = rgb(66, 66, 70);
    c[ImGuiCol_TabSelectedOverline] = rgb(255, 205, 70);
    c[ImGuiCol_TabDimmed] = rgb(44, 44, 46);
    c[ImGuiCol_TabDimmedSelected] = rgb(58, 58, 62);
    c[ImGuiCol_DockingPreview] = rgb(255, 205, 70, 0.5f);
    c[ImGuiCol_DockingEmptyBg] = rgb(36, 36, 38);
    c[ImGuiCol_PlotLines] = rgb(160, 160, 165);
    c[ImGuiCol_TableHeaderBg] = rgb(48, 48, 52);
    c[ImGuiCol_TableBorderStrong] = rgb(32, 32, 34);
    c[ImGuiCol_TableBorderLight] = rgb(44, 44, 46);
    c[ImGuiCol_TableRowBg] = rgb(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = rgb(255, 255, 255, 0.03f);
    c[ImGuiCol_TextSelectedBg] = rgb(255, 205, 70, 0.3f);
    c[ImGuiCol_NavHighlight] = rgb(255, 205, 70);
    c[ImGuiCol_ModalWindowDimBg] = rgb(0, 0, 0, 0.45f);
    s.ScaleAllSizes(scale);
}

} // namespace hd
