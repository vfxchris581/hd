// Progressive CPU path tracer used by the Render View and `hdc render`.
#pragma once
#include "Geometry.h"
#include "RenderSettings.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace hd {

class PathTracer {
public:
    PathTracer();
    ~PathTracer();
    PathTracer(const PathTracer&) = delete;
    PathTracer& operator=(const PathTracer&) = delete;

    // Starts rendering a snapshot of geo in the background (cancels any render in progress).
    // The camera comes from settings (eye(), camTarget, camFov).
    void start(const Geometry& geo, const RenderSettings& settings);
    void stop();
    void wait(); // block until the current render finishes
    bool running() const;
    int samplesDone() const;
    int samplesTarget() const;
    double secondsElapsed() const;
    // Copies the latest tonemapped image (RGBA8, top row first) if it is newer than `version`.
    bool fetch(std::vector<uint8_t>& rgba, int& w, int& h, uint64_t& version);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Writes an 8-bit RGB PNG from RGBA data (alpha ignored).
bool writePng(const std::string& path, const std::vector<uint8_t>& rgba, int w, int h, std::string* error = nullptr);

} // namespace hd
