#include "Json.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace hd {

static void escapeString(std::string& out, const std::string& s) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) { char buf[8]; std::snprintf(buf, sizeof buf, "\\u%04x", c); out += buf; }
            else out += char(c);
        }
    }
    out += '"';
}

void Json::dumpTo(std::string& out, int indent, int level) const {
    auto nl = [&](int lvl) {
        if (indent <= 0) return;
        out += '\n';
        out.append(size_t(lvl * indent), ' ');
    };
    switch (type_) {
    case Null: out += "null"; break;
    case Bool: out += b_ ? "true" : "false"; break;
    case Number: {
        if (std::isfinite(n_) && n_ == std::floor(n_) && std::fabs(n_) < 1e15) {
            char buf[32]; std::snprintf(buf, sizeof buf, "%lld", (long long)n_); out += buf;
        } else if (std::isfinite(n_)) {
            char buf[32]; std::snprintf(buf, sizeof buf, "%.9g", n_); out += buf;
        } else out += "0";
        break;
    }
    case String: escapeString(out, s_); break;
    case Array: {
        out += '[';
        bool simple = true;
        for (auto& v : arr_) if (v.type_ == Array || v.type_ == Object) simple = false;
        for (size_t i = 0; i < arr_.size(); ++i) {
            if (i) out += simple ? ", " : ",";
            if (!simple) nl(level + 1);
            arr_[i].dumpTo(out, indent, level + 1);
        }
        if (!simple && !arr_.empty()) nl(level);
        out += ']';
        break;
    }
    case Object: {
        out += '{';
        size_t i = 0;
        for (auto& [k, v] : obj_) {
            if (i++) out += ',';
            nl(level + 1);
            escapeString(out, k);
            out += indent > 0 ? ": " : ":";
            v.dumpTo(out, indent, level + 1);
        }
        if (!obj_.empty()) nl(level);
        out += '}';
        break;
    }
    }
}

std::string Json::dump(int indent) const {
    std::string s;
    dumpTo(s, indent, 0);
    return s;
}

namespace {
struct Parser {
    const std::string& t;
    size_t i = 0;
    explicit Parser(const std::string& s) : t(s) {}
    [[noreturn]] void fail(const std::string& m) { throw std::runtime_error(m + " at offset " + std::to_string(i)); }
    void ws() { while (i < t.size() && (t[i] == ' ' || t[i] == '\n' || t[i] == '\r' || t[i] == '\t')) ++i; }
    bool lit(const char* s) {
        size_t n = std::char_traits<char>::length(s);
        if (t.compare(i, n, s) == 0) { i += n; return true; }
        return false;
    }
    std::string parseString() {
        if (t[i] != '"') fail("expected string");
        ++i;
        std::string out;
        while (i < t.size() && t[i] != '"') {
            char c = t[i++];
            if (c == '\\') {
                if (i >= t.size()) fail("bad escape");
                char e = t[i++];
                switch (e) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u': {
                    if (i + 4 > t.size()) fail("bad unicode escape");
                    unsigned cp = unsigned(std::strtoul(t.substr(i, 4).c_str(), nullptr, 16));
                    i += 4;
                    if (cp < 0x80) out += char(cp);
                    else if (cp < 0x800) { out += char(0xC0 | (cp >> 6)); out += char(0x80 | (cp & 0x3F)); }
                    else { out += char(0xE0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
                    break;
                }
                default: out += e;
                }
            } else out += c;
        }
        if (i >= t.size()) fail("unterminated string");
        ++i;
        return out;
    }
    Json value() {
        ws();
        if (i >= t.size()) fail("unexpected end");
        char c = t[i];
        if (c == '{') {
            ++i;
            Json o = Json::object();
            ws();
            if (t[i] == '}') { ++i; return o; }
            while (true) {
                ws();
                std::string k = parseString();
                ws();
                if (t[i] != ':') fail("expected ':'");
                ++i;
                o[k] = value();
                ws();
                if (t[i] == ',') { ++i; continue; }
                if (t[i] == '}') { ++i; break; }
                fail("expected ',' or '}'");
            }
            return o;
        }
        if (c == '[') {
            ++i;
            Json a = Json::array();
            ws();
            if (t[i] == ']') { ++i; return a; }
            while (true) {
                a.push(value());
                ws();
                if (t[i] == ',') { ++i; continue; }
                if (t[i] == ']') { ++i; break; }
                fail("expected ',' or ']'");
            }
            return a;
        }
        if (c == '"') return Json(parseString());
        if (lit("true")) return Json(true);
        if (lit("false")) return Json(false);
        if (lit("null")) return Json();
        char* end = nullptr;
        double d = std::strtod(t.c_str() + i, &end);
        if (end == t.c_str() + i) fail("unexpected character");
        i = size_t(end - t.c_str());
        return Json(d);
    }
};
} // namespace

Json Json::parse(const std::string& text, std::string* error) {
    try {
        Parser p(text);
        Json v = p.value();
        return v;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return Json();
    }
}

} // namespace hd
