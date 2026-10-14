#pragma once
// Minimal JSON config reader/writer (no external dependencies)
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <fstream>
#include <sstream>
#include <cctype>

namespace Json {

class Value;

using Object = std::map<std::string, std::shared_ptr<Value>>;
using Array  = std::vector<std::shared_ptr<Value>>;

enum class Type { Null, String, Number, Bool, Array, Object };

class Value {
public:
    Type type = Type::Null;
    std::string s;
    double n = 0;
    bool b = false;
    Array arr;
    Object obj;

    Value() : type(Type::Null) {}
    explicit Value(std::string v) : type(Type::String), s(std::move(v)) {}
    explicit Value(double v) : type(Type::Number), n(v) {}
    explicit Value(bool v) : type(Type::Bool), b(v) {}
    explicit Value(Array v) : type(Type::Array), arr(std::move(v)) {}
    explicit Value(Object v) : type(Type::Object), obj(std::move(v)) {}

    bool IsString() const { return type == Type::String; }
    bool IsNumber() const { return type == Type::Number; }
    bool IsBool()   const { return type == Type::Bool; }
    bool IsNull()   const { return type == Type::Null; }
    bool IsArray()  const { return type == Type::Array; }
    bool IsObject() const { return type == Type::Object; }

    const std::string& AsString() const { return s; }
    double AsNumber() const { return n; }
    bool   AsBool()   const { return b; }
    const Array&  AsArray()  const { return arr; }
    const Object& AsObject() const { return obj; }
};

using ValuePtr = std::shared_ptr<Value>;

Object Parse(const std::string& json);
Object LoadFromFile(const std::string& path);

std::string Stringify(const Value& v, int indent = 0);
inline std::string Stringify(ValuePtr v, int indent = 0) { return v ? Stringify(*v, indent) : "null"; }
bool SaveToFile(const std::string& path, const Object& obj);

// ---------------------------------------------------------------------------
// Top-level array / scalar support.
//
// Every C# config file except stacktrace_settings.json is a *bare array*
// (paths.json, git_repos.json, urls.json, notes.json, tips.json …) and
// ide_preference.json is a bare string. These entry points make the C++ side
// able to read and write exactly the same shapes.
// ---------------------------------------------------------------------------
ValuePtr ParseValue(const std::string& json);            // any top-level value
ValuePtr LoadValueFromFile(const std::string& path);
Array    ParseArray(const std::string& json);
Array    LoadArrayFromFile(const std::string& path);

bool SaveValueToFile(const std::string& path, const Value& v);
bool SaveArrayToFile(const std::string& path, const Array& arr);
bool SaveStringToFile(const std::string& path, const std::string& s);   // writes "s"

std::string StripBom(const std::string& text);

// ---------------------------------------------------------------------------
// Coercion helpers (tolerate number-vs-string drift between C# and C++)
// ---------------------------------------------------------------------------
int         AsInt(const Value& v, int def = 0);
std::string AsText(const Value& v, const std::string& def = "");
bool        AsFlag(const Value& v, bool def = false);

std::vector<std::string> AsStringList(const Array& arr);
std::vector<Object>      AsDictList(const Array& arr);

// Case-insensitive member lookup (C# serialises PascalCase, C++ used camelCase)
const Value* FindValue(const Object& obj, const std::string& key);
std::string  OptStringCI(const Object& obj, const std::string& key, const std::string& def = "");
bool         OptBoolCI(const Object& obj, const std::string& key, bool def = false);
int          OptInt(const Object& obj, const std::string& key, int def = 0);
double       OptNumberCI(const Object& obj, const std::string& key, double def = 0.0);

// Convenience helpers
inline std::string OptString(const Object& obj, const std::string& key, const std::string& def = "") {
    auto it = obj.find(key);
    return (it != obj.end() && it->second->IsString()) ? it->second->AsString() : def;
}
inline bool OptBool(const Object& obj, const std::string& key, bool def = false) {
    auto it = obj.find(key);
    return (it != obj.end() && it->second->IsBool()) ? it->second->AsBool() : def;
}
inline double OptNumber(const Object& obj, const std::string& key, double def = 0.0) {
    auto it = obj.find(key);
    return (it != obj.end() && it->second->IsNumber()) ? it->second->AsNumber() : def;
}
inline const Array* OptArray(const Object& obj, const std::string& key) {
    auto it = obj.find(key);
    return (it != obj.end() && it->second->IsArray()) ? &it->second->AsArray() : nullptr;
}

} // namespace Json
