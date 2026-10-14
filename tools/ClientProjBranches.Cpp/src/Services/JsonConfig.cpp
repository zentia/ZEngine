#include "JsonConfig.h"

#include <cstdio>
#include <cstdint>
#include <stdexcept>
#include <algorithm>

namespace Json {
namespace {

// ---------------------------------------------------------------------------
// UTF-8 encoding of a single code point (for \uXXXX escapes)
// ---------------------------------------------------------------------------
void AppendUtf8(std::string& out, uint32_t cp)
{
    if (cp < 0x80)
    {
        out += static_cast<char>(cp);
    }
    else if (cp < 0x800)
    {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    else if (cp < 0x10000)
    {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    else
    {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

int HexDigitValue(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------
struct Parser {
    const std::string& src;
    size_t pos = 0;

    explicit Parser(const std::string& s) : src(s) {}

    char peek() {
        while (pos < src.size() && std::isspace((unsigned char)src[pos])) ++pos;
        return pos < src.size() ? src[pos] : 0;
    }
    char next() { peek(); return pos < src.size() ? src[pos++] : 0; }
    void expect(char c) { if (next() != c) throw std::runtime_error("Unexpected char"); }

    ValuePtr parseValue() {
        char c = peek();
        if (c == '"') return std::make_shared<Value>(parseString());
        if (c == 't' || c == 'f') return std::make_shared<Value>(parseBool());
        if (c == 'n') { pos += 4; return std::make_shared<Value>(); }
        if (c == '-' || std::isdigit((unsigned char)c)) return std::make_shared<Value>(parseNumber());
        if (c == '[') return parseArray();
        if (c == '{') return parseObjectVal();
        throw std::runtime_error("Unexpected token");
    }

    std::string parseString() {
        expect('"');
        std::string s;
        while (pos < src.size() && src[pos] != '"') {
            if (src[pos] == '\\') {
                ++pos;
                if (pos >= src.size()) break;
                const char esc = src[pos++];
                switch (esc) {
                case 'n':  s += '\n'; break;
                case 'r':  s += '\r'; break;
                case 't':  s += '\t'; break;
                case 'b':  s += '\b'; break;
                case 'f':  s += '\f'; break;
                case '/':  s += '/';  break;
                case '"':  s += '"';  break;
                case '\\': s += '\\'; break;
                case 'u': {
                    uint32_t cp = 0;
                    bool valid = true;
                    for (int i = 0; i < 4; ++i) {
                        if (pos >= src.size()) { valid = false; break; }
                        const int digit = HexDigitValue(src[pos++]);
                        if (digit < 0) { valid = false; break; }
                        cp = (cp << 4) | static_cast<uint32_t>(digit);
                    }
                    if (!valid) break;
                    // Surrogate pair
                    if (cp >= 0xD800 && cp <= 0xDBFF &&
                        pos + 1 < src.size() && src[pos] == '\\' && src[pos + 1] == 'u') {
                        const size_t save = pos;
                        pos += 2;
                        uint32_t low = 0;
                        bool lowValid = true;
                        for (int i = 0; i < 4; ++i) {
                            if (pos >= src.size()) { lowValid = false; break; }
                            const int digit = HexDigitValue(src[pos++]);
                            if (digit < 0) { lowValid = false; break; }
                            low = (low << 4) | static_cast<uint32_t>(digit);
                        }
                        if (lowValid && low >= 0xDC00 && low <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                        } else {
                            pos = save;
                        }
                    }
                    AppendUtf8(s, cp);
                    break;
                }
                default: s += esc; break;
                }
            } else {
                s += src[pos++];
            }
        }
        expect('"');
        return s;
    }

    double parseNumber() {
        size_t start = pos;
        if (src[pos] == '-') ++pos;
        while (pos < src.size() && std::isdigit((unsigned char)src[pos])) ++pos;
        if (pos < src.size() && src[pos] == '.') {
            ++pos;
            while (pos < src.size() && std::isdigit((unsigned char)src[pos])) ++pos;
        }
        if (pos < src.size() && (src[pos] == 'e' || src[pos] == 'E')) {
            ++pos;
            if (pos < src.size() && (src[pos] == '+' || src[pos] == '-')) ++pos;
            while (pos < src.size() && std::isdigit((unsigned char)src[pos])) ++pos;
        }
        return std::stod(src.substr(start, pos - start));
    }

    bool parseBool() {
        if (src[pos] == 't') { pos += 4; return true; }
        if (src[pos] == 'f') { pos += 5; return false; }
        throw std::runtime_error("Expected bool");
    }

    ValuePtr parseArray() {
        expect('[');
        Array arr;
        if (peek() != ']') {
            do { arr.push_back(parseValue()); } while (peek() == ',' && (next(), true));
        }
        expect(']');
        return std::make_shared<Value>(std::move(arr));
    }

    ValuePtr parseObjectVal() {
        expect('{');
        Object obj;
        if (peek() != '}') {
            do {
                std::string key = parseString();
                expect(':');
                obj[key] = parseValue();
            } while (peek() == ',' && (next(), true));
        }
        expect('}');
        return std::make_shared<Value>(std::move(obj));
    }
};

std::string ReadAllText(const std::string& path, bool& ok)
{
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) { ok = false; return {}; }
    std::stringstream buffer;
    buffer << file.rdbuf();
    ok = true;
    return StripBom(buffer.str());
}

} // namespace

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------

std::string StripBom(const std::string& text)
{
    if (text.size() >= 3 &&
        static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF)
    {
        return text.substr(3);
    }
    return text;
}

Object Parse(const std::string& json)
{
    try {
        auto val = Parser(StripBom(json)).parseObjectVal();
        return val ? val->AsObject() : Object{};
    } catch (...) {
        return {};
    }
}

ValuePtr ParseValue(const std::string& json)
{
    try {
        return Parser(StripBom(json)).parseValue();
    } catch (...) {
        return nullptr;
    }
}

Array ParseArray(const std::string& json)
{
    auto value = ParseValue(json);
    return (value && value->IsArray()) ? value->AsArray() : Array{};
}

Object LoadFromFile(const std::string& path)
{
    bool ok = false;
    const std::string text = ReadAllText(path, ok);
    if (!ok) return {};
    return Parse(text);
}

ValuePtr LoadValueFromFile(const std::string& path)
{
    bool ok = false;
    const std::string text = ReadAllText(path, ok);
    if (!ok) return nullptr;
    return ParseValue(text);
}

Array LoadArrayFromFile(const std::string& path)
{
    auto value = LoadValueFromFile(path);
    return (value && value->IsArray()) ? value->AsArray() : Array{};
}

// ---------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------

static std::string escape(const std::string& s) {
    std::string r;
    for (char c : s) {
        switch (c) {
        case '"':  r += "\\\""; break;
        case '\\': r += "\\\\"; break;
        case '\n': r += "\\n";  break;
        case '\r': r += "\\r";  break;
        case '\t': r += "\\t";  break;
        case '\b': r += "\\b";  break;
        case '\f': r += "\\f";  break;
        default:   r += c;      break;
        }
    }
    return r;
}

static std::string indentStr(int n) { return std::string(n * 2, ' '); }

std::string Stringify(const Value& v, int indent) {
    if (v.IsNull()) return "null";
    if (v.IsString()) return "\"" + escape(v.AsString()) + "\"";
    if (v.IsNumber()) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%.15g", v.AsNumber());
        return buf;
    }
    if (v.IsBool()) return v.AsBool() ? "true" : "false";

    if (v.IsArray()) {
        const auto& arr = v.AsArray();
        if (arr.empty()) return "[]";
        std::string s = "[\n";
        for (size_t i = 0; i < arr.size(); ++i) {
            s += indentStr(indent + 1) + (arr[i] ? Stringify(*arr[i], indent + 1) : "null");
            if (i + 1 < arr.size()) s += ",";
            s += "\n";
        }
        return s + indentStr(indent) + "]";
    }

    if (v.IsObject()) {
        const auto& obj = v.AsObject();
        if (obj.empty()) return "{}";
        std::string s = "{\n";
        size_t i = 0;
        for (const auto& [k, val] : obj) {
            s += indentStr(indent + 1) + "\"" + escape(k) + "\": "
               + (val ? Stringify(*val, indent + 1) : "null");
            if (++i < obj.size()) s += ",";
            s += "\n";
        }
        return s + indentStr(indent) + "}";
    }
    return "null";
}

static bool WriteTextFile(const std::string& path, const std::string& text)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) return false;
    file << text;
    return true;
}

bool SaveToFile(const std::string& path, const Object& obj) {
    Value wrapper(obj);
    return WriteTextFile(path, Stringify(wrapper, 0) + "\n");
}

bool SaveValueToFile(const std::string& path, const Value& v) {
    return WriteTextFile(path, Stringify(v, 0) + "\n");
}

bool SaveArrayToFile(const std::string& path, const Array& arr) {
    Value wrapper(arr);
    return WriteTextFile(path, Stringify(wrapper, 0) + "\n");
}

bool SaveStringToFile(const std::string& path, const std::string& s) {
    return WriteTextFile(path, Stringify(Value(s), 0) + "\n");
}

// ---------------------------------------------------------------------------
// Coercion helpers
// ---------------------------------------------------------------------------

int AsInt(const Value& v, int def) {
    if (v.IsNumber()) return static_cast<int>(v.AsNumber());
    if (v.IsString()) { try { return std::stoi(v.AsString()); } catch (...) { return def; } }
    if (v.IsBool())   return v.AsBool() ? 1 : 0;
    return def;
}

std::string AsText(const Value& v, const std::string& def) {
    if (v.IsString()) return v.AsString();
    if (v.IsNumber()) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%.15g", v.AsNumber());
        return buf;
    }
    if (v.IsBool()) return v.AsBool() ? "true" : "false";
    return def;
}

bool AsFlag(const Value& v, bool def) {
    if (v.IsBool())   return v.AsBool();
    if (v.IsNumber()) return v.AsNumber() != 0.0;
    if (v.IsString()) {
        const std::string& s = v.AsString();
        if (s == "true" || s == "True" || s == "1") return true;
        if (s == "false" || s == "False" || s == "0") return false;
    }
    return def;
}

std::vector<std::string> AsStringList(const Array& arr) {
    std::vector<std::string> result;
    for (const auto& v : arr) if (v) result.push_back(AsText(*v));
    return result;
}

std::vector<Object> AsDictList(const Array& arr) {
    std::vector<Object> result;
    for (const auto& v : arr) if (v && v->IsObject()) result.push_back(v->AsObject());
    return result;
}

const Value* FindValue(const Object& obj, const std::string& key) {
    auto it = obj.find(key);
    if (it != obj.end()) return it->second.get();

    // Case-insensitive fallback (C# PascalCase vs C++ camelCase).
    std::string lower;
    lower.reserve(key.size());
    for (char c : key) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    for (const auto& [k, v] : obj) {
        std::string candidate;
        candidate.reserve(k.size());
        for (char c : k) candidate += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (candidate == lower) return v.get();
    }
    return nullptr;
}

std::string OptStringCI(const Object& obj, const std::string& key, const std::string& def) {
    const Value* v = FindValue(obj, key);
    return v ? AsText(*v, def) : def;
}

bool OptBoolCI(const Object& obj, const std::string& key, bool def) {
    const Value* v = FindValue(obj, key);
    return v ? AsFlag(*v, def) : def;
}

int OptInt(const Object& obj, const std::string& key, int def) {
    const Value* v = FindValue(obj, key);
    return v ? AsInt(*v, def) : def;
}

double OptNumberCI(const Object& obj, const std::string& key, double def) {
    const Value* v = FindValue(obj, key);
    if (!v) return def;
    if (v->IsNumber()) return v->AsNumber();
    if (v->IsString()) { try { return std::stod(v->AsString()); } catch (...) { return def; } }
    return def;
}

} // namespace Json
