#include "Expr.h"
#include "Noise.h"
#include <cctype>
#include <cstdlib>
#include <map>
#include <stdexcept>

namespace hd {

enum Fn {
    F_SIN, F_COS, F_TAN, F_ASIN, F_ACOS, F_ATAN, F_ATAN2, F_SQRT, F_ABS, F_FLOOR, F_CEIL, F_FRAC, F_ROUND,
    F_POW, F_EXP, F_LOG, F_MIN, F_MAX, F_CLAMP, F_FIT, F_FIT01, F_LERP, F_SMOOTH, F_LENGTH, F_NORMALIZE,
    F_DOT, F_CROSS, F_SET, F_NOISE, F_CURLNOISE, F_RAND, F_DEGREES, F_RADIANS, F_SIGN, F_MOD, F_DISTANCE,
    F_HSV2RGB, F_COUNT
};

struct FnInfo { const char* name; int minArgs, maxArgs; };
static const FnInfo kFns[F_COUNT] = {
    {"sin", 1, 1}, {"cos", 1, 1}, {"tan", 1, 1}, {"asin", 1, 1}, {"acos", 1, 1}, {"atan", 1, 1}, {"atan2", 2, 2},
    {"sqrt", 1, 1}, {"abs", 1, 1}, {"floor", 1, 1}, {"ceil", 1, 1}, {"frac", 1, 1}, {"rint", 1, 1},
    {"pow", 2, 2}, {"exp", 1, 1}, {"log", 1, 1}, {"min", 2, 2}, {"max", 2, 2}, {"clamp", 3, 3},
    {"fit", 5, 5}, {"fit01", 3, 3}, {"lerp", 3, 3}, {"smooth", 3, 3}, {"length", 1, 1}, {"normalize", 1, 1},
    {"dot", 2, 2}, {"cross", 2, 2}, {"set", 1, 3}, {"noise", 1, 1}, {"curlnoise", 1, 1}, {"rand", 1, 1},
    {"degrees", 1, 1}, {"radians", 1, 1}, {"sign", 1, 1}, {"mod", 2, 2}, {"distance", 2, 2}, {"hsvtorgb", 1, 1},
};

enum Op {
    O_ADD, O_SUB, O_MUL, O_DIV, O_MOD, O_LT, O_GT, O_LE, O_GE, O_EQ, O_NE, O_AND, O_OR, O_NEG, O_NOT,
    O_SET, O_ADDSET, O_SUBSET, O_MULSET, O_DIVSET
};

struct Program::Node {
    enum K { Num, Attr, Local, Glob, Unary, Binary, Ternary, Call, Comp, Assign, Block, If, For, Decl, ExprStmt } k;
    float num = 0;
    int slot = 0;     // attr / local / global index
    int op = 0;
    int fn = 0;
    int comp = -1;    // component for lvalues / Comp
    int declVec = 0;  // declaration type (1 vector)
    Node* a = nullptr; Node* b = nullptr; Node* c = nullptr; Node* d = nullptr;
    std::vector<Node*> kids;
};

Program::~Program() = default;

namespace {

enum class Tk { Num, Ident, Attr, Dollar, Op, End };
struct Token {
    Tk t = Tk::End;
    std::string s;
    float num = 0;
    char attrType = 0; // 'f','v','i' prefix or 0
    size_t pos = 0;
};

std::vector<Token> tokenize(const std::string& src) {
    std::vector<Token> out;
    size_t i = 0, n = src.size();
    auto isId = [](char c) { return std::isalnum((unsigned char)c) || c == '_'; };
    while (i < n) {
        char c = src[i];
        if (std::isspace((unsigned char)c)) { ++i; continue; }
        if (c == '/' && i + 1 < n && src[i + 1] == '/') { while (i < n && src[i] != '\n') ++i; continue; }
        if (c == '/' && i + 1 < n && src[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(src[i] == '*' && src[i + 1] == '/')) ++i;
            i += 2;
            continue;
        }
        Token t; t.pos = i;
        if (std::isdigit((unsigned char)c) || (c == '.' && i + 1 < n && std::isdigit((unsigned char)src[i + 1]))) {
            char* end = nullptr;
            t.num = std::strtof(src.c_str() + i, &end);
            i = size_t(end - src.c_str());
            t.t = Tk::Num;
            out.push_back(t);
            continue;
        }
        // typed attribute: v@foo, f@foo, i@foo
        if ((c == 'v' || c == 'f' || c == 'i' || c == 's') && i + 1 < n && src[i + 1] == '@') {
            t.attrType = c;
            i += 2;
            size_t s = i;
            while (i < n && isId(src[i])) ++i;
            t.t = Tk::Attr; t.s = src.substr(s, i - s);
            if (t.s.empty()) throw std::runtime_error("expected attribute name after @");
            out.push_back(t);
            continue;
        }
        if (c == '@' || c == '$') {
            ++i;
            size_t s = i;
            while (i < n && isId(src[i])) ++i;
            t.t = c == '@' ? Tk::Attr : Tk::Dollar;
            t.s = src.substr(s, i - s);
            if (t.s.empty()) throw std::runtime_error(std::string("expected name after ") + c);
            out.push_back(t);
            continue;
        }
        if (std::isalpha((unsigned char)c) || c == '_') {
            size_t s = i;
            while (i < n && isId(src[i])) ++i;
            t.t = Tk::Ident; t.s = src.substr(s, i - s);
            out.push_back(t);
            continue;
        }
        static const char* ops2[] = {"+=", "-=", "*=", "/=", "==", "!=", "<=", ">=", "&&", "||", "++", "--"};
        bool found = false;
        for (const char* o : ops2) {
            if (src.compare(i, 2, o) == 0) { t.t = Tk::Op; t.s = o; i += 2; found = true; break; }
        }
        if (!found) {
            if (std::string("+-*/%<>=!?:;,(){}.").find(c) == std::string::npos)
                throw std::runtime_error(std::string("unexpected character '") + c + "'");
            t.t = Tk::Op; t.s = std::string(1, c); ++i;
        }
        out.push_back(t);
    }
    Token e; e.t = Tk::End; e.pos = n;
    out.push_back(e);
    return out;
}

bool isVectorAttrName(const std::string& n) {
    return n == "P" || n == "N" || n == "Cd" || n == "v" || n == "up" || n == "uv" || n == "rest" || n == "force" || n == "scale";
}

} // namespace

using PNode = Program::Node;

struct ExprParser {
    Program& prog;
    std::vector<Token> toks;
    size_t p = 0;
    struct LocalVar { int slot; bool vec; };
    std::vector<std::map<std::string, LocalVar>> scopes;
    std::map<std::string, int> attrIndex;

    ExprParser(Program& pr, std::vector<Token> t) : prog(pr), toks(std::move(t)) { scopes.emplace_back(); }

    [[noreturn]] void fail(const std::string& m) {
        throw std::runtime_error(m + " (at char " + std::to_string(toks[p].pos) + ")");
    }
    const Token& cur() const { return toks[p]; }
    bool isOp(const char* s) const { return cur().t == Tk::Op && cur().s == s; }
    bool acceptOp(const char* s) { if (isOp(s)) { ++p; return true; } return false; }
    void expectOp(const char* s) { if (!acceptOp(s)) fail(std::string("expected '") + s + "'"); }
    bool isIdent(const char* s) const { return cur().t == Tk::Ident && cur().s == s; }

    PNode* mk(PNode::K k) {
        prog.pool_.push_back(std::make_unique<PNode>());
        PNode* n = prog.pool_.back().get();
        n->k = k;
        return n;
    }

    int attrSlot(const std::string& name, char type) {
        auto it = attrIndex.find(name);
        if (it != attrIndex.end()) return it->second;
        AttribBinding b;
        b.name = name;
        b.tuple = type == 'v' ? 3 : (type == 'f' || type == 'i') ? 1 : (isVectorAttrName(name) ? 3 : 1);
        prog.attribs_.push_back(b);
        int idx = int(prog.attribs_.size()) - 1;
        attrIndex[name] = idx;
        return idx;
    }

    LocalVar* findLocal(const std::string& n) {
        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
            auto f = it->find(n);
            if (f != it->end()) return &f->second;
        }
        return nullptr;
    }

    // ---- statements
    PNode* program(bool exprMode) {
        if (exprMode) {
            PNode* e = expression();
            if (cur().t != Tk::End) fail("unexpected token '" + cur().s + "'");
            return e;
        }
        PNode* blk = mk(PNode::Block);
        while (cur().t != Tk::End) blk->kids.push_back(statement());
        return blk;
    }

    PNode* statement() {
        if (acceptOp(";")) return mk(PNode::Block);
        if (acceptOp("{")) {
            scopes.emplace_back();
            PNode* blk = mk(PNode::Block);
            while (!isOp("}")) {
                if (cur().t == Tk::End) fail("missing '}'");
                blk->kids.push_back(statement());
            }
            ++p;
            scopes.pop_back();
            return blk;
        }
        if (isIdent("if")) {
            ++p;
            expectOp("(");
            PNode* n = mk(PNode::If);
            n->a = expression();
            expectOp(")");
            n->b = statement();
            if (isIdent("else")) { ++p; n->c = statement(); }
            return n;
        }
        if (isIdent("for")) {
            ++p;
            expectOp("(");
            scopes.emplace_back();
            PNode* n = mk(PNode::For);
            n->a = isOp(";") ? nullptr : simpleStatement();
            expectOp(";");
            n->b = isOp(";") ? nullptr : expression();
            expectOp(";");
            n->c = isOp(")") ? nullptr : expression();
            expectOp(")");
            n->d = statement();
            scopes.pop_back();
            return n;
        }
        PNode* s = simpleStatement();
        expectOp(";");
        return s;
    }

    PNode* simpleStatement() {
        if (isIdent("float") || isIdent("vector") || isIdent("int")) {
            bool vec = cur().s == "vector";
            ++p;
            PNode* blk = mk(PNode::Block);
            do {
                if (cur().t != Tk::Ident) fail("expected variable name");
                std::string name = cur().s;
                ++p;
                PNode* d = mk(PNode::Decl);
                d->slot = prog.numLocals_++;
                if (prog.numLocals_ > 64) fail("too many local variables");
                d->declVec = vec;
                if (acceptOp("=")) d->a = expression();
                scopes.back()[name] = {d->slot, vec};
                blk->kids.push_back(d);
            } while (acceptOp(","));
            return blk;
        }
        PNode* n = mk(PNode::ExprStmt);
        n->a = expression();
        return n;
    }

    // ---- expressions
    PNode* expression() { return assignment(); }

    bool isLValue(PNode* n) {
        if (n->k == PNode::Attr || n->k == PNode::Local) return true;
        if (n->k == PNode::Comp && (n->a->k == PNode::Attr || n->a->k == PNode::Local)) return true;
        return false;
    }

    PNode* assignment() {
        PNode* lhs = ternary();
        static const std::pair<const char*, int> aops[] = {{"=", O_SET}, {"+=", O_ADDSET}, {"-=", O_SUBSET}, {"*=", O_MULSET}, {"/=", O_DIVSET}};
        for (auto& [s, op] : aops) {
            if (isOp(s)) {
                if (!isLValue(lhs)) fail("left side of assignment is not assignable");
                ++p;
                PNode* n = mk(PNode::Assign);
                n->op = op;
                n->a = lhs;
                n->b = assignment();
                markWritten(lhs);
                return n;
            }
        }
        if (isOp("++") || isOp("--")) {
            if (!isLValue(lhs)) fail("operand of ++/-- is not assignable");
            int op = cur().s == "++" ? O_ADDSET : O_SUBSET;
            ++p;
            PNode* n = mk(PNode::Assign);
            n->op = op; n->a = lhs;
            n->b = mk(PNode::Num); n->b->num = 1;
            markWritten(lhs);
            return n;
        }
        return lhs;
    }
    void markWritten(PNode* lv) {
        PNode* base = lv->k == PNode::Comp ? lv->a : lv;
        if (base->k == PNode::Attr) prog.attribs_[base->slot].written = true;
    }

    PNode* ternary() {
        PNode* c = logicOr();
        if (acceptOp("?")) {
            PNode* n = mk(PNode::Ternary);
            n->a = c;
            n->b = expression();
            expectOp(":");
            n->c = ternary();
            return n;
        }
        return c;
    }
    PNode* bin(int op, PNode* a, PNode* b) { PNode* n = mk(PNode::Binary); n->op = op; n->a = a; n->b = b; return n; }
    PNode* logicOr() { PNode* l = logicAnd(); while (acceptOp("||")) l = bin(O_OR, l, logicAnd()); return l; }
    PNode* logicAnd() { PNode* l = equality(); while (acceptOp("&&")) l = bin(O_AND, l, equality()); return l; }
    PNode* equality() {
        PNode* l = relational();
        while (true) {
            if (acceptOp("==")) l = bin(O_EQ, l, relational());
            else if (acceptOp("!=")) l = bin(O_NE, l, relational());
            else return l;
        }
    }
    PNode* relational() {
        PNode* l = additive();
        while (true) {
            if (acceptOp("<")) l = bin(O_LT, l, additive());
            else if (acceptOp(">")) l = bin(O_GT, l, additive());
            else if (acceptOp("<=")) l = bin(O_LE, l, additive());
            else if (acceptOp(">=")) l = bin(O_GE, l, additive());
            else return l;
        }
    }
    PNode* additive() {
        PNode* l = multiplicative();
        while (true) {
            if (acceptOp("+")) l = bin(O_ADD, l, multiplicative());
            else if (acceptOp("-")) l = bin(O_SUB, l, multiplicative());
            else return l;
        }
    }
    PNode* multiplicative() {
        PNode* l = unary();
        while (true) {
            if (acceptOp("*")) l = bin(O_MUL, l, unary());
            else if (acceptOp("/")) l = bin(O_DIV, l, unary());
            else if (acceptOp("%")) l = bin(O_MOD, l, unary());
            else return l;
        }
    }
    PNode* unary() {
        if (acceptOp("-")) { PNode* n = mk(PNode::Unary); n->op = O_NEG; n->a = unary(); return n; }
        if (acceptOp("+")) return unary();
        if (acceptOp("!")) { PNode* n = mk(PNode::Unary); n->op = O_NOT; n->a = unary(); return n; }
        return postfix();
    }
    PNode* postfix() {
        PNode* e = primary();
        while (isOp(".")) {
            ++p;
            if (cur().t != Tk::Ident || cur().s.size() != 1) fail("expected component .x .y .z (or .r .g .b)");
            char c = cur().s[0];
            int comp = (c == 'x' || c == 'r' || c == 'u') ? 0 : (c == 'y' || c == 'g' || c == 'v') ? 1 : (c == 'z' || c == 'b' || c == 'w') ? 2 : -1;
            if (comp < 0) fail("bad component ." + cur().s);
            ++p;
            PNode* n = mk(PNode::Comp);
            n->a = e; n->comp = comp;
            e = n;
        }
        return e;
    }
    PNode* primary() {
        const Token& t = cur();
        if (t.t == Tk::Num) { ++p; PNode* n = mk(PNode::Num); n->num = t.num; return n; }
        if (acceptOp("(")) { PNode* e = expression(); expectOp(")"); return e; }
        if (acceptOp("{")) {
            // vector literal {a, b, c}
            PNode* n = mk(PNode::Call);
            n->fn = F_SET;
            do { n->kids.push_back(expression()); } while (acceptOp(","));
            expectOp("}");
            if (n->kids.size() != 3) fail("vector literal needs 3 components");
            return n;
        }
        if (t.t == Tk::Attr) {
            ++p;
            std::string name = t.s;
            if (name == "Frame") { prog.usesTime_ = true; return glob(Global::Frame); }
            if (name == "Time") { prog.usesTime_ = true; return glob(Global::Time); }
            if (name == "ptnum" || name == "primnum" || name == "elemnum") return glob(Global::PtNum);
            if (name == "numpt" || name == "numprim" || name == "numelem") return glob(Global::NumPt);
            PNode* n = mk(PNode::Attr);
            n->slot = attrSlot(name, t.attrType);
            prog.attribs_[n->slot].read = true;
            return n;
        }
        if (t.t == Tk::Dollar) {
            ++p;
            const std::string& v = t.s;
            if (v == "F" || v == "FF") { prog.usesTime_ = true; return glob(Global::Frame); }
            if (v == "T") { prog.usesTime_ = true; return glob(Global::Time); }
            if (v == "PT") return glob(Global::PtNum);
            if (v == "NPT") return glob(Global::NumPt);
            if (v == "PI") return glob(Global::Pi);
            if (v == "FPS") return glob(Global::Fps);
            fail("unknown variable $" + v);
        }
        if (t.t == Tk::Ident) {
            std::string name = t.s;
            ++p;
            if (acceptOp("(")) {
                int fn = -1;
                for (int i = 0; i < F_COUNT; ++i) if (name == kFns[i].name) { fn = i; break; }
                if (name == "atan" ) fn = F_ATAN;
                if (fn < 0) fail("unknown function '" + name + "'");
                PNode* n = mk(PNode::Call);
                n->fn = fn;
                if (!isOp(")")) { do { n->kids.push_back(expression()); } while (acceptOp(",")); }
                expectOp(")");
                if (fn == F_ATAN && n->kids.size() == 2) n->fn = F_ATAN2;
                const FnInfo& fi = kFns[n->fn];
                if (int(n->kids.size()) < fi.minArgs || int(n->kids.size()) > fi.maxArgs)
                    fail("wrong number of arguments to " + name + "()");
                return n;
            }
            if (LocalVar* lv = findLocal(name)) {
                PNode* n = mk(PNode::Local);
                n->slot = lv->slot;
                return n;
            }
            if (name == "PI" || name == "M_PI") return glob(Global::Pi);
            fail("unknown identifier '" + name + "'");
        }
        if (t.t == Tk::End) fail("unexpected end of input");
        fail("unexpected token '" + t.s + "'");
    }
    PNode* glob(Global g) { PNode* n = mk(PNode::Glob); n->slot = int(g); return n; }
};

namespace {

struct Exec {
    Program::Context& ctx;
    Value locals[64];

    static Value mapv(const Value& a, float (*f)(float)) {
        Value r = a;
        for (int i = 0; i < 3; ++i) r.v[i] = f(a.v[i]);
        return r;
    }
    static Value zip(const Value& a, const Value& b, float (*f)(float, float)) {
        Value r;
        r.n = std::max(a.n, b.n);
        for (int i = 0; i < 3; ++i) r.v[i] = f(a.v[a.n == 3 ? i : 0], b.v[b.n == 3 ? i : 0]);
        return r;
    }
    static bool truthy(const Value& v) { return v.v[0] != 0.f; }

    Value eval(const PNode* n) {
        switch (n->k) {
        case PNode::Num: return Value(n->num);
        case PNode::Attr: return ctx.attribValues[n->slot];
        case PNode::Local: return locals[n->slot];
        case PNode::Glob: return Value(ctx.globals[n->slot]);
        case PNode::Comp: { Value v = eval(n->a); return Value(v.v[v.n == 3 ? n->comp : 0]); }
        case PNode::Unary: {
            Value a = eval(n->a);
            if (n->op == O_NEG) { for (float& f : a.v) f = -f; return a; }
            return Value(truthy(a) ? 0.f : 1.f);
        }
        case PNode::Binary: {
            if (n->op == O_AND) return Value(truthy(eval(n->a)) && truthy(eval(n->b)) ? 1.f : 0.f);
            if (n->op == O_OR) return Value(truthy(eval(n->a)) || truthy(eval(n->b)) ? 1.f : 0.f);
            Value a = eval(n->a), b = eval(n->b);
            return binop(n->op, a, b);
        }
        case PNode::Ternary: return truthy(eval(n->a)) ? eval(n->b) : eval(n->c);
        case PNode::Call: return call(n);
        case PNode::Assign: return assign(n);
        case PNode::Block: { for (auto* k : n->kids) eval(k); return Value(); }
        case PNode::If: {
            if (truthy(eval(n->a))) eval(n->b);
            else if (n->c) eval(n->c);
            return Value();
        }
        case PNode::For: {
            if (n->a) eval(n->a);
            for (int guard = 0; guard < 100000; ++guard) {
                if (n->b && !truthy(eval(n->b))) break;
                eval(n->d);
                if (n->c) eval(n->c);
            }
            return Value();
        }
        case PNode::Decl: {
            Value v = n->a ? eval(n->a) : Value(0.f);
            locals[n->slot] = coerce(v, n->declVec ? 3 : 1);
            return locals[n->slot];
        }
        case PNode::ExprStmt: eval(n->a); return Value();
        }
        return Value();
    }

    static Value coerce(const Value& v, int n) {
        Value r = v;
        if (n == 3 && v.n == 1) { r.v[1] = r.v[2] = v.v[0]; }
        r.n = n;
        return r;
    }

    static Value binop(int op, const Value& a, const Value& b) {
        switch (op) {
        case O_ADD: return zip(a, b, [](float x, float y) { return x + y; });
        case O_SUB: return zip(a, b, [](float x, float y) { return x - y; });
        case O_MUL: return zip(a, b, [](float x, float y) { return x * y; });
        case O_DIV: return zip(a, b, [](float x, float y) { return y != 0.f ? x / y : 0.f; });
        case O_MOD: return zip(a, b, [](float x, float y) { return y != 0.f ? std::fmod(x, y) : 0.f; });
        case O_LT: return Value(a.v[0] < b.v[0] ? 1.f : 0.f);
        case O_GT: return Value(a.v[0] > b.v[0] ? 1.f : 0.f);
        case O_LE: return Value(a.v[0] <= b.v[0] ? 1.f : 0.f);
        case O_GE: return Value(a.v[0] >= b.v[0] ? 1.f : 0.f);
        case O_EQ: {
            bool eq = a.v[0] == b.v[0];
            if (a.n == 3 && b.n == 3) eq = eq && a.v[1] == b.v[1] && a.v[2] == b.v[2];
            return Value(eq ? 1.f : 0.f);
        }
        case O_NE: {
            Value e = binop(O_EQ, a, b);
            return Value(e.v[0] != 0 ? 0.f : 1.f);
        }
        }
        return Value();
    }

    Value* lvalueBase(const PNode* lv) {
        const PNode* base = lv->k == PNode::Comp ? lv->a : lv;
        return base->k == PNode::Attr ? &ctx.attribValues[base->slot] : &locals[base->slot];
    }

    Value assign(const PNode* n) {
        Value rhs = eval(n->b);
        Value* target = lvalueBase(n->a);
        int comp = n->a->k == PNode::Comp ? n->a->comp : -1;
        if (comp >= 0) {
            Value cur(target->v[target->n == 3 ? comp : 0]);
            Value res = n->op == O_SET ? Value(rhs.v[0]) : binop(n->op - O_SET - 1 + O_ADD, cur, Value(rhs.v[0]));
            if (target->n == 3) target->v[comp] = res.v[0];
            else target->v[0] = target->v[1] = target->v[2] = res.v[0];
            return *target;
        }
        Value res = n->op == O_SET ? rhs : binop(n->op - O_SET - 1 + O_ADD, *target, rhs);
        int tn = target->n;
        *target = coerce(res, tn);
        if (tn == 1) { target->v[1] = target->v[2] = target->v[0]; }
        return *target;
    }

    Value call(const PNode* n) {
        Value a[5];
        int na = int(n->kids.size());
        for (int i = 0; i < na && i < 5; ++i) a[i] = eval(n->kids[i]);
        switch (n->fn) {
        case F_SIN: return mapv(a[0], [](float x) { return std::sin(x); });
        case F_COS: return mapv(a[0], [](float x) { return std::cos(x); });
        case F_TAN: return mapv(a[0], [](float x) { return std::tan(x); });
        case F_ASIN: return mapv(a[0], [](float x) { return std::asin(std::clamp(x, -1.f, 1.f)); });
        case F_ACOS: return mapv(a[0], [](float x) { return std::acos(std::clamp(x, -1.f, 1.f)); });
        case F_ATAN: return mapv(a[0], [](float x) { return std::atan(x); });
        case F_ATAN2: return zip(a[0], a[1], [](float y, float x) { return std::atan2(y, x); });
        case F_SQRT: return mapv(a[0], [](float x) { return x > 0 ? std::sqrt(x) : 0.f; });
        case F_ABS: return mapv(a[0], [](float x) { return std::fabs(x); });
        case F_FLOOR: return mapv(a[0], [](float x) { return std::floor(x); });
        case F_CEIL: return mapv(a[0], [](float x) { return std::ceil(x); });
        case F_FRAC: return mapv(a[0], [](float x) { return x - std::floor(x); });
        case F_ROUND: return mapv(a[0], [](float x) { return std::round(x); });
        case F_POW: return zip(a[0], a[1], [](float x, float y) { return std::pow(x, y); });
        case F_EXP: return mapv(a[0], [](float x) { return std::exp(x); });
        case F_LOG: return mapv(a[0], [](float x) { return x > 0 ? std::log(x) : -1e30f; });
        case F_MIN: return zip(a[0], a[1], [](float x, float y) { return std::min(x, y); });
        case F_MAX: return zip(a[0], a[1], [](float x, float y) { return std::max(x, y); });
        case F_CLAMP: {
            Value lo = zip(a[0], a[1], [](float x, float y) { return std::max(x, y); });
            return zip(lo, a[2], [](float x, float y) { return std::min(x, y); });
        }
        case F_FIT: {
            Value r; r.n = std::max(a[0].n, std::max(a[3].n, a[4].n));
            for (int i = 0; i < 3; ++i) {
                auto g = [&](const Value& v) { return v.v[v.n == 3 ? i : 0]; };
                float omin = g(a[1]), omax = g(a[2]);
                float t = omax != omin ? (g(a[0]) - omin) / (omax - omin) : 0.f;
                t = std::clamp(t, 0.f, 1.f);
                r.v[i] = g(a[3]) + (g(a[4]) - g(a[3])) * t;
            }
            return r;
        }
        case F_FIT01: {
            Value r; r.n = std::max(a[0].n, std::max(a[1].n, a[2].n));
            for (int i = 0; i < 3; ++i) {
                auto g = [&](const Value& v) { return v.v[v.n == 3 ? i : 0]; };
                float t = std::clamp(g(a[0]), 0.f, 1.f);
                r.v[i] = g(a[1]) + (g(a[2]) - g(a[1])) * t;
            }
            return r;
        }
        case F_LERP: {
            Value r; r.n = std::max(a[0].n, a[1].n);
            for (int i = 0; i < 3; ++i) {
                auto g = [&](const Value& v) { return v.v[v.n == 3 ? i : 0]; };
                r.v[i] = g(a[0]) + (g(a[1]) - g(a[0])) * g(a[2]);
            }
            return r;
        }
        case F_SMOOTH: {
            float lo = a[0].v[0], hi = a[1].v[0];
            Value r = a[2];
            for (float& x : r.v) {
                float t = hi != lo ? std::clamp((x - lo) / (hi - lo), 0.f, 1.f) : 0.f;
                x = t * t * (3 - 2 * t);
            }
            return r;
        }
        case F_LENGTH: return Value(a[0].n == 3 ? length(a[0].vec()) : std::fabs(a[0].v[0]));
        case F_NORMALIZE: return Value(normalize(a[0].vec()));
        case F_DOT: return Value(dot(a[0].vec(), a[1].vec()));
        case F_CROSS: return Value(cross(a[0].vec(), a[1].vec()));
        case F_DISTANCE: return Value(length(a[0].vec() - a[1].vec()));
        case F_SET: {
            if (na == 1) return coerce(a[0], 3);
            if (na == 3) return Value(Vec3(a[0].v[0], a[1].v[0], a[2].v[0]));
            return Value(Vec3(a[0].v[0], a[1].v[0], 0));
        }
        case F_NOISE: {
            Vec3 p = a[0].vec();
            if (a[0].n == 1) p = Vec3(a[0].v[0], 0.37f, 0.71f);
            return Value(perlin3(p.x, p.y, p.z) * 0.5f + 0.5f);
        }
        case F_CURLNOISE: return Value(curlNoise3(a[0].vec()));
        case F_RAND: {
            if (a[0].n == 3) {
                float s = a[0].v[0] * 12.9898f + a[0].v[1] * 78.233f + a[0].v[2] * 37.719f;
                return Value(hashRandom(s));
            }
            return Value(hashRandom(a[0].v[0]));
        }
        case F_DEGREES: return mapv(a[0], [](float x) { return x * 180.f / kPi; });
        case F_RADIANS: return mapv(a[0], [](float x) { return x * kPi / 180.f; });
        case F_SIGN: return mapv(a[0], [](float x) { return x > 0 ? 1.f : (x < 0 ? -1.f : 0.f); });
        case F_MOD: return zip(a[0], a[1], [](float x, float y) { return y != 0.f ? x - y * std::floor(x / y) : 0.f; });
        case F_HSV2RGB: {
            Vec3 hsv = a[0].vec();
            float h = (hsv.x - std::floor(hsv.x)) * 6.f, s = hsv.y, v = hsv.z;
            int i = int(h) % 6; float f = h - std::floor(h);
            float pp = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
            Vec3 rgb;
            switch (i) {
            case 0: rgb = {v, t, pp}; break; case 1: rgb = {q, v, pp}; break;
            case 2: rgb = {pp, v, t}; break; case 3: rgb = {pp, q, v}; break;
            case 4: rgb = {t, pp, v}; break; default: rgb = {v, pp, q};
            }
            return Value(rgb);
        }
        }
        return Value();
    }
};

} // namespace

std::unique_ptr<Program> Program::compile(const std::string& src, bool expressionMode, std::string& error) {
    std::unique_ptr<Program> prog(new Program());
    try {
        ExprParser ps(*prog, tokenize(src));
        prog->root_ = ps.program(expressionMode);
    } catch (const std::exception& e) {
        error = e.what();
        return nullptr;
    }
    return prog;
}

Value Program::run(Context& ctx) const {
    ctx.globals[int(Global::Pi)] = kPi;
    Exec ex{ctx, {}};
    return root_ ? ex.eval(root_) : Value();
}

bool isExpressionString(const std::string& s) {
    if (s.empty()) return false;
    char* end = nullptr;
    std::strtod(s.c_str(), &end);
    while (end && *end && std::isspace((unsigned char)*end)) ++end;
    return !(end && *end == 0);
}

bool evalExpression(const std::string& src, float frame, float fps, float& out, std::string* error) {
    std::string err;
    auto prog = Program::compile(src, true, err);
    if (!prog) { if (error) *error = err; return false; }
    if (!prog->attribs().empty()) { if (error) *error = "attributes (@) are not available in parameter expressions"; return false; }
    Program::Context ctx;
    ctx.globals[int(Global::Frame)] = frame;
    ctx.globals[int(Global::Time)] = (frame - 1.f) / fps;
    ctx.globals[int(Global::Fps)] = fps;
    out = prog->run(ctx).f();
    return true;
}

} // namespace hd
