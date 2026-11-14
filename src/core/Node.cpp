#include "Node.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
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
    if (type_->timeDependent) return true;
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
float CookContext::f(const char* name) const {
    return node_.evalFloat(requireParm(node_, name), 0, graph_.frame, graph_.fps);
}
int CookContext::i(const char* name) const { return int(std::lround(f(name))); }
Vec3 CookContext::v(const char* name) const {
    const Parm& p = requireParm(node_, name);
    return {node_.evalFloat(p, 0, graph_.frame, graph_.fps), node_.evalFloat(p, 1, graph_.frame, graph_.fps),
            node_.evalFloat(p, 2, graph_.frame, graph_.fps)};
}
const std::string& CookContext::s(const char* name) const { return requireParm(node_, name).str; }
float CookContext::frame() const { return graph_.frame; }
float CookContext::fps() const { return graph_.fps; }
float CookContext::time() const { return (graph_.frame - 1.f) / graph_.fps; }
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
        n->cookStamp_ = ++stampCounter_;
        n->cookMs_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        ++totalCooks_;
        any = true;
    }
    n->cooking_ = false;
    return any;
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
        nodes.push(jn);
    }
    root["nodes"] = nodes;
    return root;
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
    const Json& nodes = j.get("nodes");
    std::vector<std::pair<Node*, const Json*>> created;
    std::string warnings;
    for (auto& jn : nodes.items()) {
        Node* n = createNode(jn.get("type").str(), float(jn.get("x").num()), float(jn.get("y").num()), jn.get("name").str());
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
        created.push_back({n, &jn});
    }
    for (auto& [n, jn] : created) {
        const Json& ins = jn->get("inputs");
        for (size_t i = 0; i < ins.size() && i < n->inputs.size(); ++i)
            if (!ins[i].isNull()) connect(findNode(ins[i].str()), n, int(i));
    }
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
