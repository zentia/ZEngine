#pragma once

#include <cstdint>
#include <ctime>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// 资产索引条目（轻量级，只存储索引信息）
class AssetIndexEntry
{
public:
    std::string asset_path;     // .zasset 文件路径（相对路径）
    std::string asset_type;     // 资产类型（通过反射获取）
    std::string guid;           // 唯一标识符
    std::time_t last_modified;  // 最后修改时间
    uint64_t file_size;         // 文件大小（字节）
};

// 扫描状态
enum class ScanStatus
{
    Idle,       // 空闲
    Scanning,   // 正在扫描
    Completed,  // 扫描完成
    Failed      // 扫描失败
};

// 扫描进度回调
using ScanProgressCallback = std::function<void(float progress, size_t scanned, size_t total)>;

// UE IAssetRegistry::OnAssetAdded / OnAssetRemoved / OnAssetUpdated (unified).
enum class AssetRegistryChangeType : uint8_t
{
    Added,
    Removed,
    Updated,
};

struct AssetRegistryChangeEvent
{
    AssetRegistryChangeType change_type = AssetRegistryChangeType::Updated;
    std::string asset_path;  // registry key (relative to scan root)
    AssetIndexEntry entry;   // new state (Added/Updated) or removed entry (Removed)
};

using AssetRegistryUpdatedCallback = std::function<void(const AssetRegistryChangeEvent&)>;
using AssetRegistryUpdatedHandle = uint32_t;

// UE FARFilter-lite: AND across non-empty fields (path prefix, exact type, name substring).
struct AssetRegistryQueryFilter
{
    std::string package_path_prefix;  // relative to scan root, forward slashes
    std::string asset_type;           // exact class name, e.g. "MaterialRes"
    std::string name_substring;       // matched against the .zasset stem
};

class AssetRegistry
{
public:
    // 析构函数：确保线程在对象销毁前正确关闭
    ~AssetRegistry();

    // 初始化：从缓存文件加载，然后增量更新
    // asset_folder: 资产文件夹路径（如 "Content/"）
    // cache_path: 缓存文件路径（如 ".zengine/AssetRegistry.cache"）
    void Initialize(const std::filesystem::path& asset_folder, const std::filesystem::path& cache_path);

    // 同步扫描所有 .zasset 文件（阻塞调用）
    void ScanAssets(const std::filesystem::path& asset_folder);

    // 异步扫描所有 .zasset 文件（非阻塞）
    void ScanAssetsAsync(const std::filesystem::path& asset_folder);

    // 增量更新：只扫描变化的 .zasset 文件
    void IncrementalUpdate(const std::filesystem::path& asset_folder);

    // 保存缓存到磁盘
    void SaveCache(const std::filesystem::path& cache_path);

    // 从缓存文件加载
    bool LoadCache(const std::filesystem::path& cache_path);

    // Query assets (UE IAssetRegistry::GetAssets-style filter).
    std::vector<AssetIndexEntry> FindAssets(const AssetRegistryQueryFilter& filter) const;

    // Convenience: "type:ShaderRes", "path:Combat/", or bare path/type substring.
    std::vector<AssetIndexEntry> FindAssets(const std::string& filter) const;

    std::filesystem::path GetScanRoot() const;

    // Path/GUID lookups. Returned by VALUE (under the read-lock briefly held
    // internally) -- the alternative of returning a pointer into m_AssetMap
    // is unsafe across concurrent refresh/remove. Optional is empty when the
    // key isn't tracked. Mirrors UE's FAssetRegistryModule::GetAssetByPath().
    std::optional<AssetIndexEntry> GetAssetIndex(const std::string& asset_path) const;
    std::optional<AssetIndexEntry> GetAssetIndexByGUID(const std::string& guid) const;

    // Returns all asset paths registered under the given asset_type
    // (e.g. "ShaderRes", "PrefabRes"). Match is case-sensitive and uses
    // the SAME spelling AssetManager::GetAssetTypeName() returns -- the
    // class name with the trailing "Res" included. Mirrors UE's
    // GetAssetsByClass(). Empty vector when nothing matches or when the
    // initial scan hasn't completed yet.
    //
    // Paths are RELATIVE to the asset_folder that was passed to
    // Initialize() / scanAssets*() (matches the keys stored in m_AssetMap).
    // Use GetAssetsByTypeAbsolute() if you want std::filesystem::path
    // values you can pass straight to AssetManager::ReadObject().
    std::vector<std::string> GetAssetsByType(const std::string& asset_type) const;
    std::vector<std::filesystem::path> GetAssetsByTypeAbsolute(const std::string& asset_type) const;

    // 获取依赖关系（需要从 .zasset 文件加载元数据）
    std::vector<std::string> GetDependencies(const std::string& asset_path) const;
    std::vector<std::string> GetReferencers(const std::string& asset_path) const;

    // 资产变化处理
    void RefreshAsset(const std::string& asset_path);
    void RemoveAsset(const std::string& asset_path);
    void AddAsset(const std::string& asset_path);
    // Re-key registry + dependency/referencer maps (GUID unchanged). Caller
    // must rename the .zasset on disk first. Returns false if old_key missing
    // or new_key already occupied.
    bool RenameAsset(const std::string& old_asset_path, const std::string& new_asset_path);

    // 扫描状态
    ScanStatus getScanStatus() const { return m_ScanStatus; }
    bool isScanning() const { return m_ScanStatus == ScanStatus::Scanning; }
    float getScanProgress() const { return m_ScanProgress; }
    size_t getScannedCount() const { return m_ScannedCount; }
    size_t getTotalCount() const { return m_TotalCount; }

    // 设置扫描进度回调
    void SetScanProgressCallback(ScanProgressCallback callback);

    // Registry change notifications (Added / Removed / Updated). Callbacks
    // always run on the thread that calls DispatchPendingAssetChanges()
    // (Editor main thread via EditorAssetManager::TickWatcher). Mutations
    // from the async scan thread only enqueue; they never invoke listeners
    // directly.
    AssetRegistryUpdatedHandle RegisterOnAssetUpdated(AssetRegistryUpdatedCallback callback);
    void UnregisterOnAssetUpdated(AssetRegistryUpdatedHandle handle);
    void DispatchPendingAssetChanges();

    // 等待扫描完成
    void WaitForScanComplete();

    // 线程安全访问
    std::shared_lock<std::shared_mutex> AcquireReadLock() const;
    std::unique_lock<std::shared_mutex> AcquireWriteLock();

private:
    // 内部扫描方法
    void scanAssetsInternal(const std::filesystem::path& asset_folder, bool incremental);
    AssetIndexEntry ScanSingleAsset(const std::filesystem::path& asset_path);

    void RemoveDependencyEdgesLocked(const std::string& relative_path);
    void IndexDependenciesLocked(const std::string& relative_path, const std::filesystem::path& abs_path);
    void EnsureDependenciesIndexed(const std::string& relative_path) const;
    void RebuildReferencerMapLocked();

    void EnqueueAssetChange(AssetRegistryChangeEvent event);

    // 索引存储
    std::unordered_map<std::string, AssetIndexEntry> m_AssetMap;                // path -> index
    std::unordered_map<std::string, std::string> m_GuidToPath;                  // guid -> path
    std::unordered_map<std::string, std::vector<std::string>> m_DependencyMap;  // path -> dependency paths
    std::unordered_map<std::string, std::vector<std::string>> m_ReferencerMap;  // path -> referencers (paths)

    // type -> set<path> reverse index. Maintained alongside m_AssetMap by
    // every mutating path (scan, addAsset, refreshAsset, removeAsset). Lets
    // callers like the Inspector "pick-a-shader" popup avoid a full
    // recursive_directory_iterator + per-file header read every time the
    // dropdown opens. Equivalent to UE's class->asset list inside
    // FAssetRegistry. unordered_set rather than vector so the maintenance
    // cost stays O(1) when an asset's type changes after a refresh
    // (different type -> erase from old bucket, insert into new).
    std::unordered_map<std::string, std::unordered_set<std::string>> m_TypeToPaths;

    // 扫描状态
    ScanStatus m_ScanStatus = ScanStatus::Idle;
    float m_ScanProgress = 0.0f;
    size_t m_ScannedCount = 0;
    size_t m_TotalCount = 0;
    std::thread m_ScanThread;
    ScanProgressCallback m_ProgressCallback;

    std::unordered_map<AssetRegistryUpdatedHandle, AssetRegistryUpdatedCallback> m_AssetUpdatedListeners;
    std::mutex m_ListenerMutex;
    AssetRegistryUpdatedHandle m_NextListenerHandle = 1;

    std::deque<AssetRegistryChangeEvent> m_PendingChanges;
    std::mutex m_PendingChangesMutex;
    bool m_SuppressAssetChangeNotifications {false};

    // The asset_folder that was last passed to a scan*() call. Stored so
    // GetAssetsByTypeAbsolute() can rehydrate the relative paths in
    // m_AssetMap back into absolute filesystem paths without the caller
    // having to thread through ProjectInfo.
    std::filesystem::path m_ScanRoot;

    // 线程安全
    mutable std::shared_mutex m_Mutex;  // 读写锁，支持并发读取
};