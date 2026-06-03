#include "AssetRegistry.h"

#include "Runtime/Asset/AssetFile.h"
#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Core/Memory/MemoryManager.h"
#include "Runtime/Core/Serialize/SerializedFile.h"
#include "Runtime/Platform/Encoding/EncodingUtils.h"
#include "Runtime/Platform/Path/Path.h"
#include "Runtime/Resource/Asset/AssetManager.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <fstream>
#include <thread>

namespace
{
    std::time_t fileTimeToTimeT(std::filesystem::file_time_type file_time)
    {
        const auto system_time = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            file_time - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
        return std::chrono::system_clock::to_time_t(system_time);
    }

    // Insert/replace an entry in m_AssetMap and keep the GUID + by-type
    // reverse indices in sync. Called under a write-lock by every mutating
    // entry point (initial scan, incremental scan, addAsset, refreshAsset).
    // Centralising this keeps the three maps from drifting apart -- which
    // is the kind of bug that only shows up after a rename + type-change
    // and would otherwise be very hard to track down.
    void upsertEntryLocked(
        const std::string& relative_path,
        const AssetIndexEntry& entry,
        std::unordered_map<std::string, AssetIndexEntry>& asset_map,
        std::unordered_map<std::string, std::string>& guid_to_path,
        std::unordered_map<std::string, std::unordered_set<std::string>>& type_to_paths)
    {
        if (entry.guid.empty())
        {
            // Unidentified file -- caller's job to decide whether to log.
            // We still don't index something with no GUID because it can't
            // be looked up by GUID later anyway.
            return;
        }

        // If this path already had an entry, evict it from the old buckets
        // first. This is what makes "asset type changed after re-import"
        // (e.g. someone replaced a Material .zasset with a Texture .zasset
        // at the same path) consistent.
        auto existing_it = asset_map.find(relative_path);
        if (existing_it != asset_map.end())
        {
            const AssetIndexEntry& old_entry = existing_it->second;
            if (!old_entry.guid.empty() && old_entry.guid != entry.guid)
            {
                auto guid_it = guid_to_path.find(old_entry.guid);
                if (guid_it != guid_to_path.end() && guid_it->second == relative_path)
                {
                    guid_to_path.erase(guid_it);
                }
            }
            if (!old_entry.asset_type.empty() && old_entry.asset_type != entry.asset_type)
            {
                auto type_it = type_to_paths.find(old_entry.asset_type);
                if (type_it != type_to_paths.end())
                {
                    type_it->second.erase(relative_path);
                    if (type_it->second.empty())
                    {
                        type_to_paths.erase(type_it);
                    }
                }
            }
        }

        asset_map[relative_path] = entry;
        guid_to_path[entry.guid] = relative_path;
        if (!entry.asset_type.empty())
        {
            type_to_paths[entry.asset_type].insert(relative_path);
        }
    }

    // Inverse of upsertEntryLocked: pull a path out of all three maps.
    void eraseEntryLocked(
        const std::string& relative_path,
        std::unordered_map<std::string, AssetIndexEntry>& asset_map,
        std::unordered_map<std::string, std::string>& guid_to_path,
        std::unordered_map<std::string, std::unordered_set<std::string>>& type_to_paths)
    {
        auto it = asset_map.find(relative_path);
        if (it == asset_map.end())
        {
            return;
        }

        const AssetIndexEntry& entry = it->second;
        if (!entry.guid.empty())
        {
            auto guid_it = guid_to_path.find(entry.guid);
            if (guid_it != guid_to_path.end() && guid_it->second == relative_path)
            {
                guid_to_path.erase(guid_it);
            }
        }
        if (!entry.asset_type.empty())
        {
            auto type_it = type_to_paths.find(entry.asset_type);
            if (type_it != type_to_paths.end())
            {
                type_it->second.erase(relative_path);
                if (type_it->second.empty())
                {
                    type_to_paths.erase(type_it);
                }
            }
        }
        asset_map.erase(it);
    }

    std::string toLowerCopy(std::string s)
    {
        for (char& c : s)
        {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return s;
    }

    bool pathStartsWith(const std::string& path, const std::string& prefix)
    {
        if (prefix.empty())
        {
            return true;
        }
        if (path.size() < prefix.size())
        {
            return false;
        }
        return path.compare(0, prefix.size(), prefix) == 0;
    }

    AssetRegistryQueryFilter parseFilterString(const std::string& filter)
    {
        AssetRegistryQueryFilter out;
        if (filter.empty())
        {
            return out;
        }
        constexpr const char* kTypePrefix = "type:";
        constexpr const char* kPathPrefix = "path:";
        if (filter.rfind(kTypePrefix, 0) == 0)
        {
            out.asset_type = filter.substr(std::strlen(kTypePrefix));
            return out;
        }
        if (filter.rfind(kPathPrefix, 0) == 0)
        {
            out.package_path_prefix = filter.substr(std::strlen(kPathPrefix));
            return out;
        }
        // Bare token: match type exactly when it looks like a res_type name, else path substring.
        if (filter.size() > 3 && filter.compare(filter.size() - 3, 3, "Res") == 0)
        {
            out.asset_type = filter;
        }
        else
        {
            out.package_path_prefix = filter;
        }
        return out;
    }

    bool matchesFilter(const AssetIndexEntry& entry, const AssetRegistryQueryFilter& filter)
    {
        if (!filter.asset_type.empty() && entry.asset_type != filter.asset_type)
        {
            return false;
        }
        if (!filter.package_path_prefix.empty())
        {
            const std::string path_lower = toLowerCopy(entry.asset_path);
            const std::string prefix_lower = toLowerCopy(filter.package_path_prefix);
            if (path_lower.find(prefix_lower) == std::string::npos && !pathStartsWith(path_lower, prefix_lower))
            {
                return false;
            }
        }
        if (!filter.name_substring.empty())
        {
            const std::filesystem::path p(entry.asset_path);
            const std::string stem = toLowerCopy(p.stem().string());
            const std::string needle = toLowerCopy(filter.name_substring);
            if (stem.find(needle) == std::string::npos)
            {
                return false;
            }
        }
        return true;
    }

    void replaceRegistryPathToken(std::vector<std::string>& paths, const std::string& old_key, const std::string& new_key)
    {
        for (std::string& path : paths)
        {
            if (path == old_key)
            {
                path = new_key;
            }
        }
    }

    std::string normaliseToRegistryKey(const std::string& asset_path, const std::filesystem::path& scan_root)
    {
        if (asset_path.empty() || scan_root.empty())
        {
            return asset_path;
        }
        std::filesystem::path p(asset_path);
        if (!p.is_absolute())
        {
            return p.generic_string();
        }
        std::error_code ec;
        auto rel = std::filesystem::relative(p, scan_root, ec);
        if (ec || rel.empty())
        {
            return p.generic_string();
        }
        return rel.generic_string();
    }
}  // namespace

// Bumped any time the on-disk cache layout changes. LoadCache() accepts
// [k_cache_version_min, k_cache_version]; older or newer files are rejected
// (caller falls back to a full re-scan).
//
// Layout v1:
//   uint32_t magic           = 'ZARC' (0x5A415243)
//   uint32_t version         = 1
//   string   scan_root
//   uint64_t entry_count
//   for each entry:
//     string path, guid, asset_type
//     int64_t  last_modified
//     uint64_t file_size
//   uint32_t magic_tail      = 'ZARE'
//
// Layout v2 (adds dependency graph; referencer map rebuilt on load):
//   ... same header + entries as v1 ...
//   uint64_t dep_owner_count
//   for each owner with at least one dependency:
//     string owner_path
//     uint32_t dep_count
//     dep_count x string dep_path
//   uint32_t magic_tail      = 'ZARE'
constexpr uint32_t k_cache_magic = 0x5A415243;       // "ZARC"
constexpr uint32_t k_cache_magic_tail = 0x5A415245;  // "ZARE"
constexpr uint32_t k_cache_version_min = 1;
constexpr uint32_t k_cache_version = 2;
constexpr uint32_t k_cache_max_deps_per_asset = 65536;

namespace
{
    void writeU32(std::ofstream& s, uint32_t v)
    {
        s.write(reinterpret_cast<const char*>(&v), sizeof(v));
    }
    void writeU64(std::ofstream& s, uint64_t v)
    {
        s.write(reinterpret_cast<const char*>(&v), sizeof(v));
    }
    void writeI64(std::ofstream& s, int64_t v)
    {
        s.write(reinterpret_cast<const char*>(&v), sizeof(v));
    }
    void writeStr(std::ofstream& s, const std::string& v)
    {
        const uint32_t n = static_cast<uint32_t>(v.size());
        writeU32(s, n);
        if (n > 0)
            s.write(v.data(), n);
    }

    bool readU32(std::ifstream& s, uint32_t& v)
    {
        s.read(reinterpret_cast<char*>(&v), sizeof(v));
        return static_cast<bool>(s);
    }
    bool readU64(std::ifstream& s, uint64_t& v)
    {
        s.read(reinterpret_cast<char*>(&v), sizeof(v));
        return static_cast<bool>(s);
    }
    bool readI64(std::ifstream& s, int64_t& v)
    {
        s.read(reinterpret_cast<char*>(&v), sizeof(v));
        return static_cast<bool>(s);
    }
    bool readStr(std::ifstream& s, std::string& v, uint32_t max_sane = 4096)
    {
        uint32_t n = 0;
        if (!readU32(s, n))
            return false;
        if (n > max_sane)
            return false;  // sanity-cap; corrupt cache otherwise
        v.resize(n);
        if (n > 0)
            s.read(v.data(), n);
        return static_cast<bool>(s);
    }
}  // namespace

AssetRegistry::~AssetRegistry()
{
    // 确保线程在对象销毁前正确关闭
    WaitForScanComplete();
}

void AssetRegistry::Initialize(const std::filesystem::path& asset_folder, const std::filesystem::path& cache_path)
{
    // Cold-start optimisation: if a cache exists, hydrate the in-memory maps
    // from it first (cheap deserialise -- no .zasset header reads), then
    // hand off to the async scanner which will use incrementalUpdate-style
    // semantics to reconcile. Without this every project open re-reads
    // every .zasset header on startup.
    //
    // The scanner is still kicked off async so the editor UI doesn't block
    // on it, but the by-type / by-guid lookups already work against the
    // hydrated cache from frame 0.
    {
        auto lock = AcquireWriteLock();
        m_ScanRoot = asset_folder;
    }
    if (!cache_path.empty())
    {
        LoadCache(cache_path);
    }
    ScanAssetsAsync(asset_folder);
}

void AssetRegistry::ScanAssets(const std::filesystem::path& asset_folder) {}

void AssetRegistry::ScanAssetsAsync(const std::filesystem::path& asset_folder)
{
    // 如果已经在扫描，等待完成
    if (m_ScanStatus == ScanStatus::Scanning)
    {
        WaitForScanComplete();
    }

    // 确保之前的线程已结束
    if (m_ScanThread.joinable())
    {
        m_ScanThread.join();
    }

    // 重置状态
    {
        auto lock = AcquireWriteLock();
        m_ScanStatus = ScanStatus::Scanning;
        m_ScanProgress = 0.0f;
        m_ScannedCount = 0;
        m_TotalCount = 0;
        m_ScanRoot = asset_folder;
        m_SuppressAssetChangeNotifications = true;
    }

    // 启动异步扫描线程
    m_ScanThread = std::thread([this, asset_folder]() {
        try
        {
            // 1. 收集所有 .zasset 文件（不需要锁）
            std::vector<std::filesystem::path> asset_files;
            for (const auto& entry : std::filesystem::recursive_directory_iterator(asset_folder))
            {
                if (entry.is_regular_file() && entry.path().extension() == ".zasset")
                {
                    asset_files.push_back(entry.path());
                }
            }

            // 更新总数（需要锁）
            {
                auto lock = AcquireWriteLock();
                m_TotalCount = asset_files.size();
                m_ScannedCount = 0;
            }

            // 2. 扫描每个文件 -- but skip the header read entirely when an
            //    in-memory entry already matches the on-disk file's
            //    (mtime, size). This makes a "warm" launch (cache hydrated
            //    by Initialize()) finish in O(filesystem stat) instead of
            //    O(filesystem stat + per-file header read), which is what
            //    UE's AssetRegistry calls "incremental scan".
            for (const auto& asset_path : asset_files)
            {
                std::string relative_path = Path::GetRelativePath(asset_folder, asset_path).generic_string();

                std::time_t fs_mtime = 0;
                uint64_t fs_size = 0;
                std::error_code stat_ec;
                if (std::filesystem::exists(asset_path, stat_ec))
                {
                    fs_size = std::filesystem::file_size(asset_path, stat_ec);
                    auto ft = std::filesystem::last_write_time(asset_path, stat_ec);
                    if (!stat_ec)
                    {
                        fs_mtime = fileTimeToTimeT(ft);
                    }
                }

                bool needs_rescan = true;
                bool had_indexed_entry = false;
                {
                    auto lock = AcquireReadLock();
                    auto it = m_AssetMap.find(relative_path);
                    if (it != m_AssetMap.end() && !it->second.guid.empty() && it->second.last_modified == fs_mtime && it->second.file_size == fs_size)
                    {
                        needs_rescan = false;
                    }
                    else if (it != m_AssetMap.end() && !it->second.guid.empty())
                    {
                        had_indexed_entry = true;
                    }
                }

                AssetIndexEntry entry;
                if (needs_rescan)
                {
                    entry = ScanSingleAsset(asset_path);
                }

                AssetRegistryChangeEvent pending_change;
                bool enqueue_change = false;

                // 更新资源映射和进度（需要锁）
                float progress = 0.0f;
                size_t scanned = 0;
                {
                    auto lock = AcquireWriteLock();
                    if (needs_rescan)
                    {
                        upsertEntryLocked(relative_path, entry, m_AssetMap, m_GuidToPath, m_TypeToPaths);
                        if (!entry.guid.empty())
                        {
                            IndexDependenciesLocked(relative_path, asset_path);
                            pending_change.change_type = had_indexed_entry ? AssetRegistryChangeType::Updated
                                                                          : AssetRegistryChangeType::Added;
                            pending_change.asset_path = relative_path;
                            pending_change.entry = entry;
                            enqueue_change = true;
                        }
                    }

                    m_ScannedCount++;
                    m_ScanProgress = static_cast<float>(m_ScannedCount) / m_TotalCount;
                    progress = m_ScanProgress;
                    scanned = m_ScannedCount;
                }

                if (enqueue_change)
                {
                    EnqueueAssetChange(std::move(pending_change));
                }

                // 调用回调（不需要锁）
                if (m_ProgressCallback)
                {
                    m_ProgressCallback(progress, scanned, m_TotalCount);
                }
            }

            // 3. 检查已删除的文件（清理不在文件系统中的资源）
            std::vector<std::string> to_remove;
            std::vector<AssetRegistryChangeEvent> removal_events;
            {
                auto lock = AcquireWriteLock();
                for (const auto& [path, entry] : m_AssetMap)
                {
                    std::filesystem::path full_path = asset_folder / path;
                    if (!std::filesystem::exists(full_path))
                    {
                        to_remove.push_back(path);
                    }
                }
                // Drop them under the same write-lock to avoid the
                // RemoveAsset()-acquires-lock-again deadlock that
                // incrementalUpdate hit before.
                for (const auto& path : to_remove)
                {
                    auto it = m_AssetMap.find(path);
                    if (it != m_AssetMap.end() && !it->second.guid.empty())
                    {
                        AssetRegistryChangeEvent ev;
                        ev.change_type = AssetRegistryChangeType::Removed;
                        ev.asset_path = path;
                        ev.entry = it->second;
                        removal_events.push_back(std::move(ev));
                    }
                    RemoveDependencyEdgesLocked(path);
                    eraseEntryLocked(path, m_AssetMap, m_GuidToPath, m_TypeToPaths);
                }
            }
            for (auto& ev : removal_events)
            {
                EnqueueAssetChange(std::move(ev));
            }

            // 更新完成状态
            {
                auto lock = AcquireWriteLock();
                m_ScanStatus = ScanStatus::Completed;
                m_SuppressAssetChangeNotifications = false;
            }
            AssetRegistryChangeEvent bulk_refresh;
            bulk_refresh.change_type = AssetRegistryChangeType::Updated;
            bulk_refresh.asset_path.clear();
            EnqueueAssetChange(std::move(bulk_refresh));
        }
        catch (const std::exception& e)
        {
            LOG_ERROR(ZAsset,
                      "[AssetRegistry::scanAssetsAsync]Error during async asset scan: {}",
                      Encoding::GetExceptionMessage(e));
            auto lock = AcquireWriteLock();
            m_ScanStatus = ScanStatus::Failed;
            m_SuppressAssetChangeNotifications = false;
        }
    });
}
void AssetRegistry::IncrementalUpdate(const std::filesystem::path& asset_folder)
{
    auto lock = AcquireWriteLock();

    m_ScanRoot = asset_folder;

    // 1. 收集所有 .zasset 文件
    std::vector<std::filesystem::path> asset_files;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(asset_folder))
    {
        if (entry.is_regular_file() && entry.path().extension() == ".zasset")
        {
            asset_files.push_back(entry.path());
        }
    }

    m_TotalCount = asset_files.size();
    m_ScannedCount = 0;

    // 2. 检查每个文件
    for (const auto& asset_path : asset_files)
    {
        std::string relative_path = Path::GetRelativePath(asset_folder, asset_path).generic_string();

        // 检查是否已存在
        auto it = m_AssetMap.find(relative_path);
        if (it != m_AssetMap.end())
        {
            // 检查时间戳是否变化
            auto current_time = std::filesystem::last_write_time(asset_path);
            auto current_time_t = fileTimeToTimeT(current_time);
            if (current_time_t == it->second.last_modified)
            {
                // 未变化，跳过
                m_ScannedCount++;
                continue;
            }
        }

        const bool had_indexed_entry = (it != m_AssetMap.end() && !it->second.guid.empty());

        // 文件新增或变化，重新扫描
        AssetIndexEntry entry = ScanSingleAsset(asset_path);
        upsertEntryLocked(relative_path, entry, m_AssetMap, m_GuidToPath, m_TypeToPaths);
        if (!entry.guid.empty())
        {
            IndexDependenciesLocked(relative_path, asset_path);
            AssetRegistryChangeEvent ev;
            ev.change_type =
                had_indexed_entry ? AssetRegistryChangeType::Updated : AssetRegistryChangeType::Added;
            ev.asset_path = relative_path;
            ev.entry = entry;
            EnqueueAssetChange(std::move(ev));
        }

        m_ScannedCount++;
        m_ScanProgress = static_cast<float>(m_ScannedCount) / m_TotalCount;

        if (m_ProgressCallback)
        {
            m_ProgressCallback(m_ScanProgress, m_ScannedCount, m_TotalCount);
        }
    }

    // 3. 检查已删除的文件
    std::vector<std::string> to_remove;
    for (const auto& [path, entry] : m_AssetMap)
    {
        std::filesystem::path full_path = asset_folder / path;
        if (!std::filesystem::exists(full_path))
        {
            to_remove.push_back(path);
        }
    }

    std::vector<AssetRegistryChangeEvent> removal_events;
    for (const auto& path : to_remove)
    {
        // We are still holding the function-scoped write lock here, so we
        // can NOT route through RemoveAsset() (which acquires its own
        // write lock and would deadlock). Call the helper directly.
        auto map_it = m_AssetMap.find(path);
        if (map_it != m_AssetMap.end() && !map_it->second.guid.empty())
        {
            AssetRegistryChangeEvent ev;
            ev.change_type = AssetRegistryChangeType::Removed;
            ev.asset_path = path;
            ev.entry = map_it->second;
            removal_events.push_back(std::move(ev));
        }
        RemoveDependencyEdgesLocked(path);
        eraseEntryLocked(path, m_AssetMap, m_GuidToPath, m_TypeToPaths);
    }

    for (auto& ev : removal_events)
    {
        EnqueueAssetChange(std::move(ev));
    }

    m_ScanStatus = ScanStatus::Completed;
}
void AssetRegistry::SaveCache(const std::filesystem::path& cache_path)
{
    if (cache_path.empty())
    {
        return;
    }
    try
    {
        std::error_code ec;
        std::filesystem::create_directories(cache_path.parent_path(), ec);
        // ec ignored -- if create fails, ofstream will fail and we log below.

        // Write to a temp file first then rename, so a crash mid-write never
        // leaves a torn cache that LoadCache() would silently accept up to
        // the truncation point. Same pattern ScriptRegistry uses for its
        // JSON.
        const auto tmp_path = cache_path.string() + ".tmp";
        std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
        if (!out.is_open())
        {
            LOG_WARNING(ZAsset, "AssetRegistry::saveCache: cannot open {}", tmp_path);
            return;
        }

        // Snapshot under read-lock; we want to avoid holding the lock across
        // the ofstream writes (file IO is comparatively slow).
        std::vector<AssetIndexEntry> snapshot;
        std::vector<std::pair<std::string, std::vector<std::string>>> dep_snapshot;
        std::string scan_root_str;
        {
            auto lock = AcquireReadLock();
            snapshot.reserve(m_AssetMap.size());
            for (const auto& [path, entry] : m_AssetMap)
            {
                snapshot.push_back(entry);
            }
            dep_snapshot.reserve(m_DependencyMap.size());
            for (const auto& [owner_path, deps] : m_DependencyMap)
            {
                if (!deps.empty() && m_AssetMap.find(owner_path) != m_AssetMap.end())
                {
                    dep_snapshot.emplace_back(owner_path, deps);
                }
            }
            scan_root_str = m_ScanRoot.generic_string();
        }

        writeU32(out, k_cache_magic);
        writeU32(out, k_cache_version);
        // Persist the scan_root so a load after a project move can detect
        // the mismatch and force a full re-scan instead of silently
        // serving relative paths against the wrong root.
        writeStr(out, scan_root_str);
        writeU64(out, static_cast<uint64_t>(snapshot.size()));

        for (const auto& e : snapshot)
        {
            writeStr(out, e.asset_path);
            writeStr(out, e.guid);
            writeStr(out, e.asset_type);
            writeI64(out, static_cast<int64_t>(e.last_modified));
            writeU64(out, e.file_size);
        }

        writeU64(out, static_cast<uint64_t>(dep_snapshot.size()));
        for (const auto& [owner_path, deps] : dep_snapshot)
        {
            writeStr(out, owner_path);
            const uint32_t dep_count = static_cast<uint32_t>(deps.size());
            writeU32(out, dep_count);
            for (const std::string& dep_path : deps)
            {
                writeStr(out, dep_path);
            }
        }

        writeU32(out, k_cache_magic_tail);
        out.close();
        if (!out)
        {
            LOG_WARNING(ZAsset, "AssetRegistry::saveCache: write failed for {}", tmp_path);
            std::filesystem::remove(tmp_path, ec);
            return;
        }

        std::filesystem::rename(tmp_path, cache_path, ec);
        if (ec)
        {
            // rename failed (cross-volume? perm?). Try copy+remove fallback.
            std::filesystem::copy_file(
                tmp_path, cache_path, std::filesystem::copy_options::overwrite_existing, ec);
            std::filesystem::remove(tmp_path, ec);
        }
    }
    catch (const std::exception& e)
    {
        LOG_ERROR(ZAsset, "AssetRegistry::SaveCache failed: {}", Encoding::GetExceptionMessage(e));
    }
}
bool AssetRegistry::LoadCache(const std::filesystem::path& cache_path)
{
    if (cache_path.empty())
    {
        return false;
    }
    std::error_code ec;
    if (!std::filesystem::exists(cache_path, ec))
    {
        return false;
    }

    try
    {
        std::ifstream in(cache_path, std::ios::binary);
        if (!in.is_open())
        {
            return false;
        }

        uint32_t magic = 0, version = 0;
        if (!readU32(in, magic) || magic != k_cache_magic)
        {
            LOG_WARNING(ZAsset, "AssetRegistry::loadCache: bad magic, will rescan: {}", cache_path.generic_string());
            return false;
        }
        if (!readU32(in, version) || version < k_cache_version_min || version > k_cache_version)
        {
            // Schema rev -- silently drop the old cache; full rescan is
            // cheap enough on a typical project.
            return false;
        }

        std::string saved_scan_root;
        if (!readStr(in, saved_scan_root, 32768))
        {
            return false;
        }
        uint64_t count = 0;
        if (!readU64(in, count))
        {
            return false;
        }

        // Rebuild three-map state into temporary structures, then swap under
        // write-lock. This way a corrupt cache doesn't half-populate the
        // live registry.
        std::unordered_map<std::string, AssetIndexEntry> tmp_map;
        std::unordered_map<std::string, std::string> tmp_guid;
        std::unordered_map<std::string, std::unordered_set<std::string>> tmp_type;
        std::unordered_map<std::string, std::vector<std::string>> tmp_dep;
        tmp_map.reserve(static_cast<size_t>(count));
        tmp_guid.reserve(static_cast<size_t>(count));

        for (uint64_t i = 0; i < count; ++i)
        {
            AssetIndexEntry entry;
            int64_t lm = 0;
            if (!readStr(in, entry.asset_path) || !readStr(in, entry.guid) || !readStr(in, entry.asset_type) ||
                !readI64(in, lm) || !readU64(in, entry.file_size))
            {
                LOG_WARNING(ZAsset, "AssetRegistry::loadCache: truncated at entry {}/{}", i, count);
                return false;
            }
            entry.last_modified = static_cast<std::time_t>(lm);
            // Use the same insertion path the live scanners use, so every
            // map invariant (e.g. "guid empty -> not indexed") matches.
            upsertEntryLocked(entry.asset_path, entry, tmp_map, tmp_guid, tmp_type);
        }

        if (version >= 2)
        {
            uint64_t dep_owner_count = 0;
            if (!readU64(in, dep_owner_count))
            {
                LOG_WARNING(ZAsset, "AssetRegistry::loadCache: missing dependency section");
                return false;
            }

            for (uint64_t i = 0; i < dep_owner_count; ++i)
            {
                std::string owner_path;
                uint32_t dep_count = 0;
                if (!readStr(in, owner_path) || !readU32(in, dep_count))
                {
                    LOG_WARNING(ZAsset, "AssetRegistry::loadCache: truncated dependency owner {}/{}", i, dep_owner_count);
                    return false;
                }
                if (dep_count > k_cache_max_deps_per_asset)
                {
                    LOG_WARNING(ZAsset, "AssetRegistry::loadCache: insane dep_count {}", dep_count);
                    return false;
                }
                if (tmp_map.find(owner_path) == tmp_map.end())
                {
                    for (uint32_t j = 0; j < dep_count; ++j)
                    {
                        std::string skip_dep;
                        if (!readStr(in, skip_dep))
                        {
                            return false;
                        }
                    }
                    continue;
                }

                std::vector<std::string> deps;
                deps.reserve(dep_count);
                for (uint32_t j = 0; j < dep_count; ++j)
                {
                    std::string dep_path;
                    if (!readStr(in, dep_path))
                    {
                        LOG_WARNING(ZAsset,
                                    "AssetRegistry::loadCache: truncated dependency {}/{} for {}",
                                    j,
                                    dep_count,
                                    owner_path);
                        return false;
                    }
                    if (dep_path.empty() || dep_path == owner_path || tmp_map.find(dep_path) == tmp_map.end())
                    {
                        continue;
                    }
                    if (std::find(deps.begin(), deps.end(), dep_path) == deps.end())
                    {
                        deps.push_back(std::move(dep_path));
                    }
                }
                if (!deps.empty())
                {
                    tmp_dep.emplace(std::move(owner_path), std::move(deps));
                }
            }
        }

        uint32_t tail = 0;
        if (!readU32(in, tail) || tail != k_cache_magic_tail)
        {
            LOG_WARNING(ZAsset, "AssetRegistry::loadCache: missing tail marker, rescan");
            return false;
        }

        const size_t dep_owners_loaded = tmp_dep.size();
        {
            auto lock = AcquireWriteLock();
            m_AssetMap = std::move(tmp_map);
            m_GuidToPath = std::move(tmp_guid);
            m_TypeToPaths = std::move(tmp_type);
            m_DependencyMap = std::move(tmp_dep);
            m_ReferencerMap.clear();
            RebuildReferencerMapLocked();
            // Don't overwrite m_ScanRoot unconditionally -- the caller
            // (initialize) will set it before any incremental update. We
            // only use saved_scan_root as a soft sanity hint here, because
            // a project move legitimately changes the absolute path.
        }
        if (version >= 2 && dep_owners_loaded > 0)
        {
            LOG_INFO(ZAsset,
                     "AssetRegistry::loadCache: restored {} dependency owners from v{}",
                     dep_owners_loaded,
                     version);
        }
        return true;
    }
    catch (const std::exception& e)
    {
        LOG_ERROR(ZAsset, "Failed to load cache: {}", Encoding::GetExceptionMessage(e));
        std::filesystem::remove(cache_path, ec);
        return false;
    }
}
std::vector<AssetIndexEntry> AssetRegistry::FindAssets(const AssetRegistryQueryFilter& filter) const
{
    std::vector<AssetIndexEntry> result;
    auto lock = AcquireReadLock();
    result.reserve(m_AssetMap.size());
    for (const auto& [path, entry] : m_AssetMap)
    {
        (void)path;
        if (matchesFilter(entry, filter))
        {
            result.push_back(entry);
        }
    }
    return result;
}

std::vector<AssetIndexEntry> AssetRegistry::FindAssets(const std::string& filter) const
{
    return FindAssets(parseFilterString(filter));
}

std::filesystem::path AssetRegistry::GetScanRoot() const
{
    auto lock = AcquireReadLock();
    return m_ScanRoot;
}

std::optional<AssetIndexEntry> AssetRegistry::GetAssetIndex(const std::string& asset_path) const
{
    auto lock = AcquireReadLock();
    auto it = m_AssetMap.find(asset_path);
    if (it == m_AssetMap.end())
    {
        return std::nullopt;
    }
    return it->second;
}

std::optional<AssetIndexEntry> AssetRegistry::GetAssetIndexByGUID(const std::string& guid) const
{
    auto lock = AcquireReadLock();
    auto guid_iter = m_GuidToPath.find(guid);
    if (guid_iter == m_GuidToPath.end())
    {
        return std::nullopt;
    }
    auto entry_iter = m_AssetMap.find(guid_iter->second);
    if (entry_iter == m_AssetMap.end())
    {
        // Index drift -- shouldn't happen now that upsert/erase keep both
        // maps in sync, but stay defensive instead of returning a stale
        // entry that doesn't exist on disk.
        return std::nullopt;
    }
    return entry_iter->second;
}

std::vector<std::string> AssetRegistry::GetAssetsByType(const std::string& asset_type) const
{
    std::vector<std::string> result;
    auto lock = AcquireReadLock();
    auto it = m_TypeToPaths.find(asset_type);
    if (it == m_TypeToPaths.end())
    {
        return result;
    }
    result.reserve(it->second.size());
    for (const auto& path : it->second)
    {
        result.push_back(path);
    }
    return result;
}

std::vector<std::filesystem::path> AssetRegistry::GetAssetsByTypeAbsolute(const std::string& asset_type) const
{
    std::vector<std::filesystem::path> result;
    auto lock = AcquireReadLock();
    auto it = m_TypeToPaths.find(asset_type);
    if (it == m_TypeToPaths.end() || m_ScanRoot.empty())
    {
        return result;
    }
    result.reserve(it->second.size());
    for (const auto& rel : it->second)
    {
        // Relative paths in m_AssetMap were generated via
        // Path::GetRelativePath(m_ScanRoot, abs).generic_string(); just
        // recompose. operator/= handles the platform separator and
        // generic_string() in the caller normalises if needed.
        result.push_back(m_ScanRoot / rel);
    }
    return result;
}

std::vector<std::string> AssetRegistry::GetDependencies(const std::string& asset_path) const
{
    std::filesystem::path scan_root;
    {
        auto lock = AcquireReadLock();
        scan_root = m_ScanRoot;
    }
    const std::string key = normaliseToRegistryKey(asset_path, scan_root);
    if (key.empty())
    {
        return {};
    }
    EnsureDependenciesIndexed(key);
    auto lock = AcquireReadLock();
    auto it = m_DependencyMap.find(key);
    if (it == m_DependencyMap.end())
    {
        return {};
    }
    return it->second;
}

std::vector<std::string> AssetRegistry::GetReferencers(const std::string& asset_path) const
{
    std::filesystem::path scan_root;
    {
        auto lock = AcquireReadLock();
        scan_root = m_ScanRoot;
    }
    const std::string key = normaliseToRegistryKey(asset_path, scan_root);
    if (key.empty())
    {
        return {};
    }
    EnsureDependenciesIndexed(key);
    auto lock = AcquireReadLock();
    auto it = m_ReferencerMap.find(key);
    if (it == m_ReferencerMap.end())
    {
        return {};
    }
    return it->second;
}

void AssetRegistry::RebuildReferencerMapLocked()
{
    m_ReferencerMap.clear();
    for (const auto& [owner_path, deps] : m_DependencyMap)
    {
        for (const std::string& dep_path : deps)
        {
            auto& refs = m_ReferencerMap[dep_path];
            if (std::find(refs.begin(), refs.end(), owner_path) == refs.end())
            {
                refs.push_back(owner_path);
            }
        }
    }
}

void AssetRegistry::RemoveDependencyEdgesLocked(const std::string& relative_path)
{
    auto dep_it = m_DependencyMap.find(relative_path);
    if (dep_it != m_DependencyMap.end())
    {
        for (const std::string& dep_path : dep_it->second)
        {
            auto ref_it = m_ReferencerMap.find(dep_path);
            if (ref_it != m_ReferencerMap.end())
            {
                auto& refs = ref_it->second;
                refs.erase(std::remove(refs.begin(), refs.end(), relative_path), refs.end());
            }
        }
        m_DependencyMap.erase(dep_it);
    }
    m_ReferencerMap.erase(relative_path);
}

void AssetRegistry::IndexDependenciesLocked(const std::string& relative_path, const std::filesystem::path& abs_path)
{
    RemoveDependencyEdgesLocked(relative_path);

    if (!std::filesystem::exists(abs_path))
    {
        m_DependencyMap[relative_path] = {};
        return;
    }

    std::vector<std::string> deps;
    SerializedFile* serialized_file = MemoryManager::CreateObject<SerializedFile>();
    if (serialized_file->InitializeRead(abs_path, AssetManager::kCacheSize) == kSerializedFileLoadError_None)
    {
        for (const FileIdentifier& external : serialized_file->GetExternalRefs())
        {
            std::string dep_key;
            if (!external.guid.empty())
            {
                auto guid_it = m_GuidToPath.find(external.guid);
                if (guid_it != m_GuidToPath.end())
                {
                    dep_key = guid_it->second;
                }
            }
            if (dep_key.empty() && !external.pathName.empty())
            {
                dep_key = normaliseToRegistryKey(external.pathName, m_ScanRoot);
                if (m_AssetMap.find(dep_key) == m_AssetMap.end())
                {
                    dep_key.clear();
                }
            }
            if (!dep_key.empty() && dep_key != relative_path)
            {
                if (std::find(deps.begin(), deps.end(), dep_key) == deps.end())
                {
                    deps.push_back(dep_key);
                }
            }
        }
    }
    MemoryManager::DestroyObject(serialized_file);

    m_DependencyMap[relative_path] = deps;
    for (const std::string& dep : deps)
    {
        auto& refs = m_ReferencerMap[dep];
        if (std::find(refs.begin(), refs.end(), relative_path) == refs.end())
        {
            refs.push_back(relative_path);
        }
    }
}

void AssetRegistry::EnsureDependenciesIndexed(const std::string& relative_path) const
{
    {
        auto lock = AcquireReadLock();
        if (m_DependencyMap.find(relative_path) != m_DependencyMap.end())
        {
            return;
        }
    }

    std::filesystem::path scan_root;
    {
        auto lock = AcquireReadLock();
        scan_root = m_ScanRoot;
    }
    if (scan_root.empty())
    {
        return;
    }

    const std::filesystem::path abs_path = scan_root / relative_path;
    auto lock = const_cast<AssetRegistry*>(this)->AcquireWriteLock();
    if (m_DependencyMap.find(relative_path) != m_DependencyMap.end())
    {
        return;
    }
    const_cast<AssetRegistry*>(this)->IndexDependenciesLocked(relative_path, abs_path);
}

void AssetRegistry::EnqueueAssetChange(AssetRegistryChangeEvent event)
{
    if (m_SuppressAssetChangeNotifications)
    {
        return;
    }
    std::lock_guard<std::mutex> lock(m_PendingChangesMutex);
    m_PendingChanges.push_back(std::move(event));
}

AssetRegistryUpdatedHandle AssetRegistry::RegisterOnAssetUpdated(AssetRegistryUpdatedCallback callback)
{
    if (!callback)
    {
        return 0;
    }
    std::lock_guard<std::mutex> lock(m_ListenerMutex);
    const AssetRegistryUpdatedHandle handle = m_NextListenerHandle++;
    m_AssetUpdatedListeners.emplace(handle, std::move(callback));
    return handle;
}

void AssetRegistry::UnregisterOnAssetUpdated(AssetRegistryUpdatedHandle handle)
{
    if (handle == 0)
    {
        return;
    }
    std::lock_guard<std::mutex> lock(m_ListenerMutex);
    m_AssetUpdatedListeners.erase(handle);
}

void AssetRegistry::DispatchPendingAssetChanges()
{
    std::deque<AssetRegistryChangeEvent> batch;
    {
        std::lock_guard<std::mutex> lock(m_PendingChangesMutex);
        batch.swap(m_PendingChanges);
    }
    if (batch.empty())
    {
        return;
    }

    // Last event per path wins (coalesces scan/import bursts).
    std::unordered_map<std::string, AssetRegistryChangeEvent> coalesced;
    coalesced.reserve(batch.size());
    for (auto& ev : batch)
    {
        coalesced[ev.asset_path] = std::move(ev);
    }

    std::vector<AssetRegistryUpdatedCallback> listeners;
    {
        std::lock_guard<std::mutex> lock(m_ListenerMutex);
        listeners.reserve(m_AssetUpdatedListeners.size());
        for (const auto& [id, cb] : m_AssetUpdatedListeners)
        {
            (void)id;
            if (cb)
            {
                listeners.push_back(cb);
            }
        }
    }
    if (listeners.empty())
    {
        return;
    }

    for (const auto& [path, ev] : coalesced)
    {
        (void)path;
        for (const auto& listener : listeners)
        {
            listener(ev);
        }
    }
}

void AssetRegistry::RefreshAsset(const std::string& asset_path)
{
    // Re-scan the file and replace its entry. We accept either an absolute
    // path (FileSystemWatcher / save callbacks) or a registry-relative path
    // and normalise to the latter, because the scanner stores keys relative
    // to m_ScanRoot. Without this normalisation an importer save site that
    // passes the absolute path back through RefreshAsset() would create a
    // SECOND entry under the absolute path that GetAssetIndex(rel) can
    // never find.
    std::filesystem::path scan_root;
    {
        auto lock = AcquireReadLock();
        scan_root = m_ScanRoot;
    }

    std::filesystem::path abs_path(asset_path);
    if (!abs_path.is_absolute() && !scan_root.empty())
    {
        abs_path = scan_root / abs_path;
    }
    const std::string key = normaliseToRegistryKey(asset_path, scan_root);

    AssetIndexEntry entry = ScanSingleAsset(abs_path);
    bool had_indexed_entry = false;
    {
        auto lock = AcquireWriteLock();
        auto it = m_AssetMap.find(key);
        if (it != m_AssetMap.end() && !it->second.guid.empty())
        {
            had_indexed_entry = true;
        }
        upsertEntryLocked(key, entry, m_AssetMap, m_GuidToPath, m_TypeToPaths);
        if (!entry.guid.empty())
        {
            IndexDependenciesLocked(key, abs_path);
        }
    }
    if (!entry.guid.empty())
    {
        AssetRegistryChangeEvent ev;
        ev.change_type =
            had_indexed_entry ? AssetRegistryChangeType::Updated : AssetRegistryChangeType::Added;
        ev.asset_path = key;
        ev.entry = entry;
        EnqueueAssetChange(std::move(ev));
    }
}

void AssetRegistry::RemoveAsset(const std::string& asset_path)
{
    std::filesystem::path scan_root;
    {
        auto lock = AcquireReadLock();
        scan_root = m_ScanRoot;
    }
    const std::string key = normaliseToRegistryKey(asset_path, scan_root);

    std::optional<AssetIndexEntry> removed_entry;
    {
        auto lock = AcquireWriteLock();
        auto it = m_AssetMap.find(key);
        if (it != m_AssetMap.end() && !it->second.guid.empty())
        {
            removed_entry = it->second;
        }
        RemoveDependencyEdgesLocked(key);
        eraseEntryLocked(key, m_AssetMap, m_GuidToPath, m_TypeToPaths);
    }
    if (removed_entry.has_value())
    {
        AssetRegistryChangeEvent ev;
        ev.change_type = AssetRegistryChangeType::Removed;
        ev.asset_path = key;
        ev.entry = std::move(removed_entry.value());
        EnqueueAssetChange(std::move(ev));
    }
}

void AssetRegistry::AddAsset(const std::string& asset_path)
{
    // addAsset is just refreshAsset under a different name in the public
    // API -- the file may or may not have been seen before. Same impl.
    RefreshAsset(asset_path);
}

bool AssetRegistry::RenameAsset(const std::string& old_asset_path, const std::string& new_asset_path)
{
    std::filesystem::path scan_root;
    {
        auto lock = AcquireReadLock();
        scan_root = m_ScanRoot;
    }
    if (scan_root.empty())
    {
        return false;
    }

    const std::string old_key = normaliseToRegistryKey(old_asset_path, scan_root);
    const std::string new_key = normaliseToRegistryKey(new_asset_path, scan_root);
    if (old_key.empty() || new_key.empty() || old_key == new_key)
    {
        return false;
    }

    std::filesystem::path new_abs(new_asset_path);
    if (!new_abs.is_absolute())
    {
        new_abs = scan_root / new_abs;
    }

    std::optional<AssetIndexEntry> old_entry;
    std::optional<AssetIndexEntry> new_entry;

    {
        auto lock = AcquireWriteLock();
        const auto old_it = m_AssetMap.find(old_key);
        if (old_it == m_AssetMap.end())
        {
            return false;
        }
        if (m_AssetMap.find(new_key) != m_AssetMap.end())
        {
            return false;
        }

        old_entry = old_it->second;

        if (auto dep_it = m_DependencyMap.find(old_key); dep_it != m_DependencyMap.end())
        {
            std::vector<std::string> deps = std::move(dep_it->second);
            m_DependencyMap.erase(dep_it);
            auto& new_deps = m_DependencyMap[new_key];
            new_deps = std::move(deps);
            for (const std::string& dep : new_deps)
            {
                replaceRegistryPathToken(m_ReferencerMap[dep], old_key, new_key);
            }
        }

        if (auto ref_it = m_ReferencerMap.find(old_key); ref_it != m_ReferencerMap.end())
        {
            std::vector<std::string> referencers = std::move(ref_it->second);
            m_ReferencerMap.erase(ref_it);
            auto& new_refs = m_ReferencerMap[new_key];
            new_refs = std::move(referencers);
            for (const std::string& owner : new_refs)
            {
                auto owner_it = m_DependencyMap.find(owner);
                if (owner_it != m_DependencyMap.end())
                {
                    replaceRegistryPathToken(owner_it->second, old_key, new_key);
                }
            }
        }

        for (auto& [owner, deps] : m_DependencyMap)
        {
            if (owner != new_key)
            {
                replaceRegistryPathToken(deps, old_key, new_key);
            }
        }
        for (auto& [dep, referencers] : m_ReferencerMap)
        {
            if (dep != new_key)
            {
                replaceRegistryPathToken(referencers, old_key, new_key);
            }
        }

        AssetIndexEntry entry = old_it->second;
        entry.asset_path = new_key;
        eraseEntryLocked(old_key, m_AssetMap, m_GuidToPath, m_TypeToPaths);
        upsertEntryLocked(new_key, entry, m_AssetMap, m_GuidToPath, m_TypeToPaths);

        std::error_code exists_ec;
        if (std::filesystem::exists(new_abs, exists_ec) && !exists_ec)
        {
            IndexDependenciesLocked(new_key, new_abs);
            auto new_it = m_AssetMap.find(new_key);
            if (new_it != m_AssetMap.end())
            {
                new_entry = new_it->second;
            }
        }
        else
        {
            new_entry = entry;
        }
    }

    if (old_entry.has_value())
    {
        AssetRegistryChangeEvent removed;
        removed.change_type = AssetRegistryChangeType::Removed;
        removed.asset_path = old_key;
        removed.entry = std::move(old_entry.value());
        EnqueueAssetChange(std::move(removed));
    }
    if (new_entry.has_value())
    {
        AssetRegistryChangeEvent added;
        added.change_type = AssetRegistryChangeType::Added;
        added.asset_path = new_key;
        added.entry = std::move(new_entry.value());
        EnqueueAssetChange(std::move(added));
    }
    return true;
}

void AssetRegistry::SetScanProgressCallback(ScanProgressCallback callback)
{
    auto lock = AcquireWriteLock();
    m_ProgressCallback = std::move(callback);
}

void AssetRegistry::WaitForScanComplete()
{
    if (m_ScanThread.joinable())
    {
        m_ScanThread.join();
    }
}

std::shared_lock<std::shared_mutex> AssetRegistry::AcquireReadLock() const
{
    return std::shared_lock<std::shared_mutex>(m_Mutex);
}

std::unique_lock<std::shared_mutex> AssetRegistry::AcquireWriteLock()
{
    return std::unique_lock<std::shared_mutex>(m_Mutex);
}

AssetIndexEntry AssetRegistry::ScanSingleAsset(const std::filesystem::path& asset_path)
{
    AssetIndexEntry entry;
    entry.asset_path = asset_path.generic_string();
    entry.guid = "";
    entry.asset_type = "";
    entry.file_size = 0;
    entry.last_modified = 0;

    try
    {
        // Get file size and last modified time
        if (std::filesystem::exists(asset_path))
        {
            entry.file_size = std::filesystem::file_size(asset_path);
            auto file_time = std::filesystem::last_write_time(asset_path);
            entry.last_modified = fileTimeToTimeT(file_time);
        }

        // Try to read the asset file header directly
        std::ifstream file(asset_path, std::ios::binary);
        if (file.is_open() && file.good())
        {
            AssetFileHeader header;
            file.read(reinterpret_cast<char*>(&header), sizeof(AssetFileHeader));

            if (file.gcount() == sizeof(AssetFileHeader))
            {
                // Verify magic number
                if (header.magic == k_zasset_magic)
                {
                    // header.guid / header.asset_type are fixed-size char
                    // arrays. Use strnlen to bound the string construction
                    // in case a writer ever forgets to null-terminate the
                    // last byte (defence in depth -- the canonical writer
                    // is `SerializedFile`, which zero-initialises the
                    // 176-byte header before stamping fields).
                    entry.guid = std::string(header.guid, ::strnlen(header.guid, sizeof(header.guid)));
                    entry.asset_type = std::string(header.asset_type,
                                                   ::strnlen(header.asset_type, sizeof(header.asset_type)));
                }
                else
                {
                    // Magic mismatch -- this might be a legacy text-serialised
                    // .zasset (JSON), or a malformed file. Try the
                    // AssetManager fallback to recover the type via
                    // reflection.
                    //
                    // IMPORTANT: when the fallback succeeds we still need a
                    // non-empty guid for upsertEntryLocked to actually index
                    // the entry. Synthesise one deterministically from the
                    // path so re-scans land on the SAME synthetic guid (so
                    // the GUID->path map is stable across runs even for
                    // legacy files). The "legacy:" prefix keeps these
                    // synthesised IDs distinguishable from real GUIDs.
                    entry.asset_type = GET_SYSTEM(AssetManager)->GetAssetTypeName(asset_path);
                    if (!entry.asset_type.empty())
                    {
                        entry.guid = "legacy:" + asset_path.generic_string();
                    }
                    else
                    {
                        LOG_WARNING(ZAsset, "Invalid asset file magic number: {}", asset_path.generic_string());
                    }
                }
            }
            else
            {
                LOG_WARNING(
                    ZAsset, "Failed to read asset file header (file too small): {}", asset_path.generic_string());
            }
            file.close();
        }
        else
        {
            LOG_WARNING(ZAsset, "Failed to open asset file: {}", asset_path.generic_string());
        }
    }
    catch (const std::exception& e)
    {
        LOG_ERROR(
            ZAsset, "Error scanning asset file {}: {}", asset_path.generic_string(), Encoding::GetExceptionMessage(e));
    }

    return entry;
}