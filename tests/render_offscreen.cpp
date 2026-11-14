// Linux developer check: renders a scene through the real viewport renderer
// using a surfaceless EGL context (Mesa), and writes a PPM image.
//   hd_render_test <scene.hdg|-> <out.ppm> [width height]
#include "core/Node.h"
#include "render/GL.h"
#include "render/Renderer.h"
#include <cstdio>
#include <dlfcn.h>

typedef void* EGLDisplay; typedef void* EGLConfig; typedef void* EGLContext; typedef void* EGLSurface;
typedef int EGLint; typedef unsigned EGLenum; typedef unsigned EGLBoolean;
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#define EGL_SURFACE_TYPE 0x3033
#define EGL_PBUFFER_BIT 0x0001
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_OPENGL_BIT 0x0008
#define EGL_NONE 0x3038
#define EGL_OPENGL_API 0x30A2
#define EGL_CONTEXT_MAJOR_VERSION 0x3098
#define EGL_CONTEXT_MINOR_VERSION 0x30FB
#define EGL_CONTEXT_OPENGL_PROFILE_MASK 0x30FD
#define EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT 0x00000001

static void* (*p_eglGetProcAddress)(const char*);
static void* getProc(const char* n) { return p_eglGetProcAddress(n); }

int main(int argc, char** argv) {
    const char* scene = argc > 1 ? argv[1] : "-";
    const char* out = argc > 2 ? argv[2] : "render.ppm";
    int W = argc > 4 ? std::atoi(argv[3]) : 960, H = argc > 4 ? std::atoi(argv[4]) : 600;

    void* lib = dlopen("libEGL.so.1", RTLD_NOW);
    if (!lib) { std::printf("no libEGL\n"); return 2; }
    p_eglGetProcAddress = (void* (*)(const char*))dlsym(lib, "eglGetProcAddress");
    auto getPlatformDisplay = (EGLDisplay(*)(EGLenum, void*, const EGLint*))getProc("eglGetPlatformDisplayEXT");
    auto initialize = (EGLBoolean(*)(EGLDisplay, EGLint*, EGLint*))dlsym(lib, "eglInitialize");
    auto chooseConfig = (EGLBoolean(*)(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*))dlsym(lib, "eglChooseConfig");
    auto bindAPI = (EGLBoolean(*)(EGLenum))dlsym(lib, "eglBindAPI");
    auto createContext = (EGLContext(*)(EGLDisplay, EGLConfig, EGLContext, const EGLint*))dlsym(lib, "eglCreateContext");
    auto makeCurrent = (EGLBoolean(*)(EGLDisplay, EGLSurface, EGLSurface, EGLContext))dlsym(lib, "eglMakeCurrent");
    EGLDisplay dpy = getPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, nullptr, nullptr);
    EGLint maj, min;
    if (!dpy || !initialize(dpy, &maj, &min)) { std::printf("egl init failed\n"); return 2; }
    EGLint cfgAttr[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE};
    EGLConfig cfg; EGLint n = 0;
    chooseConfig(dpy, cfgAttr, &cfg, 1, &n);
    bindAPI(EGL_OPENGL_API);
    EGLint ctxAttr[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3, EGL_CONTEXT_OPENGL_PROFILE_MASK,
                        EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE};
    EGLContext ctx = createContext(dpy, n ? cfg : nullptr, nullptr, ctxAttr);
    if (!ctx || !makeCurrent(dpy, nullptr, nullptr, ctx)) { std::printf("context failed\n"); return 2; }
    const char* missing = nullptr;
    if (!hd::loadGL(getProc, &missing)) { std::printf("missing GL function %s\n", missing); return 2; }
    std::printf("GL: %s / %s\n", glGetString(GL_VERSION), glGetString(GL_RENDERER));

    hd::Graph g;
    std::string err;
    if (std::string(scene) == "-") {
        hd::Node* s = g.createNode("sphere");
        g.setDisplayNode(s);
    } else if (!g.load(scene, &err)) {
        std::printf("load failed: %s\n", err.c_str());
        return 1;
    }
    hd::Node* d = g.displayNode();
    g.cook(d);
    if (!d->error().empty()) std::printf("cook error: %s\n", d->error().c_str());
    std::printf("display %s: %zu points, %zu prims\n", d->name().c_str(), d->geometry().numPoints(), d->geometry().numPrims());

    hd::Renderer r;
    if (!r.init(err)) { std::printf("renderer init: %s\n", err.c_str()); return 1; }
    r.setGeometry(d->geometry(), d->cookStamp());
    hd::Camera cam;
    hd::Vec3 mn, mx;
    d->geometry().bounds(mn, mx);
    cam.frame(mn, mx);
    cam.dist *= 1.15f;
    hd::ViewOptions opt;
    r.render(W, H, cam, opt);
    std::vector<uint8_t> px; int w, h;
    r.readPixels(px, w, h);
    FILE* f = std::fopen(out, "wb");
    std::fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = h - 1; y >= 0; --y)
        for (int x = 0; x < w; ++x) std::fwrite(&px[(size_t(y) * w + x) * 4], 1, 3, f);
    std::fclose(f);
    GLenum e = glGetError();
    std::printf("wrote %s (%dx%d), glGetError=0x%x, tris=%zu\n", out, w, h, e, r.stats().triangles);
    return e == 0 ? 0 : 1;
}
