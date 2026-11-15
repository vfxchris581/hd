#include "Lights.h"
#include "Node.h"

namespace hd {
namespace {

ParmDef F(const char* n, const char* l, float d, float mn = 0, float mx = 1, const char* help = "") {
    ParmDef p; p.name = n; p.label = l; p.type = ParmType::Float; p.def[0] = d; p.min = mn; p.max = mx; p.help = help; return p;
}
ParmDef I(const char* n, const char* l, int d, int mn = 0, int mx = 10) {
    ParmDef p; p.name = n; p.label = l; p.type = ParmType::Int; p.def[0] = float(d); p.min = float(mn); p.max = float(mx); return p;
}
ParmDef V(const char* n, const char* l, Vec3 d, float mn = -1, float mx = 1, const char* help = "") {
    ParmDef p; p.name = n; p.label = l; p.type = ParmType::Vec3; p.def[0] = d.x; p.def[1] = d.y; p.def[2] = d.z; p.min = mn; p.max = mx; p.help = help; return p;
}
ParmDef M(const char* n, const char* l, std::vector<std::string> items, int d = 0) {
    ParmDef p; p.name = n; p.label = l; p.type = ParmType::Menu; p.menu = std::move(items); p.def[0] = float(d); return p;
}

float ef(const Node& n, const Graph& g, const char* name, float frame) {
    const Parm* p = n.parm(name);
    return p ? n.evalFloat(*p, 0, frame, g.fps) : 0.f;
}
Vec3 ev(const Node& n, const Graph& g, const char* name, float frame) {
    const Parm* p = n.parm(name);
    if (!p) return Vec3(0);
    return {n.evalFloat(*p, 0, frame, g.fps), n.evalFloat(*p, 1, frame, g.fps), n.evalFloat(*p, 2, frame, g.fps)};
}
Mat4 rotation(const Vec3& r) { return Mat4::rotateZ(radians(r.z)) * Mat4::rotateY(radians(r.y)) * Mat4::rotateX(radians(r.x)); }

std::vector<ParmDef> lightParms(float intensity, bool hasPos, bool hasRot) {
    std::vector<ParmDef> p;
    if (hasPos) p.push_back(V("t", "Translate", Vec3(0, 3, 0), -10, 10));
    if (hasRot) p.push_back(V("r", "Rotate", Vec3(0, 0, 0), -180, 180, "Lights point down their -Z axis"));
    ParmDef c = V("light_color", "Color", Vec3(1, 1, 1), 0, 1);
    c.help = "color";
    p.push_back(c);
    p.push_back(F("light_intensity", "Intensity", intensity, 0, intensity * 4));
    p.push_back(F("light_exposure", "Exposure", 0, -5, 5, "Brightness in stops: +1 doubles the light"));
    return p;
}

void addLight(std::vector<NodeType>& types, const char* name, const char* label, const char* desc, std::vector<ParmDef> parms) {
    NodeType t;
    t.name = name; t.label = label; t.category = "Lights and Cameras"; t.description = desc;
    t.parms = std::move(parms);
    t.cook = [](CookContext&) {}; // carries no geometry: the renderer and viewport read its parameters
    types.push_back(t);
}
} // namespace

bool isLightType(const std::string& n) {
    return n == "pointlight" || n == "spotlight" || n == "arealight" || n == "distantlight" || n == "envlight";
}

float SceneCamera::fovYDegrees() const {
    float aspect = float(std::max(1, resx)) / float(std::max(1, resy));
    float tanx = aperture * 0.5f / std::max(focal, 0.1f);
    return 2.f * std::atan(tanx / aspect) * 180.f / kPi;
}

Vec3 aimRotation(const Vec3& from, const Vec3& target) {
    Vec3 f = normalize(target - from);
    if (length(f) < 0.5f) return Vec3(0);
    return Vec3(std::asin(std::clamp(f.y, -1.f, 1.f)) * 180.f / kPi, std::atan2(-f.x, -f.z) * 180.f / kPi, 0);
}

std::vector<Light> collectLights(const Graph& g, float frame) {
    std::vector<Light> out;
    for (auto& up : g.nodes()) {
        const Node& n = *up;
        const std::string& tn = n.type().name;
        if (!isLightType(tn) || n.bypass) continue;
        Light l;
        l.name = n.name();
        l.color = ev(n, g, "light_color", frame);
        l.intensity = ef(n, g, "light_intensity", frame) * std::pow(2.f, ef(n, g, "light_exposure", frame));
        if (l.intensity <= 0) continue;
        l.pos = ev(n, g, "t", frame);
        Mat4 R = rotation(ev(n, g, "r", frame));
        l.dir = normalize(R.transformDir(Vec3(0, 0, -1)));
        l.ux = R.transformDir(Vec3(1, 0, 0));
        l.uy = R.transformDir(Vec3(0, 1, 0));
        if (tn == "pointlight") { l.type = Light::Point; l.radius = ef(n, g, "radius", frame); }
        else if (tn == "spotlight") {
            l.type = Light::Spot; l.radius = ef(n, g, "radius", frame);
            l.coneAngle = ef(n, g, "coneangle", frame); l.coneDelta = ef(n, g, "conedelta", frame);
        } else if (tn == "arealight") {
            l.type = Light::Area;
            Vec3 s = ev(n, g, "areasize", frame);
            l.sizeX = s.x; l.sizeY = s.y;
        } else if (tn == "distantlight") { l.type = Light::Distant; l.angle = ef(n, g, "angle", frame); }
        else { l.type = Light::Env; l.sky = ef(n, g, "env_mode", frame) < 0.5f; }
        out.push_back(l);
    }
    return out;
}

std::vector<SceneCamera> collectCameras(const Graph& g, float frame) {
    std::vector<SceneCamera> out;
    for (auto& up : g.nodes()) {
        const Node& n = *up;
        if (n.type().name != "camera") continue;
        SceneCamera c;
        c.name = n.name();
        c.eye = ev(n, g, "t", frame);
        c.fwd = normalize(rotation(ev(n, g, "r", frame)).transformDir(Vec3(0, 0, -1)));
        c.focal = std::max(1.f, ef(n, g, "focal", frame));
        c.aperture = std::max(1.f, ef(n, g, "aperture", frame));
        c.resx = std::clamp(int(ef(n, g, "resx", frame)), 16, 4096);
        c.resy = std::clamp(int(ef(n, g, "resy", frame)), 16, 4096);
        out.push_back(c);
    }
    return out;
}

void registerLightNodes(std::vector<NodeType>& types) {
    {
        NodeType t;
        t.name = "camera"; t.label = "Camera"; t.category = "Lights and Cameras";
        t.description = "A camera to look through and render from. Pick it from the Scene View's camera menu.";
        t.parms = {V("t", "Translate", Vec3(0, 2, 8), -20, 20), V("r", "Rotate", Vec3(-10, 0, 0), -180, 180, "Cameras look down their -Z axis (Z rotation / roll is ignored)"),
                   F("focal", "Focal Length", 50, 10, 200, "Millimetres. Larger = more zoomed in"), F("aperture", "Aperture", 41.4214f, 10, 100, "Horizontal film size in mm"),
                   I("resx", "Resolution X", 1280, 16, 4096), I("resy", "Resolution Y", 720, 16, 4096)};
        t.cook = [](CookContext&) {};
        types.push_back(t);
    }
    {
        auto p = lightParms(20, true, false);
        p.push_back(F("radius", "Size", 0.05f, 0, 1, "Bigger lights give softer shadows"));
        addLight(types, "pointlight", "Point Light", "Shines in all directions from a point.", p);
    }
    {
        auto p = lightParms(40, true, true);
        p.push_back(F("coneangle", "Cone Angle", 45, 1, 160));
        p.push_back(F("conedelta", "Cone Delta", 10, 0, 60, "Width of the soft edge, in degrees"));
        p.push_back(F("radius", "Size", 0.05f, 0, 1, "Bigger lights give softer shadows"));
        addLight(types, "spotlight", "Spot Light", "A cone of light pointing down the light's -Z axis.", p);
    }
    {
        auto p = lightParms(20, true, true);
        p.push_back(V("areasize", "Area Size", Vec3(1, 1, 0), 0, 5, "Width and height of the rectangle (third value unused)"));
        addLight(types, "arealight", "Area Light", "A glowing rectangle: soft light and soft shadows.", p);
    }
    {
        auto p = lightParms(3, true, true);
        p.push_back(F("angle", "Sun Angle", 1, 0, 20, "Angular size in degrees: larger = softer shadows"));
        addLight(types, "distantlight", "Distant Light", "Parallel light from far away, like the sun. Only its rotation matters; Translate just moves the icon.", p);
    }
    {
        auto p = lightParms(1, true, false);
        p.push_back(M("env_mode", "Type", {"Sky", "Constant Color (ambient)"}));
        addLight(types, "envlight", "Environment Light", "Light from all around: a sky, or one even color.", p);
    }
}

} // namespace hd
