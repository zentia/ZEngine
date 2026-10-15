// Stub constructors + OnGUI for ZSlate editor windows excluded from build.
// Linked by EditorWindowRegistry.

#include "Editor/EditorWindow/ZSlateSceneWindow/ZSlateSceneWindow.h"
#include "Editor/EditorWindow/ZSlateUMGDesignerWindow/ZSlateUMGDesignerWindow.h"
#include "Editor/EditorWindow/ZSlateConsoleWindow/ZSlateConsoleWindow.h"
// ZSlateDemoWindow: deleted.
#include "Editor/EditorWindow/ZSlateInspectorWindow/ZSlateInspectorWindow.h"
#include "Editor/EditorWindow/ZSlatePackageManagerWindow/ZSlatePackageManagerWindow.h"
#include "Editor/EditorWindow/ZSlatePreviewWindow/ZSlatePreviewWindow.h"
#include "Editor/EditorWindow/ZSlateAnimationWindow/ZSlateAnimationWindow.h"
#include "Editor/EditorWindow/ZSlateBlueprintWindow/ZSlateBlueprintWindow.h"
#include "Editor/EditorWindow/ZSlateTimelineWindow/ZSlateTimelineWindow.h"
#include "Editor/EditorWindow/ZSlateHierarchyWindow/ZSlateHierarchyWindow.h"
#include "Editor/EditorWindow/ZSlateMaterialEditorWindow/ZSlateMaterialEditorWindow.h"
// ZSlateInsightsWindow: deleted (Insights -> standalone ZInsights app).

#define STUB_WIN(cls, name) \
    cls::cls(EditorUI* ui) : EditorWindow(ui, name) {} \
    void cls::OnGUI() {}

#define STUB_PLAY(cls, name, vp) \
    cls::cls(EditorUI* ui) : PlayModeView(ui, name, static_cast<ViewportType>(vp)) {} \
    void cls::OnGUI() {}

// ZSlateUMGDesignerWindow: real implementation is built (its CMake exclude is gone).
// ZSlateConsoleWindow: real implementation is built (its CMake exclude is gone).
// ZSlateDemoWindow: deleted.
// ZSlateInspectorWindow: real implementation is built (its CMake exclude is
// gone; see ZSlate-heavy editor windows above).
// ZSlatePackageManagerWindow: real implementation is built (its CMake exclude is gone).
// ZSlateAnimationWindow: real implementation is built (its CMake exclude is gone).
// ZSlateBlueprintWindow: real implementation is built (its CMake exclude is gone).
// ZSlateTimelineWindow: real implementation is built (its CMake exclude is gone).
// ZSlateMaterialEditorWindow: real implementation is built (its CMake exclude is gone).
// ZSlatePreviewWindow: real implementation is built (native zasset preview).
// ZSlateInsightsWindow: deleted (Insights -> standalone ZInsights app).
// ZSlateHierarchyWindow + ZSlateSceneWindow: real implementations are built
// (their CMake excludes are gone; see ZSlate-heavy editor windows above).

// ZSlateConsoleWindow's virtual dtor is defined by the real implementation
// (ZSlateConsoleWindow.cpp), which is built again.
