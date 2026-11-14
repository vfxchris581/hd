// HD — procedural 3D VFX environment. Application entry point.
#ifndef GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_NONE
#endif
#include "App.h"
#include "render/GL.h"
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static hd::App* g_app = nullptr;
static std::vector<std::string> g_dropped;

static void* glProc(const char* name) { return reinterpret_cast<void*>(glfwGetProcAddress(name)); }
static void onGlfwError(int code, const char* msg) { std::fprintf(stderr, "GLFW error %d: %s\n", code, msg); }
static void onDrop(GLFWwindow*, int count, const char** paths) {
    for (int i = 0; i < count; ++i) g_dropped.emplace_back(paths[i]);
}

static ImFont* tryFont(const std::vector<std::string>& candidates, float px) {
    ImGuiIO& io = ImGui::GetIO();
    for (auto& path : candidates) {
        std::error_code ec;
        if (!fs::exists(path, ec)) continue;
        ImFontConfig cfg;
        cfg.OversampleH = 2;
        cfg.OversampleV = 2;
        if (ImFont* f = io.Fonts->AddFontFromFileTTF(path.c_str(), px, &cfg)) return f;
    }
    return nullptr;
}

int main(int argc, char** argv) {
    glfwSetErrorCallback(onGlfwError);
    if (!glfwInit()) return 1;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    glfwWindowHint(GLFW_COCOA_RETINA_FRAMEBUFFER, GLFW_TRUE);
#endif
    glfwWindowHint(GLFW_MAXIMIZED, GLFW_TRUE);
    GLFWwindow* window = glfwCreateWindow(1600, 960, "HD", nullptr, nullptr);
    if (!window) {
        std::fprintf(stderr, "Could not create an OpenGL 3.3 window.\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
    const char* missing = nullptr;
    if (!hd::loadGL(glProc, &missing)) {
        std::fprintf(stderr, "OpenGL function missing: %s\n", missing ? missing : "?");
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    static std::string iniPath = (fs::temp_directory_path() / "hd_layout.ini").string();
    if (const char* home = std::getenv("HOME")) iniPath = (fs::path(home) / ".hd_layout.ini").string();
    else if (const char* up = std::getenv("USERPROFILE")) iniPath = (fs::path(up) / ".hd_layout.ini").string();
    io.IniFilename = iniPath.c_str();

    // DPI handling: macOS reports a framebuffer scale, Windows/Linux a content scale.
    float xs = 1, ys = 1;
    glfwGetWindowContentScale(window, &xs, &ys);
    int ww, wh, fw, fh;
    glfwGetWindowSize(window, &ww, &wh);
    glfwGetFramebufferSize(window, &fw, &fh);
    float fbScale = ww > 0 ? float(fw) / float(ww) : 1.f;
    float uiScale = fbScale > 1.01f ? 1.f : xs; // on retina the framebuffer already scales
    float fontPx = 15.f * uiScale * fbScale;

    ImFont* ui = tryFont({"/System/Library/Fonts/SFNS.ttf", "/System/Library/Fonts/Supplemental/Arial.ttf", "/Library/Fonts/Arial.ttf",
                          "C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/arial.ttf",
                          "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf"},
                         fontPx);
    if (!ui) {
        ImFontConfig cfg;
        cfg.SizePixels = 13.f * uiScale * fbScale;
        io.Fonts->AddFontDefault(&cfg);
    }
    ImFont* mono = tryFont({"/System/Library/Fonts/Menlo.ttc", "/System/Library/Fonts/Monaco.ttf", "C:/Windows/Fonts/consola.ttf",
                            "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", "/usr/share/fonts/TTF/DejaVuSansMono.ttf"},
                           14.f * uiScale * fbScale);
    io.FontGlobalScale = 1.f / fbScale;
    hd::applyHoudiniTheme(uiScale);

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330 core");

    hd::App app;
    app.monoFont = mono;
    {
        std::error_code ec;
        fs::path exeDir = fs::absolute(fs::path(argv[0]), ec).parent_path();
        app.examplesDir = (exeDir / "examples").string();
    }
    g_app = &app;
    std::string startup = argc > 1 ? argv[1] : "";
    if (!app.init(window, startup)) {
        std::fprintf(stderr, "Failed to initialise the viewport renderer.\n");
    }
    glfwSetDropCallback(window, onDrop);

    while (true) {
        glfwPollEvents();
        if (glfwWindowShouldClose(window)) {
            if (app.handleCloseRequest()) break;
            glfwSetWindowShouldClose(window, 0);
        }
        for (auto& p : g_dropped) app.openPath(p);
        g_dropped.clear();

        if (glfwGetWindowAttrib(window, GLFW_ICONIFIED)) {
            glfwWaitEventsTimeout(0.1);
            continue;
        }
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        app.frame();
        ImGui::Render();
        int dw, dh;
        glfwGetFramebufferSize(window, &dw, &dh);
        glViewport(0, 0, dw, dh);
        glClearColor(0.12f, 0.12f, 0.13f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    app.shutdown();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
