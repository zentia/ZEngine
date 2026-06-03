#pragma once

#include <string>

// ImGui dock / layout keys for built-in editor panels (Unity EditorWindow title contract).
namespace EditorLayoutWindowIds
{
    inline constexpr const char* kHierarchy = "Hierarchy";
    inline constexpr const char* kInspector = "Inspector";
    inline constexpr const char* kPreview = "Preview";
    // UE-style asset browser panel (was "Project").
    inline constexpr const char* kContentBrowser = "Content Browser";
    inline constexpr const char* kConsole = "Console";
    inline constexpr const char* kScene = "Scene";
    inline constexpr const char* kGame = "Game";
    inline constexpr const char* kTimeline = "Timeline";
    inline constexpr const char* kBlueprint = "Blueprint";
    inline constexpr const char* kAnimation = "Animation";
    inline constexpr const char* kMaterial = "Material";
    inline constexpr const char* kPackageManager = "Package Manager";
    inline constexpr const char* kInsights = "Insights";
    inline constexpr const char* kZSlate = "ZSlate";
    // Legacy dock titles from the parallel ZSlate rollout (pre-default-switch).
    // Kept so old .zlayout.json / imgui.ini that reference these strings can
    // be re-mapped on load; not registered in EditorWindowRegistry anymore.
    inline constexpr const char* kZSlateInspector = "ZSlate Inspector";
    inline constexpr const char* kZSlateHierarchy = "ZSlate Hierarchy";
    inline constexpr const char* kZSlateConsole = "ZSlate Console";
    inline constexpr const char* kZSlateProject = "ZSlate Project";
    inline constexpr const char* kUMGDesigner = "UMG Designer";

    // Deprecated alias; prefer kContentBrowser.
    inline constexpr const char* kProject = kContentBrowser;

    // Remap persisted layout / floating-panel titles to the current canonical name.
    inline std::string RemapLegacyPanelTitle(std::string panel_id)
    {
        if (panel_id == "Project" || panel_id == kZSlateProject)
        {
            return std::string(kContentBrowser);
        }
        return panel_id;
    }
}  // namespace EditorLayoutWindowIds
