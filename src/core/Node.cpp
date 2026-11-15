#include "Node.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <map>
#include <sstream>
#include <unordered_map>

namespace hd {

// ---------------------------------------------------------------- Parm
bool Parm::isDefault() const {
    if (!def) return true;
    if (def->type == ParmType::String || def->type == ParmType::Code || def->type == ParmType::File)
        return str == def->defStr;
    for (int c = 0; c < def->components(); ++c)
        if (val[c] != def->def[c] || !expr[c].empty()) return false;
    return true;
}

// ---------------------------------------------------------------- Node
Node::Node(int id, const NodeType* type, std::string name) : id_(id), type_(type), name_(std::move(name)) {
    inputs.assign(size_t(type->maxInputs), nullptr);
    parms_.reserve(type->parms.size());
    for (const ParmDef& d : type->parms) {
        Parm p;
        p.def = &d;
        for (int c = 0; c < 3; ++c) p.val[c] = d.def[c];
        p.str = d.defStr;
        parms_.push_back(p);
    }
}

Node::~Node() = default;

Parm* Node::parm(const std::string& name) {
    for (auto& p : parms_) if (p.def->name == name) return &p;
    return nullptr;
}
const Parm* Node::parm(const std::string& name) const {
    for (auto& p : parms_) if (p.def->name == name) return &p;
    return nullptr;
}

namespace {
struct CachedExpr {
    std::unique_ptr<Program> prog;
    std::string error;
};
std::unordered_map<std::string, CachedExpr>& exprCache() {
    static std::unordered_map<std::string, CachedExpr> c;
    return c;
}
} // namespace

float Node::evalFloat(const Parm& p, int comp, float frame, float fps) const {
    const std::string& e = p.expr[comp];
    if (e.empty()) return p.val[comp];
    auto& cache = exprCache();
    auto it = cache.find(e);
    if (it == cache.end()) {
        if (cache.size() > 4096) cache.clear();
        CachedExpr ce;
        ce.prog = Program::compile(e, true, ce.error);
        if (ce.prog && !ce.prog->attribs().empty()) { ce.prog.reset(); ce.error = "@attributes not allowed here"; }
        it = cache.emplace(e, std::move(ce)).first;
    }
    if (!it->second.prog) return p.val[comp];
    Program::Context ctx;
    ctx.globals[int(Global::Frame)] = frame;
    ctx.globals[int(Global::Time)] = (frame - 1.f) / fps;
    ctx.globals[int(Global::Fps)] = fps;
    return it->second.prog->run(ctx).f();
}

bool Node::isTimeDependent() const {
    if (type_->timeDependent || dynamicTimeDep_) return true;
    if (sub_)
        for (auto& c : sub_->nodes()) if (c->isTimeDependent()) return true;
    for (auto& p : parms_) {
        for (int c = 0; c < 3; ++c)
            if (!p.expr[c].empty() && (p.expr[c].find("$F") != std::string::npos || p.expr[c].find("$T") != std::string::npos))
                return true;
        if (p.def->type == ParmType::Code &&
            (p.str.find("@Time") != std::string::npos || p.str.find("@Frame") != std::string::npos ||
             p.str.find("$F") != std::string::npos || p.str.find("$T") != std::string::npos))
            return true;
    }
    return false;
}

// ---------------------------------------------------------------- CookContext
const Geometry& CookContext::input(int i) const {
    static const Geometry empty;
    return hasInput(i) ? *inputs_[i] : empty;
}
int CookContext::numConnectedInputs() const {
    int n = 0;
    for (auto* g : inputs_) if (g) ++n;
    return n;
}
static const Parm& requireParm(const Node& n, const char* name) {
    const Parm* p = n.parm(name);
    if (!p) throw std::runtime_error(std::string("internal: unknown parameter '") + name + "'");
    return *p;
}
float CookContext::evalFrame() const { return hasEvalFrame_ ? evalFrame_ : graph_.frame; }
float CookContext::f(const char* name) const {
    return node_.evalFloat(requireParm(node_, name), 0, evalFrame(), graph_.fps);
}
int CookContext::i(const char* name) const { return int(std::lround(f(name))); }
Vec3 CookContext::v(const char* name) const {
    const Parm& p = requireParm(node_, name);
    float fr = evalFrame();
    return {node_.evalFloat(p, 0, fr, graph_.fps), node_.evalFloat(p, 1, fr, graph_.fps), node_.evalFloat(p, 2, fr, graph_.fps)};
}
const std::string& CookContext::s(const char* name) const { return requireParm(node_, name).str; }
float CookContext::frame() const { return evalFrame(); }
float CookContext::fps() const { return graph_.fps; }
float CookContext::time() const { return (evalFrame() - 1.f) / graph_.fps; }
bool CookContext::inputIsAnimated(int i) const {
    if (i < 0 || i >= int(node_.inputs.size()) || !node_.inputs[size_t(i)]) return false;
    return graph_.upstreamTimeDependent(node_.inputs[size_t(i)]);
}
bool CookContext::inputAtFrame(int i, float frame, Geometry& out) {
    if (i < 0 || i >= int(node_.inputs.size()) || !node_.inputs[size_t(i)]) return false;
    Node* in = node_.inputs[size_t(i)];
    float saved = graph_.frame;
    graph_.frame = frame;
    graph_.cook(in);
    out = in->geo_;
    bool ok = in->error_.empty();
    graph_.frame = saved;
    graph_.cook(in); // leave the input cooked at the current frame
    return ok;
}
uint64_t CookContext::setupSignature() const { return graph_.upstreamSignature(&node_); }
void CookContext::error(const std::string& m) { failed_ = true; node_.error_ = m; }
void CookContext::warning(const std::string& m) { node_.warning_ = m; }

// ---------------------------------------------------------------- Graph
Graph::Graph() = default;
Graph::~Graph() = default;

void Graph::clear() {
    nodes_.clear();
    display_ = nullptr;
    nextId_ = 1;
    frame = 1;
    render = RenderSettings();
}

std::string Graph::uniqueName(const std::string& base) const {
    std::string stem = base;
    while (!stem.empty() && std::isdigit((unsigned char)stem.back())) stem.pop_back();
    if (stem.empty()) stem = "node";
    if (!findNode(base) && base != stem) return base;
    for (int i = 1;; ++i) {
        std::string n = stem + std::to_string(i);
        if (!findNode(n)) return n;
    }
}

Node* Graph::createNode(const std::string& typeName, float x, float y, const std::string& name) {
    const NodeType* t = findNodeType(typeName);
    if (!t) return nullptr;
    std::string nm = name.empty() ? uniqueName(t->name + "1") : (findNode(name) ? uniqueName(name) : name);
    nodes_.push_back(std::make_unique<Node>(nextId_++, t, nm));
    Node* n = nodes_.back().get();
    n->x = x; n->y = y;
    if (!display_) display_ = n;
    if (t->name == "subnet") {
        n->sub_ = std::make_unique<Graph>();
        n->sub_->parent_ = this;
        n->sub_->owner_ = n;
        // starts as a pass-through: Input 1 -> Output
        Node* in = n->sub_->createNode("input", 0, 0);
        Node* out = n->sub_->createNode("output", 0, 110);
        n->sub_->connect(in, out, 0);
        n->sub_->setDisplayNode(out);
    }
    return n;
}

void Graph::removeNode(Node* n) {
    if (!n) return;
    for (auto& o : nodes_)
        for (auto& in : o->inputs)
            if (in == n) in = nullptr;
    if (display_ == n) display_ = nullptr;
    nodes_.erase(std::remove_if(nodes_.begin(), nodes_.end(), [n](auto& p) { return p.get() == n; }), nodes_.end());
}

Node* Graph::findNode(int id) const {
    for (auto& n : nodes_) if (n->id() == id) return n.get();
    return nullptr;
}
Node* Graph::findNode(const std::string& name) const {
    for (auto& n : nodes_) if (n->name() == name) return n.get();
    return nullptr;
}

bool Graph::rename(Node* n, const std::string& newName) {
    if (newName.empty()) return false;
    for (char c : newName) if (!(std::isalnum((unsigned char)c) || c == '_')) return false;
    if (Node* o = findNode(newName)) return o == n;
    n->name_ = newName;
    return true;
}

bool Graph::wouldCreateCycle(Node* src, Node* dst) const {
    // A cycle forms if dst is reachable upstream from src (or src == dst).
    std::vector<Node*> stack{src};
    std::vector<Node*> seen;
    while (!stack.empty()) {
        Node* c = stack.back(); stack.pop_back();
        if (c == dst) return true;
        if (std::find(seen.begin(), seen.end(), c) != seen.end()) continue;
        seen.push_back(c);
        for (Node* in : c->inputs) if (in) stack.push_back(in);
    }
    return false;
}

bool Graph::connect(Node* src, Node* dst, int idx, std::string* error) {
    if (!src || !dst || idx < 0 || idx >= int(dst->inputs.size())) {
        if (error) *error = "invalid connection";
        return false;
    }
    if (wouldCreateCycle(src, dst)) {
        if (error) *error = "connection would create a cycle";
        return false;
    }
    dst->inputs[size_t(idx)] = src;
    return true;
}

void Graph::disconnect(Node* dst, int idx) {
    if (dst && idx >= 0 && idx < int(dst->inputs.size())) dst->inputs[size_t(idx)] = nullptr;
}

std::vector<std::pair<Node*, int>> Graph::outputsOf(const Node* n) const {
    std::vector<std::pair<Node*, int>> r;
    for (auto& o : nodes_)
        for (size_t i = 0; i < o->inputs.size(); ++i)
            if (o->inputs[i] == n) r.push_back({o.get(), int(i)});
    return r;
}

uint64_t Graph::upstreamSignature(const Node* root) const {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h](uint64_t v) { h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2); h *= 1099511628211ull; };
    std::vector<const Node*> stack{root}, seen;
    while (!stack.empty()) {
        const Node* n = stack.back(); stack.pop_back();
        if (std::find(seen.begin(), seen.end(), n) != seen.end()) continue;
        seen.push_back(n);
        mix(uint64_t(n->id()));
        mix(n->parmVersion_);
        mix(n->bypass ? 2 : 1);
        if (n->sub_) mix(n->sub_->contentSignature());
        for (const Node* in : n->inputs) {
            mix(in ? uint64_t(in->id()) + 17 : 3);
            if (in) stack.push_back(in);
        }
    }
    mix(uint64_t(fps * 1000));
    return h;
}

bool Graph::upstreamTimeDependent(const Node* root) const {
    std::vector<const Node*> stack{root}, seen;
    while (!stack.empty()) {
        const Node* n = stack.back(); stack.pop_back();
        if (std::find(seen.begin(), seen.end(), n) != seen.end()) continue;
        seen.push_back(n);
        if (n->isTimeDependent()) return true;
        for (const Node* in : n->inputs) if (in) stack.push_back(in);
    }
    return false;
}

bool Graph::cook(Node* n) {
    if (!n) return false;
    if (n->cooking_) {
        n->error_ = "Cycle detected while cooking";
        return false;
    }
    n->cooking_ = true;
    bool any = false;
    std::vector<uint64_t> stamps(n->inputs.size(), 0);
    std::vector<const Geometry*> ins(n->inputs.size(), nullptr);
    std::string inputError;
    for (size_t i = 0; i < n->inputs.size(); ++i) {
        Node* in = n->inputs[i];
        if (!in) continue;
        any |= cook(in);
        stamps[i] = in->cookStamp_;
        ins[i] = &in->geo_;
        if (!in->error_.empty() && inputError.empty()) inputError = "Error in input '" + in->name() + "'";
    }
    bool timeDep = n->isTimeDependent();
    bool need = n->cookStamp_ == 0 || n->cookedParmVersion_ != n->parmVersion_ || stamps != n->cookedInputStamps_ ||
                n->cookedBypass_ != n->bypass || (timeDep && n->cookedFrame_ != frame);
    if (n->sub_ && n->cookedSubSig_ != n->sub_->contentSignature()) need = true;
    if (need) {
        auto t0 = std::chrono::steady_clock::now();
        Geometry out;
        n->error_.clear();
        n->warning_.clear();
        int connected = 0;
        for (int i = 0; i < n->type().minInputs; ++i) if (ins[size_t(i)]) ++connected;
        if (!inputError.empty()) {
            n->error_ = inputError;
        } else if (n->bypass) {
            if (!ins.empty() && ins[0]) out = *ins[0];
        } else if (connected < n->type().minInputs) {
            n->error_ = "Not enough sources specified.";
        } else if (n->type().cook) {
            CookContext ctx(*n, *this, ins, out);
            try {
                n->type().cook(ctx);
            } catch (const std::exception& e) {
                n->error_ = e.what();
            }
            if (!n->error_.empty()) out.clear();
        }
        out.syncAttribSizes();
        n->geo_ = std::move(out);
        n->cookedInputStamps_ = stamps;
        n->cookedParmVersion_ = n->parmVersion_;
        n->cookedFrame_ = frame;
        n->cookedBypass_ = n->bypass;
        if (n->sub_) n->cookedSubSig_ = n->sub_->contentSignature();
        n->cookStamp_ = ++stampCounter_;
        n->cookMs_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        ++totalCooks_;
        any = true;
    }
    n->cooking_ = false;
    return any;
}

// ---------------------------------------------------------------- Subnetworks
Node* Graph::outputNode() const {
    for (auto& n : nodes_) if (n->type().name == "output") return n.get();
    return display_;
}

uint64_t Graph::contentSignature() const {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h](uint64_t v) { h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2); h *= 1099511628211ull; };
    for (auto& n : nodes_) {
        mix(uint64_t(n->id()));
        mix(n->parmVersion_);
        mix(n->bypass ? 2 : 1);
        for (const Node* in : n->inputs) mix(in ? uint64_t(in->id()) + 17 : 3);
        if (n->sub_) mix(n->sub_->contentSignature());
    }
    mix(display_ ? uint64_t(display_->id()) + 5 : 7);
    return h;
}

bool Graph::fetchExternal(int idx, float atFrame, Geometry& out, std::string* err) {
    if (!parent_ || !owner_ || idx < 0 || idx >= int(owner_->inputs.size())) return false;
    Node* in = owner_->inputs[size_t(idx)];
    if (!in) return false;
    const float saved = parent_->frame;
    parent_->frame = atFrame;
    parent_->cook(in);
    out = in->geo_;
    bool ok = in->error_.empty();
    if (!ok && err) *err = "Error in subnet input '" + in->name() + "'";
    if (atFrame != saved) {
        parent_->frame = saved;
        parent_->cook(in); // leave the outer node cooked at the current frame
    }
    return ok;
}

void Graph::cookSubnet(CookContext& c) {
    Node& node = c.node();
    Graph& outer = c.graph();
    Graph* sub = node.sub_.get();
    if (!sub) return;
    sub->frame = outer.frame;
    sub->fps = outer.fps;
    sub->frameStart = outer.frameStart;
    sub->frameEnd = outer.frameEnd;
    // tell the Subnet Input nodes when what they read has changed, and whether it is animated
    for (auto& ch : sub->nodes_) {
        if (ch->type().name != "input") continue;
        int idx = int(std::lround(ch->parm("index")->val[0])) - 1;
        Node* in = idx >= 0 && idx < int(node.inputs.size()) ? node.inputs[size_t(idx)] : nullptr;
        uint64_t sig = in ? outer.upstreamSignature(in) : 1;
        if (ch->extSig_ != sig) { ch->extSig_ = sig; ch->touch(); }
        ch->dynamicTimeDep_ = in && outer.upstreamTimeDependent(in);
    }
    Node* out = sub->outputNode();
    if (!out) { c.warning("The subnet is empty."); return; }
    sub->cook(out);
    if (!out->error_.empty()) { c.error("Inside subnet: " + out->name() + ": " + out->error_); return; }
    c.out = out->geo_;
}

Node* Graph::collapseToSubnet(const std::vector<Node*>& sel, std::string* err) {
    auto fail = [&](const char* m) { if (err) *err = m; return nullptr; };
    if (sel.empty()) return fail("Select the nodes to collapse first.");
    auto inSel = [&](const Node* n) { return n && std::find(sel.begin(), sel.end(), n) != sel.end(); };
    // the one node whose result leaves the selection
    Node* outNode = nullptr;
    std::vector<std::pair<Node*, int>> consumers;
    for (auto& o : nodes_) {
        if (inSel(o.get())) continue;
        for (size_t i = 0; i < o->inputs.size(); ++i) {
            Node* src = o->inputs[i];
            if (!inSel(src)) continue;
            if (outNode && outNode != src) return fail("The selection feeds other nodes from more than one node. A subnet has a single output.");
            outNode = src;
            consumers.push_back({o.get(), int(i)});
        }
    }
    const bool hadDisplay = inSel(display_);
    if (hadDisplay) {
        if (outNode && outNode != display_) return fail("The displayed node and the node feeding the rest of the network differ. A subnet has a single output.");
        outNode = display_;
    }
    if (!outNode) { // nothing downstream: use the lowest node that nothing else in the selection reads
        for (Node* n : sel) {
            bool read = false;
            for (Node* m : sel) for (Node* in : m->inputs) read |= in == n;
            if (!read && (!outNode || n->y > outNode->y)) outNode = n;
        }
        if (!outNode) outNode = sel.back();
    }
    // outside nodes feeding the selection become the subnet's inputs
    std::vector<Node*> ext;
    for (Node* n : sel)
        for (Node* src : n->inputs)
            if (src && !inSel(src) && std::find(ext.begin(), ext.end(), src) == ext.end()) ext.push_back(src);
    if (ext.size() > 4) return fail("The selection reads from more than 4 outside nodes. A subnet has 4 inputs.");

    float minx = 1e30f, miny = 1e30f, maxy = -1e30f, cx = 0, cy = 0;
    for (Node* n : sel) { minx = std::min(minx, n->x); miny = std::min(miny, n->y); maxy = std::max(maxy, n->y); cx += n->x; cy += n->y; }
    cx /= float(sel.size()); cy /= float(sel.size());
    std::vector<const Node*> csel(sel.begin(), sel.end());
    Json js = nodesToJson(csel);
    const std::string outName = outNode->name();
    struct Wire { std::string node; int input; int ext; };
    std::vector<Wire> wires;
    for (Node* n : sel)
        for (size_t k = 0; k < n->inputs.size(); ++k)
            for (size_t e = 0; e < ext.size(); ++e)
                if (n->inputs[k] == ext[e]) wires.push_back({n->name(), int(k), int(e)});

    Node* sn = createNode("subnet", cx, cy);
    if (!sn) return fail("Subnet node type is missing.");
    Graph* sub = sn->sub_.get();
    sub->clear();
    sub->addNodesFromJson(js, -minx, -miny + 120);
    for (size_t e = 0; e < ext.size(); ++e) {
        Node* in = sub->createNode("input", float(e) * 220.f, 0);
        in->parm("index")->val[0] = float(e + 1);
        for (auto& w : wires)
            if (w.ext == int(e)) sub->connect(in, sub->findNode(w.node), w.input);
    }
    Node* inner = sub->findNode(outName);
    Node* o = sub->createNode("output", inner ? inner->x : 0, maxy - miny + 240);
    sub->connect(inner, o, 0);
    sub->setDisplayNode(o);

    for (size_t e = 0; e < ext.size(); ++e) connect(ext[e], sn, int(e));
    for (Node* n : std::vector<Node*>(sel)) removeNode(n);
    for (auto& [dst, idx] : consumers) connect(sn, dst, idx);
    if (hadDisplay || !display_) display_ = sn;
    return sn;
}

// ---------------------------------------------------------------- Serialization
Json Graph::toJson() const {
    Json root = Json::object();
    root["format"] = "hd-graph";
    root["version"] = 1;
    root["frame"] = double(frame);
    root["fps"] = double(fps);
    Json range = Json::array();
    range.push(frameStart); range.push(frameEnd);
    root["range"] = range;
    root["display"] = display_ ? display_->name() : std::string();
    {
        const RenderSettings& r = render;
        Json j = Json::object();
        j["width"] = r.width; j["height"] = r.height; j["samples"] = r.samples; j["bounces"] = r.bounces;
        j["sunAzimuth"] = double(r.sunAzimuth); j["sunElevation"] = double(r.sunElevation);
        j["sunIntensity"] = double(r.sunIntensity); j["skyIntensity"] = double(r.skyIntensity);
        j["exposure"] = double(r.exposure); j["curveWidth"] = double(r.curveWidth);
        j["ground"] = r.ground; j["groundY"] = double(r.groundY);
        if (r.hasCamera) {
            Json c = Json::array();
            for (float v : {r.camTarget.x, r.camTarget.y, r.camTarget.z, r.camYaw, r.camPitch, r.camDist, r.camFov}) c.push(double(v));
            j["camera"] = c;
        }
        root["render"] = j;
    }
    Json nodes = Json::array();
    for (auto& n : nodes_) {
        Json jn = Json::object();
        jn["name"] = n->name();
        jn["type"] = n->type().name;
        jn["x"] = double(n->x);
        jn["y"] = double(n->y);
        if (n->bypass) jn["bypass"] = true;
        if (!n->comment.empty()) jn["comment"] = n->comment;
        Json parms = Json::object();
        for (auto& p : n->parms()) {
            if (p.isDefault()) continue;
            const ParmDef& d = *p.def;
            if (d.type == ParmType::String || d.type == ParmType::Code || d.type == ParmType::File) {
                parms[d.name] = p.str;
                continue;
            }
            if (d.components() == 3) {
                Json a = Json::array();
                for (int c = 0; c < 3; ++c) a.push(double(p.val[c]));
                parms[d.name] = a;
            } else {
                parms[d.name] = double(p.val[0]);
            }
            bool hasExpr = false;
            for (int c = 0; c < d.components(); ++c) hasExpr |= !p.expr[c].empty();
            if (hasExpr) {
                Json e = Json::array();
                for (int c = 0; c < d.components(); ++c) e.push(p.expr[c]);
                parms[d.name + ".expr"] = e;
            }
        }
        jn["parms"] = parms;
        Json inputs = Json::array();
        for (Node* in : n->inputs) inputs.push(in ? Json(in->name()) : Json());
        jn["inputs"] = inputs;
        if (n->sub_) jn["subnet"] = n->sub_->toJson();
        nodes.push(jn);
    }
    root["nodes"] = nodes;
    return root;
}

std::vector<Node*> Graph::addNodesFromJson(const Json& nodes, float dx, float dy, std::string* warningsOut) {
    std::vector<std::pair<Node*, const Json*>> created;
    std::map<std::string, Node*> byOldName; // names can change when they clash with existing nodes
    std::string warnings;
    for (auto& jn : nodes.items()) {
        Node* n = createNode(jn.get("type").str(), float(jn.get("x").num()) + dx, float(jn.get("y").num()) + dy, jn.get("name").str());
        if (!n) { warnings += "unknown node type '" + jn.get("type").str() + "'; "; continue; }
        n->bypass = jn.get("bypass").boolean();
        n->comment = jn.get("comment").str();
        const Json& parms = jn.get("parms");
        for (auto& p : n->parms()) {
            const ParmDef& d = *p.def;
            if (!parms.has(d.name)) continue;
            const Json& v = parms.get(d.name);
            if (d.type == ParmType::String || d.type == ParmType::Code || d.type == ParmType::File) {
                p.str = v.str();
                continue;
            }
            if (v.isArray()) for (int c = 0; c < 3 && c < int(v.size()); ++c) p.val[c] = float(v[size_t(c)].num());
            else p.val[0] = float(v.num());
            const Json& e = parms.get(d.name + ".expr");
            for (int c = 0; c < 3 && c < int(e.size()); ++c) p.expr[c] = e[size_t(c)].str();
        }
        if (n->sub_ && jn.get("subnet").isObject()) n->sub_->fromJson(jn.get("subnet"));
        byOldName[jn.get("name").str()] = n;
        created.push_back({n, &jn});
    }
    std::vector<Node*> result;
    for (auto& [n, jn] : created) {
        const Json& ins = jn->get("inputs");
        for (size_t i = 0; i < ins.size() && i < n->inputs.size(); ++i) {
            if (ins[i].isNull()) continue;
            auto it = byOldName.find(ins[i].str());
            if (it != byOldName.end()) connect(it->second, n, int(i));
        }
        result.push_back(n);
    }
    if (warningsOut) *warningsOut += warnings;
    return result;
}

Json Graph::nodesToJson(const std::vector<const Node*>& subset) const {
    Json all = toJson().get("nodes");
    Json out = Json::array();
    for (auto& jn : all.items())
        for (const Node* n : subset)
            if (n && n->name() == jn.get("name").str()) { out.push(jn); break; }
    return out;
}

bool Graph::fromJson(const Json& j, std::string* error) {
    if (!j.isObject() || j.get("format").str() != "hd-graph") {
        if (error) *error = "not an HD graph file";
        return false;
    }
    clear();
    frame = float(j.get("frame").num(1));
    fps = float(j.get("fps").num(24));
    if (j.get("range").size() == 2) {
        frameStart = int(j.get("range")[0].num(1));
        frameEnd = int(j.get("range")[1].num(240));
    }
    if (j.get("render").isObject()) {
        const Json& r = j.get("render");
        RenderSettings d;
        render.width = int(r.get("width").num(d.width)); render.height = int(r.get("height").num(d.height));
        render.samples = int(r.get("samples").num(d.samples)); render.bounces = int(r.get("bounces").num(d.bounces));
        render.sunAzimuth = float(r.get("sunAzimuth").num(d.sunAzimuth)); render.sunElevation = float(r.get("sunElevation").num(d.sunElevation));
        render.sunIntensity = float(r.get("sunIntensity").num(d.sunIntensity)); render.skyIntensity = float(r.get("skyIntensity").num(d.skyIntensity));
        render.exposure = float(r.get("exposure").num(d.exposure)); render.curveWidth = float(r.get("curveWidth").num(d.curveWidth));
        render.ground = r.get("ground").boolean(d.ground); render.groundY = float(r.get("groundY").num(d.groundY));
        const Json& c = r.get("camera");
        if (c.size() == 7) {
            render.hasCamera = true;
            render.camTarget = Vec3(float(c[0].num()), float(c[1].num()), float(c[2].num()));
            render.camYaw = float(c[3].num()); render.camPitch = float(c[4].num()); render.camDist = float(c[5].num()); render.camFov = float(c[6].num());
        }
    }
    std::string warnings;
    addNodesFromJson(j.get("nodes"), 0, 0, &warnings);
    display_ = findNode(j.get("display").str());
    if (!display_ && !nodes_.empty()) display_ = nodes_.back().get();
    if (error && !warnings.empty()) *error = warnings;
    return true;
}

bool Graph::save(const std::string& path, std::string* error) const {
    std::ofstream f(path, std::ios::binary);
    if (!f) { if (error) *error = "cannot write " + path; return false; }
    f << toJson().dump(2) << "\n";
    return bool(f);
}

bool Graph::load(const std::string& path, std::string* error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { if (error) *error = "cannot open " + path; return false; }
    std::stringstream ss;
    ss << f.rdbuf();
    std::string perr;
    Json j = Json::parse(ss.str(), &perr);
    if (!perr.empty()) { if (error) *error = perr; return false; }
    return fromJson(j, error);
}

} // namespace hd
