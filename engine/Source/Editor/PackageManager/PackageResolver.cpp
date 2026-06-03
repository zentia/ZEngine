#include "PackageResolver.h"

#include "PackageManifest.h"

#include "Runtime/Core/Base/Macro.h"
#include "core/Log/LogSystem.h"

#include "rapidjson/document.h"
#include "rapidjson/prettywriter.h"
#include "rapidjson/stringbuffer.h"

#include <fstream>
#include <sstream>

namespace
{

    std::string CmakeVarNameFromPackageId(const std::string& name)
    {
        std::string v = "ZENGINE_PKG_";
        for (char c : name)
        {
            if (c == '.')
            {
                v += '_';
            }
            else
            {
                v += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            }
        }
        return v;
    }

    std::filesystem::path WeaklyCanonicalOrAbsolute(const std::filesystem::path& p)
    {
        std::error_code ec;
        auto c = std::filesystem::weakly_canonical(p, ec);
        if (!ec)
        {
            return c;
        }
        return std::filesystem::absolute(p);
    }

}  // namespace

std::filesystem::path PackageResolver::GetEngineCatalogPath(const std::filesystem::path& engine_root)
{
    return engine_root / "engine" / "Packages" / "manifest.json";
}

std::filesystem::path PackageResolver::GetProjectPackagesDir(const std::filesystem::path& project_root)
{
    return project_root / "Packages";
}

std::filesystem::path PackageResolver::GetProjectManifestPath(const std::filesystem::path& project_root)
{
    return GetProjectPackagesDir(project_root) / "manifest.json";
}

std::filesystem::path PackageResolver::GetProjectLockPath(const std::filesystem::path& project_root)
{
    return GetProjectPackagesDir(project_root) / "packages-lock.json";
}

bool PackageResolver::LoadEngineCatalog(std::string& out_error)
{
    const auto catalog_path = GetEngineCatalogPath(m_EngineRoot);
    if (!std::filesystem::exists(catalog_path))
    {
        out_error = "engine package catalog not found: " + catalog_path.generic_string();
        return false;
    }
    return PackageManifest::ParseEngineCatalog(catalog_path, m_EngineCatalog, out_error);
}

bool PackageResolver::LocatePackage(const std::string& name,
                                    const std::string& constraint,
                                    ResolvedPackage& out_pkg,
                                    std::string& out_error) const
{
    std::string unsupported;
    if (PackageManifest::IsUnsupportedSourceSpecifier(constraint, unsupported))
    {
        out_error = unsupported + " (" + name + ")";
        return false;
    }

    // 1) Embedded: <Project>/Packages/<name>/package.json
    const std::filesystem::path embedded_root = m_PackagesDir / name;
    const std::filesystem::path embedded_manifest = embedded_root / "package.json";
    if (std::filesystem::exists(embedded_manifest))
    {
        PackageManifestData manifest;
        if (!PackageManifest::ParseFromFile(embedded_manifest, manifest, out_error))
        {
            return false;
        }
        if (manifest.name != name)
        {
            LOG_WARNING(ZPackage,
                        "embedded package folder '{}' has package.json name '{}' (expected '{}')",
                        name,
                        manifest.name,
                        name);
        }
        if (!PackageManifest::VersionSatisfies(manifest.version, constraint))
        {
            out_error = "embedded package " + name + " version " + manifest.version +
                        " does not satisfy " + constraint;
            return false;
        }
        out_pkg.name = name;
        out_pkg.version = manifest.version;
        out_pkg.source = PackageSource::Embedded;
        out_pkg.root_path = WeaklyCanonicalOrAbsolute(embedded_root);
        out_pkg.manifest = std::move(manifest);
        out_pkg.dependencies = out_pkg.manifest.dependencies;
        return true;
    }

    // 2) file: specifier
    if (constraint.rfind("file:", 0) == 0)
    {
        std::string rel = constraint.substr(5);
        while (!rel.empty() && (rel[0] == '/' || rel[0] == '\\'))
        {
            rel.erase(rel.begin());
        }
        const std::filesystem::path root = WeaklyCanonicalOrAbsolute(m_ProjectRoot / rel);
        const std::filesystem::path manifest_path = root / "package.json";
        if (!std::filesystem::exists(manifest_path))
        {
            out_error = "file: package missing package.json at " + root.generic_string();
            return false;
        }
        PackageManifestData manifest;
        if (!PackageManifest::ParseFromFile(manifest_path, manifest, out_error))
        {
            return false;
        }
        out_pkg.name = name;
        out_pkg.version = manifest.version;
        out_pkg.source = PackageSource::File;
        out_pkg.root_path = root;
        out_pkg.manifest = std::move(manifest);
        out_pkg.dependencies = out_pkg.manifest.dependencies;
        return true;
    }

    // 3) Builtin catalog
    auto cat_it = m_EngineCatalog.find(name);
    if (cat_it == m_EngineCatalog.end())
    {
        out_error = "package not found: " + name + " (not embedded, not in engine catalog)";
        return false;
    }
    const PackageManifestData& cat = cat_it->second;
    if (!PackageManifest::VersionSatisfies(cat.version, constraint))
    {
        out_error = "builtin package " + name + " version " + cat.version + " does not satisfy " +
                    constraint;
        return false;
    }

    auto mod_it = cat.modules.find("__root__");
    if (mod_it == cat.modules.end() || mod_it->second.empty())
    {
        out_error = "builtin package " + name + " missing catalog path";
        return false;
    }

    const std::filesystem::path root =
        WeaklyCanonicalOrAbsolute(m_EngineRoot / mod_it->second);
    const std::filesystem::path manifest_path = root / "package.json";
    PackageManifestData manifest = cat;
    if (std::filesystem::exists(manifest_path))
    {
        std::string parse_err;
        PackageManifestData disk;
        if (PackageManifest::ParseFromFile(manifest_path, disk, parse_err))
        {
            manifest = std::move(disk);
        }
        else
        {
            LOG_WARNING(ZPackage,
                        "builtin package {}: failed to read {}: {}",
                        name,
                        manifest_path.generic_string(),
                        parse_err);
        }
    }

    out_pkg.name = name;
    out_pkg.version = cat.version;
    out_pkg.source = PackageSource::Builtin;
    out_pkg.root_path = root;
    out_pkg.manifest = std::move(manifest);
    out_pkg.dependencies = out_pkg.manifest.dependencies;
    return true;
}

bool PackageResolver::ResolveOne(const std::string& name,
                                 const std::string& constraint,
                                 int depth,
                                 std::unordered_map<std::string, ResolvedPackage>& resolved,
                                 std::vector<std::string>& stack,
                                 std::string& out_error)
{
    if (resolved.find(name) != resolved.end())
    {
        return true;
    }

    for (const std::string& s : stack)
    {
        if (s == name)
        {
            out_error = "cyclic package dependency involving " + name;
            return false;
        }
    }
    stack.push_back(name);

    ResolvedPackage pkg;
    if (!LocatePackage(name, constraint, pkg, out_error))
    {
        stack.pop_back();
        return false;
    }
    pkg.depth = depth;
    resolved[name] = pkg;

    for (const auto& dep : pkg.dependencies)
    {
        if (!ResolveOne(dep.first, dep.second, depth + 1, resolved, stack, out_error))
        {
            stack.pop_back();
            return false;
        }
    }

    stack.pop_back();
    return true;
}

PackageResolver::Result PackageResolver::Resolve(
    const std::filesystem::path& engine_root,
    const std::filesystem::path& project_root,
    const std::unordered_map<std::string, std::string>& project_dependencies)
{
    Result result;
    m_EngineRoot = engine_root;
    m_ProjectRoot = project_root;
    m_PackagesDir = GetProjectPackagesDir(project_root);

    std::string err;
    if (!LoadEngineCatalog(err))
    {
        result.error_message = err;
        return result;
    }

    std::vector<std::string> stack;
    for (const auto& dep : project_dependencies)
    {
        if (!ResolveOne(dep.first, dep.second, 0, result.packages, stack, err))
        {
            result.error_message = err;
            return result;
        }
    }

    result.success = true;
    return result;
}

bool PackageResolver::WriteLockFile(const std::filesystem::path& lock_path, const Result& result)
{
    if (!result.success)
    {
        return false;
    }

    rapidjson::Document doc;
    doc.SetObject();
    auto& alloc = doc.GetAllocator();

    doc.AddMember("lockVersion", 1, alloc);

    rapidjson::Value deps(rapidjson::kObjectType);
    for (const auto& kv : result.packages)
    {
        const ResolvedPackage& p = kv.second;
        rapidjson::Value entry(rapidjson::kObjectType);
        entry.AddMember("version",
                        rapidjson::Value(p.version.c_str(),
                                         static_cast<rapidjson::SizeType>(p.version.size()),
                                         alloc),
                        alloc);
        entry.AddMember("depth", p.depth, alloc);
        entry.AddMember("source",
                        rapidjson::Value(PackageSourceToString(p.source),
                                         alloc),
                        alloc);
        entry.AddMember("path",
                        rapidjson::Value(p.root_path.generic_string().c_str(),
                                         static_cast<rapidjson::SizeType>(
                                             p.root_path.generic_string().size()),
                                         alloc),
                        alloc);

        rapidjson::Value subdeps(rapidjson::kObjectType);
        for (const auto& d : p.dependencies)
        {
            subdeps.AddMember(
                rapidjson::Value(d.first.c_str(),
                                 static_cast<rapidjson::SizeType>(d.first.size()),
                                 alloc),
                rapidjson::Value(d.second.c_str(),
                                 static_cast<rapidjson::SizeType>(d.second.size()),
                                 alloc),
                alloc);
        }
        entry.AddMember("dependencies", subdeps, alloc);
        deps.AddMember(rapidjson::Value(p.name.c_str(),
                                        static_cast<rapidjson::SizeType>(p.name.size()),
                                        alloc),
                       entry,
                       alloc);
    }
    doc.AddMember("dependencies", deps, alloc);

    rapidjson::StringBuffer sb;
    rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(sb);
    doc.Accept(writer);

    std::error_code ec;
    std::filesystem::create_directories(lock_path.parent_path(), ec);
    const auto tmp = lock_path.string() + ".tmp";
    {
        std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
        if (!ofs.is_open() || !ofs.write(sb.GetString(), static_cast<std::streamsize>(sb.GetSize())))
        {
            return false;
        }
    }
    std::filesystem::rename(tmp, lock_path, ec);
    if (ec)
    {
        std::filesystem::copy_file(tmp, lock_path, std::filesystem::copy_options::overwrite_existing, ec);
        std::filesystem::remove(tmp, ec);
    }
    return true;
}

bool PackageResolver::WriteCMakeFragment(const std::filesystem::path& cmake_path,
                                         const Result& result)
{
    if (!result.success)
    {
        return false;
    }

    std::ostringstream ss;
    ss << "# Auto-generated by ZEngine PackageManager. Do not edit.\n";
    for (const auto& kv : result.packages)
    {
        const ResolvedPackage& p = kv.second;
        ss << "set(" << CmakeVarNameFromPackageId(p.name) << "_ROOT \""
           << p.root_path.generic_string() << "\")\n";
    }

    std::error_code ec;
    std::filesystem::create_directories(cmake_path.parent_path(), ec);
    const auto tmp = cmake_path.string() + ".tmp";
    {
        std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
        const std::string content = ss.str();
        if (!ofs.is_open() || !ofs.write(content.data(), static_cast<std::streamsize>(content.size())))
        {
            return false;
        }
    }
    std::filesystem::rename(tmp, cmake_path, ec);
    if (ec)
    {
        std::filesystem::copy_file(tmp, cmake_path, std::filesystem::copy_options::overwrite_existing, ec);
        std::filesystem::remove(tmp, ec);
    }
    return true;
}
