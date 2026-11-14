// Procedural node graph (SOP-style): nodes with parameters and inputs that
// cook Geometry on demand, with dependency tracking and time dependence.
#pragma once
#include "Expr.h"
#include "Geometry.h"
#include "Json.h"
#include "RenderSettings.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace hd {

enum class ParmType { Float, Int, Toggle, Vec3, String, Menu, Code, File };

struct ParmDef {
    std::string name;
    std::string label;
    ParmType type = ParmType::Float;
    float def[3] = {0, 0, 0};
    std::string defStr;
    float min = 0, max = 1;        // UI slider range
    std::vector<std::string> menu; // for Menu
    std::string help;
    int components() const { return type == ParmType::Vec3 ? 3 : 1; }
};

struct Parm {
    const ParmDef* def = nullptr;
    float val[3] = {0, 0, 0};
    std::string expr[3];  // per-component expression (empty = constant)
    std::string str;      // String / Code / File value
    bool isDefault() const;
};

class Node;
class Graph;

class CookContext {
public:
    CookContext(Node& n, Graph& g, std::vector<const Geometry*> ins, Geometry& out)
        : node_(n), graph_(g), inputs_(std::move(ins)), out(out) {}
    bool hasInput(int i) const { return i < int(inputs_.size()) && inputs_[i]; }
    const Geometry& input(int i) const;
    int numConnectedInputs() const;
    float f(const char* name) const;
    int i(const char* name) const;
    bool b(const char* name) const { return i(name) != 0; }
    Vec3 v(const char* name) const;
    const std::string& s(const char* name) const;
    float frame() const;
    float time() const;
    float fps() const;
    // Cook input i at another frame (for solvers). Returns false if the input is missing or errored.
    bool inputAtFrame(int i, float frame, Geometry& out);
    // Persistent per-node state that survives between cooks (simulation caches).
    template <class T> T& state();
    // Changes whenever this node's or any upstream node's parameters/wiring change (not with time).
    uint64_t setupSignature() const;
    void error(const std::string& m);
    void warning(const std::string& m);
    bool failed() const { return failed_; }
    // Solvers evaluate parameters at the frame being simulated rather than the current one.
    void setEvalFrame(float f) { evalFrame_ = f; hasEvalFrame_ = true; }
    void clearEvalFrame() { hasEvalFrame_ = false; }
    // True if input i (or anything upstream of it) changes with time.
    bool inputIsAnimated(int i) const;
    Node& node() { return node_; }

private:
    Node& node_;
    Graph& graph_;
    std::vector<const Geometry*> inputs_;
    bool failed_ = false;
    float evalFrame_ = 0;
    bool hasEvalFrame_ = false;
    float evalFrame() const;

public:
    Geometry& out;
};

using CookFn = std::function<void(CookContext&)>;

struct NodeType {
    std::string name;   // internal, e.g. "box"
    std::string label;  // UI, e.g. "Box"
    std::string category;
    std::string description;
    int minInputs = 0;
    int maxInputs = 0;
    std::vector<std::string> inputLabels;
    std::vector<ParmDef> parms;
    CookFn cook;
    bool timeDependent = false;
};

const std::vector<NodeType>& nodeTypes();
const NodeType* findNodeType(const std::string& name);

class Node {
public:
    Node(int id, const NodeType* type, std::string name);

    int id() const { return id_; }
    const NodeType& type() const { return *type_; }
    const std::string& name() const { return name_; }

    float x = 0, y = 0;       // network position
    bool bypass = false;
    std::string comment;

    std::vector<Node*> inputs; // size == type().maxInputs

    std::vector<Parm>& parms() { return parms_; }
    const std::vector<Parm>& parms() const { return parms_; }
    Parm* parm(const std::string& name);
    const Parm* parm(const std::string& name) const;
    // Evaluate a numeric parm component at the given frame (handles expressions).
    float evalFloat(const Parm& p, int comp, float frame, float fps) const;
    void touch() { ++parmVersion_; }   // mark parms changed
    bool isTimeDependent() const;

    // cook results
    const Geometry& geometry() const { return geo_; }
    const std::string& error() const { return error_; }
    const std::string& warning() const { return warning_; }
    double cookTimeMs() const { return cookMs_; }
    uint64_t cookStamp() const { return cookStamp_; }
    uint64_t parmVersion() const { return parmVersion_; }
    // Simulation nodes report which frames they have cached (for the timeline).
    int simCacheStart = 0, simCacheEnd = -1;

private:
    friend class Graph;
    friend class CookContext;
    int id_;
    const NodeType* type_;
    std::string name_;
    std::vector<Parm> parms_;

    Geometry geo_;
    std::string error_, warning_;
    double cookMs_ = 0;
    uint64_t parmVersion_ = 1;
    uint64_t cookStamp_ = 0;
    // memo of what we cooked with
    uint64_t cookedParmVersion_ = 0;
    float cookedFrame_ = -1e30f;
    bool cookedBypass_ = false;
    std::vector<uint64_t> cookedInputStamps_;
    bool cooking_ = false;
    std::shared_ptr<void> state_;
};

template <class T> T& CookContext::state() {
    if (!node_.state_) node_.state_ = std::make_shared<T>();
    return *static_cast<T*>(node_.state_.get());
}

class Graph {
public:
    Graph();
    ~Graph();

    Node* createNode(const std::string& type, float x = 0, float y = 0, const std::string& name = "");
    void removeNode(Node* n);
    Node* findNode(int id) const;
    Node* findNode(const std::string& name) const;
    const std::vector<std::unique_ptr<Node>>& nodes() const { return nodes_; }

    bool connect(Node* src, Node* dst, int inputIndex, std::string* error = nullptr);
    void disconnect(Node* dst, int inputIndex);
    bool wouldCreateCycle(Node* src, Node* dst) const;
    std::vector<std::pair<Node*, int>> outputsOf(const Node* n) const; // (consumer, input index)
    bool rename(Node* n, const std::string& newName);
    std::string uniqueName(const std::string& base) const;

    Node* displayNode() const { return display_; }
    void setDisplayNode(Node* n) { display_ = n; }

    float frame = 1.f;
    float fps = 24.f;
    int frameStart = 1;
    int frameEnd = 240;
    RenderSettings render;

    // Cook node (and its inputs as needed). Returns true if anything recooked.
    bool cook(Node* n);
    // Total number of node cooks performed (for stats/tests).
    uint64_t totalCooks() const { return totalCooks_; }
    // Hash of the parameters and wiring of n and everything upstream of it.
    uint64_t upstreamSignature(const Node* n) const;
    bool upstreamTimeDependent(const Node* n) const;

    void clear();
    Json toJson() const;
    bool fromJson(const Json& j, std::string* error = nullptr);
    bool save(const std::string& path, std::string* error = nullptr) const;
    bool load(const std::string& path, std::string* error = nullptr);

private:
    std::vector<std::unique_ptr<Node>> nodes_;
    Node* display_ = nullptr;
    int nextId_ = 1;
    uint64_t stampCounter_ = 0;
    uint64_t totalCooks_ = 0;
};

} // namespace hd
