#pragma once

#include "Editor/EditorSceneManager/EditorSceneManager.h"  // EditorAxisMode
#include "Editor/EditorWindow/PlayModeView/PlayModeView.h"
#include "Editor/Menu/ZSlatePopupMenu.h"
#include "Runtime/Slate/Application/SlateDragDrop.h"
#include "Runtime/Slate/Core/SlatePaint.h"
#include "Runtime/UI/Render/UIRenderer.h"
#include "Runtime/Slate/Application/SlateInput.h"
#include "Runtime/UI/Core/UITypes.h"

#include <memory>

class RenderCamera;

namespace ZSlate
{
class SWidget;
class SButton;
}

// Native ZSlate replacement for the legacy ImGui SceneWindow.
//
// Like the Game viewport, the 3D scene composites UNDER this transparent
// (NoBackground) panel; PlayModeView::UpdateViewport reports the panel work rect
// to RenderSystem. What changed vs. the old SceneWindow is that the interactive
// editor *chrome* now renders through ZSlate instead of ImGui:
//   - Toolbar (axis-mode toggles, 2D toggle, Skybox toggle, Scene Camera) -> ZSlate
//     SButtons painted into the reserved menu-bar strip, routed manually.
//   - Right-click context menu -> ZSlatePopupMenu.
//   - "Scene Camera" settings -> a ZSlate floating panel (sliders + checkboxes),
//     hosted by a small inline controller (paint-on-top + route + outside-click
//     dismiss).
//
// Camera-preview frame chrome is painted through the native UIRenderer overlay
// (border + title bar); RenderSystem still composites the live preview texture.
//
// Camera navigation (orbit / pan / zoom / WASD) and click-to-pick read from the
// GLFW-backed EditorSlateHost when native hosting is active.
//
// Cross-window drag-drop (Project asset -> scene placement, Hierarchy object ->
// reparent-in-viewport) is intentionally NOT handled here: the ZSlate drag
// operation is per-window-router, so a drag started in the (ZSlate) Project /
// Hierarchy windows is invisible to this window. The old ImGui-payload drop path
// was already dead once those windows moved to ZSlate. Restoring it needs an
// app-level (global) drag operation and is tracked as backlog.
class ZSlateSceneWindow : public PlayModeView
{
public:
    explicit ZSlateSceneWindow(EditorUI* editor_ui);
    void OnGUI() override;
    bool SupportsNativeHosting() const override { return true; }

private:
    void OnViewportHidden() override;

    // --- Toolbar -------------------------------------------------------------
    void BuildToolbar(float scale);
    std::shared_ptr<ZSlate::SWidget> MakeAxisButton(const char* label,
                                                    EditorAxisMode mode,
                                                    bool selected,
                                                    bool disabled,
                                                    float scale);

    // --- Popups (right-click context menu) ----------------------------------
    void OpenContextMenu(const Vector2& anchor, float scale);

    // --- Scene Camera settings panel ----------------------------------------
    void BuildCameraPanel(float scale);

    // --- Camera preview overlay (native chrome + RenderSystem composite) ----
    void UpdateCameraPreviewOverlay(const UIRect& work_rect, float viewport_origin_x, float viewport_origin_y);
    void UpdateViewportForWorkRect(const UIRect& work_rect);

    // --- Cross-window drag-drop (asset placement / object reparent) ---------
    // Consumes the process-wide active drag op (ZSlate::GetActiveDragOperation)
    // published by the Project / Hierarchy routers. The drop fires on THIS
    // window's own left-release edge over the viewport, caching the op from the
    // prior frame so window draw order vs. the source router clearing the op is
    // irrelevant.
    void HandleDragDropDrop(bool chrome_capturing, const UIRect& work_rect);

    // --- Navigation / picking (EditorSlateHost when native-hosted) ----------
    void HandleSceneViewNavigation(bool chrome_capturing, const UIRect& work_rect);
    void HandleSceneGizmoDrag(bool chrome_capturing, const UIRect& work_rect);
    void HandleContextMenu(bool chrome_capturing, const UIRect& work_rect, float scale);
    void PanSceneViewCamera(const std::shared_ptr<RenderCamera>& editor_camera,
                            const Vector2& mouse_delta,
                            float viewport_height) const;
    void ZoomSceneViewCamera(const std::shared_ptr<RenderCamera>& editor_camera, float wheel_delta) const;
    void MoveSceneViewCameraWithKeyboard(const std::shared_ptr<RenderCamera>& editor_camera) const;

    // Toolbar.
    std::shared_ptr<ZSlate::SWidget> m_Toolbar;
    ZSlate::SlateInputRouter m_ToolbarInput;
    float m_BuiltToolbarScale {-1.0f};
    int m_BuiltAxisMode {-1};
    bool m_BuiltSceneView2D {false};
    bool m_BuiltSkyboxVisible {true};
    std::shared_ptr<ZSlate::SButton> m_CameraButton;

    // Shared popup controller (context menu).
    ZSlate::ZSlatePopupMenu m_Popup;

    // Scene Camera settings floating panel.
    std::shared_ptr<ZSlate::SWidget> m_CameraPanel;
    ZSlate::SlateInputRouter m_CameraPanelInput;
    bool m_CameraPanelOpen {false};
    Vector2 m_CameraPanelAnchor {0.0f, 0.0f};

    // Navigation / context state.
    bool m_TrackContextClick {false};
    bool m_IsRotatingSceneView {false};
    bool m_IsPanningSceneView {false};
    bool m_IsAltLeftPanningSceneView {false};
    bool m_IsDraggingGizmo {false};
    size_t m_GizmoDragAxis {3};
    Vector2 m_GizmoDragLastMouse {0.0f, 0.0f};
    bool m_SceneCameraDynamicClipping {true};
    bool m_SceneCameraOcclusionCulling {false};
    std::shared_ptr<RenderCamera> m_SelectedCameraPreviewCamera {nullptr};
    bool m_CameraPreviewVisible {false};
    UIRect m_CameraPreviewFrame {};
    std::string m_CameraPreviewTitle;
    Vector2 m_ContextClickPos {0.0f, 0.0f};

    bool m_PrevLeftDown {false};
    bool m_PrevRightDown {false};
    bool m_PrevMiddleDown {false};

    // Last non-null cross-window drag op seen while a gesture was in flight.
    // Used on the release edge to perform the drop (see HandleDragDropDrop).
    std::shared_ptr<ZSlate::FDragDropOperation> m_PendingDragOp {nullptr};
};
