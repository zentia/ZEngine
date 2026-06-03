#pragma once

#include "Editor/AssetPipeline/AssetImporter.h"
#include "Editor/AssetPipeline/TextureImporter/TextureImportSettingsRegistry.h"
#include "Editor/AssetRegistry/AssetRegistry.h"
#include "Editor/FileSystemWatcher/FileSystemWatcher.h"
#include "Runtime/Asset/AssetFile.h"
#include "Runtime/AssetBundle/AssetBundle.h"
#include "Runtime/Core/Base/EngineSystem.h"
#include "Runtime/Resource/Asset/AssetManager.h"
#include "SerializedSystem.h"
#include "SourceAssetRegistry.h"

#include <chrono>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

class AssetBundle;

class EditorAssetManager : public AssetManager
{
public:
    // 初始化（在编辑器启动时调用）
    bool Initialize() override;

    // P9: persist the AssetRegistry cache to disk so the next launch can
    // skip the .zasset header re-read on warm files. Also pumps any
    // pending FileSystemWatcher events out of the queue before saving.
    void Shutdown() override;

    // P9: drains the FileSystemWatcher event queue on the main thread.
    // Called from EditorApplication's per-frame tick. Must run on the same
    // thread as the registry callers (read-locks are non-recursive).
    void TickWatcher();

    // ========== 资产查询（使用 AssetRegistry） ==========

    // 查找资产（只返回索引信息，不加载完整元数据）
    std::vector<AssetIndexEntry> FindAssets(const std::string& filter) const;
    std::optional<AssetIndexEntry> GetAssetIndex(const std::string& asset_path) const;
    std::optional<AssetIndexEntry> GetAssetIndexByGUID(const std::string& guid) const;

    // Override the runtime base behaviour: prefer the AssetRegistry's
    // type-indexed lookup, fall back to the directory walk when the
    // registry is still warming up. See AssetManager::GetAssetsByType()
    // doc for why this lives on the base class.
    std::vector<std::filesystem::path>
    GetAssetsByType(const std::string& asset_type, const std::filesystem::path& search_root) const override;

    void GetAssetGuidAndType(const std::filesystem::path& asset_path,
                             std::string& outGuid,
                             std::string& outAssetType) const override;

    bool TryGetPathForGuid(const std::string& guid, std::filesystem::path& outPath) const override;

    // ========== 资产加载（使用 AssetManager） ==========

    // 加载资产（加载完整资产数据）
    template<typename AssetType>
    AssetType* loadAsset(const std::string& asset_path)
    {
        return GetEditorAssetBundle()->Get<AssetType>(asset_path);
    }

    AssetBundle* GetEditorAssetBundle();

    // ========== 资产保存 ==========
    //
    // Asset writes go through `AssetManager::WriteObjectToDiskThreadSafe`
    // (inherited from the runtime base) -- it routes the object through
    // `SerializedFile`, which is the canonical .zasset producer. The
    // pre-route-B `saveAsset(asset, metadata, path)` overload that used
    // `AssetFile::saveAsset` was a stub (its body was entirely commented
    // out) and has been removed in P2 #9. See
    // doc/BINDLESS_TEXTURE_PATH.md PR10 for the migration story.

    // ========== 资产导入 ==========

    // 导入原始资产（生成 .zasset 文件）
    bool importSourceAsset(const std::filesystem::path& source_path,
                           const std::filesystem::path& output_path,
                           const AssetImporterSettings* import_settings = nullptr);

    // 重新导入资产（源文件变化时）
    bool reimportAsset(const std::string& asset_path, const AssetImporterSettings* import_settings = nullptr);

    // ========== 资产创建/删除/移动 ==========

    // 删除资产
    bool DeleteAsset(const std::string& asset_path);

    // 重命名/移动资产
    bool MoveAsset(const std::string& old_path, const std::string& new_path);

    // ========== 依赖关系 ==========

    // 获取依赖关系（从 .zasset 文件加载元数据）
    std::vector<std::string> GetDependencies(const std::string& asset_path) const;
    std::vector<std::string> GetReferencers(const std::string& asset_path) const;

    // ========== 刷新 ==========

    // 刷新所有资产
    void RefreshAssets();

    // 刷新单个资产
    void RefreshAsset(const std::string& asset_path);

    // UE-style registry events (see AssetRegistryChangeEvent). Listeners run
    // on the main thread after TickWatcher drains pending changes.
    AssetRegistryUpdatedHandle RegisterOnAssetUpdated(AssetRegistryUpdatedCallback callback);
    void UnregisterOnAssetUpdated(AssetRegistryUpdatedHandle handle);

    // ========== 访问内部组件 ==========

    AssetRegistry& getAssetRegistry() { return m_AssetRegistry; }
    AssetImportManager& getImportManager() { return m_ImportManager; }
    FileSystemWatcher& getFileSystemWatcher() { return m_FileWatcher; }
    void
    AddSerializedSystem(std::string_view& className, std::filesystem::path& path, GUID guid, bool allowSerializeAsText);

protected:
    int InsertPathNameInternal(const std::filesystem::path& path, bool create) override;
    virtual const std::string& PathIDToPathNameInternal(int pathID) const;

private:
    AssetRegistry m_AssetRegistry;
    AssetImportManager m_ImportManager;
    FileSystemWatcher m_FileWatcher;

    // P5: incremental DataTable rebuild watcher. Rooted at <Project>/Data/
    // and filtered to .csv. Events route through compileOne / deleteFor on
    // DataTableImporter; the importer in turn writes to (or removes from)
    // <Project>/Assets/_Generated/Data/, which the *primary* m_FileWatcher
    // above is already watching, so AssetRegistry index updates fall out
    // for free without us calling RefreshAsset() a second time.
    //
    // Why a separate watcher instead of widening m_FileWatcher's root?
    // - m_FileWatcher is rooted at the project's Content folder (= the
    //   Assets/ tree) and filtered to {.zasset}. Widening it to the
    //   project root would inflate event volume by 100x in projects with
    //   sizeable Scripts/, and require teaching every onFileChanged path
    //   to discriminate by file kind. A second per-purpose watcher keeps
    //   the routing trivial (one extension per watcher, one handler per
    //   purpose) at the cost of one extra ReadDirectoryChangesW handle.
    FileSystemWatcher m_DataWatcher;

    // PR-AI2: live HLSL recompile watcher. Rooted at <Project>/Shaders/
    // and filtered to {.hlsl, .shader, .compute, .raytrace}. On
    // create/change we drop the matching DX12 cache entries so the next
    // PSO build picks up the new bytes; on delete we still drop the
    // cache (cheap; harmless if nothing is referenced anymore).
    //
    // Why a third watcher rather than widening m_FileWatcher: same
    // rationale as m_DataWatcher (one extension set per watcher, one
    // handler per purpose). Shaders / Data / Assets do not share file
    // formats and have completely different downstream effects, so
    // routing through a single multiplexed watcher would just push the
    // dispatch into every callback. The extra ReadDirectoryChangesW
    // handle is negligible (kernel objects, not filesystem cost).
    //
    // m_PendingShaderInvalidations + m_ShaderDebounceMutex implement
    // a 200 ms debounce: editors like VS Code save by writing a temp
    // file then renaming, which surfaces as one Created + one Changed
    // ~tens of milliseconds apart. Without debouncing we'd recompile
    // twice per save. 200 ms is the same Unity uses (250 ms) and
    // half UE's window (500 ms); it's enough for VS Code's sequence
    // to settle on every machine we've tested.
    FileSystemWatcher m_ShadersWatcher;

    using ShaderInvalidationDeadline = std::chrono::steady_clock::time_point;
    std::unordered_map<std::string, ShaderInvalidationDeadline> m_PendingShaderInvalidations;
    std::mutex m_ShaderDebounceMutex;

    // PR-AI3: AutoReimport. SourceAssetRegistry persists the
    // `.zasset -> (source_abs_path, source_mtime_ns)` map under
    // `<Project>/AssetRegistry/source_registry.json`. On editor focus
    // (GLFW window focus callback) we walk every entry, stat the
    // source, and queue any that changed for reimport. The reimport
    // itself runs spread across subsequent frames (N=10 per frame) so
    // a 50-asset Save-All from Photoshop doesn't pause the editor.
    //
    // Why focus-driven (not watcher-driven): source files often live
    // OUTSIDE <Project>/Assets/ (the user picked them from Photoshop's
    // export directory), and we'd have to spin a per-file FileSystemWatcher
    // root for each unique parent directory -- expensive and racy. Unity
    // takes the same focus-driven approach for its imported source assets
    // (`AssetDatabase.Refresh` runs on focus); UE's `FAutoReimportManager`
    // does watcher-driven but only because it forces sources to live
    // inside Content/. We don't enforce that.
    //
    // m_PendingReimports is the FIFO of zasset abs paths whose sources
    // have been stat-confirmed-changed but not yet reimported.
    // m_ReimportPausedUntilFocus is set right after we drain the
    // queue; the next focus event re-arms the scan. Without this flag a
    // user holding Alt-Tab between Photoshop and the editor would
    // re-scan + re-stat every entry every focus cycle, even when nothing
    // changed.
    SourceAssetRegistry m_SourceRegistry;
    TextureImportSettingsRegistry m_TextureImportSettingsRegistry;
    std::deque<std::filesystem::path> m_PendingReimports;
    std::mutex m_ReimportMutex;
    bool m_ReimportPausedUntilFocus = false;

    // Throttle. UE caps "import operations per tick" at 4
    // (FAutoReimportManager); Unity's batched ImportAssets has no
    // user-tunable cap. 10 here is a compromise: each TextureImporter
    // call at the editor scale is ~5..50 ms (DXT compression dominates),
    // so 10 per frame keeps us under a 500 ms hitch budget on the worst
    // case. Tunable by changing this constant; if we ever want a project
    // setting, plumb it through ProjectInfo.
    static constexpr int kReimportsPerFrame = 10;

    std::vector<SerializedSystem> m_SerializedSystems;
    std::unordered_map<std::string, int> m_ConstantGUIDAssetPathToIndex;
    std::vector<std::string> m_PathNames;

    // 文件系统监听回调
    void OnFileCreated(const std::filesystem::path& path);
    void OnFileChanged(const std::filesystem::path& path);
    void OnFileDeleted(const std::filesystem::path& path);

    // PR-AI2: shader watcher hooks. queueShaderInvalidation stamps a
    // 200 ms deadline and returns immediately; flushPendingShaderInvalidations
    // is called from TickWatcher() once per frame and only acts on entries
    // whose deadline has elapsed.
    void QueueShaderInvalidation(const std::filesystem::path& path);
    void FlushPendingShaderInvalidations();

    // PR-AI3 hooks.
    //
    // onEditorFocusGained: invoked from the GLFW window-focus callback
    // when the editor becomes the focused window. Walks the
    // SourceAssetRegistry, stats each source, queues changed ones into
    // m_PendingReimports. Cheap (one stat per entry); bails early if
    // m_SourceRegistry is empty.
    //
    // tickReimportQueue: dequeues up to kReimportsPerFrame entries and
    // calls the corresponding importer's source-aware reimport. Called
    // from TickWatcher() once per frame after the watcher drains.
    //
    // recordImportSource: thin wrapper for callers (AssetsMenu::ConvertAsset
    // and the future drop-import) so they don't need to know about the
    // SourceAssetRegistry directly.
    void OnEditorFocusGained();
    void TickReimportQueue();

    // PR-AI3: shared body for AutoReimport queue drain and manual
    // `reimportAsset()`. Requires a SourceAssetRegistry entry and an
    // on-disk source file.
    bool ReimportZassetWithRegistrySource(const std::filesystem::path& zasset_abs_path,
                                          const AssetImporterSettings* import_settings);

public:
    // Public so AssetsMenu and Content Browser drop-import can wire the
    // import-time entry into the registry without round-tripping through
    // a singleton accessor. EditorAssetManager itself is already a
    // SystemRegistry-resident singleton.
    void RecordImportSource(const std::filesystem::path& zasset_abs_path,
                            const std::filesystem::path& source_abs_path);

    TextureImportSettingsRegistry& GetTextureImportSettingsRegistry() { return m_TextureImportSettingsRegistry; }
    const SourceAssetRegistry& GetSourceAssetRegistry() const { return m_SourceRegistry; }

private:
    std::unique_ptr<AssetImporterSettings>
    ResolveImporterSettingsForReimport(const std::filesystem::path& zasset_abs_path,
                                       const std::filesystem::path& source_path,
                                       const std::shared_ptr<AssetImporter>& importer,
                                       const AssetImporterSettings* caller_settings) const;

    static std::filesystem::path ToAbsoluteZassetPath(const std::filesystem::path& zasset_path);
};