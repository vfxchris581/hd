// Linux developer check: runs the full HD UI headlessly (GLFW null platform + EGL
// surfaceless context), simulates a little input, and writes screenshots.
//   hd_ui_screenshot <out_prefix> [demo]
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
#include <string>

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
