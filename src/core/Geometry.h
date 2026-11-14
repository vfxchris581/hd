#pragma once
#include "Math.h"
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace hd {

enum class AttribClass { Point, Prim };

// A float attribute with tuple size 1 (float) or 3 (vector).
struct Attrib {
    int tuple = 1;
    std::vector<float> data;
    float defaults[3] = {0, 0, 0};

    size_t count() const { return tuple ? data.size() / tuple : 0; }
    float getF(size_t i) const { return data[i * tuple]; }
    Vec3 getV(size_t i) const {
        if (tuple == 3) return {data[i * 3], data[i * 3 + 1], data[i * 3 + 2]};
        float f = data[i * tuple];
        return {f, f, f};
    }
    void setF(size_t i, float v) {
        for (int k = 0; k < tuple; ++k) data[i * tuple + k] = v;
    }
    void setV(size_t i, const Vec3& v) {
        if (tuple == 3) { data[i * 3] = v.x; data[i * 3 + 1] = v.y; data[i * 3 + 2] = v.z; }
        else data[i * tuple] = v.x;
    }
    void resize(size_t n) {
        size_t old = count();
        data.resize(n * tuple);
        for (size_t i = old; i < n; ++i)
            for (int k = 0; k < tuple; ++k) data[i * tuple + k] = defaults[k];
    }
    void appendFrom(const Attrib& src, size_t i) {
        for (int k = 0; k < tuple; ++k) data.push_back(k < src.tuple ? src.data[i * src.tuple + k] : src.data[i * src.tuple]);
    }
};

struct Prim {
    int start = 0;  // first index into Geometry::vertices
    int count = 0;  // number of vertices
    bool closed = true; // closed polygon vs open polyline
};

// Dense voxel grid (fog / smoke / fire). Voxel data is shared, so copies are cheap.
// Layout: index = i + nx * (j + ny * k).
struct Volume {
    int nx = 0, ny = 0, nz = 0;
    Vec3 origin{0, 0, 0}; // world position of the grid's minimum corner
    float dx = 0.1f;      // voxel size
    std::shared_ptr<const std::vector<float>> density;
    std::shared_ptr<const std::vector<float>> heat; // optional (fire emission)
    Vec3 color{0.75f, 0.75f, 0.78f};
    float densityScale = 1.f; // render opacity multiplier
    float fire = 0.f;         // emission intensity from heat
    size_t voxels() const { return size_t(nx) * size_t(ny) * size_t(nz); }
    Vec3 bmin() const { return origin; }
    Vec3 bmax() const { return origin + Vec3(float(nx), float(ny), float(nz)) * dx; }
};

// Polygon soup geometry with named point & primitive attributes. "P" is stored separately.
class Geometry {
public:
    std::vector<Vec3> P;
    std::vector<int> vertices; // point indices
    std::vector<Prim> prims;
    std::map<std::string, Attrib> pointAttribs;
    std::map<std::string, Attrib> primAttribs;
    std::vector<Volume> volumes;

    void clear();
    bool empty() const { return P.empty() && volumes.empty(); }
    size_t numPoints() const { return P.size(); }
    size_t numPrims() const { return prims.size(); }
    size_t numVertices() const { return vertices.size(); }

    int addPoint(const Vec3& p);
    int addPrim(const std::vector<int>& pts, bool closed = true);
    int addPrim(std::initializer_list<int> pts, bool closed = true) { return addPrim(std::vector<int>(pts), closed); }
    int primVertex(int prim, int k) const { return vertices[prims[prim].start + k]; }

    Attrib* findAttrib(AttribClass c, const std::string& name);
    const Attrib* findAttrib(AttribClass c, const std::string& name) const;
    Attrib& addAttrib(AttribClass c, const std::string& name, int tuple, Vec3 def = Vec3(0));
    void removeAttrib(AttribClass c, const std::string& name);
    std::map<std::string, Attrib>& attribs(AttribClass c) { return c == AttribClass::Point ? pointAttribs : primAttribs; }
    const std::map<std::string, Attrib>& attribs(AttribClass c) const { return c == AttribClass::Point ? pointAttribs : primAttribs; }

    // Append another geometry, unioning attributes (missing ones filled with defaults).
    void merge(const Geometry& other);
    void transform(const Mat4& m);
    void bounds(Vec3& mn, Vec3& mx) const;
    Vec3 primNormal(int prim) const;
    Vec3 primCenter(int prim) const;
    float primArea(int prim) const;
    // Area-weighted smooth point normals written to the "N" point attribute.
    void computePointNormals();
    // Keep only the listed prims (and the points they use).
    Geometry extractPrims(const std::vector<int>& keepPrims) const;
    // Keep only the listed points; prims referencing removed points are dropped.
    Geometry extractPoints(const std::vector<char>& keepMask) const;
    // Remove points not referenced by any primitive.
    void removeUnusedPoints();
    // Make sure every point attribute has exactly numPoints() entries (and prims likewise).
    void syncAttribSizes();
    // Copy point attribute values from point src (in geo `from`) to point dst in this geo.
    void copyPointAttribs(const Geometry& from, int src, int dst);
};

} // namespace hd
