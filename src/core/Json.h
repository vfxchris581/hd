#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace hd {

// Minimal JSON value used for scene files (.hdg).
class Json {
public:
    enum Type { Null, Bool, Number, String, Array, Object };
    Json() = default;
    Json(bool b) : type_(Bool), b_(b) {}
    Json(double n) : type_(Number), n_(n) {}
    Json(int n) : type_(Number), n_(n) {}
    Json(const char* s) : type_(String), s_(s) {}
    Json(std::string s) : type_(String), s_(std::move(s)) {}
    static Json array() { Json j; j.type_ = Array; return j; }
    static Json object() { Json j; j.type_ = Object; return j; }

    Type type() const { return type_; }
    bool isNull() const { return type_ == Null; }
    bool isObject() const { return type_ == Object; }
    bool isArray() const { return type_ == Array; }
    double num(double def = 0) const { return type_ == Number ? n_ : (type_ == Bool ? (b_ ? 1 : 0) : def); }
    bool boolean(bool def = false) const { return type_ == Bool ? b_ : (type_ == Number ? n_ != 0 : def); }
    const std::string& str() const { return s_; }

    // arrays
    void push(Json v) { if (type_ != Array) { *this = array(); } arr_.push_back(std::move(v)); }
    size_t size() const { return type_ == Array ? arr_.size() : (type_ == Object ? obj_.size() : 0); }
    const Json& operator[](size_t i) const { static Json n; return i < arr_.size() ? arr_[i] : n; }
    const std::vector<Json>& items() const { return arr_; }

    // objects
    Json& operator[](const std::string& k) { if (type_ != Object) *this = object(); return obj_[k]; }
    const Json& get(const std::string& k) const {
        static Json n;
        auto it = obj_.find(k);
        return it == obj_.end() ? n : it->second;
    }
    bool has(const std::string& k) const { return obj_.count(k) > 0; }
    const std::map<std::string, Json>& members() const { return obj_; }

    std::string dump(int indent = 2) const;
    static Json parse(const std::string& text, std::string* error = nullptr);

private:
    void dumpTo(std::string& out, int indent, int level) const;
    Type type_ = Null;
    bool b_ = false;
    double n_ = 0;
    std::string s_;
    std::vector<Json> arr_;
    std::map<std::string, Json> obj_;
};

} // namespace hd
