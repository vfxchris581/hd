// Linux developer check: runs the full HD UI headlessly (GLFW null platform + EGL
// surfaceless context), simulates a little input, and writes screenshots.
//   hd_ui_screenshot <out_prefix> [demo [frame [render_samples]]]
#include "app/App.h"
#include "core/Demo.h"
#include "render/GL.h"
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_internal.h>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <filesystem>
#include <chrono>
#include <string>
#include <thread>

typedef void* EGLDisplay; typedef void* EGLConfig; typedef void* EGLContext;
typedef int EGLint; typedef unsigned EGLenum; typedef unsigned EGLBoolean;
static void* (*p_eglGetProcAddress)(const char*);
static void* getProc(const char* n) { return p_eglGetProcAddress(n); }

static bool makeContext() {
    void* lib = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (!lib) return false;
    p_eglGetProcAddress = (void* (*)(const char*))dlsym(lib, "eglGetProcAddress");
    auto gpd = (EGLDisplay(*)(EGLenum, void*, const EGLint*))getProc("eglGetPlatformDisplayEXT");
    auto init = (EGLBoolean(*)(EGLDisplay, EGLint*, EGLint*))dlsym(lib, "eglInitialize");
    auto choose = (EGLBoolean(*)(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*))dlsym(lib, "eglChooseConfig");
    auto bind = (EGLBoolean(*)(EGLenum))dlsym(lib, "eglBindAPI");
    auto create = (EGLContext(*)(EGLDisplay, EGLConfig, EGLContext, const EGLint*))dlsym(lib, "eglCreateContext");
    auto current = (EGLBoolean(*)(EGLDisplay, void*, void*, EGLContext))dlsym(lib, "eglMakeCurrent");
    EGLDisplay d = gpd(0x31DD, nullptr, nullptr);
    EGLint a, b;
    if (!init(d, &a, &b)) return false;
    EGLint ca[] = {0x3033, 1, 0x3040, 8, 0x3038};
    EGLConfig cfg; EGLint n = 0;
    choose(d, ca, &cfg, 1, &n);
    bind(0x30A2);
    EGLint xa[] = {0x3098, 3, 0x30FB, 3, 0x30FD, 1, 0x3038};
    EGLContext c = create(d, n ? cfg : nullptr, nullptr, xa);
    return c && current(d, nullptr, nullptr, c);
}

static void savePPM(const std::string& path, int w, int h) {
    std::vector<unsigned char> px(size_t(w) * h * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    FILE* f = std::fopen(path.c_str(), "wb");
    std::fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = h - 1; y >= 0; --y)
        for (int x = 0; x < w; ++x) std::fwrite(&px[(size_t(y) * w + x) * 4], 1, 3, f);
    std::fclose(f);
    std::printf("wrote %s\n", path.c_str());
}

int main(int argc, char** argv) {
    std::string prefix = argc > 1 ? argv[1] : "ui";
    std::string demo = argc > 2 ? argv[2] : "terrain";
    const int W = 1600, H = 940;
    glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_NULL);
    if (!glfwInit()) { std::printf("glfw init failed\n"); return 1; }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow* win = glfwCreateWindow(W, H, "HD", nullptr, nullptr);
    if (!win || !makeContext()) { std::printf("context failed\n"); return 1; }
    if (!hd::loadGL(getProc)) { std::printf("loadGL failed\n"); return 1; }

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(float(W), float(H));
    ImFontConfig cfg; cfg.OversampleH = 2;
    ImFont* ui = nullptr;
    if (std::filesystem::exists("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"))
        ui = io.Fonts->AddFontFromFileTTF("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 14.f, &cfg);
    if (!ui) io.Fonts->AddFontDefault();
    ImFont* mono = nullptr;
    if (std::filesystem::exists("/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"))
        mono = io.Fonts->AddFontFromFileTTF("/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", 13.f, &cfg);
    hd::applyHoudiniTheme(1.f);
    if (!ImGui_ImplOpenGL3_Init("#version 330 core")) { std::printf("imgui gl3 init failed\n"); return 1; }

    unsigned fbo, tex;
    glGenFramebuffers(1, &fbo);
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);

    hd::App app;
    app.monoFont = mono;
    if (!app.init(win, "")) { std::printf("app init failed\n"); return 1; }
    hd::buildDemoScene(app.graph, demo);
    app.clearSelection();
    if (hd::Node* d = app.graph.displayNode()) app.select(d);
    app.network.requestFrameAll();
    if (argc > 3) app.graph.frame = float(std::atof(argv[3]));

    auto runFrame = [&]() {
        io.DeltaTime = 1.f / 60.f;
        ImGui_ImplOpenGL3_NewFrame();
        ImGui::NewFrame();
        app.frame();
        ImGui::Render();
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glViewport(0, 0, W, H);
        glClearColor(0.1f, 0.1f, 0.1f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    };
    for (int i = 0; i < 6; ++i) runFrame();
    app.frameGeometry();
    for (int i = 0; i < 3; ++i) runFrame();
    savePPM(prefix + "_1.ppm", W, H);
    if (argc > 5 && std::string(argv[5]) == "anim") { // keyframes: marks on the timeline, colored fields, editor
        hd::Node* mv = app.net().findNode("move");
        app.clearSelection();
        if (mv) app.select(mv);
        app.graph.frame = 30;
        // set a key through the same path as Alt+click, on a parameter that has none yet
        if (mv) app.setKeyframe(mv, *mv->parm("r"));
        app.graph.frame = 37;
        app.showAnimEditor = true;
        for (int i = 0; i < 4; ++i) runFrame();
        if (ImGuiWindow* w = ImGui::FindWindowByName("Animation Editor")) { ImGui::SetWindowPos(w, ImVec2(40, 560)); ImGui::SetWindowSize(w, ImVec2(860, 300)); }
        for (int i = 0; i < 3; ++i) runFrame();
        std::printf("keys r=%zu ty=%zu\n", mv ? mv->parm("r")->keys[0].size() : 0, mv ? mv->parm("t")->keys[1].size() : 0);
        app.jumpToKey(1);
        std::printf("next key from 37 -> %d\n", int(app.graph.frame));
        app.graph.frame = 37;
        for (int i = 0; i < 2; ++i) runFrame();
        savePPM(prefix + "_anim.ppm", W, H);
        app.showAnimEditor = false;
    } else if (argc > 5 && std::string(argv[5]) == "lights") { // shelf tools: lights and a camera, then render
        app.shelfTool("spotlight");
        app.shelfTool("pointlight");
        if (hd::Node* pl = app.graph.findNode("pointlight1")) { pl->parm("light_color")->val[0] = 0.3f; pl->parm("light_color")->val[1] = 0.5f; pl->parm("light_color")->val[2] = 1.0f; }
        app.shelfTool("ambientlight");
        app.network.requestFrameAll();
        for (int i = 0; i < 6; ++i) runFrame();
        savePPM(prefix + "_lights.ppm", W, H);
        app.shelfTool("camera");
        for (int i = 0; i < 4; ++i) runFrame();
        std::printf("camera=%s lights=%zu aspect=%.3f\n", app.graph.render.cameraName.c_str(), hd::collectLights(app.graph, 1).size(), double(app.activeCamAspect));
        savePPM(prefix + "_camera.ppm", W, H);
        app.graph.render.samples = 64;
        app.startRender();
        ImGui::SetWindowFocus("Render View");
        while (app.rendering()) { runFrame(); std::this_thread::sleep_for(std::chrono::milliseconds(50)); }
        for (int i = 0; i < 3; ++i) runFrame();
        savePPM(prefix + "_lit.ppm", W, H);
    } else if (argc > 5 && std::string(argv[5]) == "subnet") { // collapse, dive in, save as asset
        app.clearSelection();
        for (const char* nm : {"rock", "rock_noise", "rock_color"})
            if (hd::Node* n = app.net().findNode(nm)) app.select(n, true);
        app.collapseSelected();
        app.network.requestFrameAll();
        for (int i = 0; i < 5; ++i) runFrame();
        savePPM(prefix + "_collapsed.ppm", W, H);
        hd::Node* sn = app.current();
        std::printf("subnet=%s inside=%zu display_pts=%zu\n", sn ? sn->name().c_str() : "-", sn && sn->subnet() ? sn->subnet()->nodes().size() : 0,
                    app.graph.displayNode()->geometry().numPoints());
        if (sn) app.saveAsset(sn);
        std::printf("assets=%zu\n", app.assets().size());
        if (sn) app.diveInto(sn);
        for (int i = 0; i < 6; ++i) runFrame();
        app.frameGeometry();
        for (int i = 0; i < 2; ++i) runFrame();
        savePPM(prefix + "_inside.ppm", W, H);
        app.goToLevel(0);
        for (int i = 0; i < 3; ++i) runFrame();
        app.undo();
        for (int i = 0; i < 3; ++i) runFrame();
        std::printf("after undo: nodes=%zu rock=%d\n", app.graph.nodes().size(), app.graph.findNode("rock") != nullptr);
        if (sn) app.deleteAsset("subnet1");
    } else if (argc > 5) { // menus + display options
        app.showDisplayOptions = true;
        app.viewOpts.gridSpacing = 0.5f; app.viewOpts.gridCells = 30;
        for (int i = 0; i < 3; ++i) runFrame();
        if (ImGuiWindow* dw = ImGui::FindWindowByName("Display Options")) ImGui::SetWindowPos(dw, ImVec2(420, 420));
        for (int i = 0; i < 2; ++i) runFrame();
        // open the File menu by clicking it
        io.AddMousePosEvent(22, 10); runFrame();
        io.AddMouseButtonEvent(0, true); runFrame();
        io.AddMouseButtonEvent(0, false); runFrame(); runFrame();
        savePPM(prefix + "_menus.ppm", W, H);
        io.AddKeyEvent(ImGuiKey_Escape, true); runFrame();
        io.AddKeyEvent(ImGuiKey_Escape, false); runFrame();
        io.AddMousePosEvent(214, 10); runFrame();  // Windows menu
        io.AddMouseButtonEvent(0, true); runFrame();
        io.AddMouseButtonEvent(0, false); runFrame(); runFrame();
        savePPM(prefix + "_windows.ppm", W, H);
        io.AddKeyEvent(ImGuiKey_Escape, true); runFrame();
        io.AddKeyEvent(ImGuiKey_Escape, false); runFrame();
        app.showDisplayOptions = false;
    }
    if (argc > 4 && std::atoi(argv[4]) > 0) { // render view
        app.graph.render.samples = std::atoi(argv[4]);
        app.startRender();
        ImGui::SetWindowFocus("Render View");
        while (app.rendering()) { runFrame(); std::this_thread::sleep_for(std::chrono::milliseconds(50)); }
        for (int i = 0; i < 3; ++i) runFrame();
        savePPM(prefix + "_render.ppm", W, H);
        ImGui::SetWindowFocus("Scene View");
        runFrame();
    }

    // Select a node with interesting parameters, show the spreadsheet tab, open Tab menu
    for (auto& n : app.graph.nodes())
        if (n->type().name == "attribwrangle" || n->type().name == "mountain") { app.select(n.get()); break; }
    for (int i = 0; i < 3; ++i) runFrame();
    savePPM(prefix + "_parms.ppm", W, H);
    ImGui::SetWindowFocus("Geometry Spreadsheet");
    for (int i = 0; i < 3; ++i) runFrame();
    // hover the network and press Tab
    ImGuiWindow* net = ImGui::FindWindowByName("Network");
    if (net) {
        io.AddMousePosEvent(net->Pos.x + net->Size.x * 0.75f, net->Pos.y + net->Size.y * 0.35f);
        runFrame();
        io.AddMouseButtonEvent(0, true); runFrame();
        io.AddMouseButtonEvent(0, false); runFrame();
        io.AddKeyEvent(ImGuiKey_Tab, true); runFrame();
        io.AddKeyEvent(ImGuiKey_Tab, false); runFrame();
        for (const char* ch = "no"; *ch; ++ch) { io.AddInputCharacter(*ch); runFrame(); }
        runFrame();
    }
    savePPM(prefix + "_2.ppm", W, H);
    io.AddKeyEvent(ImGuiKey_Escape, true); runFrame();
    io.AddKeyEvent(ImGuiKey_Escape, false); runFrame();
    std::printf("nodes=%zu display=%s\n", app.graph.nodes().size(), app.graph.displayNode() ? app.graph.displayNode()->name().c_str() : "-");

    app.shutdown();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui::DestroyContext();
    return 0;
}
