#pragma once
#include "Node.h"
#include <string>
#include <vector>

namespace hd {

struct DemoInfo { std::string name; std::string title; std::string description; };
const std::vector<DemoInfo>& demoScenes();
// Replaces the graph contents with a demo network. Returns false for unknown names.
bool buildDemoScene(Graph& g, const std::string& name);

} // namespace hd
