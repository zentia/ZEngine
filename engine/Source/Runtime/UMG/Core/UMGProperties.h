#pragma once

#include "Runtime/Core/Math/Vector2.h"
#include "Runtime/UI/Core/UITypes.h"  // UIColor (= Vector4)

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace ZUMG
{
// Typed property type tags. Persisted as a single character so the on-disk node
// (UWidgetAsset) stays a flat trio of string vectors (key / type / value),
// serialisable with existing SerializeTraits<std::vector<eastl::string>>.
enum class EPropType
{
    Float,
    Int,
    Bool,
    String,
    Color,  // "r,g,b,a"
    Vec2,   // "x,y"
};

struct FUMGProperty
{
    std::string Key;
    EPropType Type {EPropType::String};
    std::string Value;  // stringified per Type
};

// A small ordered, typed key/value store. Each UWidget reads/writes its
// authoring state through one of these; the bag round-trips to the UWidgetAsset
// node and back. Designed to be reflection-friendly for the P3 designer (it can
// enumerate Entries() and render a widget per EPropType).
class UMGPropertyBag
{
public:
    void SetFloat(const std::string& key, float v) { Put(key, EPropType::Float, FloatToStr(v)); }
    void SetInt(const std::string& key, int v) { Put(key, EPropType::Int, std::to_string(v)); }
    void SetBool(const std::string& key, bool v) { Put(key, EPropType::Bool, v ? "1" : "0"); }
    void SetString(const std::string& key, const std::string& v) { Put(key, EPropType::String, v); }
    void SetColor(const std::string& key, const UIColor& c) { Put(key, EPropType::Color, ColorToStr(c)); }
    void SetVec2(const std::string& key, const Vector2& v) { Put(key, EPropType::Vec2, Vec2ToStr(v)); }

    float GetFloat(const std::string& key, float def = 0.0f) const
    {
        const FUMGProperty* p = Find(key);
        return p ? static_cast<float>(std::atof(p->Value.c_str())) : def;
    }
    int GetInt(const std::string& key, int def = 0) const
    {
        const FUMGProperty* p = Find(key);
        return p ? std::atoi(p->Value.c_str()) : def;
    }
    bool GetBool(const std::string& key, bool def = false) const
    {
        const FUMGProperty* p = Find(key);
        return p ? (p->Value == "1" || p->Value == "true") : def;
    }
    std::string GetString(const std::string& key, const std::string& def = std::string()) const
    {
        const FUMGProperty* p = Find(key);
        return p ? p->Value : def;
    }
    UIColor GetColor(const std::string& key, const UIColor& def = UIColor(1, 1, 1, 1)) const
    {
        const FUMGProperty* p = Find(key);
        if (!p)
            return def;
        UIColor c = def;
        ParseFloats(p->Value, &c.x, 4);
        return c;
    }
    Vector2 GetVec2(const std::string& key, const Vector2& def = Vector2(0, 0)) const
    {
        const FUMGProperty* p = Find(key);
        if (!p)
            return def;
        Vector2 v = def;
        float tmp[2] = {def.x, def.y};
        ParseFloats(p->Value, tmp, 2);
        v.x = tmp[0];
        v.y = tmp[1];
        return v;
    }

    // Insert a pre-typed, pre-stringified entry (used when rehydrating a bag from
    // a serialised UWidgetAsset node).
    void SetRaw(const std::string& key, EPropType type, const std::string& value) { Put(key, type, value); }

    bool Has(const std::string& key) const { return Find(key) != nullptr; }
    const std::vector<FUMGProperty>& Entries() const { return m_Props; }
    void Clear() { m_Props.clear(); }

private:
    void Put(const std::string& key, EPropType type, std::string value)
    {
        for (FUMGProperty& p : m_Props)
        {
            if (p.Key == key)
            {
                p.Type = type;
                p.Value = std::move(value);
                return;
            }
        }
        m_Props.push_back({key, type, std::move(value)});
    }

    const FUMGProperty* Find(const std::string& key) const
    {
        for (const FUMGProperty& p : m_Props)
        {
            if (p.Key == key)
                return &p;
        }
        return nullptr;
    }

    static std::string FloatToStr(float v)
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%g", static_cast<double>(v));
        return buf;
    }
    static std::string ColorToStr(const UIColor& c)
    {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%g,%g,%g,%g", static_cast<double>(c.x), static_cast<double>(c.y),
                      static_cast<double>(c.z), static_cast<double>(c.w));
        return buf;
    }
    static std::string Vec2ToStr(const Vector2& v)
    {
        char buf[48];
        std::snprintf(buf, sizeof(buf), "%g,%g", static_cast<double>(v.x), static_cast<double>(v.y));
        return buf;
    }
    static void ParseFloats(const std::string& s, float* out, int count)
    {
        const char* p = s.c_str();
        for (int i = 0; i < count && *p; ++i)
        {
            char* end = nullptr;
            const float v = std::strtof(p, &end);
            if (end == p)
                break;
            out[i] = v;
            p = end;
            while (*p == ',' || *p == ' ')
                ++p;
        }
    }

    std::vector<FUMGProperty> m_Props;
};

inline char PropTypeToChar(EPropType t)
{
    switch (t)
    {
        case EPropType::Float: return 'f';
        case EPropType::Int: return 'i';
        case EPropType::Bool: return 'b';
        case EPropType::String: return 's';
        case EPropType::Color: return 'c';
        case EPropType::Vec2: return 'v';
    }
    return 's';
}

inline EPropType PropTypeFromChar(char c)
{
    switch (c)
    {
        case 'f': return EPropType::Float;
        case 'i': return EPropType::Int;
        case 'b': return EPropType::Bool;
        case 'c': return EPropType::Color;
        case 'v': return EPropType::Vec2;
        case 's':
        default: return EPropType::String;
    }
}
}  // namespace ZUMG
