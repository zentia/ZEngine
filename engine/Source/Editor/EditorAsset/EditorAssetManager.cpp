#include "EditorAssetManager.h"

#include "Editor/AssetPipeline/DataTableImporter/DataTableImporter.h"
#include "Editor/AssetPipeline/DataTableImporter/XlsxImporter.h"
#include "Editor/AssetPipeline/MeshImporter/MeshImporter.h"
#include "Editor/AssetPipeline/FontImporter/FontImporter.h"
#include "Editor/AssetPipeline/ShaderImporter/ShaderImporter.h"
#include "Editor/AssetPipeline/TextureImporter/TextureImporter.h"
#include "Editor/AssetPipeline/TextureImporter/TextureImporterSettings.h"
#include "Editor/EditorWindow/PreviewWindow/MeshDataPreview.h"
#include "Runtime/AssetBundle/AssetBundle.h"
#include "Runtime/BaseClasses/PPtr.h"
#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Core/Serialize/SerializedFile.h"
#include "Runtime/Function/Render/ShaderRegistry.h"
#include "Runtime/Project/ProjectInfo.h"
#include "Runtime/Scripting/ScriptRegistry.h"
#include "SerializedSystem.h"

// PR-AI2: dropping DXIL cache entries on .hlsl change is the DX12-only
// half of shader hot-reload. Vulkan / Metal / WebGL2 are not currently
// wired through the editor host (see AGENTS.md memory: Windows host is
// DX12-only; Vulkan is for Android/OHOS), so the watcher routes only
// through DX12ShaderCompiler::invalidateCacheForSource. When the host
// gains a Vulkan/Metal path, mirror this dispatch to the corresponding
// `::ShaderCompiler::invalidateCacheForSource` and that compiler's
// equivalent.
#ifdef _WIN32
    #include "Runtime/Function/Render/Interface/DX12/DX12ShaderCompiler.h"
#endif

// PR-AI3: GLFW window-focus callback wiring lives in the Editor's
// WindowSystem, not here. We register at Initialize() time and rely on
// the manager's lifetime exceeding the window (TickWatcher already
// assumes the same).
#include "Runtime/Function/Render/WindowSystem.h"

#include <algorithm>
#include <chrono>
#include <optional>
#pragma comment(lib, "ole32.lib")

namespace
{
    // <Project>/Intermediate/asset_registry.cache
    //
    // Sits under Intermediate/ on purpose -- the cache is a pure
    // performance optimisation that can be regenerated from the .zasset
    // files at any time, so it's gitignored. (Compare with AssetRegistry/
    // which IS checked in: that one stores the path<->GUID map for text
    // sources where the cache *is* the source of truth. See AGENTS.md 2.2.)
    std::filesystem::path resolveAssetRegistryCachePath()
    {
        const std::shared_ptr<ProjectInfo> project_info = GET_SYSTEM(ProjectInfo);
        if (project_info == nullptr)
        {
            return {};
        }
        const std::filesystem::path root = project_info->GetProjectRoot();
        if (root.empty())
        {
            return {};
        }
        return root / "Intermediate" / "asset_registry.cache";
    }

    bool ShouldSkipSourceDiscoveryDir(const std::filesystem::path& dir_path)
    {
        const std::string name = dir_path.filename().generic_string();
        if (name.empty())
        {
            return false;
        }
        return name == "Intermediate" || name == "node_modules" || name == ".git" || name == "AssetRegistry";
    }

    bool IsMeshSourceExtension(const std::filesystem::path& path)
    {
        std::string ext = path.extension().generic_string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return ext == ".fbx" || ext == ".obj" || ext == ".gltf" || ext == ".glb";
    }

    std::optional<std::filesystem::path> TryDiscoverMeshSourceForReimport(
        const std::filesystem::path& zasset_abs_path)
    {
        const std::shared_ptr<ProjectInfo> project_info = GET_SYSTEM(ProjectInfo);
        if (project_info == nullptr)
        {
            return std::nullopt;
        }

        const std::filesystem::path project_root = project_info->GetProjectRoot();
        if (project_root.empty())
        {
            return std::nullopt;
        }

        const std::string stem = zasset_abs_path.stem().generic_string();
        if (stem.empty())
        {
            return std::nullopt;
        }

        std::error_code ec;
        for (std::filesystem::recursive_directory_iterator it(project_root, ec), end; it != end; it.increment(ec))
        {
            if (ec)
            {
                break;
            }
            if (it->is_directory())
            {
                if (ShouldSkipSourceDiscoveryDir(it->path()))
                {
                    it.disable_recursion_pending();
                }
                continue;
            }
            if (!it->is_regular_file())
            {
                continue;
            }
            const std::filesystem::path& candidate = it->path();
            if (!IsMeshSourceExtension(candidate))
            {
                continue;
            }
            if (candidate.stem().generic_string() != stem)
            {
                continue;
            }
            return std::filesystem::absolute(candidate);
        }
        return std::nullopt;
    }
}  // namespace

bool EditorAssetManager::Initialize()
{
    auto&& projectInfo = GET_SYSTEM(ProjectInfo);

    // P9: cold-start path is loadCache -> async incremental scan reconciles.
    // saveCache happens on Shutdown (see EditorApplication tear-down).
    const std::filesystem::path cache_path = resolveAssetRegistryCachePath();
    m_AssetRegistry.Initialize(projectInfo->m_WorkingDir, cache_path);

    // FileSystemWatcher: events arrive on the watcher thread (Win32
    // ReadDirectoryChangesW / inotify / FSEvents) and get drained on the
    // main thread by EditorApplication's per-frame Tick. The handlers
    // ultimately route to AssetRegistry::refreshAsset/removeAsset, which
    // accept absolute paths and normalise them to scan-root-relative keys.
    m_FileWatcher.SetOnFileCreated(
        [this](const std::filesystem::path& p) { OnFileCreated(p); });
    m_FileWatcher.SetOnFileChanged(
        [this](const std::filesystem::path& p) { OnFileChanged(p); });
    m_FileWatcher.SetOnFileDeleted(
        [this](const std::filesystem::path& p) { OnFileDeleted(p); });
    m_FileWatcher.WatchDirectory(projectInfo->GetProjectContent());

    // Register asset importers
    auto texture_importer = std::make_shared<TextureImporter>();
    m_ImportManager.RegisterImporter(texture_importer);

    auto font_importer = std::make_shared<FontImporter>();
    m_ImportManager.RegisterImporter(font_importer);

    auto mesh_importer = std::make_shared<MeshImporter>();
    m_ImportManager.RegisterImporter(mesh_importer);

    // DataTableImporter: drives the CSV -> .zasset compile under
    // <Project>/Assets/_Generated/Data/. Registration order doesn't matter
    // (importers are looked up by extension, no overlap with TextureImporter).
    auto data_table_importer = std::make_shared<DataTableImporter>();
    m_ImportManager.RegisterImporter(data_table_importer);

    // PR #8: XlsxImporter -- the .xlsx-format counterpart to
    // DataTableImporter. They live in the same directory (Data/) and emit
    // to the same destination (_Generated/Data/<rel>.zasset); on stem
    // collision XLSX wins because CompileProject() below runs the CSV
    // pass first and the xlsx pass second, overwriting in place. The
    // dispatch table in AssetImportManager keys on extension only, so
    // there's no ambiguity at registration time.
    auto xlsx_importer = std::make_shared<XlsxImporter>();
    m_ImportManager.RegisterImporter(xlsx_importer);

    // One-shot whole-project compile of every CSV under <Project>/Data/.
    // Mirrors TypeScriptCompiler's startup `tsc` pass on .ts sources -- by
    // the time the editor first paints, the AssetRegistry has already
    // indexed all generated DataTable .zassets, so Inspector dropdowns and
    // PPtr resolution work without a delayed background scan.
    //
    // We deliberately run this AFTER registerImporter so that future code
    // paths (e.g. an "incremental rebuild on watcher event" hook) can route
    // through the same registered importer instance.
    DataTableImporter::CompileProject();

    // PR #8: XLSX pass runs SECOND so xlsx products overwrite any CSV
    // product at the same generated path. Resolves the "Foo.csv +
    // Foo.xlsx in the same directory" stem-collision design choice
    // (xlsx wins, with a LOG_WARNING per collision); see
    // XlsxImporter::CompileProject() comment block.
    XlsxImporter::CompileProject();

    // PR-SE3b: first-time import of .shader text files into ShaderRes
    // .zasset. Runs AFTER the data-table importers so the _Generated/
    // subtree is already warm. The importer skips files whose .zasset
    // already exists (A2 first-time seeding), so re-running on every
    // editor launch is idempotent and cheap.
    Runtime::ShaderImporter::ImportProjectShaders();
    Runtime::ShaderImporter::PrecompileProjectShaderVariants();

    // Texture cook (Phase 5): first-time seeding of compressed+mipped
    // Texture2D .zasset siblings for any source image under Assets/. Idempotent
    // (skips existing .zasset), so warm restarts only stat. Materials then
    // resolve the cooked variant through RenderResourceBase::LoadTexture.
    TextureImporter::ImportProjectTextures();

    if (auto shader_registry = GET_SYSTEM(ShaderRegistry))
    {
        shader_registry->rescan();
    }

    // P5: incremental rebuild. Watch <Project>/Data/ for .csv changes
    // (create / modify / delete) and route each event to DataTableImporter.
    // The watcher fires its callbacks from Update() on the main thread
    // (TickWatcher() drains both watchers below), so the handlers can
    // freely talk to ObjectManager / AssetManager just like the primary
    // .zasset watcher's handlers do.
    {
        const std::filesystem::path data_root = projectInfo->GetDataRoot();
        std::error_code ec;
        // The Data/ root is created by ProjectInfo::EnsureScriptsScaffold,
        // which has already run by now. Belt-and-suspenders create_directories
        // in case the user manually deleted Data/ between scaffold and
        // EditorAssetManager::Initialize -- without an existing dir, the
        // watcher's underlying ReadDirectoryChangesW (Windows) would fail
        // and we'd silently lose the live-rebuild feature.
        std::filesystem::create_directories(data_root, ec);

        // PR #5 + PR #8: watch both .csv (DataTableImporter) and .xlsx
        // (XlsxImporter). The watcher fires a single callback per event;
        // we dispatch on extension at the closure boundary so the watcher
        // itself stays format-agnostic. Note that .xlsx delete events
        // also trigger DataTableImporter::DeleteGeneratedFor for the CSV
        // sibling case implicitly -- if both Foo.csv and Foo.xlsx existed
        // and the user deletes only Foo.xlsx, the CSV product is rebuilt
        // automatically on the next compileOne for Foo.csv (which fires
        // because they share the destination path; we re-run CSV compile
        // here so the registry returns to a CSV-derived state).
        m_DataWatcher.SetExtensionFilter({".csv", ".xlsx"});
        auto onDataChange = [](const std::filesystem::path& p) {
            const std::string ext = [&p] {
                std::string e = p.extension().string();
                std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) {
                    return static_cast<char>(std::tolower(c));
                });
                return e;
            }();
            if (ext == ".xlsx")
            {
                XlsxImporter::CompileOne(p);
            }
            else
            {
                DataTableImporter::CompileOne(p);
                // If a .xlsx sibling exists, re-run xlsx so xlsx-wins
                // remains the invariant after a CSV save.
                std::filesystem::path sibling = p;
                sibling.replace_extension(".xlsx");
                if (std::filesystem::exists(sibling))
                {
                    XlsxImporter::CompileOne(sibling);
                }
            }
        };
        auto onDataDelete = [](const std::filesystem::path& p) {
            const std::string ext = [&p] {
                std::string e = p.extension().string();
                std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) {
                    return static_cast<char>(std::tolower(c));
                });
                return e;
            }();
            if (ext == ".xlsx")
            {
                XlsxImporter::DeleteGeneratedFor(p);
                // If a .csv sibling still exists, it was previously
                // overshadowed by xlsx; rebuild the csv product so the
                // table doesn't disappear from the AssetRegistry.
                std::filesystem::path sibling = p;
                sibling.replace_extension(".csv");
                if (std::filesystem::exists(sibling))
                {
                    DataTableImporter::CompileOne(sibling);
                }
            }
            else
            {
                DataTableImporter::DeleteGeneratedFor(p);
            }
        };
        m_DataWatcher.SetOnFileCreated(onDataChange);
        m_DataWatcher.SetOnFileChanged(onDataChange);
        m_DataWatcher.SetOnFileDeleted(onDataDelete);
        m_DataWatcher.WatchDirectory(data_root);
    }

    // PR-AI2: <Project>/Shaders/ live-recompile watcher.
    //
    // Mirrors the Data/ watcher block above. Filtered to source extensions
    // only -- compiled DXIL/SPIRV products do not live under Shaders/, they
    // live under Intermediate/Shaders/ where this watcher does not look.
    //
    // Routing: every event (create / change / delete) goes through
    // queueShaderInvalidation, which stamps a 200 ms debounce deadline.
    // FlushPendingShaderInvalidations() in TickWatcher() then drops the
    // matching DX12 cache entries when the deadline elapses. Delete is
    // also routed through the same path -- nuking cache entries for a
    // file that no longer exists is harmless and keeps the dispatch
    // table single-rule.
    {
        const std::shared_ptr<ProjectInfo> proj = projectInfo;
        std::filesystem::path shaders_root = proj ? proj->GetShadersRoot() : std::filesystem::path {};
        if (!shaders_root.empty())
        {
            std::error_code ec;
            std::filesystem::create_directories(shaders_root, ec);

            // Source extensions we know how to invalidate. Lowercase only;
            // FileSystemWatcher::ExtensionAllowed already lowercases the
            // filename's extension before comparing.
            //
            // - .hlsl       : direct DX12 source
            // - .shader     : ShaderLab front-end (ShaderLabCompiler) which
            //                 emits SPIR-V; on the Windows host this only
            //                 matters for the standalone unit test today,
            //                 but we still surface a watcher event so a
            //                 future ShaderLab-DX12 wiring picks it up
            //                 without re-touching this file.
            // - .compute    : DX12 compute shader source
            // - .raytrace   : DX12 raytracing shader source
            // - .hlsli      : header-only includes; not compiled directly
            //                 but we still listen so a future
            //                 include->referencer reverse index can
            //                 piggyback. Today the recursive include
            //                 mtime scan in dx12_shader_compiler already
            //                 catches stale caches when the user edits
            //                 the dependent .hlsl, so this is a safety
            //                 net rather than a correctness must-have.
            m_ShadersWatcher.SetExtensionFilter({".hlsl", ".hlsli", ".shader", ".compute", ".raytrace"});
            auto onShaderEvent = [this](const std::filesystem::path& p) {
                QueueShaderInvalidation(p);
            };
            m_ShadersWatcher.SetOnFileCreated(onShaderEvent);
            m_ShadersWatcher.SetOnFileChanged(onShaderEvent);
            m_ShadersWatcher.SetOnFileDeleted(onShaderEvent);
            m_ShadersWatcher.WatchDirectory(shaders_root);
        }
    }

    // PR-AI3: AutoReimport setup. Two pieces of wiring:
    //
    //   1. Bring up SourceAssetRegistry by reading
    //      <Project>/AssetRegistry/source_registry.json. A missing file
    //      is the cold-start case (handled internally) -- we'll start
    //      writing entries as soon as the user runs an import. A parse
    //      error is non-fatal: registry initialises empty, AutoReimport
    //      is just inactive until the next import re-populates it.
    //
    //   2. Subscribe to GLFW's window-focus callback via
    //      WindowSystem::registerOnWindowFocusFunc. The handler is a
    //      capture-by-this lambda; safe because EditorAssetManager
    //      lives until Shutdown(), which runs AFTER WindowSystem's own
    //      Shutdown() per the SystemRegistry teardown order.
    //
    //      We deliberately do NOT inspect `focused` here -- a
    //      focus-loss event is harmless to handle (we just trigger a
    //      no-op scan of mtime entries since nothing changed yet).
    //      Filtering out focus-loss events is done inside
    //      onEditorFocusGained itself.
    m_SourceRegistry.Initialize();
    m_TextureImportSettingsRegistry.Initialize();

    if (auto window_system = GET_SYSTEM(WindowSystem))
    {
        window_system->registerOnWindowFocusFunc([this](int focused) {
            if (focused != 0)
            {
                OnEditorFocusGained();
            }
        });
    }

    return true;
}

std::vector<AssetIndexEntry> EditorAssetManager::FindAssets(const std::string& filter) const
{
    return m_AssetRegistry.FindAssets(filter);
}

std::vector<std::filesystem::path>
EditorAssetManager::GetAssetsByType(const std::string& asset_type, const std::filesystem::path& search_root) const
{
    // Try the indexed lookup first. This is the fast path; the registry's
    // by-type reverse index turns this into an O(matches) hash hit instead
    // of N file-header reads.
    std::vector<std::filesystem::path> indexed = m_AssetRegistry.GetAssetsByTypeAbsolute(asset_type);

    if (!indexed.empty() && !search_root.empty())
    {
        // The registry was scanned at the project root (m_WorkingDir),
        // which is a SUPERSET of any per-call search_root passed in by
        // callers (e.g. inspector limits to ProjectContent/, render limits
        // to GetProjectAssetRoot()). Filter to entries actually under
        // search_root so we don't surface engine/editor-internal .zassets
        // that the original directory walk would never have returned.
        std::error_code ec;
        std::filesystem::path canonical_root = std::filesystem::weakly_canonical(search_root, ec);
        if (ec)
        {
            canonical_root = search_root;
        }
        const std::string root_str = canonical_root.generic_string();

        std::vector<std::filesystem::path> filtered;
        filtered.reserve(indexed.size());
        for (auto& abs_path : indexed)
        {
            std::error_code inner_ec;
            std::filesystem::path canonical_path = std::filesystem::weakly_canonical(abs_path, inner_ec);
            if (inner_ec)
            {
                canonical_path = abs_path;
            }
            const std::string path_str = canonical_path.generic_string();
            if (path_str.size() >= root_str.size() && path_str.compare(0, root_str.size(), root_str) == 0)
            {
                filtered.push_back(std::move(abs_path));
            }
        }
        return filtered;
    }

    // Registry is still warming up (scan in flight on first frames after
    // project open) or asset_type isn't tracked -- fall back to the base
    // class's directory walk. Same observable result as the legacy code,
    // just costs a header read per .zasset for this single call.
    return AssetManager::GetAssetsByType(asset_type, search_root);
}

void EditorAssetManager::GetAssetGuidAndType(const std::filesystem::path& asset_path,
                                             std::string& outGuid,
                                             std::string& outAssetType) const
{
    outGuid.clear();
    outAssetType.clear();
    if (asset_path.empty())
    {
        return;
    }

    const std::filesystem::path scan_root = m_AssetRegistry.GetScanRoot();
    std::string rel_key = asset_path.generic_string();
    if (!scan_root.empty())
    {
        std::error_code ec;
        if (asset_path.is_absolute())
        {
            auto rel = std::filesystem::relative(asset_path, scan_root, ec);
            if (!ec && !rel.empty())
            {
                rel_key = rel.generic_string();
            }
        }
    }

    if (const std::optional<AssetIndexEntry> index = m_AssetRegistry.GetAssetIndex(rel_key))
    {
        outGuid = index->guid;
        outAssetType = index->asset_type;
        if (!outGuid.empty() || !outAssetType.empty())
        {
            return;
        }
    }

    AssetManager::GetAssetGuidAndType(asset_path, outGuid, outAssetType);
}

bool EditorAssetManager::TryGetPathForGuid(const std::string& guid, std::filesystem::path& outPath) const
{
    outPath.clear();
    if (guid.empty())
    {
        return false;
    }

    const std::optional<AssetIndexEntry> index = m_AssetRegistry.GetAssetIndexByGUID(guid);
    if (!index.has_value() || index->asset_path.empty())
    {
        return false;
    }

    const std::filesystem::path scan_root = m_AssetRegistry.GetScanRoot();
    if (!scan_root.empty())
    {
        outPath = scan_root / index->asset_path;
    }
    else
    {
        outPath = index->asset_path;
    }
    return true;
}

bool EditorAssetManager::DeleteAsset(const std::string& asset_path)
{
    const std::filesystem::path abs_path = ToAbsoluteZassetPath(asset_path);
    if (abs_path.empty())
    {
        return false;
    }
    if (abs_path.extension() != ".zasset")
    {
        LOG_WARNING(ZAsset, "DeleteAsset: not a .zasset path: {}", abs_path.generic_string());
        return false;
    }

    std::error_code exists_ec;
    if (!std::filesystem::exists(abs_path, exists_ec) || exists_ec)
    {
        LOG_WARNING(ZAsset, "DeleteAsset: file not found: {}", abs_path.generic_string());
        return false;
    }

    const std::vector<std::string> referencers = GetReferencers(asset_path);
    if (!referencers.empty())
    {
        LOG_WARNING(ZAsset,
                    "DeleteAsset blocked: '{}' is referenced by {} asset(s)",
                    abs_path.generic_string(),
                    referencers.size());
        for (const std::string& ref_path : referencers)
        {
            LOG_WARNING(ZAsset, "  referencer: {}", ref_path);
        }
        return false;
    }

    if (auto asset_manager = GET_SYSTEM(AssetManager))
    {
        asset_manager->UnloadStream(abs_path);
    }

    std::error_code remove_ec;
    if (!std::filesystem::remove(abs_path, remove_ec) || remove_ec)
    {
        LOG_ERROR(ZAsset,
                  "DeleteAsset: failed to remove {} ({})",
                  abs_path.generic_string(),
                  remove_ec ? remove_ec.message() : "unknown error");
        return false;
    }

    // Registry + dependency graph (RemoveDependencyEdgesLocked inside).
    // FileSystemWatcher will also fire OnFileDeleted; this call is idempotent.
    m_AssetRegistry.RemoveAsset(asset_path);
    m_SourceRegistry.RemoveEntry(abs_path);
    m_TextureImportSettingsRegistry.RemoveEntry(abs_path);

    LOG_INFO(ZAsset, "DeleteAsset: removed {}", abs_path.generic_string());
    return true;
}

bool EditorAssetManager::MoveAsset(const std::string& old_path, const std::string& new_path)
{
    const std::filesystem::path old_abs = ToAbsoluteZassetPath(old_path);
    const std::filesystem::path new_abs = ToAbsoluteZassetPath(new_path);
    if (old_abs.empty() || new_abs.empty())
    {
        return false;
    }
    if (old_abs.extension() != ".zasset" || new_abs.extension() != ".zasset")
    {
        LOG_WARNING(ZAsset,
                    "MoveAsset: both paths must be .zasset ({} -> {})",
                    old_abs.generic_string(),
                    new_abs.generic_string());
        return false;
    }

    std::error_code exists_ec;
    if (!std::filesystem::exists(old_abs, exists_ec) || exists_ec)
    {
        LOG_WARNING(ZAsset, "MoveAsset: source not found: {}", old_abs.generic_string());
        return false;
    }
    if (std::filesystem::exists(new_abs, exists_ec) && !exists_ec)
    {
        LOG_WARNING(ZAsset, "MoveAsset: destination already exists: {}", new_abs.generic_string());
        return false;
    }

    if (auto asset_manager = GET_SYSTEM(AssetManager))
    {
        asset_manager->UnloadStream(old_abs);
    }

    std::error_code rename_ec;
    std::filesystem::rename(old_abs, new_abs, rename_ec);
    if (rename_ec)
    {
        LOG_ERROR(ZAsset,
                  "MoveAsset: rename failed {} -> {} ({})",
                  old_abs.generic_string(),
                  new_abs.generic_string(),
                  rename_ec.message());
        return false;
    }

    if (!m_AssetRegistry.RenameAsset(old_abs.generic_string(), new_abs.generic_string()))
    {
        LOG_ERROR(ZAsset,
                  "MoveAsset: registry rename failed {} -> {}",
                  old_abs.generic_string(),
                  new_abs.generic_string());
        std::error_code rollback_ec;
        std::filesystem::rename(new_abs, old_abs, rollback_ec);
        if (rollback_ec)
        {
            LOG_ERROR(ZAsset,
                      "MoveAsset: rollback rename failed {} -> {} ({})",
                      new_abs.generic_string(),
                      old_abs.generic_string(),
                      rollback_ec.message());
        }
        return false;
    }

    m_SourceRegistry.RemoveEntry(old_abs);
    m_TextureImportSettingsRegistry.RemoveEntry(old_abs);

    LOG_INFO(ZAsset, "MoveAsset: {} -> {}", old_abs.generic_string(), new_abs.generic_string());
    return true;
}
std::vector<std::string> EditorAssetManager::GetDependencies(const std::string& asset_path) const
{
    return m_AssetRegistry.GetDependencies(asset_path);
}

std::vector<std::string> EditorAssetManager::GetReferencers(const std::string& asset_path) const
{
    return m_AssetRegistry.GetReferencers(asset_path);
}
void EditorAssetManager::RefreshAssets() {}
void EditorAssetManager::RefreshAsset(const std::string& asset_path)
{
    // Public single-asset refresh. Routes straight to the registry; the
    // registry handles abs/rel path normalisation internally.
    m_AssetRegistry.RefreshAsset(asset_path);
}

void EditorAssetManager::Shutdown()
{
    // Stop receiving events first, otherwise the destructor of m_FileWatcher
    // would still be racing with onFileXxx callbacks against a half-torn-down
    // registry. StopWatching() also joins the watcher thread.
    m_FileWatcher.StopWatching();

    // Same teardown ordering for the secondary Data/ watcher. Stop it
    // before the registry shutdown so any in-flight DataTableImporter
    // recompile finishes against a still-valid ObjectManager / AssetManager.
    m_DataWatcher.StopWatching();

    // PR-AI2: stop Shaders/ watcher. We do NOT force-flush pending
    // invalidations here -- any entries still inside the 200 ms debounce
    // window will be picked up next launch by the cache's own mtime
    // freshness Check (last_write_time(cache) < last_write_time(src)
    // -> miss, recompile). FlushPendingShaderInvalidations() below is
    // a deadline-respecting drain; it's a no-op for entries still in
    // the window, which is the correct behaviour at shutdown.
    m_ShadersWatcher.StopWatching();
    FlushPendingShaderInvalidations();

    // PR-AI3: persist any in-flight registry edits and clear in-memory
    // state. Edits during the session already saved on every record /
    // removeEntry call, so this is mostly a defensive flush. Drop the
    // pending reimport queue too -- on next launch we'll re-stat
    // everything via the focus callback anyway.
    m_SourceRegistry.Shutdown();
    m_TextureImportSettingsRegistry.Shutdown();
    {
        std::lock_guard<std::mutex> lk(m_ReimportMutex);
        m_PendingReimports.clear();
        m_ReimportPausedUntilFocus = false;
    }

    // Wait for the in-flight async scan to finish so SaveCache() observes
    // a fully-populated map. Without this an editor that quits during
    // the first scan would persist a cache containing only N% of the
    // assets, and the next launch would silently treat the missing
    // entries as "removed by the user".
    m_AssetRegistry.WaitForScanComplete();

    const std::filesystem::path cache_path = resolveAssetRegistryCachePath();
    if (!cache_path.empty())
    {
        m_AssetRegistry.SaveCache(cache_path);
    }
}

void EditorAssetManager::TickWatcher()
{
    // Drains the watcher's event queues on the main thread. Each pop fires
    // exactly one of onFileCreated/Changed/Deleted, which in turn talks
    // to AssetRegistry::refreshAsset/removeAsset.
    m_FileWatcher.Update();

    // P5: drain the Data/ watcher in the same tick. Order matters:
    // m_DataWatcher's handlers WRITE .zassets under Assets/_Generated/Data/,
    // and m_FileWatcher above is the one that picks those writes up and
    // updates the AssetRegistry. By draining data_watcher AFTER file_watcher
    // we ensure that, on the rare frame where both fire for related events,
    // the registry sees the fresh .zasset on the next tick instead of
    // double-pumping in the same frame -- which would fight whatever
    // ReadObject the inspector might be holding.
    m_DataWatcher.Update();

    // PR-AI2: drain Shaders/ watcher into m_PendingShaderInvalidations,
    // then act on entries whose 200 ms debounce window has elapsed.
    // The drain step (Update()) only enqueues; the act step
    // (FlushPendingShaderInvalidations()) is what calls into
    // DX12ShaderCompiler. Splitting these two means a save burst (VS
    // Code's "Save All") that lands several events inside one frame all
    // collapse into a single invalidation per source file.
    m_ShadersWatcher.Update();
    FlushPendingShaderInvalidations();

    // PR-AI3: drain up to kReimportsPerFrame entries from the AutoReimport
    // queue. Each drain may write new bytes into a `.zasset` under
    // <Project>/Assets/, which the m_FileWatcher above already drained
    // for THIS frame -- so the AssetRegistry refresh will land on the
    // NEXT frame. That one-frame delay is intentional and matches the
    // ordering rationale documented in the data-watcher block above:
    // we don't want to trip the Inspector's ReadObject in the same frame
    // a reimport rewrites the bytes underneath it.
    TickReimportQueue();

    if (auto shader_registry = GET_SYSTEM(ShaderRegistry))
    {
        shader_registry->TickDeferredSave();
    }
    if (auto script_registry = GET_SYSTEM(ScriptRegistry))
    {
        script_registry->TickDeferredSave();
    }

    // Deliver registry Added/Removed/Updated events on the main thread
    // (async scan and FileSystemWatcher handlers only enqueue).
    m_AssetRegistry.DispatchPendingAssetChanges();
}

AssetRegistryUpdatedHandle EditorAssetManager::RegisterOnAssetUpdated(AssetRegistryUpdatedCallback callback)
{
    return m_AssetRegistry.RegisterOnAssetUpdated(std::move(callback));
}

void EditorAssetManager::UnregisterOnAssetUpdated(AssetRegistryUpdatedHandle handle)
{
    m_AssetRegistry.UnregisterOnAssetUpdated(handle);
}

void EditorAssetManager::OnFileCreated(const std::filesystem::path& path)
{
    // The watcher only fires for paths that pass its extension filter
    // (default {".zasset"} -- see AGENTS.md 2.6). New file: index it now
    // so the next Project-window refresh / inspector dropdown sees it.
    m_AssetRegistry.RefreshAsset(path.generic_string());
}

void EditorAssetManager::OnFileChanged(const std::filesystem::path& path)
{
    // FileSystemWatcher coalesces the post-rename "MODIFIED" event back to
    // changed; we just re-scan. If the asset_type changed (rare: someone
    // overwrote an existing .zasset with a different type), upsertEntryLocked
    // will move it between by-type buckets correctly.
    m_AssetRegistry.RefreshAsset(path.generic_string());
}

void EditorAssetManager::OnFileDeleted(const std::filesystem::path& path)
{
    m_AssetRegistry.RemoveAsset(path.generic_string());

    // PR-AI3: a `.zasset` was deleted from disk -- whether by the user,
    // the editor itself (PR-PW2 deletion), or sync tooling. The
    // AutoReimport entry for it is now meaningless; drop it so the
    // next focus tick doesn't try to reimport into a non-existent
    // target file. removeEntry is a no-op if no entry exists.
    m_SourceRegistry.RemoveEntry(path);
    m_TextureImportSettingsRegistry.RemoveEntry(path);
}

// ---------------------------------------------------------------------------
// PR-AI2: Shader live-recompile (debounced)
// ---------------------------------------------------------------------------

void EditorAssetManager::QueueShaderInvalidation(const std::filesystem::path& path)
{
    // Debounce window. 200 ms is half UE's (FAutoReimportManager uses
    // 500 ms) and matches Unity's ImportingAssets coalesce window. It
    // covers VS Code's "write temp + rename" save sequence on every
    // platform we've measured. Tunable by changing this single constant.
    constexpr auto kDebounceWindow = std::chrono::milliseconds(200);

    // Use generic_string to keep the map key stable across the
    // create / change pair the watcher can deliver back-to-back for a
    // single user save (the OS and the watcher backend may produce
    // either a `\` or `/` separator depending on path source).
    const std::string key = path.generic_string();
    const auto deadline = std::chrono::steady_clock::now() + kDebounceWindow;

    std::lock_guard<std::mutex> lock(m_ShaderDebounceMutex);
    // upsert: if a deadline already exists, push it forward. This is the
    // standard "debounce" semantic -- as long as edits keep arriving,
    // the actual invalidation keeps getting deferred. It will fire only
    // after kDebounceWindow of quiet.
    m_PendingShaderInvalidations[key] = deadline;
}

void EditorAssetManager::FlushPendingShaderInvalidations()
{
    if (m_PendingShaderInvalidations.empty())
    {
        return;
    }

    const auto now = std::chrono::steady_clock::now();

    // Move the elapsed entries out under the lock, then act on them
    // outside the lock. Same pattern as ProjectWindow's drop-import
    // drain (PR-AI1): we never want to hold a debounce mutex while
    // calling into the DX12 compiler in case it ever grew a callback
    // that re-entered queueShaderInvalidation.
    std::vector<std::filesystem::path> due;
    {
        std::lock_guard<std::mutex> lock(m_ShaderDebounceMutex);
        due.reserve(m_PendingShaderInvalidations.size());
        for (auto it = m_PendingShaderInvalidations.begin();
             it != m_PendingShaderInvalidations.end();)
        {
            if (it->second <= now)
            {
                due.emplace_back(it->first);
                it = m_PendingShaderInvalidations.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    if (due.empty())
    {
        return;
    }

    if (auto shader_registry = GET_SYSTEM(ShaderRegistry))
    {
        for (const auto& src : due)
        {
            std::string ext = src.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (ext == ".shader")
            {
                shader_registry->OnShaderFileEvent(src);
                Runtime::ShaderImporter::PrecompileShaderVariants(src);
            }
        }
    }

#ifdef _WIN32
    int total_invalidated = 0;
    for (const auto& src : due)
    {
        // .hlsli (header) edits: there's no cache slot keyed on the
        // header itself -- the cache key is `<top_level_src_hash>`.
        // For now we still pass the .hlsli through: invalidateCacheForSource
        // will compute a hash that matches no slot and return 0, which
        // is a no-op. Future work: maintain a reverse "header -> top
        // shader" index here and invalidate every dependent .hlsl. For
        // safety we already have the recursive include-mtime check in
        // dx12_shader_compiler that catches stale caches at the next
        // compile, so missing this reverse-index for now means at most
        // a one-frame delay until the user re-touches the dependent
        // .hlsl, not stale binaries.
        const int n = DX12ShaderCompiler::InvalidateCacheForSource(src);
        total_invalidated += n;
    }
    if (total_invalidated > 0)
    {
        LOG_INFO(ZShader,
                 "PR-AI2: shader watcher invalidated {} cache entr{} across {} source file(s)",
                 total_invalidated,
                 total_invalidated == 1 ? "y" : "ies",
                 due.size());
    }
#else
    // Non-Windows hosts (macOS / Linux dev builds): no DX12 compiler
    // available, so we just log the events. When/if the host gains a
    // Vulkan or Metal recompile path, route here.
    LOG_INFO(ZShader,
             "PR-AI2: shader watcher saw {} source change(s); no DX12 compiler on this host",
             due.size());
#endif
}

// ---------------------------------------------------------------------------
// PR-AI3: AutoReimport
// ---------------------------------------------------------------------------

void EditorAssetManager::RecordImportSource(const std::filesystem::path& zasset_abs_path,
                                            const std::filesystem::path& source_abs_path)
{
    // Thin wrapper so AssetsMenu / ProjectWindow don't need to learn the
    // SourceAssetRegistry type. This also gives us a single funnel for
    // future "did the importer succeed?" checks if we ever want to gate
    // the registry write on import success.
    m_SourceRegistry.Record(zasset_abs_path, source_abs_path);
}

bool EditorAssetManager::importSourceAsset(const std::filesystem::path& source_path,
                                           const std::filesystem::path& output_path,
                                           const AssetImporterSettings* import_settings)
{
    if (source_path.empty() || output_path.empty())
    {
        return false;
    }

    if (!m_ImportManager.ImportAsset(source_path, output_path, import_settings))
    {
        return false;
    }

    RecordImportSource(output_path, source_path);

    if (auto importer = m_ImportManager.FindImporter(source_path))
    {
        if (std::dynamic_pointer_cast<TextureImporter>(importer) != nullptr)
        {
            const std::filesystem::path zasset_abs = ToAbsoluteZassetPath(output_path);
            TextureImporterSettings stored;
            if (const auto* texture_settings = dynamic_cast<const TextureImporterSettings*>(import_settings))
            {
                stored = *texture_settings;
            }
            else if (auto defaults = importer->GetDefaultSettings())
            {
                if (const auto* texture_defaults = dynamic_cast<const TextureImporterSettings*>(defaults.get()))
                {
                    stored = *texture_defaults;
                }
            }
            m_TextureImportSettingsRegistry.Set(zasset_abs, stored);
        }
    }

    m_AssetRegistry.RefreshAsset(output_path.generic_string());
    return true;
}

bool EditorAssetManager::reimportAsset(const std::string& asset_path,
                                       const AssetImporterSettings* import_settings)
{
    if (asset_path.empty())
    {
        return false;
    }

    std::filesystem::path zasset_path(asset_path);
    std::error_code ec;
    if (!zasset_path.is_absolute())
    {
        const std::shared_ptr<ProjectInfo> project_info = GET_SYSTEM(ProjectInfo);
        if (project_info == nullptr)
        {
            LOG_WARNING(ZAsset, "reimportAsset: no project loaded");
            return false;
        }
        const std::filesystem::path content_root = project_info->GetProjectContent();
        if (content_root.empty())
        {
            return false;
        }
        zasset_path = std::filesystem::absolute(content_root / zasset_path, ec);
    }

    if (!std::filesystem::exists(zasset_path, ec) || ec)
    {
        LOG_WARNING(ZAsset,
                    "reimportAsset: .zasset not found '{}'",
                    zasset_path.generic_string());
        return false;
    }

    return ReimportZassetWithRegistrySource(zasset_path, import_settings);
}

bool EditorAssetManager::ReimportZassetWithRegistrySource(
    const std::filesystem::path& zasset_abs_path,
    const AssetImporterSettings* import_settings)
{
    std::filesystem::path source_path;
    if (const auto entry_opt = m_SourceRegistry.Lookup(zasset_abs_path); entry_opt.has_value())
    {
        source_path = std::filesystem::path(entry_opt->source_path);
    }
    else if (const auto discovered = TryDiscoverMeshSourceForReimport(zasset_abs_path); discovered.has_value())
    {
        source_path = discovered.value();
        RecordImportSource(zasset_abs_path, source_path);
        LOG_INFO(ZAsset,
                 "reimportAsset: discovered mesh source '{}' for '{}' (registry seeded)",
                 source_path.generic_string(),
                 zasset_abs_path.generic_string());
    }
    else
    {
        LOG_WARNING(ZAsset,
                    "reimportAsset: no SourceAssetRegistry entry for '{}' "
                    "(import once via Assets menu or OS drop to enable reimport)",
                    zasset_abs_path.generic_string());
        return false;
    }

    std::error_code source_ec;
    if (!std::filesystem::exists(source_path, source_ec) || source_ec)
    {
        LOG_WARNING(ZAsset,
                    "reimportAsset: source file missing for '{}' (was '{}')",
                    zasset_abs_path.generic_string(),
                    source_path.generic_string());
        return false;
    }

    auto importer = m_ImportManager.FindImporter(source_path);
    if (importer == nullptr)
    {
        LOG_WARNING(ZAsset,
                    "reimportAsset: no importer for source '{}' (zasset='{}')",
                    source_path.generic_string(),
                    zasset_abs_path.generic_string());
        return false;
    }

    std::unique_ptr<AssetImporterSettings> resolved_settings =
        ResolveImporterSettingsForReimport(zasset_abs_path, source_path, importer, import_settings);
    const AssetImporterSettings* settings_ref = import_settings;
    if (resolved_settings != nullptr)
    {
        settings_ref = resolved_settings.get();
    }
    else if (settings_ref == nullptr)
    {
        resolved_settings = importer->GetDefaultSettings();
        if (!resolved_settings)
        {
            return false;
        }
        settings_ref = resolved_settings.get();
    }

    if (!importer->Reimport(zasset_abs_path, source_path, *settings_ref))
    {
        LOG_WARNING(ZAsset,
                    "reimportAsset: importer returned false for '{}' (source='{}')",
                    zasset_abs_path.generic_string(),
                    source_path.generic_string());
        return false;
    }

    m_AssetRegistry.RefreshAsset(zasset_abs_path.generic_string());

    SourceAssetRegistry::Entry mtime_probe;
    mtime_probe.source_path = source_path.generic_string();
    int64_t new_mtime = 0;
    (void)SourceAssetRegistry::HasSourceChanged(mtime_probe, &new_mtime);
    if (new_mtime != 0)
    {
        m_SourceRegistry.UpdateMtime(zasset_abs_path, new_mtime);
    }

    MeshDataPreview::InvalidatePreview(zasset_abs_path);

    LOG_INFO(ZAsset,
             "reimportAsset: ok '{}' <- '{}'",
             zasset_abs_path.generic_string(),
             source_path.generic_string());
    return true;
}

void EditorAssetManager::OnEditorFocusGained()
{
    // The focus callback fires for every focus-gained event, including
    // legitimate user actions like clicking on a different ImGui window
    // inside the editor (which can pulse focus on some platforms).
    // m_ReimportPausedUntilFocus is the gate that says "we already
    // scanned and started draining; don't re-scan until that drain
    // finishes". Without it, a user holding Alt-Tab between Photoshop
    // and the editor would scan + re-stat every entry every focus
    // cycle, redundantly enqueueing the same paths.
    {
        std::lock_guard<std::mutex> lk(m_ReimportMutex);
        if (m_ReimportPausedUntilFocus)
        {
            // Drain in progress -- the queue itself will clear the
            // pause flag when it empties; until then, ignore focus
            // events.
            return;
        }
    }

    // Walk every recorded entry under SourceAssetRegistry's lock,
    // collect the changed ones into a local vector, then push them
    // onto the queue under m_ReimportMutex. Two-stage to avoid any
    // possibility of holding both locks at once.
    std::vector<std::filesystem::path> newly_changed;
    m_SourceRegistry.ForEach(
        [&newly_changed](const std::filesystem::path& zasset,
                         const SourceAssetRegistry::Entry& entry) {
            int64_t live_mtime = 0;
            if (SourceAssetRegistry::HasSourceChanged(entry, &live_mtime))
            {
                newly_changed.emplace_back(zasset);
            }
            else if (live_mtime == 0)
            {
                // Source file disappeared. Log a single warning per
                // missing source (we can't dedupe cross-frame without
                // a cache, but this only fires once per focus event).
                // We deliberately do NOT remove the registry entry --
                // the user might just have unplugged the external
                // drive holding the source; on next focus with the
                // drive back, autoreimport works again with no manual
                // intervention. This matches UE's "Locate file"
                // dialog behaviour, simplified to a log line.
                LOG_WARNING(ZAsset,
                            "AutoReimport: source file missing for '{}' (was '{}'), reimport skipped",
                            zasset.generic_string(),
                            entry.source_path);
            }
        });

    if (newly_changed.empty())
    {
        return;
    }

    {
        std::lock_guard<std::mutex> lk(m_ReimportMutex);
        for (auto& z : newly_changed)
        {
            m_PendingReimports.emplace_back(std::move(z));
        }
        m_ReimportPausedUntilFocus = true;
    }
    LOG_INFO(ZAsset,
             "AutoReimport: focus tick queued {} asset(s) for Reimport (cap={}/frame)",
             newly_changed.size(),
             kReimportsPerFrame);
}

void EditorAssetManager::TickReimportQueue()
{
    // Cheap fast path so per-frame TickWatcher cost is negligible when
    // nothing's queued (the common case).
    {
        std::lock_guard<std::mutex> lk(m_ReimportMutex);
        if (m_PendingReimports.empty())
        {
            // Queue drained -- arm the next focus event to re-scan.
            m_ReimportPausedUntilFocus = false;
            return;
        }
    }

    // Pop up to kReimportsPerFrame entries under the lock, then act on
    // them outside it. Same lock-discipline pattern as
    // flushPendingShaderInvalidations / executePendingOsDropImports.
    std::vector<std::filesystem::path> batch;
    {
        std::lock_guard<std::mutex> lk(m_ReimportMutex);
        const int take = std::min(kReimportsPerFrame, static_cast<int>(m_PendingReimports.size()));
        batch.reserve(static_cast<size_t>(take));
        for (int i = 0; i < take; ++i)
        {
            batch.emplace_back(std::move(m_PendingReimports.front()));
            m_PendingReimports.pop_front();
        }
    }

    int reimported = 0;
    int failed = 0;
    int skipped = 0;
    for (const auto& zasset_path : batch)
    {
        if (!m_SourceRegistry.Lookup(zasset_path).has_value())
        {
            ++skipped;
            continue;
        }
        if (ReimportZassetWithRegistrySource(zasset_path, nullptr))
        {
            ++reimported;
        }
        else
        {
            ++failed;
        }
    }

    if (reimported + failed + skipped > 0)
    {
        LOG_INFO(ZAsset,
                 "AutoReimport: tick processed {} (ok={}, failed={}, skipped={}); {} remaining",
                 batch.size(),
                 reimported,
                 failed,
                 skipped,
                 [this] {
                     std::lock_guard<std::mutex> lk(m_ReimportMutex);
                     return m_PendingReimports.size();
                 }());
    }
}

void EditorAssetManager::AddSerializedSystem(std::string_view& className,
                                             std::filesystem::path& path,
                                             GUID guid,
                                             bool allowSerializedAsText)
{
    m_SerializedSystems.emplace_back(nullptr, path, className, guid, allowSerializedAsText);
}

int EditorAssetManager::InsertPathNameInternal(const std::filesystem::path& path, bool create)
{
    if (path.empty())
        return -1;

    const std::string path_name = path.lexically_normal().string();
    auto found = m_ConstantGUIDAssetPathToIndex.find(path_name);
    if (found != m_ConstantGUIDAssetPathToIndex.end())
        return found->second;

    if (create)
    {
        const int path_id = static_cast<int>(m_PathNames.size());
        m_ConstantGUIDAssetPathToIndex.emplace(path_name, path_id);
        m_PathNames.push_back(path_name);
        AddStream();
        return path_id;
    }

    return -1;
}

const std::string& EditorAssetManager::PathIDToPathNameInternal(int pathID) const
{
    static const std::string empty_path;
    if (pathID < 0 || pathID >= static_cast<int>(m_PathNames.size()))
    {
        return empty_path;
    }
    return m_PathNames[pathID];
}

std::filesystem::path EditorAssetManager::ToAbsoluteZassetPath(const std::filesystem::path& zasset_path)
{
    if (zasset_path.empty())
    {
        return {};
    }
    if (zasset_path.is_absolute())
    {
        return zasset_path;
    }
    const std::shared_ptr<ProjectInfo> project_info = GET_SYSTEM(ProjectInfo);
    if (project_info == nullptr)
    {
        return zasset_path;
    }
    const std::filesystem::path content_root = project_info->GetProjectContent();
    if (content_root.empty())
    {
        return zasset_path;
    }
    std::error_code ec;
    return std::filesystem::absolute(content_root / zasset_path, ec);
}

std::unique_ptr<AssetImporterSettings> EditorAssetManager::ResolveImporterSettingsForReimport(
    const std::filesystem::path& zasset_abs_path,
    const std::filesystem::path& source_path,
    const std::shared_ptr<AssetImporter>& importer,
    const AssetImporterSettings* caller_settings) const
{
    if (importer == nullptr)
    {
        return nullptr;
    }

    if (std::dynamic_pointer_cast<TextureImporter>(importer) == nullptr)
    {
        if (caller_settings != nullptr)
        {
            return nullptr;
        }
        return importer->GetDefaultSettings();
    }

    if (caller_settings != nullptr)
    {
        if (const auto* texture_settings = dynamic_cast<const TextureImporterSettings*>(caller_settings))
        {
            return std::make_unique<TextureImporterSettings>(*texture_settings);
        }
        return nullptr;
    }

    (void)source_path;
    const std::filesystem::path key_path = zasset_abs_path.empty() ? source_path : zasset_abs_path;
    if (const auto stored = m_TextureImportSettingsRegistry.Lookup(key_path))
    {
        return std::make_unique<TextureImporterSettings>(*stored);
    }
    return importer->GetDefaultSettings();
}

AssetBundle* EditorAssetManager::GetEditorAssetBundle()
{
    int instanceID = GetInstanceIDFromPathAndFileID(GET_SYSTEM(ConfigManager)->GetResourceFolder(), 1);
    PPtr<AssetBundle> res(instanceID);
    if (res.IsNull())
        LOG_FATAL(ZEditor, "Failed to load Editor resource file");
    return res;
}