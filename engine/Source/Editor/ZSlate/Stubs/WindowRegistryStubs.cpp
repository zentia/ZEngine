// Stub constructors + OnGUI for ZSlate editor windows excluded from build.
// Linked by EditorWindowRegistry.

#include "Editor/EditorWindow/ZSlateSceneWindow/ZSlateSceneWindow.h"
#include "Editor/EditorWindow/ZSlateUMGDesignerWindow/ZSlateUMGDesignerWindow.h"
#include "Editor/EditorWindow/ZSlateConsoleWindow/ZSlateConsoleWindow.h"
#include "Editor/EditorWindow/ZSlateDemoWindow/ZSlateDemoWindow.h"
#include "Editor/EditorWindow/ZSlateInspectorWindow/ZSlateInspectorWindow.h"
#include "Editor/EditorWindow/ZSlatePackageManagerWindow/ZSlatePackageManagerWindow.h"
#include "Editor/EditorWindow/ZSlatePreviewWindow/ZSlatePreviewWindow.h"
#include "Editor/EditorWindow/ZSlateAnimationWindow/ZSlateAnimationWindow.h"
#include "Editor/EditorWindow/ZSlateBlueprintWindow/ZSlateBlueprintWindow.h"
#include "Editor/EditorWindow/ZSlateTimelineWindow/ZSlateTimelineWindow.h"
#include "Editor/EditorWindow/ZSlateHierarchyWindow/ZSlateHierarchyWindow.h"
#include "Editor/EditorWindow/ZSlateMaterialEditorWindow/ZSlateMaterialEditorWindow.h"
#include "Editor/EditorWindow/ZSlateInsightsWindow/ZSlateInsightsWindow.h"

#define STUB_WIN(cls, name) \
    cls::cls(EditorUI* ui) : EditorWindow(ui, name) {} \
    void cls::OnGUI() {}

#define STUB_PLAY(cls, name, vp) \
    cls::cls(EditorUI* ui) : PlayModeView(ui, name, static_cast<ViewportType>(vp)) {} \
    void cls::OnGUI() {}

STUB_WIN(ZSlateUMGDesignerWindow, "UMGDesigner")
STUB_WIN(ZSlateConsoleWindow, "Console")
STUB_WIN(ZSlateDemoWindow, "Demo")
STUB_WIN(ZSlateInspectorWindow, "Inspector")
STUB_WIN(ZSlatePackageManagerWindow, "PackageManager")
STUB_WIN(ZSlateAnimationWindow, "Animation")
STUB_WIN(ZSlateBlueprintWindow, "Blueprint")
STUB_WIN(ZSlateTimelineWindow, "Timeline")
STUB_WIN(ZSlateHierarchyWindow, "Hierarchy")
STUB_WIN(ZSlateMaterialEditorWindow, "MaterialEditor")
STUB_WIN(ZSlatePreviewWindow, "Preview")
STUB_WIN(ZSlateInsightsWindow, "Insights")

// ZSlateConsoleWindow declares virtual ~ZSlateConsoleWindow() explicitly
ZSlateConsoleWindow::~ZSlateConsoleWindow() = default;

STUB_PLAY(ZSlateSceneWindow, "Scene", 1)
void ZSlateSceneWindow::OnViewportHidden() {}
