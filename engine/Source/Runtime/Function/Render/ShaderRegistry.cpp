#include "Runtime/Function/Render/ShaderRegistry.h"

#include "Runtime/Core/Base/EngineSystem.h"
#include "Runtime/Platform/Encoding/EncodingUtils.h"
#include "Runtime/Project/ProjectInfo.h"
#include "core/Log/LogSystem.h"
#include "rapidjson/document.h"
#include "rapidjson/filereadstream.h"
#include "rapidjson/prettywriter.h"
#include "rapidjson/stringbuffer.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <regex>

namespace
{
    constexpr uint64_t kFnvOffsetBasis = 0xcbf29ce484222325ULL;
    constexpr uint64_t kFnvPrime = 0x100000001b3ULL;

    uint64_t fnv1a64(const uint8_t* data, size_t len, uint64_t seed)
    {
        uint64_t h = seed;
        for (size_t i = 0; i < len; ++i)
        {
            h ^= static_cast<uint64_t>(data[i]);
            h *= kFnvPrime;
        }
        return h;
    }

    eastl::string toHex64(uint64_t v)
    {
        static const char* digits = "0123456789abcdef";
        char buf[16];
        for (int i = 15; i >= 0; --i)
        {
            buf[i] = digits[v & 0xF];
            v >>= 4;
        }
        return eastl::string(buf, 16);
    }

    std::string readSmallTextFile(const std::filesystem::path& abs_path, size_t max_bytes = 16 * 1024)
    {
        FILE* f = fopen(abs_path.string().c_str(), "rb");
        if (!f)
        {
            return {};
        }
        std::string buf;
        buf.resize(max_bytes);
        size_t n = fread(buf.data(), 1, max_bytes, f);
        buf.resize(n);
        fclose(f);
        return buf;
    }

    std::string readWholeFile(const std::filesystem::path& abs_path)
    {
        FILE* f = fopen(abs_path.string().c_str(), "rb");
        if (!f)
        {
            return {};
        }
        fseek(f, 0, SEEK_END);
        long fsz = ftell(f);
        if (fsz <= 0)
        {
            fclose(f);
            return {};
        }
        std::string buf(static_cast<size_t>(fsz), '\0');
        fseek(f, 0, SEEK_SET);
        fread(buf.data(), 1, static_cast<size_t>(fsz), f);
        fclose(f);
        return buf;
    }

    eastl::string contentHashHex(const std::string& bytes)
    {
        if (bytes.empty())
        {
            return {};
        }
        const uint64_t h = fnv1a64(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), kFnvOffsetBasis);
        return toHex64(h);
    }

    eastl::string toEastl(const std::string& s)
    {
        return eastl::string(s.c_str(), s.size());
    }

    std::string toStd(const eastl::string& s)
    {
        return std::string(s.c_str(), s.size());
    }

    bool isShaderSourceExtension(const std::filesystem::path& p)
    {
        std::string ext = p.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return ext == ".shader";
    }
}  // namespace

std::vector<std::type_index> ShaderRegistry::GetDependencies() const
{
    return {GET_SYSTEM_TYPE(ProjectInfo)};
}

bool ShaderRegistry::Initialize()
{
    auto project_info = GET_SYSTEM(ProjectInfo);
    if (project_info == nullptr || project_info->GetProjectRoot().empty())
    {
        LOG_INFO(ZShaderRegistry, "No project loaded; ShaderRegistry idle.");
        return true;
    }

    m_ProjectRoot = project_info->GetProjectRoot();
    m_ShadersRoot = project_info->GetShadersRoot();
    m_RegistryFile = project_info->GetShaderRegistryPath();

    LoadFromDisk();
    rescan();
    FlushPendingSave();

    LOG_INFO(ZShaderRegistry,
             "Initialised: shaders_root={}, registry={}, entries={}",
             m_ShadersRoot.generic_string(),
             m_RegistryFile.generic_string(),
             static_cast<int>(m_ByGuid.size()));
    return true;
}

namespace
{
    constexpr auto kRegistrySaveDebounce = std::chrono::milliseconds(500);
}  // namespace

void ShaderRegistry::ScheduleSave()
{
    const auto deadline = std::chrono::steady_clock::now() + kRegistrySaveDebounce;
    std::lock_guard lk(m_Mutex);
    m_SavePending = true;
    m_SaveDeadline = deadline;
}

void ShaderRegistry::FlushPendingSave() const
{
    bool do_save = false;
    {
        std::lock_guard lk(m_Mutex);
        if (!m_SavePending)
        {
            return;
        }
        m_SavePending = false;
        do_save = true;
    }
    if (do_save)
    {
        SaveToDisk();
    }
}

void ShaderRegistry::TickDeferredSave()
{
    bool do_save = false;
    {
        std::lock_guard lk(m_Mutex);
        if (!m_SavePending || std::chrono::steady_clock::now() < m_SaveDeadline)
        {
            return;
        }
        m_SavePending = false;
        do_save = true;
    }
    if (do_save)
    {
        SaveToDisk();
    }
}

void ShaderRegistry::Shutdown()
{
    FlushPendingSave();
    std::lock_guard lk(m_Mutex);
    m_ByGuid.clear();
    m_GuidByPath.clear();
    m_GuidByName.clear();
}

ShaderRegistryEntry* ShaderRegistry::FindByGuid(const eastl::string& guid)
{
    std::lock_guard lk(m_Mutex);
    auto it = m_ByGuid.find(guid);
    return it == m_ByGuid.end() ? nullptr : &it->second;
}

const ShaderRegistryEntry* ShaderRegistry::FindByGuid(const eastl::string& guid) const
{
    std::lock_guard lk(m_Mutex);
    auto it = m_ByGuid.find(guid);
    return it == m_ByGuid.end() ? nullptr : &it->second;
}

ShaderRegistryEntry* ShaderRegistry::FindByPath(const eastl::string& source_rel_path)
{
    const eastl::string key = NormaliseRelPath(std::filesystem::path(source_rel_path.c_str()));
    std::lock_guard lk(m_Mutex);
    auto git = m_GuidByPath.find(key);
    if (git == m_GuidByPath.end())
    {
        return nullptr;
    }
    auto it = m_ByGuid.find(git->second);
    return it == m_ByGuid.end() ? nullptr : &it->second;
}

const ShaderRegistryEntry* ShaderRegistry::FindByPath(const eastl::string& source_rel_path) const
{
    return const_cast<ShaderRegistry*>(this)->FindByPath(source_rel_path);
}

ShaderRegistryEntry* ShaderRegistry::FindByName(const eastl::string& shader_name)
{
    if (shader_name.empty())
    {
        return nullptr;
    }
    const eastl::string key = NormaliseShaderNameKey(shader_name);
    std::lock_guard lk(m_Mutex);
    auto git = m_GuidByName.find(key);
    if (git == m_GuidByName.end())
    {
        return nullptr;
    }
    auto it = m_ByGuid.find(git->second);
    return it == m_ByGuid.end() ? nullptr : &it->second;
}

const ShaderRegistryEntry* ShaderRegistry::FindByName(const eastl::string& shader_name) const
{
    return const_cast<ShaderRegistry*>(this)->FindByName(shader_name);
}

std::vector<ShaderRegistryEntry*> ShaderRegistry::GetAll() const
{
    std::lock_guard lk(m_Mutex);
    std::vector<ShaderRegistryEntry*> out;
    out.reserve(m_ByGuid.size());
    for (auto& kv : m_ByGuid)
    {
        out.push_back(const_cast<ShaderRegistryEntry*>(&kv.second));
    }
    return out;
}

std::filesystem::path ShaderRegistry::GetRegistryFilePath() const
{
    return m_RegistryFile;
}

eastl::string ShaderRegistry::DeterministicGuidFromPath(const eastl::string& rel_path)
{
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(rel_path.c_str());
    const size_t n = rel_path.size();
    const uint64_t hi = fnv1a64(bytes, n, kFnvOffsetBasis);
    const uint64_t lo = fnv1a64(bytes, n, hi ^ 0x9e3779b97f4a7c15ULL);
    return toHex64(hi) + toHex64(lo);
}

eastl::string ShaderRegistry::ComputeZassetRelPath(const eastl::string& rel_under_shaders_root)
{
    std::string rel = toStd(rel_under_shaders_root);
    std::replace(rel.begin(), rel.end(), '\\', '/');
    return eastl::string(("Assets/_Generated/Shaders/" + rel + ".zasset").c_str());
}

void ShaderRegistry::rescan()
{
    if (m_ProjectRoot.empty() || m_ShadersRoot.empty())
    {
        return;
    }

    std::error_code ec;
    if (!std::filesystem::exists(m_ShadersRoot, ec))
    {
        std::lock_guard lk(m_Mutex);
        if (!m_ByGuid.empty())
        {
            m_ByGuid.clear();
            m_GuidByPath.clear();
            m_GuidByName.clear();
            ScheduleSave();
        }
        return;
    }

    const auto on_disk = EnumerateShaderFiles(m_ProjectRoot, m_ShadersRoot);

    bool changed = false;
    {
        std::lock_guard lk(m_Mutex);

        eastl::unordered_map<eastl::string, bool> on_disk_set;
        on_disk_set.reserve(on_disk.size());
        for (const auto& rel : on_disk)
        {
            on_disk_set[rel] = true;
        }

        eastl::vector<eastl::string> new_paths;
        eastl::vector<eastl::string> orphan_guids;

        for (const auto& rel : on_disk)
        {
            auto pit = m_GuidByPath.find(rel);
            if (pit != m_GuidByPath.end())
            {
                ShaderRegistryEntry& entry = m_ByGuid[pit->second];
                const std::filesystem::path abs = m_ProjectRoot / std::filesystem::path(toStd(rel));
                const int64_t new_mtime = FileMTimeNs(abs);
                if (new_mtime != entry.m_SourceMtimeNs)
                {
                    entry.m_SourceMtimeNs = new_mtime;
                    entry.m_ShaderName = ParseShaderNameFromSource(abs);
                    entry.m_SourceContentHash = contentHashHex(readWholeFile(abs));
                    RebuildNameIndexForEntry(entry);
                    changed = true;
                }
                else if (entry.m_SourceContentHash.empty())
                {
                    entry.m_SourceContentHash = contentHashHex(readWholeFile(abs));
                    changed = true;
                }
            }
            else
            {
                new_paths.push_back(rel);
            }
        }

        for (auto& kv : m_ByGuid)
        {
            if (on_disk_set.find(kv.second.m_SourceRelPath) == on_disk_set.end())
            {
                orphan_guids.push_back(kv.first);
            }
        }

        eastl::unordered_map<eastl::string, eastl::string> orphan_by_hash;
        for (const auto& g : orphan_guids)
        {
            auto it = m_ByGuid.find(g);
            if (it != m_ByGuid.end() && !it->second.m_SourceContentHash.empty())
            {
                orphan_by_hash.insert({it->second.m_SourceContentHash, g});
            }
        }

        for (const auto& rel : new_paths)
        {
            const std::filesystem::path abs = m_ProjectRoot / std::filesystem::path(toStd(rel));
            const std::string content = readWholeFile(abs);
            const eastl::string hash = contentHashHex(content);
            const int64_t mtime = FileMTimeNs(abs);

            if (!hash.empty())
            {
                auto hit = orphan_by_hash.find(hash);
                if (hit != orphan_by_hash.end())
                {
                    auto git = m_ByGuid.find(hit->second);
                    if (git != m_ByGuid.end())
                    {
                        ShaderRegistryEntry& entry = git->second;
                        RemoveNameIndexForEntry(entry);
                        m_GuidByPath.erase(entry.m_SourceRelPath);
                        entry.m_SourceRelPath = rel;
                        entry.m_SourceMtimeNs = mtime;
                        entry.m_ShaderName = ParseShaderNameFromSource(abs);
                        entry.m_SourceContentHash = hash;
                        std::error_code rel_ec;
                        const std::filesystem::path rel_under_shaders =
                            std::filesystem::relative(abs, m_ShadersRoot, rel_ec);
                        if (!rel_ec && !rel_under_shaders.empty())
                        {
                            entry.m_ZassetRelPath = ComputeZassetRelPath(
                                NormaliseRelPath(rel_under_shaders).c_str());
                        }
                        m_GuidByPath.emplace(rel, entry.m_Guid);
                        RebuildNameIndexForEntry(entry);
                        orphan_by_hash.erase(hit);
                        for (auto oit = orphan_guids.begin(); oit != orphan_guids.end(); ++oit)
                        {
                            if (*oit == hit->second)
                            {
                                orphan_guids.erase(oit);
                                break;
                            }
                        }
                        changed = true;
                        continue;
                    }
                }
            }

            eastl::string guid = DeterministicGuidFromPath(rel);
            int bump = 0;
            while (m_ByGuid.count(guid) != 0 && bump < 256)
            {
                eastl::string salted = rel + eastl::string(":");
                salted += eastl::string(std::to_string(bump).c_str());
                guid = DeterministicGuidFromPath(salted);
                ++bump;
            }

            ShaderRegistryEntry entry {};
            entry.m_Guid = guid;
            entry.m_SourceRelPath = rel;
            entry.m_SourceMtimeNs = mtime;
            entry.m_ShaderName = ParseShaderNameFromSource(abs);
            entry.m_SourceContentHash = hash;
            std::error_code rel_ec;
            const std::filesystem::path rel_under_shaders =
                std::filesystem::relative(abs, m_ShadersRoot, rel_ec);
            if (!rel_ec && !rel_under_shaders.empty())
            {
                entry.m_ZassetRelPath =
                    ComputeZassetRelPath(NormaliseRelPath(rel_under_shaders).c_str());
            }

            m_GuidByPath.emplace(rel, guid);
            RebuildNameIndexForEntry(entry);
            m_ByGuid.emplace(guid, std::move(entry));
            changed = true;
        }

        for (const auto& g : orphan_guids)
        {
            auto git = m_ByGuid.find(g);
            if (git == m_ByGuid.end())
            {
                continue;
            }
            RemoveNameIndexForEntry(git->second);
            m_GuidByPath.erase(git->second.m_SourceRelPath);
            m_ByGuid.erase(git);
            changed = true;
        }
    }

    if (changed)
    {
        ScheduleSave();
    }
}

void ShaderRegistry::OnShaderFileEvent(const std::filesystem::path& abs_shader_path)
{
    if (m_ProjectRoot.empty() || abs_shader_path.empty())
    {
        return;
    }

    if (!isShaderSourceExtension(abs_shader_path))
    {
        return;
    }

    bool changed = false;
    std::error_code exists_ec;
    if (std::filesystem::exists(abs_shader_path, exists_ec) && !exists_ec)
    {
        UpsertEntryForAbsPath(abs_shader_path, &changed);
    }
    else
    {
        RemoveEntryForAbsPath(abs_shader_path, &changed);
    }

    if (changed)
    {
        ScheduleSave();
    }
}

bool ShaderRegistry::LoadFromDisk()
{
    if (m_RegistryFile.empty())
    {
        return false;
    }
    std::error_code ec;
    if (!std::filesystem::exists(m_RegistryFile, ec))
    {
        return false;
    }

    FILE* f = fopen(m_RegistryFile.string().c_str(), "rb");
    if (!f)
    {
        LOG_WARNING(ZShaderRegistry, "Cannot open registry: {}", m_RegistryFile.generic_string());
        return false;
    }

    char readBuffer[65536];
    rapidjson::FileReadStream is(f, readBuffer, sizeof(readBuffer));
    rapidjson::Document doc;
    doc.ParseStream(is);
    fclose(f);
    if (doc.HasParseError() || !doc.IsObject())
    {
        LOG_WARNING(ZShaderRegistry,
                    "Registry JSON parse failed; rebuilding: {}",
                    m_RegistryFile.generic_string());
        return false;
    }

    auto entries_it = doc.FindMember("entries");
    if (entries_it == doc.MemberEnd() || !entries_it->value.IsArray())
    {
        return false;
    }

    std::lock_guard lk(m_Mutex);
    m_ByGuid.clear();
    m_GuidByPath.clear();
    m_GuidByName.clear();

    for (auto& e : entries_it->value.GetArray())
    {
        if (!e.IsObject())
        {
            continue;
        }
        auto guid_it = e.FindMember("guid");
        auto path_it = e.FindMember("path");
        if (guid_it == e.MemberEnd() || !guid_it->value.IsString())
        {
            continue;
        }
        if (path_it == e.MemberEnd() || !path_it->value.IsString())
        {
            continue;
        }

        ShaderRegistryEntry entry {};
        entry.m_Guid = toEastl(guid_it->value.GetString());
        entry.m_SourceRelPath = NormaliseRelPath(std::filesystem::path(path_it->value.GetString()));

        auto name_it = e.FindMember("name");
        if (name_it != e.MemberEnd() && name_it->value.IsString())
        {
            entry.m_ShaderName = toEastl(name_it->value.GetString());
        }

        auto zasset_it = e.FindMember("zasset");
        if (zasset_it != e.MemberEnd() && zasset_it->value.IsString())
        {
            entry.m_ZassetRelPath = NormaliseRelPath(std::filesystem::path(zasset_it->value.GetString()));
        }

        auto mtime_it = e.FindMember("mtime");
        if (mtime_it != e.MemberEnd() && mtime_it->value.IsInt64())
        {
            entry.m_SourceMtimeNs = mtime_it->value.GetInt64();
        }

        auto hash_it = e.FindMember("content_hash");
        if (hash_it != e.MemberEnd() && hash_it->value.IsString())
        {
            entry.m_SourceContentHash = toEastl(hash_it->value.GetString());
        }

        m_GuidByPath.emplace(entry.m_SourceRelPath, entry.m_Guid);
        if (!entry.m_ShaderName.empty())
        {
            m_GuidByName.emplace(NormaliseShaderNameKey(entry.m_ShaderName), entry.m_Guid);
        }
        m_ByGuid.emplace(entry.m_Guid, std::move(entry));
    }

    return true;
}

bool ShaderRegistry::SaveToDisk() const
{
    if (m_RegistryFile.empty())
    {
        return false;
    }

    try
    {
        std::filesystem::create_directories(m_RegistryFile.parent_path());

        rapidjson::Document doc;
        doc.SetObject();
        auto& alloc = doc.GetAllocator();
        doc.AddMember("version", 1, alloc);

        rapidjson::Value entries(rapidjson::kArrayType);
        {
            std::lock_guard lk(m_Mutex);

            std::vector<const ShaderRegistryEntry*> sorted;
            sorted.reserve(m_ByGuid.size());
            for (auto& kv : m_ByGuid)
            {
                sorted.push_back(&kv.second);
            }
            std::sort(sorted.begin(),
                      sorted.end(),
                      [](const ShaderRegistryEntry* a, const ShaderRegistryEntry* b) {
                          return a->m_SourceRelPath < b->m_SourceRelPath;
                      });

            for (const ShaderRegistryEntry* entry : sorted)
            {
                rapidjson::Value obj(rapidjson::kObjectType);
                obj.AddMember("guid",
                              rapidjson::Value(entry->m_Guid.c_str(),
                                               static_cast<rapidjson::SizeType>(entry->m_Guid.size()),
                                               alloc),
                              alloc);
                obj.AddMember("path",
                              rapidjson::Value(entry->m_SourceRelPath.c_str(),
                                               static_cast<rapidjson::SizeType>(entry->m_SourceRelPath.size()),
                                               alloc),
                              alloc);
                obj.AddMember("name",
                              rapidjson::Value(entry->m_ShaderName.c_str(),
                                               static_cast<rapidjson::SizeType>(entry->m_ShaderName.size()),
                                               alloc),
                              alloc);
                obj.AddMember("zasset",
                              rapidjson::Value(entry->m_ZassetRelPath.c_str(),
                                               static_cast<rapidjson::SizeType>(entry->m_ZassetRelPath.size()),
                                               alloc),
                              alloc);
                obj.AddMember("mtime", static_cast<int64_t>(entry->m_SourceMtimeNs), alloc);
                obj.AddMember("content_hash",
                              rapidjson::Value(entry->m_SourceContentHash.c_str(),
                                               static_cast<rapidjson::SizeType>(entry->m_SourceContentHash.size()),
                                               alloc),
                              alloc);
                entries.PushBack(obj, alloc);
            }
        }
        doc.AddMember("entries", entries, alloc);

        std::filesystem::path temp = m_RegistryFile;
        temp += ".tmp";

        {
            FILE* f = fopen(temp.string().c_str(), "wb");
            if (!f)
            {
                LOG_ERROR(ZShaderRegistry, "Cannot write registry tmp: {}", temp.generic_string());
                return false;
            }
            rapidjson::StringBuffer sb;
            rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(sb);
            doc.Accept(writer);
            fwrite(sb.GetString(), 1, sb.GetSize(), f);
            fclose(f);
        }

        std::error_code rename_ec;
        std::filesystem::rename(temp, m_RegistryFile, rename_ec);
        if (rename_ec)
        {
            std::filesystem::copy_file(temp,
                                       m_RegistryFile,
                                       std::filesystem::copy_options::overwrite_existing,
                                       rename_ec);
            std::filesystem::remove(temp);
            if (rename_ec)
            {
                LOG_ERROR(ZShaderRegistry,
                          "Cannot finalise registry: {} ({})",
                          m_RegistryFile.generic_string(),
                          rename_ec.message());
                return false;
            }
        }
        return true;
    }
    catch (const std::exception& e)
    {
        LOG_ERROR(ZShaderRegistry, "Save registry failed: {}", Encoding::GetExceptionMessage(e));
        return false;
    }
}

std::vector<eastl::string> ShaderRegistry::EnumerateShaderFiles(const std::filesystem::path& project_root,
                                                                const std::filesystem::path& shaders_root)
{
    std::vector<eastl::string> out;
    std::error_code ec;
    if (!std::filesystem::exists(shaders_root, ec))
    {
        return out;
    }

    for (auto& entry : std::filesystem::recursive_directory_iterator(
             shaders_root, std::filesystem::directory_options::skip_permission_denied, ec))
    {
        if (ec)
        {
            ec.clear();
            continue;
        }
        if (!entry.is_regular_file(ec))
        {
            continue;
        }
        const std::filesystem::path& abs = entry.path();
        if (!isShaderSourceExtension(abs))
        {
            continue;
        }
        std::filesystem::path rel = std::filesystem::relative(abs, project_root, ec);
        if (ec || rel.empty())
        {
            ec.clear();
            continue;
        }
        out.push_back(NormaliseRelPath(rel));
    }
    return out;
}

eastl::string ShaderRegistry::ParseShaderNameFromSource(const std::filesystem::path& abs_path)
{
    const std::string head = readSmallTextFile(abs_path);
    if (!head.empty())
    {
        static const std::regex re(R"re(Shader\s+"([^"]+)")re", std::regex::ECMAScript);
        std::smatch m;
        if (std::regex_search(head, m, re) && m.size() >= 2)
        {
            return toEastl(m[1].str());
        }
    }
    return toEastl(abs_path.stem().generic_string());
}

int64_t ShaderRegistry::FileMTimeNs(const std::filesystem::path& abs_path)
{
    std::error_code ec;
    auto ft = std::filesystem::last_write_time(abs_path, ec);
    if (ec)
    {
        return 0;
    }
    return std::chrono::duration_cast<std::chrono::nanoseconds>(ft.time_since_epoch()).count();
}

eastl::string ShaderRegistry::NormaliseRelPath(const std::filesystem::path& rel_path)
{
    std::string s = rel_path.generic_string();
#if defined(_WIN32)
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#endif
    return eastl::string(s.c_str(), s.size());
}

eastl::string ShaderRegistry::NormaliseShaderNameKey(const eastl::string& name)
{
    std::string s = toStd(name);
#if defined(_WIN32)
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#endif
    return eastl::string(s.c_str(), s.size());
}

void ShaderRegistry::RebuildNameIndexForEntry(const ShaderRegistryEntry& entry)
{
    if (!entry.m_ShaderName.empty())
    {
        m_GuidByName[NormaliseShaderNameKey(entry.m_ShaderName)] = entry.m_Guid;
    }
}

void ShaderRegistry::RemoveNameIndexForEntry(const ShaderRegistryEntry& entry)
{
    if (entry.m_ShaderName.empty())
    {
        return;
    }
    const eastl::string key = NormaliseShaderNameKey(entry.m_ShaderName);
    auto it = m_GuidByName.find(key);
    if (it != m_GuidByName.end() && it->second == entry.m_Guid)
    {
        m_GuidByName.erase(it);
    }
}

void ShaderRegistry::UpsertEntryForAbsPath(const std::filesystem::path& abs_shader_path, bool* out_changed)
{
    if (out_changed)
    {
        *out_changed = false;
    }

    std::error_code rel_ec;
    const std::filesystem::path rel_under_shaders =
        std::filesystem::relative(abs_shader_path, m_ShadersRoot, rel_ec);
    if (rel_ec || rel_under_shaders.empty())
    {
        return;
    }

    std::filesystem::path rel_from_project = std::filesystem::relative(abs_shader_path, m_ProjectRoot, rel_ec);
    if (rel_ec || rel_from_project.empty())
    {
        return;
    }

    const eastl::string rel_key = NormaliseRelPath(rel_from_project);
    const std::string content = readWholeFile(abs_shader_path);
    const eastl::string hash = contentHashHex(content);
    const int64_t mtime = FileMTimeNs(abs_shader_path);

    std::lock_guard lk(m_Mutex);

    auto pit = m_GuidByPath.find(rel_key);
    if (pit != m_GuidByPath.end())
    {
        ShaderRegistryEntry& entry = m_ByGuid[pit->second];
        bool entry_changed = false;
        if (mtime != entry.m_SourceMtimeNs)
        {
            entry.m_SourceMtimeNs = mtime;
            entry_changed = true;
        }
        const eastl::string new_name = ParseShaderNameFromSource(abs_shader_path);
        if (new_name != entry.m_ShaderName)
        {
            RemoveNameIndexForEntry(entry);
            entry.m_ShaderName = new_name;
            RebuildNameIndexForEntry(entry);
            entry_changed = true;
        }
        if (hash != entry.m_SourceContentHash)
        {
            entry.m_SourceContentHash = hash;
            entry_changed = true;
        }
        const eastl::string new_zasset =
            ComputeZassetRelPath(NormaliseRelPath(rel_under_shaders).c_str());
        if (new_zasset != entry.m_ZassetRelPath)
        {
            entry.m_ZassetRelPath = new_zasset;
            entry_changed = true;
        }
        if (out_changed)
        {
            *out_changed = entry_changed;
        }
        return;
    }

    eastl::string guid = DeterministicGuidFromPath(rel_key);
    int bump = 0;
    while (m_ByGuid.count(guid) != 0 && bump < 256)
    {
        eastl::string salted = rel_key + eastl::string(":");
        salted += eastl::string(std::to_string(bump).c_str());
        guid = DeterministicGuidFromPath(salted);
        ++bump;
    }

    ShaderRegistryEntry entry {};
    entry.m_Guid = guid;
    entry.m_SourceRelPath = rel_key;
    entry.m_SourceMtimeNs = mtime;
    entry.m_ShaderName = ParseShaderNameFromSource(abs_shader_path);
    entry.m_SourceContentHash = hash;
    entry.m_ZassetRelPath = ComputeZassetRelPath(NormaliseRelPath(rel_under_shaders).c_str());

    m_GuidByPath.emplace(rel_key, guid);
    RebuildNameIndexForEntry(entry);
    m_ByGuid.emplace(guid, std::move(entry));

    if (out_changed)
    {
        *out_changed = true;
    }
}

void ShaderRegistry::RemoveEntryForAbsPath(const std::filesystem::path& abs_shader_path, bool* out_changed)
{
    if (out_changed)
    {
        *out_changed = false;
    }

    std::error_code rel_ec;
    const std::filesystem::path rel_from_project =
        std::filesystem::relative(abs_shader_path, m_ProjectRoot, rel_ec);
    if (rel_ec || rel_from_project.empty())
    {
        return;
    }

    const eastl::string rel_key = NormaliseRelPath(rel_from_project);

    std::lock_guard lk(m_Mutex);
    auto pit = m_GuidByPath.find(rel_key);
    if (pit == m_GuidByPath.end())
    {
        return;
    }

    auto git = m_ByGuid.find(pit->second);
    if (git != m_ByGuid.end())
    {
        RemoveNameIndexForEntry(git->second);
        m_ByGuid.erase(git);
    }
    m_GuidByPath.erase(pit);

    if (out_changed)
    {
        *out_changed = true;
    }
}
