#include "PackageManifest.h"

#include "rapidjson/document.h"
#include "rapidjson/istreamwrapper.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace
{

    bool LoadJsonDocument(const std::filesystem::path& path, rapidjson::Document& doc, std::string& out_error)
    {
        std::ifstream ifs(path, std::ios::binary);
        if (!ifs.is_open())
        {
            out_error = "cannot open " + path.generic_string();
            return false;
        }
        rapidjson::IStreamWrapper isw(ifs);
        doc.ParseStream(isw);
        if (doc.HasParseError())
        {
            out_error = "JSON parse error in " + path.generic_string();
            return false;
        }
        return true;
    }

    std::string GetStringMember(const rapidjson::Value& obj, const char* key)
    {
        if (!obj.IsObject() || !obj.HasMember(key))
        {
            return {};
        }
        const rapidjson::Value& v = obj[key];
        if (!v.IsString())
        {
            return {};
        }
        return v.GetString();
    }

    bool ParseDependencyMap(const rapidjson::Value& obj,
                            std::unordered_map<std::string, std::string>& out)
    {
        if (!obj.IsObject())
        {
            return true;
        }
        for (auto it = obj.MemberBegin(); it != obj.MemberEnd(); ++it)
        {
            if (!it->name.IsString() || !it->value.IsString())
            {
                continue;
            }
            out[it->name.GetString()] = it->value.GetString();
        }
        return true;
    }

    bool ParseVersionTuple(const std::string& ver, int& major, int& minor, int& patch)
    {
        major = minor = patch = 0;
        std::stringstream ss(ver);
        char dot = 0;
        if (!(ss >> major))
        {
            return false;
        }
        if (ss.peek() == '.')
        {
            ss >> dot >> minor;
        }
        if (ss.peek() == '.')
        {
            ss >> dot >> patch;
        }
        return true;
    }

}  // namespace

namespace PackageManifest
{

bool VersionSatisfies(const std::string& resolved_version, const std::string& constraint)
{
    if (resolved_version.empty() || constraint.empty())
    {
        return false;
    }

    std::string c = constraint;
    bool min_inclusive = false;
    if (c.rfind(">=", 0) == 0)
    {
        min_inclusive = true;
        c = c.substr(2);
    }
    else if (!c.empty() && c[0] == '^')
    {
        // npm/UPM caret: V1 treats ^x.y.z as >=x.y.z (no upper bound).
        min_inclusive = true;
        c = c.substr(1);
    }

    if (!min_inclusive)
    {
        return resolved_version == c;
    }

    int r_maj = 0, r_min = 0, r_pat = 0;
    int c_maj = 0, c_min = 0, c_pat = 0;
    if (!ParseVersionTuple(resolved_version, r_maj, r_min, r_pat) ||
        !ParseVersionTuple(c, c_maj, c_min, c_pat))
    {
        return false;
    }
    if (r_maj != c_maj)
    {
        return r_maj > c_maj;
    }
    if (r_min != c_min)
    {
        return r_min > c_min;
    }
    return r_pat >= c_pat;
}

bool IsUnsupportedSourceSpecifier(const std::string& specifier, std::string& out_reason)
{
    if (specifier.rfind("git:", 0) == 0 || specifier.rfind("git+", 0) == 0)
    {
        out_reason = "git dependencies are not supported in ZPM V1";
        return true;
    }
    if (specifier.find("://") != std::string::npos &&
        specifier.rfind("file:", 0) != 0)
    {
        out_reason = "registry/URL dependencies are not supported in ZPM V1";
        return true;
    }
    return false;
}

bool ParseFromFile(const std::filesystem::path& path, PackageManifestData& out, std::string& out_error)
{
    rapidjson::Document doc;
    if (!LoadJsonDocument(path, doc, out_error))
    {
        return false;
    }
    if (!doc.IsObject())
    {
        out_error = "package manifest root must be an object";
        return false;
    }

    out.name = GetStringMember(doc, "name");
    out.version = GetStringMember(doc, "version");
    out.display_name = GetStringMember(doc, "displayName");
    out.description = GetStringMember(doc, "description");

    if (doc.HasMember("dependencies") && doc["dependencies"].IsObject())
    {
        ParseDependencyMap(doc["dependencies"], out.dependencies);
    }
    if (doc.HasMember("optionalDependencies") && doc["optionalDependencies"].IsObject())
    {
        ParseDependencyMap(doc["optionalDependencies"], out.optional_dependencies);
    }
    if (doc.HasMember("modules") && doc["modules"].IsObject())
    {
        const rapidjson::Value& mods = doc["modules"];
        for (auto it = mods.MemberBegin(); it != mods.MemberEnd(); ++it)
        {
            if (it->name.IsString() && it->value.IsString())
            {
                out.modules[it->name.GetString()] = it->value.GetString();
            }
        }
    }

    if (out.name.empty())
    {
        out_error = "package.json missing required field \"name\"";
        return false;
    }
    if (out.version.empty())
    {
        out.version = "0.0.0";
    }
    return true;
}

bool ParseProjectManifest(const std::filesystem::path& path,
                          std::unordered_map<std::string, std::string>& out_deps,
                          std::string& out_error)
{
    rapidjson::Document doc;
    if (!LoadJsonDocument(path, doc, out_error))
    {
        return false;
    }
    if (!doc.IsObject())
    {
        out_error = "project manifest root must be an object";
        return false;
    }
    if (!doc.HasMember("dependencies") || !doc["dependencies"].IsObject())
    {
        out_deps.clear();
        return true;
    }
    return ParseDependencyMap(doc["dependencies"], out_deps);
}

bool ParseEngineCatalog(const std::filesystem::path& catalog_path,
                        std::unordered_map<std::string, PackageManifestData>& out_packages,
                        std::string& out_error)
{
    rapidjson::Document doc;
    if (!LoadJsonDocument(catalog_path, doc, out_error))
    {
        return false;
    }
    if (!doc.IsObject() || !doc.HasMember("packages") || !doc["packages"].IsObject())
    {
        out_error = "engine catalog missing \"packages\" object";
        return false;
    }

    const rapidjson::Value& pkgs = doc["packages"];
    for (auto it = pkgs.MemberBegin(); it != pkgs.MemberEnd(); ++it)
    {
        if (!it->name.IsString() || !it->value.IsObject())
        {
            continue;
        }
        PackageManifestData entry;
        entry.name = it->name.GetString();
        const rapidjson::Value& obj = it->value;
        entry.version = GetStringMember(obj, "version");
        entry.display_name = GetStringMember(obj, "displayName");
        entry.description = GetStringMember(obj, "description");
        if (obj.HasMember("dependencies") && obj["dependencies"].IsObject())
        {
            ParseDependencyMap(obj["dependencies"], entry.dependencies);
        }
        if (entry.version.empty())
        {
            entry.version = "0.0.0";
        }
        // Catalog path is stored as a synthetic module entry for resolution.
        const std::string rel = GetStringMember(obj, "path");
        if (!rel.empty())
        {
            entry.modules["__root__"] = rel;
        }
        out_packages[entry.name] = std::move(entry);
    }
    return true;
}

}  // namespace PackageManifest
