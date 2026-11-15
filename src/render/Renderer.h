// OpenGL 3.3 viewport renderer: shaded surfaces, wireframe, curves, points,
// normals, ground grid and gradient background, rendered into an MSAA framebuffer.
#pragma once
#include "core/Geometry.h"
#include <cstdint>
#include <string>
#include <vector>

namespace hd {

struct Camera {
    Vec3 target{0, 0, 0};
    float yaw = 45.f;     // degrees around Y
    float pitch = 25.f;   // degrees above the horizon
    float dist = 8.f;
    float fovY = 45.f;    // degrees
    float nearClip = 0.01f, farClip = 10000.f;

    Vec3 eye() const;
    Mat4 view() const;
    Mat4 proj(float aspect) const;
    void tumble(float dxPixels, float dyPixels);
    void pan(float dxPixels, float dyPixels, float viewportHeight);
    void dolly(float amount); // >0 moves in
    void frame(const Vec3& mn, const Vec3& mx);
    void home();
};

struct ViewOptions {
    bool shaded = true;
    bool wireframe = false;   // wire-over-shaded
    bool points = false;
    bool normals = false;
    bool grid = true;
    bool backfaceTint = false;
    float pointSize = 5.f;
    float normalScale = 0.15f;
    Vec3 defaultColor{0.72f, 0.74f, 0.78f};
    // reference (ground) plane
    int gridPlane = 0;          // 0 = XZ, 1 = XY, 2 = YZ
    Vec3 gridOrigin{0, 0, 0};
    float gridSpacing = 1.f;
    int gridCells = 20;         // lines across
    int gridRuler = 5;          // brighter line every N cells
    float gridOpacity = 0.55f;
    // background gradient
    Vec3 bgTop{0.40f, 0.43f, 0.48f};
    Vec3 bgBottom{0.13f, 0.135f, 0.15f};
};

struct RenderStats {
    size_t triangles = 0, lines = 0, points = 0, volumes = 0;
};

class Renderer {
public:
    Renderer() = default;
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // Requires a current OpenGL 3.3 core context and loadGL() done.
    bool init(std::string& error);
    void shutdown();

    // Upload geometry if stamp changed (stamp 0 always uploads).
    void setGeometry(const Geometry& g, uint64_t stamp);
    void clearGeometry();
    // Extra wireframe drawn over the scene (light and camera icons): x,y,z,r,g,b per vertex, as line pairs.
    void setGuides(const std::vector<float>& lines);

    // Render into the internal framebuffer (resized as needed). Returns the color texture.
    unsigned render(int width, int height, const Camera& cam, const ViewOptions& opt);
    unsigned texture() const { return resolveTex_; }
    // Read back the last rendered image as RGBA8, bottom-up rows.
    void readPixels(std::vector<uint8_t>& rgba, int& w, int& h);
    const RenderStats& stats() const { return stats_; }

private:
    struct Batch {
        unsigned vao = 0, vbo = 0;
        int count = 0;
    };
    void upload(Batch& b, const std::vector<float>& data, int floatsPerVertex, const int* layout, int nAttribs);
    void ensureTargets(int w, int h);
    void drawBatch(const Batch& b, unsigned mode);
    void rebuildGrid(const ViewOptions& opt);
    float gridKey_[8] = {-1, 0, 0, 0, 0, 0, 0, 0};
    void drawVolumes(int w, int h, const Camera& cam);
    struct GpuVolume {
        unsigned tex = 0;
        Vec3 mn, mx, color;
        float density = 1, fire = 0, step = 0.05f;
    };
    std::vector<GpuVolume> volumes_;
    Batch cube_, guides_;
    std::vector<float> guideData_;
    unsigned progVol_ = 0, depthTex_ = 0, volFbo_ = 0;

    bool inited_ = false;
    unsigned progMesh_ = 0, progLine_ = 0, progPoint_ = 0, progBg_ = 0;
    Batch tris_, wire_, curves_, points_, loose_, normals_, grid_;
    unsigned bgVao_ = 0;
    bool hasPscale_ = false;
    bool pointCloud_ = false;
    Vec3 boundsMin_{0, 0, 0}, boundsMax_{0, 0, 0};
    uint64_t stamp_ = ~uint64_t(0);

    int fbW_ = 0, fbH_ = 0, samples_ = 4;
    unsigned msFbo_ = 0, msColor_ = 0, msDepth_ = 0;
    unsigned resolveFbo_ = 0, resolveTex_ = 0;
    RenderStats stats_;
};

} // namespace hd
