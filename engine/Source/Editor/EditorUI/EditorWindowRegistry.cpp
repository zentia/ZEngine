#include "EditorWindowRegistry.h"

#include "Editor/EditorLayout/EditorLayoutWindowIds.h"
#include "Editor/EditorUI/EditorUI.h"
#include "Editor/EditorWindow/ZSlateGameWindow/ZSlateGameWindow.h"
#include "Editor/EditorWindow/ZSlateSceneWindow/ZSlateSceneWindow.h"
#include "Editor/EditorWindow/ZSlateUMGDesignerWindow/ZSlateUMGDesignerWindow.h"
#include "Editor/EditorWindow/ZSlateConsoleWindow/ZSlateConsoleWindow.h"
#include "Editor/EditorWindow/ZSlateDemoWindow/ZSlateDemoWindow.h"
#include "Editor/EditorWindow/ZSlateInspectorWindow/ZSlateInspectorWindow.h"
#include "Editor/EditorWindow/ZSlatePackageManagerWindow/ZSlatePackageManagerWindow.h"
#include "Editor/EditorWindow/ZSlatePreviewWindow/ZSlatePreviewWindow.h"
// ZSlateContentBrowserWindow: excluded (ContentBrowserContext reference members
// prevent trivial stubbing while UIRenderer->ISlateRenderer migration is in progress)
#include "Editor/EditorWindow/ZSlateAnimationWindow/ZSlateAnimationWindow.h"
#include "Editor/EditorWindow/ZSlateBlueprintWindow/ZSlateBlueprintWindow.h"
#include "Editor/EditorWindow/ZSlateMaterialEditorWindow/ZSlateMaterialEditorWindow.h"
#include "Editor/EditorWindow/ZSlateHierarchyWindow/ZSlateHierarchyWindow.h"
#include "Editor/EditorWindow/ZSlateTimelineWindow/ZSlateTimelineWindow.h"
#include "Editor/EditorWindow/ZSlateInsightsWindow/ZSlateInsightsWindow.h"
#include "Runtime/Core/Memory/MemoryManager.h"

namespace
{
    template<typename T>
    EditorWindow* NewEditorPanel(EditorUI* editor_ui)
    {
        return MemoryManager::CreateObject<T>(editor_ui);
    }

    const std::vector<EditorWindowDescriptor> kBuiltinDescriptors = {
        {EditorLayoutWindowIds::kGame, EditorWindowCategory::Scene, true, typeid(ZSlateGameWindow),
         NewEditorPanel<ZSlateGameWindow>},
        {EditorLayoutWindowIds::kConsole, EditorWindowCategory::General, true, typeid(ZSlateConsoleWindow),
         NewEditorPanel<ZSlateConsoleWindow>},
        // ContentBrowser: excluded (ContentBrowserContext references prevent trivial stubbing)
        // {EditorLayoutWindowIds::kContentBrowser, EditorWindowCategory::General, true, ...},
        {EditorLayoutWindowIds::kInspector, EditorWindowCategory::General, true,
         typeid(ZSlateInspectorWindow), NewEditorPanel<ZSlateInspectorWindow>},
        {EditorLayoutWindowIds::kPreview, EditorWindowCategory::General, true, typeid(ZSlatePreviewWindow),
         NewEditorPanel<ZSlatePreviewWindow>},
        {EditorLayoutWindowIds::kHierarchy, EditorWindowCategory::General, true, typeid(ZSlateHierarchyWindow),
         NewEditorPanel<ZSlateHierarchyWindow>},
        {EditorLayoutWindowIds::kScene, EditorWindowCategory::Scene, true, typeid(ZSlateSceneWindow),
         NewEditorPanel<ZSlateSceneWindow>},
        {EditorLayoutWindowIds::kBlueprint, EditorWindowCategory::Animation, false, typeid(ZSlateBlueprintWindow),
         NewEditorPanel<ZSlateBlueprintWindow>},
        {EditorLayoutWindowIds::kMaterial, EditorWindowCategory::General, false, typeid(ZSlateMaterialEditorWindow),
         NewEditorPanel<ZSlateMaterialEditorWindow>},
        {EditorLayoutWindowIds::kAnimation, EditorWindowCategory::Animation, false, typeid(ZSlateAnimationWindow),
         NewEditorPanel<ZSlateAnimationWindow>},
        {EditorLayoutWindowIds::kTimeline, EditorWindowCategory::Animation, false, typeid(ZSlateTimelineWindow),
         NewEditorPanel<ZSlateTimelineWindow>},
        {EditorLayoutWindowIds::kPackageManager, EditorWindowCategory::Package, false,
         typeid(ZSlatePackageManagerWindow), NewEditorPanel<ZSlatePackageManagerWindow>},
        {EditorLayoutWindowIds::kInsights, EditorWindowCategory::General, false, typeid(ZSlateInsightsWindow),
         NewEditorPanel<ZSlateInsightsWindow>},
        {EditorLayoutWindowIds::kZSlate, EditorWindowCategory::General, true, typeid(ZSlateDemoWindow),
         NewEditorPanel<ZSlateDemoWindow>},
        {EditorLayoutWindowIds::kUMGDesigner, EditorWindowCategory::General, true,
         typeid(ZSlateUMGDesignerWindow), NewEditorPanel<ZSlateUMGDesignerWindow>},
    };
}  // namespace

void EditorWindowRegistry::RegisterBuiltinWindows(EditorUI& editor_ui)
{
    for (const EditorWindowDescriptor& descriptor : kBuiltinDescriptors)
    {
        editor_ui.RegisterEditorWindow(descriptor);
    }
}

const std::vector<EditorWindowDescriptor>& EditorWindowRegistry::GetBuiltinDescriptors()
{
    return kBuiltinDescriptors;
}
