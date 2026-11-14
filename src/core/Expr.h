// VEX-lite: a small expression / snippet language used by Point Wrangle
// and by parameter expressions (e.g. "sin($T*2)*0.5").
#pragma once
#include "Math.h"
#include <memory>
#include <string>
#include <vector>

namespace hd {

struct Value {
    int n = 1; // 1 = float, 3 = vector
    float v[3] = {0, 0, 0};
    Value() = default;
    Value(float f) : n(1) { v[0] = v[1] = v[2] = f; }
    Value(const Vec3& p) : n(3) { v[0] = p.x; v[1] = p.y; v[2] = p.z; }
    float f() const { return v[0]; }
    Vec3 vec() const { return n == 3 ? Vec3(v[0], v[1], v[2]) : Vec3(v[0]); }
};

struct AttribBinding {
    std::string name;
    int tuple = 1;      // 1 or 3
    bool written = false;
    bool read = false;
};

// Globals available to programs.
enum class Global { Frame, Time, PtNum, NumPt, Pi, Fps, Count };

class Program {
public:
    ~Program();
    // Compile a snippet. In expression mode a single expression is expected and its value
    // is the result of run(); in snippet mode statements separated by ';' are allowed.
    static std::unique_ptr<Program> compile(const std::string& src, bool expressionMode, std::string& error);

    const std::vector<AttribBinding>& attribs() const { return attribs_; }
    bool usesTime() const { return usesTime_; }

    // Execution state. attribValues must be sized like attribs().
    struct Context {
        Value* attribValues = nullptr;
        float globals[int(Global::Count)] = {};
    };
    Value run(Context& ctx) const;

    struct Node;
private:
    Program() = default;
    std::vector<AttribBinding> attribs_;
    int numLocals_ = 0;
    bool usesTime_ = false;
    Node* root_ = nullptr;
    std::vector<std::unique_ptr<Node>> pool_;
    friend struct ExprParser;
};

// Convenience: evaluate a one-off parameter expression.
bool evalExpression(const std::string& src, float frame, float fps, float& out, std::string* error = nullptr);
// True if the string looks like an expression rather than a plain number.
bool isExpressionString(const std::string& s);

} // namespace hd
