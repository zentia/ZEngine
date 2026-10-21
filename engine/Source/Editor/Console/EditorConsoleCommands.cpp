#include "EditorConsoleCommands.h"

#include "Editor/AssetPipeline/TextureImporter/TextureImporter.h"
#include "Editor/AssetPipeline/TextureImporter/TextureImporterSettings.h"
#include "Editor/Build/AssetBundleBuilder.h"
#include "Editor/AssetRegistry/AssetRegistry.h"
#include "Editor/EditorApplication/EditorApplication.h"
#include "Editor/EditorAsset/EditorAssetManager.h"
// ZSlateInsightsWindow: deleted (Insights -> standalone ZInsights app).
#if defined(_WIN32)
    #include "Editor/RenderDoc/RenderDoc.h"
    #include "RenderDocAPI.h"
#endif
#include "Editor/Undo/EditorUndo.h"
#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Function/Console/ConsoleManager.h"
#include "Runtime/Function/Framework/Level/Level.h"
#include "Runtime/Function/Framework/World/WorldManager.h"
#include "Runtime/Function/Render/Pipeline/RenderPipelineSettings.h"
#include "Runtime/Profiler/InsightsTrace.h"
#include "Runtime/Project/ProjectInfo.h"
#include "Runtime/Resource/Asset/AssetManager.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace
{
    EditorAssetManager* GetEditorAssetManager()
    {
        return dynamic_cast<EditorAssetManager*>(GET_SYSTEM(AssetManager));
    }

    bool CmdObjList(const std::vector<std::string>&)
    {
        auto world = GET_SYSTEM(WorldManager);
        if (!world)
        {
            LOG_ERROR(ZConsole, "WorldManager not available");
            return false;
        }

        Level* level = world->getCurrentActiveLevel();
        if (!level)
        {
            LOG_WARNING(ZConsole, "No active level loaded");
            return true;
        }

        const auto& objects = level->getAllGObjects();
        LOG_INFO(ZConsole, "Level '{}' - {} object(s):", level->getLevelResUrl().c_str(), objects.size());
        for (const auto& [id, object] : objects)
        {
            if (!object)
            {
                continue;
            }
            LOG_INFO(ZConsole, "  [{}] {}", static_cast<uint64_t>(id), object->GetName().c_str());
        }
        return true;
    }

    bool CmdObj(const std::vector<std::string>& args)
    {
        if (args.empty() || args[0] == "list")
        {
            return CmdObjList(args);
        }
        LOG_ERROR(ZConsole, "Unknown obj subcommand '{}'. Usage: obj list", args[0]);
        return false;
    }

    bool CmdLevelReload(const std::vector<std::string>&)
    {
        auto world = GET_SYSTEM(WorldManager);
        if (!world)
        {
            LOG_ERROR(ZConsole, "WorldManager not available");
            return false;
        }
        world->ReloadCurrentLevel();
        LOG_INFO(ZConsole, "ReloadCurrentLevel requested");
        return true;
    }

    bool CmdLevelSave(const std::vector<std::string>&)
    {
        auto world = GET_SYSTEM(WorldManager);
        if (!world)
        {
            LOG_ERROR(ZConsole, "WorldManager not available");
            return false;
        }
        world->SaveCurrentLevel();
        LOG_INFO(ZConsole, "SaveCurrentLevel requested");
        return true;
    }

    bool CmdLevel(const std::vector<std::string>& args)
    {
        if (args.empty())
        {
            LOG_ERROR(ZConsole, "Usage: level reload | level save");
            return false;
        }
        if (args[0] == "reload")
        {
            return CmdLevelReload({});
        }
        if (args[0] == "save")
        {
            return CmdLevelSave({});
        }
        LOG_ERROR(ZConsole, "Unknown level subcommand '{}'", args[0]);
        return false;
    }

    bool CmdAssetFind(const std::vector<std::string>& args)
    {
        EditorAssetManager* asset_mgr = GetEditorAssetManager();
        if (!asset_mgr)
        {
            LOG_ERROR(ZConsole, "EditorAssetManager not available");
            return false;
        }
        if (args.empty())
        {
            LOG_ERROR(ZConsole, "Usage: asset.find <filter>");
            return false;
        }

        const std::vector<AssetIndexEntry> hits = asset_mgr->getAssetRegistry().FindAssets(args[0]);
        LOG_INFO(ZConsole, "asset.find '{}' -> {} hit(s)", args[0], hits.size());
        const size_t max_lines = 64;
        for (size_t i = 0; i < hits.size() && i < max_lines; ++i)
        {
            LOG_INFO(ZConsole, "  {} [{}] guid={}", hits[i].asset_path, hits[i].asset_type, hits[i].guid);
        }
        if (hits.size() > max_lines)
        {
            LOG_INFO(ZConsole, "  ... {} more", hits.size() - max_lines);
        }
        return true;
    }

    bool CmdAssetReimport(const std::vector<std::string>& args)
    {
        EditorAssetManager* asset_mgr = GetEditorAssetManager();
        if (!asset_mgr)
        {
            LOG_ERROR(ZConsole, "EditorAssetManager not available");
            return false;
        }
        if (args.empty())
        {
            LOG_ERROR(ZConsole, "Usage: asset.reimport <.zasset path>");
            return false;
        }

        std::filesystem::path zasset_path = args[0];
        if (!zasset_path.is_absolute())
        {
            auto project = GET_SYSTEM(ProjectInfo);
            if (project && !project->m_WorkingDir.empty())
            {
                zasset_path = std::filesystem::path(project->m_WorkingDir) / zasset_path;
            }
        }

        const bool ok = asset_mgr->reimportAsset(zasset_path.generic_string());
        if (!ok)
        {
            LOG_ERROR(ZConsole, "asset.reimport failed for '{}'", zasset_path.generic_string());
        }
        return ok;
    }

    bool CmdAssetCook(const std::vector<std::string>& args)
    {
        // Texture cook (Phase 6): asset.cook <platform>. Walks all project
        // textures, cooks the per-platform variant, and writes cooked .zasset
        // (source GUID preserved) to Intermediate/Cooked/<Platform>/.
        if (args.empty())
        {
            LOG_ERROR(ZConsole, "Usage: asset.cook <standalone|android|ios|ohos|webgl>");
            return false;
        }

        std::string platform = args[0];
        std::transform(platform.begin(), platform.end(), platform.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        TextureImporterSettings::BuildTarget target;
        if (platform == "standalone" || platform == "windows" || platform == "pc" || platform == "desktop")
        {
            target = TextureImporterSettings::BuildTarget::Standalone;
        }
        else if (platform == "android")
        {
            target = TextureImporterSettings::BuildTarget::Android;
        }
        else if (platform == "ios")
        {
            target = TextureImporterSettings::BuildTarget::iOS;
        }
        else if (platform == "webgl" || platform == "web")
        {
            target = TextureImporterSettings::BuildTarget::WebGL;
        }
        else
        {
            LOG_ERROR(ZConsole, "asset.cook: unknown platform '{}' (standalone|android|ios|ohos|webgl)", args[0]);
            return false;
        }

        const int cooked = TextureImporter::CookProjectTextures(target);
        LOG_INFO(ZConsole, "asset.cook {}: {} texture(s) cooked", platform, cooked);
        return true;
    }

    bool CmdAssetCount(const std::vector<std::string>&)
    {
        EditorAssetManager* asset_mgr = GetEditorAssetManager();
        if (!asset_mgr)
        {
            LOG_ERROR(ZConsole, "EditorAssetManager not available");
            return false;
        }

        AssetRegistryQueryFilter filter;
        const std::vector<AssetIndexEntry> all = asset_mgr->getAssetRegistry().FindAssets(filter);
        LOG_INFO(ZConsole, "AssetRegistry: {} asset(s), scan root '{}'",
                 all.size(),
                 asset_mgr->getAssetRegistry().GetScanRoot().generic_string());
        return true;
    }

    bool CmdPlay(const std::vector<std::string>&)
    {
        auto editor = GET_SYSTEM(Editor);
        if (!editor)
        {
            LOG_ERROR(ZConsole, "Editor not available");
            return false;
        }
        if (editor->isInEditMode())
        {
            editor->TogglePlayMode();
            LOG_INFO(ZConsole, "Play mode: {}", editor->GetPlaybackStateLabel());
        }
        else
        {
            LOG_INFO(ZConsole, "Already in play mode ({})", editor->GetPlaybackStateLabel());
        }
        return true;
    }

    bool CmdWpStatus(const std::vector<std::string>&)
    {
        auto world = GET_SYSTEM(WorldManager);
        if (!world)
        {
            LOG_ERROR(ZConsole, "WorldManager not available");
            return false;
        }

        const eastl::vector<eastl::string> urls = world->GetLoadedLevelUrls();
        LOG_INFO(ZConsole,
                 "World Partition: enabled={} async={} loaded_cells={} pending_cells={} loaded_levels={}",
                 world->IsWorldPartitionEnabled(),
                 world->GetWorldPartition().GetSettings().m_AsyncCellLoading,
                 world->GetLoadedWorldPartitionCellCount(),
                 world->GetPendingWorldPartitionCellCount(),
                 urls.size());
        return true;
    }

    bool CmdWpCells(const std::vector<std::string>&)
    {
        auto world = GET_SYSTEM(WorldManager);
        if (!world)
        {
            LOG_ERROR(ZConsole, "WorldManager not available");
            return false;
        }

        if (!world->IsWorldPartitionEnabled())
        {
            LOG_WARNING(ZConsole, "World Partition is disabled for the current world");
            return true;
        }

        const eastl::vector<eastl::string> urls = world->GetLoadedLevelUrls();
        LOG_INFO(ZConsole,
                 "World Partition: {} loaded cell(s), {} pending, {} level instance(s)",
                 world->GetLoadedWorldPartitionCellCount(),
                 world->GetPendingWorldPartitionCellCount(),
                 urls.size());
        for (const eastl::string& url : urls)
        {
            LOG_INFO(ZConsole, "  {}", url.c_str());
        }
        return true;
    }

    // Writes `Insights/trace_<timestamp>.ztrace` under the working directory.
    // These two used to be members of ZSlateInsightsWindow, which was deleted --
    // Insights is served by the standalone ZInsights app now, so `insights.dump`
    // owns the save + launch pair directly.
    std::string SaveInsightsTraceToDisk(const ZEngine::Insights::InsightsSnapshot& snapshot)
    {
        namespace fs = std::filesystem;

        std::error_code ec;
        const fs::path dir = fs::current_path(ec) / "Insights";
        fs::create_directories(dir, ec);

        const std::time_t now = std::time(nullptr);
        std::tm tm_buf {};
#ifdef _WIN32
        localtime_s(&tm_buf, &now);
#else
        localtime_r(&now, &tm_buf);
#endif
        char stamp[32];
        std::strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &tm_buf);

        const fs::path out_path = dir / (std::string("trace_") + stamp + ".ztrace");
        const std::string out_str = out_path.generic_string();

        if (!ZEngine::Insights::SaveTrace(out_str, snapshot))
        {
            LOG_ERROR(ZConsole, "Insights: failed to write trace to {}", out_str);
            return std::string();
        }
        LOG_INFO(ZConsole, "Insights: saved trace ({} tracks, {} frames) to {}", snapshot.tracks.size(),
                 snapshot.frame_starts.size(), out_str);
        return out_str;
    }

    void LaunchInsightsViewer(const std::string& trace_path)
    {
        if (trace_path.empty())
            return;
#ifdef _WIN32
        // ZInsights.exe sits next to ZEditor.exe in the same bin/ directory.
        wchar_t module_path[MAX_PATH] = {};
        if (GetModuleFileNameW(nullptr, module_path, MAX_PATH) == 0)
        {
            LOG_WARNING(ZConsole, "Insights: could not resolve module path to launch ZInsights");
            return;
        }
        std::filesystem::path viewer = std::filesystem::path(module_path).parent_path() / "ZInsights.exe";
        if (!std::filesystem::exists(viewer))
        {
            LOG_WARNING(ZConsole, "Insights: ZInsights.exe not found next to the editor ({}); trace saved only",
                        viewer.generic_string());
            return;
        }
        const std::wstring viewer_w = viewer.wstring();
        const std::wstring arg_w = L"\"" + std::filesystem::path(trace_path).wstring() + L"\"";
        const HINSTANCE rc =
            ShellExecuteW(nullptr, L"open", viewer_w.c_str(), arg_w.c_str(), nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(rc) <= 32)
        {
            LOG_WARNING(ZConsole, "Insights: ShellExecute failed to launch ZInsights.exe (code {})",
                        reinterpret_cast<INT_PTR>(rc));
        }
#else
        LOG_INFO(ZConsole, "Insights: trace saved to {} (standalone viewer is Windows-only)", trace_path);
#endif
    }

    bool CmdInsightsDump(const std::vector<std::string>& args)
    {
        ZEngine::Insights::InsightsSnapshot snapshot;
        ZEngine::Insights::InsightsTrace::Get().BuildSnapshot(snapshot);
        if (snapshot.tracks.empty())
        {
            LOG_WARNING(ZConsole,
                        "insights.dump: no captured data. Open the Insights panel first so capture turns on.");
        }

        const std::string path = SaveInsightsTraceToDisk(snapshot);
        if (path.empty())
        {
            return false;
        }
        // `insights.dump view` (or `open`) also launches the standalone viewer.
        if (!args.empty() && (args[0] == "view" || args[0] == "open"))
        {
            LaunchInsightsViewer(path);
        }
        return true;
    }

    bool CmdRenderPath(const std::vector<std::string>& args)
    {
        using RenderPipelineSettings::RenderPath;
        if (args.empty())
        {
            LOG_INFO(ZConsole,
                     "Render path: configured={}, effective={}. Usage: r.renderpath auto|desktop|mobile",
                     RenderPipelineSettings::ToString(RenderPipelineSettings::GetConfiguredPath()),
                     RenderPipelineSettings::ToString(RenderPipelineSettings::GetEffectivePath()));
            return true;
        }

        std::string mode = args[0];
        std::transform(mode.begin(), mode.end(), mode.begin(), [](unsigned char c) { return std::tolower(c); });

        RenderPath path = RenderPath::Auto;
        if (mode == "auto" || mode == "0")
        {
            path = RenderPath::Auto;
        }
        else if (mode == "desktop" || mode == "1")
        {
            path = RenderPath::Desktop;
        }
        else if (mode == "mobile" || mode == "2")
        {
            path = RenderPath::Mobile;
        }
        else
        {
            LOG_ERROR(ZConsole, "Unknown render path '{}'. Usage: r.renderpath auto|desktop|mobile", args[0]);
            return false;
        }

        RenderPipelineSettings::SetConfiguredPath(path);
        LOG_INFO(ZConsole,
                 "Render path -> {} (effective {})",
                 RenderPipelineSettings::ToString(path),
                 RenderPipelineSettings::ToString(RenderPipelineSettings::GetEffectivePath()));
        return true;
    }

    bool CmdPause(const std::vector<std::string>&)
    {
        auto editor = GET_SYSTEM(Editor);
        if (!editor)
        {
            LOG_ERROR(ZConsole, "Editor not available");
            return false;
        }
        if (editor->isPlaying() && !editor->isPaused())
        {
            editor->TogglePauseMode();
            LOG_INFO(ZConsole, "Playback: {}", editor->GetPlaybackStateLabel());
        }
        else
        {
            LOG_INFO(ZConsole, "Pause ignored (state: {})", editor->GetPlaybackStateLabel());
        }
        return true;
    }

    bool CmdRenderDocLoad(const std::vector<std::string>&)
    {
#if defined(_WIN32)
        if (!Runtime::RenderDoc::IsInstalled())
        {
            LOG_WARNING(ZConsole, "RenderDoc not found. Build tools/renderdoc or pass --renderdoc-dll <path>");
            return false;
        }

        Runtime::RenderDoc::Load();
        if (Runtime::RenderDoc::IsLoaded())
        {
            LOG_INFO(ZConsole, "RenderDoc loaded from {}", Z::RenderDocAPI::GetModulePath());
            return true;
        }

        LOG_ERROR(ZConsole, "RenderDoc failed to load from {}", Z::RenderDocAPI::GetModulePath());
        return false;
#else
        LOG_WARNING(ZConsole, "RenderDoc is only supported on Windows desktop builds");
        return false;
#endif
    }

    bool CmdRenderDocCapture(const std::vector<std::string>&)
    {
#if defined(_WIN32)
        Runtime::RenderDoc::RequestCapture();
        return Runtime::RenderDoc::IsSupported();
#else
        LOG_WARNING(ZConsole, "RenderDoc is only supported on Windows desktop builds");
        return false;
#endif
    }

    bool CmdRenderDocStatus(const std::vector<std::string>&)
    {
#if defined(_WIN32)
        LOG_INFO(ZConsole, "RenderDoc installed: {}", Runtime::RenderDoc::IsInstalled() ? "yes" : "no");
        LOG_INFO(ZConsole, "RenderDoc loaded: {}", Runtime::RenderDoc::IsLoaded() ? "yes" : "no");
        LOG_INFO(ZConsole, "RenderDoc supported (DX12): {}", Runtime::RenderDoc::IsSupported() ? "yes" : "no");
        if (Runtime::RenderDoc::IsInstalled())
        {
            LOG_INFO(ZConsole, "RenderDoc module path: {}", Z::RenderDocAPI::GetModulePath());
        }
        return true;
#else
        LOG_INFO(ZConsole, "RenderDoc is only supported on Windows desktop builds");
        return true;
#endif
    }

    bool CmdBundleBuild(const std::vector<std::string>& args)
    {
        // PR-PAK1: bundle.build [platform] [--force] -- dependency walk + cook
        // + .zbundle + manifest under <project>/Build/<Platform>/.
        //
        // --force is the escape hatch for "I don't believe the cache": it
        // re-cooks and rewrites everything. It is NOT needed for "I changed
        // something" -- changed inputs are detected by content hash.
        bool force = false;
        std::string platform;
        for (const std::string& arg : args)
        {
            if (arg == "--force" || arg == "-f")
            {
                force = true;
                continue;
            }
            if (platform.empty())
            {
                platform = arg;
            }
        }
        if (platform.empty())
        {
            platform = "standalone";
        }
        std::transform(platform.begin(), platform.end(), platform.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        TextureImporterSettings::BuildTarget target = TextureImporterSettings::BuildTarget::Standalone;
        if (platform == "android")
        {
            target = TextureImporterSettings::BuildTarget::Android;
        }
        else if (platform == "ios")
        {
            target = TextureImporterSettings::BuildTarget::iOS;
        }
        else if (platform == "webgl" || platform == "web")
        {
            target = TextureImporterSettings::BuildTarget::WebGL;
        }
        else if (platform == "ohos" || platform == "harmonyos")
        {
            target = TextureImporterSettings::BuildTarget::OHOS;
        }
        else if (platform != "standalone" && platform != "windows" && platform != "pc" && platform != "desktop")
        {
            LOG_ERROR(ZConsole, "Usage: bundle.build <standalone|android|ios|ohos|webgl>");
            return false;
        }

        AssetBundleBuildOptions options;
        options.target = target;
        options.force_rebuild = force;
        const AssetBundleBuildResult result = AssetBundleBuilder::Build(options);
        if (!result.succeeded)
        {
            LOG_ERROR(ZPak, "bundle.build failed: {}", result.error);
            return false;
        }

        LOG_INFO(ZPak,
                 "bundle.build: {} bundle(s) ({} rebuilt, {} skipped), {} asset(s), {} script(s), {} "
                 "cooked texture(s), {} bytes -> {}",
                 result.bundle_count,
                 result.rebuilt_bundle_count,
                 result.skipped_bundle_count,
                 result.asset_count,
                 result.script_count,
                 result.cooked_texture_count,
                 result.total_bytes,
                 result.output_dir.generic_string());
        return true;
    }

    // ---- undo / redo ---------------------------------------------------------------
    // Thin console front end for the editor undo system (Editor/Undo/EditorUndo.h).
    // Useful for scripted checks: `undo 3` followed by `redo 3` must restore the scene
    // exactly, and `undo.history` shows what is queued without opening the Edit menu.

    bool ParseStepCount(const std::string& text, size_t& out)
    {
        try
        {
            size_t consumed = 0;
            const unsigned long value = std::stoul(text, &consumed);
            if (consumed != text.size() || value == 0)
            {
                return false;
            }
            out = static_cast<size_t>(value);
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    bool CmdUndo(const std::vector<std::string>& args)
    {
        EditorUndoSystem* undo = EditorUndo::GetSystem();
        if (undo == nullptr)
        {
            LOG_ERROR(ZConsole, "Undo system not available");
            return false;
        }

        size_t count = 1;
        if (!args.empty() && !ParseStepCount(args[0], count))
        {
            LOG_ERROR(ZConsole, "Usage: undo [steps]");
            return false;
        }

        const std::string top_title = undo->GetCore().GetUndoTitle();
        const size_t done = count == 1 ? (undo->PerformUndo() ? 1u : 0u) : undo->PerformUndoMany(count);
        if (done == 0)
        {
            LOG_WARNING(ZConsole, "Nothing to undo ({} queued step(s))", undo->GetCore().GetUndoCount());
            return true;
        }

        LOG_INFO(ZConsole, "Undo: {} step(s) -- '{}'", done, count == 1 ? top_title.c_str() : undo->GetCore().GetUndoTitle().c_str());
        LOG_INFO(ZConsole, "Undo: {} step(s) left, {} redoable", undo->GetCore().GetUndoCount(),
                 undo->GetCore().GetRedoCount());
        return true;
    }

    bool CmdRedo(const std::vector<std::string>& args)
    {
        EditorUndoSystem* undo = EditorUndo::GetSystem();
        if (undo == nullptr)
        {
            LOG_ERROR(ZConsole, "Undo system not available");
            return false;
        }

        size_t count = 1;
        if (!args.empty() && !ParseStepCount(args[0], count))
        {
            LOG_ERROR(ZConsole, "Usage: redo [steps]");
            return false;
        }

        const std::string top_title = undo->GetCore().GetRedoTitle();
        size_t done = 0;
        while (done < count && undo->PerformRedo())
        {
            ++done;
        }
        if (done == 0)
        {
            LOG_WARNING(ZConsole, "Nothing to redo ({} queued step(s))", undo->GetCore().GetRedoCount());
            return true;
        }

        LOG_INFO(ZConsole, "Redo: {} step(s) -- '{}'", done, count == 1 ? top_title.c_str() : undo->GetCore().GetRedoTitle().c_str());
        LOG_INFO(ZConsole, "Redo: {} step(s) left, {} undoable", undo->GetCore().GetRedoCount(),
                 undo->GetCore().GetUndoCount());
        return true;
    }

    bool CmdUndoHistory(const std::vector<std::string>& args)
    {
        EditorUndoSystem* undo = EditorUndo::GetSystem();
        if (undo == nullptr)
        {
            LOG_ERROR(ZConsole, "Undo system not available");
            return false;
        }

        size_t max_rows = 20;
        if (!args.empty() && !ParseStepCount(args[0], max_rows))
        {
            LOG_ERROR(ZConsole, "Usage: undo.history [max rows]");
            return false;
        }

        const UndoSystem& core = undo->GetCore();
        LOG_INFO(ZConsole, "Undo history: {} undo / {} redo step(s), ~{} KB, state id {}", core.GetUndoCount(),
                 core.GetRedoCount(), core.GetMemoryBytes() / 1024, core.GetCurrentStateId());
        LOG_INFO(ZConsole, "  limits: {} step(s) / ~{} KB, recording {}", core.GetLimits().max_transactions,
                 core.GetLimits().max_memory_bytes / 1024, core.IsRecordingSuspended() ? "suspended" : "on");

        const std::vector<std::string> undo_titles = core.GetUndoTitles(max_rows);
        for (size_t i = 0; i < undo_titles.size(); ++i)
        {
            LOG_INFO(ZConsole, "  undo [{}] {}", i + 1, undo_titles[i].c_str());
        }
        const std::vector<std::string> redo_titles = core.GetRedoTitles(max_rows);
        for (size_t i = 0; i < redo_titles.size(); ++i)
        {
            LOG_INFO(ZConsole, "  redo [{}] {}", i + 1, redo_titles[i].c_str());
        }
        if (core.GetUndoCount() > undo_titles.size())
        {
            LOG_INFO(ZConsole, "  ... ({} more, raise the row limit)", core.GetUndoCount() - undo_titles.size());
        }
        return true;
    }

    bool CmdUndoClear(const std::vector<std::string>&)
    {
        EditorUndoSystem* undo = EditorUndo::GetSystem();
        if (undo == nullptr)
        {
            LOG_ERROR(ZConsole, "Undo system not available");
            return false;
        }
        undo->ClearHistory("console command");
        LOG_INFO(ZConsole, "Undo history cleared");
        return true;
    }
}  // namespace

void RegisterEditorConsoleCommands(ConsoleManager& console)
{
    console.RegisterCommand("obj", "Scene objects. Usage: obj list", CmdObj);
    console.RegisterCommand("level", "Level ops. Usage: level reload | level save", CmdLevel);
    console.RegisterCommand("asset.find", "Search AssetRegistry. Usage: asset.find <filter>", CmdAssetFind);
    console.RegisterCommand("asset.reimport", "Reimport a .zasset from its source. Usage: asset.reimport <path>",
                            CmdAssetReimport);
    console.RegisterCommand("asset.count", "Print AssetRegistry asset count", CmdAssetCount);
    console.RegisterCommand("asset.cook",
                            "Cook project textures for a platform. Usage: asset.cook <standalone|android|ios|ohos|webgl>",
                            CmdAssetCook);
    console.RegisterCommand("play", "Enter play mode (editor)", CmdPlay);
    console.RegisterCommand("pause", "Pause play mode (editor)", CmdPause);
    console.RegisterCommand("undo", "Undo editor steps. Usage: undo [steps]", CmdUndo);
    console.RegisterCommand("redo", "Redo editor steps. Usage: redo [steps]", CmdRedo);
    console.RegisterCommand("undo.history", "Print the undo/redo stacks. Usage: undo.history [max rows]", CmdUndoHistory);
    console.RegisterCommand("undo.clear", "Drop the whole undo history", CmdUndoClear);
    console.RegisterCommand("r.renderpath",
                            "Switch render path. Usage: r.renderpath auto|desktop|mobile (mirrors r.RenderPath CVar)",
                            CmdRenderPath);
    console.RegisterCommand("wp.status", "World Partition status", CmdWpStatus);
    console.RegisterCommand("wp.cells", "World Partition loaded cell summary", CmdWpCells);
    console.RegisterCommand("insights.dump",
                            "Save current Insights capture to <cwd>/Insights/*.ztrace. Usage: insights.dump [view]",
                            CmdInsightsDump);
    console.RegisterCommand("renderdoc.load", "Load RenderDoc in-app API", CmdRenderDocLoad);
    console.RegisterCommand("renderdoc.capture", "Capture the next rendered frame with RenderDoc", CmdRenderDocCapture);
    console.RegisterCommand("renderdoc.status", "Print RenderDoc integration status", CmdRenderDocStatus);
    console.RegisterCommand("bundle.build",
                            "Package the project into .zbundle files (incremental; --force rebuilds "
                            "everything). Usage: bundle.build [standalone|android|ios|ohos|webgl] [--force]",
                            CmdBundleBuild);
}
