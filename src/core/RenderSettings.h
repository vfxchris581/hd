#pragma once
#include "Math.h"
#include <string>

namespace hd {

// Settings for the path-traced Render View; saved with the scene.
struct RenderSettings {
    int width = 960, height = 540;
    int samples = 64;
    int bounces = 4;
    float sunAzimuth = 40.f;   // degrees around Y
    float sunElevation = 50.f; // degrees above the horizon
    float sunIntensity = 3.f;
    float skyIntensity = 1.f;
    float exposure = 1.f;
    float curveWidth = 0.012f; // world-space thickness of polylines
    bool ground = false;       // infinite ground plane
    float groundY = 0.f;
    // camera (orbit parameters, same as the viewport)
    bool hasCamera = false;
    Vec3 camTarget{0, 0, 0};
    float camYaw = 45.f, camPitch = 25.f, camDist = 8.f, camFov = 45.f;

    // Name of the camera node to look through and render from ("" = the free perspective view).
    std::string cameraName;
    // Set by the app when rendering through a camera node (not saved).
    bool useEye = false;
    Vec3 camEye{0, 0, 0};

    Vec3 eye() const {
        if (useEye) return camEye;
        float p = radians(camPitch), y = radians(camYaw);
        return camTarget + Vec3(std::cos(p) * std::sin(y), std::sin(p), std::cos(p) * std::cos(y)) * camDist;
    }
};

} // namespace hd
