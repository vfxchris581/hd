#pragma once
#include "Geometry.h"
#include <string>

namespace hd {

// Wavefront OBJ import / export (positions, normals, polygons, polylines).
bool loadObj(const std::string& path, Geometry& out, std::string* error = nullptr);
bool saveObj(const std::string& path, const Geometry& geo, std::string* error = nullptr);
// Simple ASCII PLY export of points + Cd (useful for point clouds).
bool savePly(const std::string& path, const Geometry& geo, std::string* error = nullptr);

} // namespace hd
