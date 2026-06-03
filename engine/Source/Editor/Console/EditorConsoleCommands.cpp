#include "EditorConsoleCommands.h"

#include "Editor/AssetRegistry/AssetRegistry.h"
#include "Editor/EditorApplication/EditorApplication.h"
#include "Editor/EditorAsset/EditorAssetManager.h"
#include "Editor/EditorWindow/ZSlateInsightsWindow/ZSlateInsightsWindow.h"
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
        return std::dynamic_pointer_cast<EditorAssetManager>(GET_SYSTEM(AssetManager)).get();
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

    bool CmdInsightsDump(const std::vector<std::string>& args)
    {
        ZEngine::Insights::InsightsSnapshot snapshot;
        ZEngine::Insights::InsightsTrace::Get().BuildSnapshot(snapshot);
        if (snapshot.tracks.empty())
        {
            LOG_WARNING(ZConsole,
                        "insights.dump: no captured data. Open the Insights panel first so capture turns on.");
        }

        const std::string path = ZSlateInsightsWindow::SaveTraceToDisk(snapshot);
        if (path.empty())
        {
            return false;
        }
        // `insights.dump view` (or `open`) also launches the standalone viewer.
        if (!args.empty() && (args[0] == "view" || args[0] == "open"))
        {
            ZSlateInsightsWindow::LaunchStandaloneViewer(path);
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
}  // namespace

void RegisterEditorConsoleCommands(ConsoleManager& console)
{
    console.RegisterCommand("obj", "Scene objects. Usage: obj list", CmdObj);
    console.RegisterCommand("level", "Level ops. Usage: level reload | level save", CmdLevel);
    console.RegisterCommand("asset.find", "Search AssetRegistry. Usage: asset.find <filter>", CmdAssetFind);
    console.RegisterCommand("asset.reimport", "Reimport a .zasset from its source. Usage: asset.reimport <path>",
                            CmdAssetReimport);
    console.RegisterCommand("asset.count", "Print AssetRegistry asset count", CmdAssetCount);
    console.RegisterCommand("play", "Enter play mode (editor)", CmdPlay);
    console.RegisterCommand("pause", "Pause play mode (editor)", CmdPause);
    console.RegisterCommand("r.renderpath",
                            "Switch render path. Usage: r.renderpath auto|desktop|mobile (mirrors r.RenderPath CVar)",
                            CmdRenderPath);
    console.RegisterCommand("wp.status", "World Partition status", CmdWpStatus);
    console.RegisterCommand("wp.cells", "World Partition loaded cell summary", CmdWpCells);
    console.RegisterCommand("insights.dump",
                            "Save current Insights capture to <cwd>/Insights/*.ztrace. Usage: insights.dump [view]",
                            CmdInsightsDump);
}
