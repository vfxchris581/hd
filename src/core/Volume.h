// Volume helpers: voxelisation and a small parallel-for.
#pragma once
#include "Geometry.h"
#include <functional>
#include <vector>

namespace hd {

// Runs fn(begin, end) over [0, n) split across worker threads (blocks until done).
void parallelFor(int n, const std::function<void(int, int)>& fn);

// Marks the voxels covered by g. Closed meshes are filled solid; geometry without polygons
// is stamped as spheres at its points (radius from pscale, or pointRadius).
// Returns the number of voxels set.
size_t voxelizeGeometry(const Geometry& g, int nx, int ny, int nz, const Vec3& origin, float dx, float pointRadius,
                        std::vector<unsigned char>& mask);

} // namespace hd
