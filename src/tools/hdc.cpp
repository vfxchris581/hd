// hdc — headless HD command line (like hbatch): cook scenes, export geometry, make demos.
#include "core/Demo.h"
#include "core/GeoIO.h"
#include "core/Lights.h"
#include "core/Node.h"
#include "core/PathTracer.h"
#include <cstdio>
#include <cstring>
#include <string>

using namespace hd;

static void usage() {
    std::printf(
        "hdc - HD headless tool\n\n"
        "  hdc info   <scene.hdg> [--frame N]            list nodes and cook stats\n"
        "  hdc cook   <scene.hdg> <out.obj|out.ply> [--node NAME] [--frame N]\n"
        "  hdc frames <scene.hdg> <out_$F.obj> <start> <end> [--node NAME]   export a sequence\n"
        "  hdc render <scene.hdg> <out.png> [--frame N | --frames A B] [--samples N] [--size W H] [--node NAME] [--camera NAME]\n"
        "             path-trace the scene (a sequence writes out.0001.png, out.0002.png, ...)\n"
        "  hdc demo   <name> <out.hdg>                   write a demo scene\n"
        "  hdc demos                                     list demo scenes\n"
        "  hdc nodes                                     list node types\n");
}

static const char* opt(int argc, char** argv, const char* name) {
    for (int i = 1; i + 1 < argc; ++i) if (!std::strcmp(argv[i], name)) return argv[i + 1];
    return nullptr;
}

static bool exportGeo(const std::string& path, const Geometry& g, std::string& err) {
    if (path.size() > 4 && path.substr(path.size() - 4) == ".ply") return savePly(path, g, &err);
    return saveObj(path, g, &err);
}

int main(int argc, char** argv) {
    if (argc < 2) { usage(); return 1; }
    std::string cmd = argv[1];
    if (cmd == "nodes") {
        for (auto& t : nodeTypes())
            std::printf("%-15s %-18s %-11s %s\n", t.name.c_str(), t.label.c_str(), t.category.c_str(), t.description.c_str());
        return 0;
    }
    if (cmd == "demos") {
        for (auto& d : demoScenes()) std::printf("%-10s %s\n", d.name.c_str(), d.description.c_str());
        return 0;
    }
    if (cmd == "demo" && argc >= 4) {
        Graph g;
        if (!buildDemoScene(g, argv[2])) { std::printf("unknown demo '%s'\n", argv[2]); return 1; }
        std::string err;
        if (!g.save(argv[3], &err)) { std::printf("%s\n", err.c_str()); return 1; }
        std::printf("wrote %s\n", argv[3]);
        return 0;
    }
    if (cmd == "render" && argc >= 4) {
        Graph g;
        std::string err;
        if (!g.load(argv[2], &err)) { std::printf("error: %s\n", err.c_str()); return 1; }
        Node* target = g.displayNode();
        if (const char* n = opt(argc, argv, "--node")) target = g.findNode(n);
        if (!target) { std::printf("error: no node to render\n"); return 1; }
        RenderSettings rs = g.render;
        if (const char* s = opt(argc, argv, "--samples")) rs.samples = std::atoi(s);
        for (int i = 1; i + 2 < argc; ++i)
            if (!std::strcmp(argv[i], "--size")) { rs.width = std::atoi(argv[i + 1]); rs.height = std::atoi(argv[i + 2]); }
        int a = int(g.frame), b = int(g.frame);
        bool seq = false;
        if (const char* f = opt(argc, argv, "--frame")) a = b = std::atoi(f);
        for (int i = 1; i + 2 < argc; ++i)
            if (!std::strcmp(argv[i], "--frames")) { a = std::atoi(argv[i + 1]); b = std::atoi(argv[i + 2]); seq = true; }
        PathTracer pt;
        for (int f = a; f <= b; ++f) {
            g.frame = float(f);
            g.cook(target);
            if (!target->error().empty()) { std::printf("cook error in %s: %s\n", target->name().c_str(), target->error().c_str()); return 1; }
            RenderSettings r = rs;
            if (!r.hasCamera) { // frame the geometry like the viewport does
                Vec3 mn, mx;
                target->geometry().bounds(mn, mx);
                r.camTarget = (mn + mx) * 0.5f;
                r.camDist = std::max(length(mx - mn) * 0.5f, 0.05f) / std::sin(radians(r.camFov) * 0.5f) * 1.1f;
            }
            // camera node: --camera NAME, else the one saved with the scene
            std::string camName = r.cameraName;
            if (const char* cn = opt(argc, argv, "--camera")) camName = cn;
            bool sizeGiven = false;
            for (int i = 1; i < argc; ++i) sizeGiven |= !std::strcmp(argv[i], "--size");
            for (auto& cam : collectCameras(g, g.frame)) {
                if (cam.name != camName) continue;
                r.useEye = true; r.camEye = cam.eye; r.camTarget = cam.eye + cam.fwd;
                if (!sizeGiven) { r.width = cam.resx; r.height = cam.resy; }
                SceneCamera fitted = cam;
                fitted.resx = r.width; fitted.resy = r.height;
                r.camFov = fitted.fovYDegrees();
            }
            pt.start(target->geometry(), r, collectLights(g, g.frame));
            pt.wait();
            std::vector<uint8_t> img; int w = 0, h = 0; uint64_t ver = 0;
            pt.fetch(img, w, h, ver);
            std::string path = argv[3];
            if (seq) {
                char num[16]; std::snprintf(num, sizeof num, ".%04d", f);
                size_t dot = path.rfind('.');
                path.insert(dot == std::string::npos ? path.size() : dot, num);
            }
            if (!writePng(path, img, w, h, &err)) { std::printf("%s\n", err.c_str()); return 1; }
            std::printf("frame %d -> %s (%dx%d, %d samples, %.1fs)\n", f, path.c_str(), w, h, pt.samplesDone(), pt.secondsElapsed());
        }
        return 0;
    }
    if ((cmd == "info" || cmd == "cook" || cmd == "frames") && argc >= 3) {
        Graph g;
        std::string err;
        if (!g.load(argv[2], &err)) { std::printf("error: %s\n", err.c_str()); return 1; }
        if (!err.empty()) std::printf("warning: %s\n", err.c_str());
        if (const char* f = opt(argc, argv, "--frame")) g.frame = float(std::atof(f));
        Node* target = g.displayNode();
        if (const char* n = opt(argc, argv, "--node")) target = g.findNode(n);
        if (!target) { std::printf("error: no node to cook\n"); return 1; }
        if (cmd == "info") {
            for (auto& n : g.nodes()) g.cook(n.get());
            for (auto& n : g.nodes()) {
                std::printf("%c %-18s %-15s %7zu pts %7zu prims %8.2f ms %s\n", n.get() == g.displayNode() ? '*' : ' ',
                            n->name().c_str(), n->type().name.c_str(), n->geometry().numPoints(), n->geometry().numPrims(),
                            n->cookTimeMs(), n->error().c_str());
            }
            return 0;
        }
        if (cmd == "cook" && argc >= 4) {
            g.cook(target);
            if (!target->error().empty()) { std::printf("cook error in %s: %s\n", target->name().c_str(), target->error().c_str()); return 1; }
            if (!exportGeo(argv[3], target->geometry(), err)) { std::printf("%s\n", err.c_str()); return 1; }
            std::printf("%s: %zu points, %zu prims -> %s\n", target->name().c_str(), target->geometry().numPoints(), target->geometry().numPrims(), argv[3]);
            return 0;
        }
        if (cmd == "frames" && argc >= 6) {
            std::string pattern = argv[3];
            int a = std::atoi(argv[4]), b = std::atoi(argv[5]);
            for (int f = a; f <= b; ++f) {
                g.frame = float(f);
                g.cook(target);
                if (!target->error().empty()) { std::printf("frame %d: %s\n", f, target->error().c_str()); return 1; }
                std::string path = pattern;
                char num[16]; std::snprintf(num, sizeof num, "%04d", f);
                size_t pos = path.find("$F");
                if (pos != std::string::npos) path.replace(pos, 2, num);
                else path += std::string(".") + num;
                if (!exportGeo(path, target->geometry(), err)) { std::printf("%s\n", err.c_str()); return 1; }
                std::printf("frame %d -> %s\n", f, path.c_str());
            }
            return 0;
        }
    }
    usage();
    return 1;
}
