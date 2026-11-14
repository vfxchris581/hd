// Geometry operations shared by nodes (wrangle execution, group patterns, subdivision...).
#pragma once
#include "Expr.h"
#include "Geometry.h"
#include <string>
#include <vector>

namespace hd {

// Run a compiled VEX-lite program over every point (or primitive) of g.
// Attributes the program writes are created if missing. If results is given, it receives
// the program's final value per element (used for expression groups).
bool runOverElements(Geometry& g, AttribClass cls, const Program& prog, float frame, float fps,
                     std::vector<float>* results, std::string& error, float timeInc = -1.f);

// Select elements with a Houdini-like pattern:
//   ""  or "*"        everything
//   "0-10 15 20-30:2" numbers, ranges and stepped ranges
//   "@P.y > 0"        expression group (any VEX-lite expression; non-zero selects)
//   "!pattern"        invert
bool selectElements(const Geometry& g, AttribClass cls, const std::string& pattern, float frame, float fps,
                    std::vector<char>& mask, std::string& error);

// One level of Catmull-Clark subdivision (interpolates point attributes linearly).
Geometry catmullClark(const Geometry& in);

// Merge points closer than dist; removes degenerate primitives.
Geometry fusePoints(const Geometry& in, float dist);

} // namespace hd
