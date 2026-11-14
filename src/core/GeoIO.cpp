#include "GeoIO.h"
#include <cstdio>
#include <fstream>
#include <sstream>

namespace hd {

bool loadObj(const std::string& path, Geometry& out, std::string* error) {
    std::ifstream f(path);
    if (!f) { if (error) *error = "cannot open '" + path + "'"; return false; }
    out.clear();
    std::vector<Vec3> normals;
    std::vector<Vec3> accN;
    bool anyNormal = false;
    std::string line;
    auto resolve = [](int idx, size_t count) { return idx < 0 ? int(count) + idx : idx - 1; };
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line);
        std::string tag;
        ss >> tag;
        if (tag == "v") {
            Vec3 p; ss >> p.x >> p.y >> p.z;
            out.P.push_back(p);
            accN.push_back(Vec3(0));
        } else if (tag == "vn") {
            Vec3 n; ss >> n.x >> n.y >> n.z;
            normals.push_back(n);
        } else if (tag == "f" || tag == "l") {
            std::vector<int> pts;
            std::string tok;
            while (ss >> tok) {
                int vi = 0, ti = 0, ni = 0;
                int parsed = std::sscanf(tok.c_str(), "%d/%d/%d", &vi, &ti, &ni);
                if (parsed < 3) { ni = 0; std::sscanf(tok.c_str(), "%d//%d", &vi, &ni); }
                int p = resolve(vi, out.P.size());
                if (p < 0 || p >= int(out.P.size())) continue;
                pts.push_back(p);
                if (ni != 0) {
                    int n = resolve(ni, normals.size());
                    if (n >= 0 && n < int(normals.size())) { accN[size_t(p)] += normals[size_t(n)]; anyNormal = true; }
                }
            }
            if ((tag == "f" && pts.size() >= 3) || (tag == "l" && pts.size() >= 2)) {
                Prim pr;
                pr.start = int(out.vertices.size());
                pr.count = int(pts.size());
                pr.closed = tag == "f";
                out.vertices.insert(out.vertices.end(), pts.begin(), pts.end());
                out.prims.push_back(pr);
            }
        }
    }
    if (anyNormal) {
        Attrib& N = out.addAttrib(AttribClass::Point, "N", 3);
        for (size_t i = 0; i < out.P.size(); ++i) {
            Vec3 n = normalize(accN[i]);
            N.setV(i, length(n) > 0.5f ? n : Vec3(0, 1, 0));
        }
    }
    return true;
}

bool saveObj(const std::string& path, const Geometry& geo, std::string* error) {
    std::ofstream f(path);
    if (!f) { if (error) *error = "cannot write '" + path + "'"; return false; }
    f << "# Exported from HD\n";
    char buf[128];
    for (auto& p : geo.P) {
        std::snprintf(buf, sizeof buf, "v %.6g %.6g %.6g\n", p.x, p.y, p.z);
        f << buf;
    }
    const Attrib* N = geo.findAttrib(AttribClass::Point, "N");
    if (N && N->tuple == 3) {
        for (size_t i = 0; i < geo.numPoints(); ++i) {
            Vec3 n = N->getV(i);
            std::snprintf(buf, sizeof buf, "vn %.6g %.6g %.6g\n", n.x, n.y, n.z);
            f << buf;
        }
    }
    for (size_t pi = 0; pi < geo.numPrims(); ++pi) {
        const Prim& pr = geo.prims[pi];
        f << (pr.closed ? "f" : "l");
        for (int k = 0; k < pr.count; ++k) {
            int v = geo.vertices[size_t(pr.start + k)] + 1;
            if (N && N->tuple == 3 && pr.closed) f << ' ' << v << "//" << v;
            else f << ' ' << v;
        }
        f << '\n';
    }
    return bool(f);
}

bool savePly(const std::string& path, const Geometry& geo, std::string* error) {
    std::ofstream f(path);
    if (!f) { if (error) *error = "cannot write '" + path + "'"; return false; }
    const Attrib* Cd = geo.findAttrib(AttribClass::Point, "Cd");
    f << "ply\nformat ascii 1.0\nelement vertex " << geo.numPoints() << "\nproperty float x\nproperty float y\nproperty float z\n";
    if (Cd) f << "property uchar red\nproperty uchar green\nproperty uchar blue\n";
    f << "end_header\n";
    for (size_t i = 0; i < geo.numPoints(); ++i) {
        f << geo.P[i].x << ' ' << geo.P[i].y << ' ' << geo.P[i].z;
        if (Cd) {
            Vec3 c = Cd->getV(i);
            auto b = [](float x) { return int(std::clamp(x, 0.f, 1.f) * 255.f + 0.5f); };
            f << ' ' << b(c.x) << ' ' << b(c.y) << ' ' << b(c.z);
        }
        f << '\n';
    }
    return bool(f);
}

} // namespace hd
