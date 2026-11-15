// Lights and cameras. They are scene-level nodes (like Houdini objects): every light or
// camera node in the top-level network is part of the scene, wired or not. Bypass turns one off.
#pragma once
#include "Math.h"
#include <string>
#include <vector>

namespace hd {

class Graph;
struct NodeType;

struct Light {
    enum Type { Point, Spot, Area, Distant, Env } type = Point;
    std::string name;
    Vec3 pos{0, 0, 0};
    Vec3 dir{0, 0, -1};          // direction the light points (lights look down -Z, like cameras)
    Vec3 ux{1, 0, 0}, uy{0, 1, 0}; // area light axes
    Vec3 color{1, 1, 1};
    float intensity = 1;
    float radius = 0;            // point / spot size (soft shadows)
    float coneAngle = 45, coneDelta = 10; // spot, degrees
    float sizeX = 1, sizeY = 1;  // area
    float angle = 1;             // distant: angular size in degrees (soft shadows)
    bool sky = true;             // env: sky gradient (true) or constant color
};

struct SceneCamera {
    std::string name;
    Vec3 eye{0, 0, 5};
    Vec3 fwd{0, 0, -1};
    float focal = 50, aperture = 41.4214f; // mm; aperture is the horizontal film size
    int resx = 1280, resy = 720;
    float fovYDegrees() const; // vertical field of view for resx:resy
};

std::vector<Light> collectLights(const Graph& g, float frame);
std::vector<SceneCamera> collectCameras(const Graph& g, float frame);
// Rotation (degrees, XYZ order) that aims a light or camera at `target` from `from`.
Vec3 aimRotation(const Vec3& from, const Vec3& target);
bool isLightType(const std::string& typeName);

} // namespace hd
