#include "SourceAssetRegistry.h"

#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Platform/Encoding/EncodingUtils.h"
#include "Runtime/Project/ProjectInfo.h"
#include "rapidjson/document.h"
#include "rapidjson/istreamwrapper.h"
#include "rapidjson/prettywriter.h"
#include "rapidjson/stringbuffer.h"

#include <algorithm>
#include <fstream>

namespace
{

    // PR-AI3: forward-slash + lower-case-on-Windows normalisation. Mirrors
    // ScriptRegistry::normaliseRelPath. We can't share that helper here
    // because Editor cannot reach into Runtime's anonymous namespace.
    std::string normalisePath(const std::filesystem::path& p)
    {
        std::string s = p.generic_string();
#ifdef _WIN32
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#endif
        return s;
    }

}  // namespace

bool SourceAssetRegistry::Initialize()
{
    ProjectInfo* project_info = GET_SYSTEM(ProjectInfo);
    if (project_info == nullptr)
    {
        // No project loaded yet -- harmless. The next time a project
        // opens, EditorAssetManager will rebuild a fresh registry
        // instance.
        return true;
    }
    const std::filesystem::path registry_root = project_info->GetAssetRegistryRoot();
    if (registry_root.empty())
    {
        return true;
    }
    m_RegistryFile = registry_root / "source_registry.json";

    return LoadFromDisk();
}

void SourceAssetRegistry::Shutdown()
{
    // Persist a final time so the last batch of edits during the session
    // (which already saved on every record/remove anyway) is on disk.
    SaveToDisk();

    std::lock_guard<std::mutex> lk(m_Mutex);
    m_Entries.clear();
    m_RegistryFile.clear();
}

std::filesystem::path SourceAssetRegistry::GetRegistryFilePath() const
{
    return m_RegistryFile;
}

void SourceAssetRegistry::Record(const std::filesystem::path& zasset_abs_path,
                                 const std::filesystem::path& source_abs_path)
{
    if (zasset_abs_path.empty() || source_abs_path.empty())
    {
        return;
    }

    Entry entry;
    entry.source_path = source_abs_path.generic_string();
    entry.source_mtime_ns = MtimeNs(source_abs_path);

    {
        std::lock_guard<std::mutex> lk(m_Mutex);
        // Skip persistence work entirely if we have no project file to
        // write into. The in-memory map still gets the entry so that a
        // late-binding project open can flush it later, but for now we
        // just refuse to silently lose it.
        m_Entries[NormaliseKey(zasset_abs_path)] = entry;
    }
    SaveToDisk();
}

void SourceAssetRegistry::RemoveEntry(const std::filesystem::path& zasset_abs_path)
{
    bool changed = false;
    {
        std::lock_guard<std::mutex> lk(m_Mutex);
        auto it = m_Entries.find(NormaliseKey(zasset_abs_path));
        if (it != m_Entries.end())
        {
            m_Entries.erase(it);
            changed = true;
        }
    }
    if (changed)
    {
        SaveToDisk();
    }
}

std::optional<SourceAssetRegistry::Entry>
SourceAssetRegistry::Lookup(const std::filesystem::path& zasset_abs_path) const
{
    std::lock_guard<std::mutex> lk(m_Mutex);
    auto it = m_Entries.find(NormaliseKey(zasset_abs_path));
    if (it == m_Entries.end())
    {
        return std::nullopt;
    }
    return it->second;
}

void SourceAssetRegistry::ForEach(
    const std::function<void(const std::filesystem::path& zasset_abs_path,
                             const Entry& entry)>& visitor) const
{
    if (!visitor)
    {
        return;
    }
    std::lock_guard<std::mutex> lk(m_Mutex);
    for (const auto& kv : m_Entries)
    {
        visitor(std::filesystem::path(kv.first), kv.second);
    }
}

bool SourceAssetRegistry::HasSourceChanged(const Entry& entry, int64_t* out_current_mtime_ns)
{
    std::error_code ec;
    if (!std::filesystem::exists(entry.source_path, ec) || ec)
    {
        if (out_current_mtime_ns)
        {
            *out_current_mtime_ns = 0;
        }
        return false;
    }
    const int64_t live = MtimeNs(entry.source_path);
    if (out_current_mtime_ns)
    {
        *out_current_mtime_ns = live;
    }
    return live != entry.source_mtime_ns;
}

void SourceAssetRegistry::UpdateMtime(const std::filesystem::path& zasset_abs_path,
                                      int64_t new_mtime_ns)
{
    bool changed = false;
    {
        std::lock_guard<std::mutex> lk(m_Mutex);
        auto it = m_Entries.find(NormaliseKey(zasset_abs_path));
        if (it != m_Entries.end() && it->second.source_mtime_ns != new_mtime_ns)
        {
            it->second.source_mtime_ns = new_mtime_ns;
            changed = true;
        }
    }
    if (changed)
    {
        SaveToDisk();
    }
}

std::string SourceAssetRegistry::NormaliseKey(const std::filesystem::path& zasset_abs_path)
{
    // We use the absolute path as key. weakly_canonical resolves "..",
    // ".", and case differences on case-insensitive filesystems via
    // a single FS lookup; if that fails (file might not exist yet on
    // a re-import that races a delete) fall back to absolute().
    std::error_code ec;
    auto abs = std::filesystem::weakly_canonical(zasset_abs_path, ec);
    if (ec || abs.empty())
    {
        abs = std::filesystem::absolute(zasset_abs_path, ec);
        if (ec || abs.empty())
        {
            abs = zasset_abs_path;
        }
    }
    return normalisePath(abs);
}

int64_t SourceAssetRegistry::MtimeNs(const std::filesystem::path& abs_path)
{
    std::error_code ec;
    auto t = std::filesystem::last_write_time(abs_path, ec);
    if (ec)
    {
        return 0;
    }
    // file_time_type's clock differs per-platform; converting via
    // duration_cast<nanoseconds>(t.time_since_epoch()).count() yields a
    // monotonically meaningful integer per machine, which is all we
    // need for "did mtime change since import?". Cross-machine entries
    // surviving a VCS pull are tolerated -- the first focus tick will
    // see "different" mtime and trigger one harmless reimport, which
    // updates the entry to the local mtime baseline.
    return std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
}

bool SourceAssetRegistry::LoadFromDisk()
{
    if (m_RegistryFile.empty())
    {
        return false;
    }
    std::error_code ec;
    if (!std::filesystem::exists(m_RegistryFile, ec) || ec)
    {
        // Missing file is the cold-start case; not an error.
        return true;
    }
    std::ifstream ifs(m_RegistryFile, std::ios::binary);
    if (!ifs.is_open())
    {
        LOG_WARNING(ZAsset, "SourceAssetRegistry: cannot open '{}'", m_RegistryFile.generic_string());
        return false;
    }
    rapidjson::IStreamWrapper isw(ifs);
    rapidjson::Document doc;
    doc.ParseStream(isw);
    if (doc.HasParseError() || !doc.IsObject())
    {
        LOG_WARNING(ZAsset,
                    "SourceAssetRegistry: parse failed for '{}'; will rebuild from scratch",
                    m_RegistryFile.generic_string());
        return false;
    }

    decltype(m_Entries) loaded;

    if (doc.HasMember("entries") && doc["entries"].IsArray())
    {
        for (const auto& v : doc["entries"].GetArray())
        {
            if (!v.IsObject())
            {
                continue;
            }
            // NOTE: do not call v.GetObject() -- on Windows <windows.h>
            // macro-expands GetObject into GetObjectA, breaking the rapidjson
            // member resolution. v itself already exposes HasMember/operator[]
            // when v.IsObject() is true.
            if (!v.HasMember("zasset") || !v["zasset"].IsString() ||
                !v.HasMember("source") || !v["source"].IsString())
            {
                continue;
            }
            Entry e;
            e.source_path = v["source"].GetString();
            if (v.HasMember("mtime_ns") && v["mtime_ns"].IsInt64())
            {
                e.source_mtime_ns = v["mtime_ns"].GetInt64();
            }
            // Re-key through normaliseKey so a JSON shipped from a
            // case-different machine still hits the same slot here.
            loaded[NormaliseKey(std::filesystem::path(v["zasset"].GetString()))] = std::move(e);
        }
    }

    {
        std::lock_guard<std::mutex> lk(m_Mutex);
        m_Entries = std::move(loaded);
    }
    LOG_INFO(ZAsset,
             "SourceAssetRegistry: loaded {} entries from '{}'",
             m_Entries.size(),
             m_RegistryFile.generic_string());
    return true;
}

bool SourceAssetRegistry::SaveToDisk() const
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
            std::lock_guard<std::mutex> lk(m_Mutex);

            // Stable VCS-friendly order, sorted by zasset key.
            std::vector<std::pair<std::string, Entry>> sorted;
            sorted.reserve(m_Entries.size());
            for (const auto& kv : m_Entries)
            {
                sorted.emplace_back(kv.first, kv.second);
            }
            std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

            for (const auto& kv : sorted)
            {
                rapidjson::Value entry(rapidjson::kObjectType);
                entry.AddMember("zasset",
                                rapidjson::Value(kv.first.c_str(),
                                                 static_cast<rapidjson::SizeType>(kv.first.size()),
                                                 alloc),
                                alloc);
                entry.AddMember("source",
                                rapidjson::Value(kv.second.source_path.c_str(),
                                                 static_cast<rapidjson::SizeType>(kv.second.source_path.size()),
                                                 alloc),
                                alloc);
                entry.AddMember("mtime_ns",
                                static_cast<int64_t>(kv.second.source_mtime_ns),
                                alloc);
                entries.PushBack(entry, alloc);
            }
        }
        doc.AddMember("entries", entries, alloc);

        // Atomic write: temp + rename. Same pattern ScriptRegistry uses.
        std::filesystem::path temp = m_RegistryFile;
        temp += ".tmp";

        {
            std::ofstream ofs(temp, std::ios::binary);
            if (!ofs.is_open())
            {
                LOG_ERROR(ZAsset,
                          "SourceAssetRegistry: cannot open tmp file for writing: {}",
                          temp.generic_string());
                return false;
            }
            rapidjson::StringBuffer sb;
            rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(sb);
            doc.Accept(writer);
            ofs << sb.GetString();
        }
        std::error_code ec;
        std::filesystem::rename(temp, m_RegistryFile, ec);
        if (ec)
        {
            std::filesystem::copy_file(temp, m_RegistryFile, std::filesystem::copy_options::overwrite_existing, ec);
            std::filesystem::remove(temp);
            if (ec)
            {
                LOG_ERROR(ZAsset,
                          "SourceAssetRegistry: finalise failed: {} ({})",
                          m_RegistryFile.generic_string(),
                          ec.message());
                return false;
            }
        }
        return true;
    }
    catch (const std::exception& e)
    {
        LOG_ERROR(ZAsset, "SourceAssetRegistry: save failed: {}", Encoding::GetExceptionMessage(e));
        return false;
    }
}
