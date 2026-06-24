#include "ZSlateSceneWindow.h"

#include "Editor/EditorDragDrop/EditorDragDrop.h"
#include "Editor/EditorHierarchy/EditorHierarchyReparent.h"
#include "Editor/EditorInputManager/EditorInputManager.h"
#include "Editor/EditorLayout/EditorLayoutWindowIds.h"
#include "Editor/EditorScenePlacement/EditorScenePlacement.h"
#include "Editor/EditorSceneManager/EditorSceneManager.h"
#include "Editor/ZSlate/Backend/ZSlateEditorOverlay.h"
#include "Editor/ZSlate/Backend/EditorSlateHost.h"
#include "Runtime/UI/Render/UIRenderer.h"
#include "Runtime/Function/Input/KeyCodes.h"

#include "Runtime/BaseClasses/GameObject.h"
#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Core/Math/MathHeaders.h"
#include "Runtime/Function/Framework/Component/Camera/CameraComponent.h"
#include "Runtime/Function/Framework/Level/Level.h"
#include "Runtime/Function/Framework/World/WorldManager.h"
#include "Runtime/Function/Render/Interface/RHI.h"
#include "Runtime/Function/Render/RenderCamera.h"
#include "Runtime/Function/Render/RenderSystem.h"
#include "Runtime/Function/Render/WindowSystem.h"
#include "ZSlate/Application/SlateApplication.h"
#include "ZSlate/Widgets/Panels/SBorder.h"
#include "ZSlate/Widgets/SBox.h"
#include "ZSlate/Widgets/SBoxPanel.h"
#include "ZSlate/Widgets/SButton.h"
#include "ZSlate/Widgets/SCheckBox.h"
#include "ZSlate/Widgets/SEditableTextBox.h"
#include "ZSlate/Widgets/SMenu.h"
#include "ZSlate/Widgets/SSlider.h"
#include "ZSlate/Widgets/SSpacer.h"
#include "ZSlate/Widgets/STextBlock.h"

#include <Application/Application.h>  // g_isPlaying

#include "Runtime/Function/Framework/Component/Transform/Transform.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace ZSlate;

namespace
{
    constexpr float k_scene_grid_plane_height = -0.05f;
    constexpr int k_scene_grid_major_line_every = 10;
    constexpr int k_scene_grid_half_major_count = 10;
    constexpr int k_scene_grid_half_minor_count = k_scene_grid_half_major_count * k_scene_grid_major_line_every;

    const UIColor kToolbarBg(0.13f, 0.13f, 0.15f, 1.0f);
    const UIColor kBtnNormal(0.20f, 0.20f, 0.23f, 1.0f);
    const UIColor kBtnHover(0.28f, 0.28f, 0.33f, 1.0f);
    const UIColor kBtnSelected(0.20f, 0.42f, 0.68f, 1.0f);
    const UIColor kBtnSelectedHover(0.26f, 0.50f, 0.78f, 1.0f);
    const UIColor kBtnText(0.90f, 0.91f, 0.94f, 1.0f);
    const UIColor kBtnTextDim(0.55f, 0.56f, 0.60f, 1.0f);
    const UIColor kLabelColor(0.78f, 0.80f, 0.85f, 1.0f);
    const UIColor kValueColor(0.88f, 0.90f, 0.94f, 1.0f);

    std::shared_ptr<STextBlock> MakeText(const std::string& text, float font_size, const UIColor& color)
    {
        auto t = std::make_shared<STextBlock>();
        t->Text = text;
        t->FontSize = font_size;
        t->Color = color;
        t->Alignment = TextAnchor::MiddleLeft;
        return t;
    }

    // ---- Editor grid overlay (native BatchedUIRenderer; non-interactive 3D-line paint) --

    float getSceneGridMinorSpacing(float camera_height_above_plane)
    {
        const float safe_height = std::max(camera_height_above_plane, 1.0f);
        const float major_spacing = std::pow(10.0f, std::floor(std::log10(safe_height)));
        return std::max(1.0f, major_spacing / static_cast<float>(k_scene_grid_major_line_every));
    }

    // Draw a straight line as a chain of short axis-aligned quads (UIRenderer has no
    // native line primitive; same approach as ZSlateAnimationWindow).
    void DrawLineQuads(UIRenderer& r, const Vector2& a, const Vector2& b, const UIColor& color, float thickness)
    {
        // Draw the whole segment as ONE oriented quad (2 triangles) via drawConvexPoly,
        // instead of a chain of 1x1 quads (which was O(length-in-pixels) per line and
        // made the editor grid cost ~100 ms/frame).
        const float dx = b.x - a.x;
        const float dy = b.y - a.y;
        const float dist = std::sqrt(dx * dx + dy * dy);
        const float half = std::max(0.5f, thickness * 0.5f);
        if (dist < 1e-3f)
        {
            r.drawQuad(UIRect(a.x - half, a.y - half, half * 2.0f, half * 2.0f), color);
            return;
        }
        const float nx = -dy / dist * half;  // screen-space normal scaled to half thickness
        const float ny = dx / dist * half;
        const Vector2 pts[4] = {
            Vector2(a.x + nx, a.y + ny),
            Vector2(b.x + nx, b.y + ny),
            Vector2(b.x - nx, b.y - ny),
            Vector2(a.x - nx, a.y - ny),
        };
        r.drawConvexPoly(pts, 4, color);
    }

    UIColor getSceneGridLineColor(int line_index, bool is_vertical_line)
    {
        if (is_vertical_line && line_index == 0)
            return UIColor(89.0f / 255.0f, 204.0f / 255.0f, 115.0f / 255.0f, 217.0f / 255.0f);
        if (!is_vertical_line && line_index == 0)
            return UIColor(217.0f / 255.0f, 89.0f / 255.0f, 89.0f / 255.0f, 217.0f / 255.0f);
        if (line_index % k_scene_grid_major_line_every == 0)
            return UIColor(148.0f / 255.0f, 161.0f / 255.0f, 184.0f / 255.0f, 140.0f / 255.0f);
        return UIColor(120.0f / 255.0f, 133.0f / 255.0f, 153.0f / 255.0f, 90.0f / 255.0f);
    }

    [[maybe_unused]] bool projectWorldToSceneRect(const Vector3& world,
                                 const Matrix4x4& view_proj,
                                 const UIRect& rect,
                                 Vector2& out_screen,
                                 float& out_ndc_z)
    {
        Vector4 clip = view_proj * Vector4(world, 1.0f);
        if (clip.w <= 1e-4f)
            return false;

        const float inv_w = 1.0f / clip.w;
        const float ndc_x = clip.x * inv_w;
        const float ndc_y = clip.y * inv_w;
        out_ndc_z = clip.z * inv_w;

        out_screen.x = rect.x + (ndc_x + 1.0f) * 0.5f * rect.width;
        out_screen.y = rect.y + (ndc_y + 1.0f) * 0.5f * rect.height;
        return true;
    }

    void drawWorldLine(UIRenderer& renderer,
                       const Vector3& world_a,
                       const Vector3& world_b,
                       const Matrix4x4& view_proj,
                       const UIRect& rect,
                       const UIColor& color,
                       float thickness = 1.0f)
    {
        Vector4 ca = view_proj * Vector4(world_a, 1.0f);
        Vector4 cb = view_proj * Vector4(world_b, 1.0f);

        // Clip the segment against the near plane (w >= w_min) in homogeneous clip
        // space BEFORE the perspective divide. This lets a single full-span line that
        // crosses behind the camera still render its visible portion -- previously each
        // grid line had to be split into ~200 short per-cell segments to avoid the
        // "behind camera endpoint" projection failure, which is what made the grid
        // cost O(N^2) draw calls per frame.
        constexpr float w_min = 1e-4f;
        const float da = ca.w - w_min;
        const float db = cb.w - w_min;
        if (da < 0.0f && db < 0.0f)
            return;  // entire segment is behind the camera
        if (da < 0.0f || db < 0.0f)
        {
            const float t = da / (da - db);
            Vector4 mid;
            mid.x = ca.x + (cb.x - ca.x) * t;
            mid.y = ca.y + (cb.y - ca.y) * t;
            mid.z = ca.z + (cb.z - ca.z) * t;
            mid.w = ca.w + (cb.w - ca.w) * t;
            if (da < 0.0f)
                ca = mid;
            else
                cb = mid;
        }

        const float inv_wa = 1.0f / ca.w;
        const float inv_wb = 1.0f / cb.w;
        const Vector2 screen_a(rect.x + (ca.x * inv_wa + 1.0f) * 0.5f * rect.width,
                               rect.y + (ca.y * inv_wa + 1.0f) * 0.5f * rect.height);
        const Vector2 screen_b(rect.x + (cb.x * inv_wb + 1.0f) * 0.5f * rect.width,
                               rect.y + (cb.y * inv_wb + 1.0f) * 0.5f * rect.height);

        DrawLineQuads(renderer, screen_a, screen_b, color, thickness);
    }

    void drawWorldRing(UIRenderer& renderer,
                       const Vector3& pivot,
                       const Vector3& tangent_u,
                       const Vector3& tangent_v,
                       float radius,
                       const Matrix4x4& view_proj,
                       const UIRect& rect,
                       const UIColor& color,
                       float thickness = 2.5f,
                       int segments = 48)
    {
        if (radius <= 1e-4f || segments < 8)
        {
            return;
        }

        Vector3 prev_point {};
        bool has_prev = false;
        for (int segment_index = 0; segment_index <= segments; ++segment_index)
        {
            const float angle = static_cast<float>(segment_index) * (2.0f * Math_PI / static_cast<float>(segments));
            const Vector3 point =
                pivot + tangent_u * (std::cos(angle) * radius) + tangent_v * (std::sin(angle) * radius);
            if (has_prev)
            {
                drawWorldLine(renderer, prev_point, point, view_proj, rect, color, thickness);
            }
            prev_point = point;
            has_prev = true;
        }
    }

    void DrawEditorGridOverlay(UIRenderer& renderer, const UIRect& rect, const std::shared_ptr<RenderCamera>& camera)
    {
        if (rect.width <= 1.0f || rect.height <= 1.0f || !camera)
            return;

        const Vector3 camera_position = camera->position();
        const float view_height = camera->IsOrthographic()
                                    ? camera->GetOrthoHalfHeight() * 2.0f
                                    : std::abs(camera_position.z - k_scene_grid_plane_height);
        const float minor_spacing = getSceneGridMinorSpacing(view_height);
        const float half_extent = minor_spacing * static_cast<float>(k_scene_grid_half_minor_count);

        const int min_x_index = static_cast<int>(std::floor((camera_position.x - half_extent) / minor_spacing));
        const int max_x_index = static_cast<int>(std::ceil((camera_position.x + half_extent) / minor_spacing));
        const int min_y_index = static_cast<int>(std::floor((camera_position.y - half_extent) / minor_spacing));
        const int max_y_index = static_cast<int>(std::ceil((camera_position.y + half_extent) / minor_spacing));

        const float grid_aspect = rect.height > 0.0f ? rect.width / rect.height : camera->getAspect();
        const Matrix4x4 view_proj =
            camera->GetProjectionMatrixForAspect(grid_aspect) * camera->getLookAtMatrix();

        // Grid lines only; 3D (RP2 combine + skybox) must show through without a dimming quad.
        // drawWorldLine), not one segment per cell. This turns the grid from
        // O((2*half_minor_count)^2) ~= 80k draw calls/frame into O(2*half_minor_count)
        // ~= 400 -- the dominant cause of the editor's ~10 FPS.
        const float y_span_min = static_cast<float>(min_y_index) * minor_spacing;
        const float y_span_max = static_cast<float>(max_y_index) * minor_spacing;
        for (int x_index = min_x_index; x_index <= max_x_index; ++x_index)
        {
            const float x = static_cast<float>(x_index) * minor_spacing;
            const UIColor color = getSceneGridLineColor(x_index, true);
            drawWorldLine(renderer, Vector3(x, y_span_min, k_scene_grid_plane_height),
                          Vector3(x, y_span_max, k_scene_grid_plane_height), view_proj, rect, color);
        }

        const float x_span_min = static_cast<float>(min_x_index) * minor_spacing;
        const float x_span_max = static_cast<float>(max_x_index) * minor_spacing;
        for (int y_index = min_y_index; y_index <= max_y_index; ++y_index)
        {
            const float y = static_cast<float>(y_index) * minor_spacing;
            const UIColor color = getSceneGridLineColor(y_index, false);
            drawWorldLine(renderer, Vector3(x_span_min, y, k_scene_grid_plane_height),
                          Vector3(x_span_max, y, k_scene_grid_plane_height), view_proj, rect, color);
        }
    }

    // Mode-specific transform gizmo overlay (UE-style: arrows / rotation rings / scale boxes).
    void DrawTransformGizmoOverlay(UIRenderer& renderer,
                                     const UIRect& rect,
                                     const std::shared_ptr<RenderCamera>& camera,
                                     EditorSceneManager& scene_manager)
    {
        if (g_isPlaying || rect.width <= 1.0f || rect.height <= 1.0f || !camera)
        {
            return;
        }

        const std::shared_ptr<GameObject> selected = scene_manager.GetSelectedGObject().lock();
        if (!selected)
        {
            return;
        }

        const Transform* transform_component = selected->tryGetComponentConst(Transform);
        if (transform_component == nullptr)
        {
            return;
        }

        Vector3 pivot;
        Vector3 decomposed_scale;
        Quaternion rotation;
        transform_component->GetLocalToWorldMatrix().Decomposition(pivot, decomposed_scale, rotation);
        const Vector3 local_scale = transform_component->GetLocalScale();

        const float aspect = rect.height > 0.0f ? rect.width / rect.height : camera->getAspect();
        const Matrix4x4 view_proj =
            camera->GetProjectionMatrixForAspect(aspect) * camera->getLookAtMatrix();

        const float translate_axis_len =
            EditorSceneManager::ComputeGizmoAxisLengthWorld(*camera, pivot, rect.width, rect.height);
        // EditorRotationAxis mesh uses radius 1.0 in a 2-unit extent gizmo.
        const float ring_radius = translate_axis_len * 0.5f;

        const Matrix4x4 rot_mat(rotation);
        const Vector3 axis_x(rot_mat[0][0], rot_mat[1][0], rot_mat[2][0]);
        const Vector3 axis_y(rot_mat[0][1], rot_mat[1][1], rot_mat[2][1]);
        const Vector3 axis_z(rot_mat[0][2], rot_mat[1][2], rot_mat[2][2]);

        constexpr float k_translate_thickness = 3.5f;
        constexpr float k_rotate_thickness = 2.5f;
        constexpr float k_scale_thickness = 3.0f;
        constexpr float k_scale_handle_px = 7.0f;
        const UIColor axis_red(1.0f, 0.25f, 0.25f, 1.0f);
        const UIColor axis_green(0.25f, 1.0f, 0.35f, 1.0f);
        const UIColor axis_blue(0.35f, 0.55f, 1.0f, 1.0f);

        auto projectWorldToScreen = [&](const Vector3& world, Vector2& out_screen) -> bool {
            Vector4 clip = view_proj * Vector4(world, 1.0f);
            constexpr float w_min = 1e-4f;
            if (clip.w <= w_min)
            {
                return false;
            }
            const float inv_w = 1.0f / clip.w;
            out_screen.x = rect.x + (clip.x * inv_w + 1.0f) * 0.5f * rect.width;
            out_screen.y = rect.y + (clip.y * inv_w + 1.0f) * 0.5f * rect.height;
            return true;
        };

        auto drawTranslateAxis = [&](const Vector3& dir, const UIColor& color) {
            drawWorldLine(renderer, pivot, pivot + dir * translate_axis_len, view_proj, rect, color, k_translate_thickness);
        };

        auto drawScaleAxis = [&](const Vector3& dir, float local_scale_component, const UIColor& color) {
            const float axis_len = EditorSceneManager::ComputeScaleModeAxisLength(
                *camera, pivot, rect.width, rect.height, local_scale_component);
            const Vector3 end = pivot + dir * axis_len;
            drawWorldLine(renderer, pivot, end, view_proj, rect, color, k_scale_thickness);
            Vector2 screen_end {};
            if (projectWorldToScreen(end, screen_end))
            {
                const float half = k_scale_handle_px * 0.5f;
                renderer.drawQuad(UIRect(screen_end.x - half, screen_end.y - half, k_scale_handle_px, k_scale_handle_px),
                                  color);
            }
        };

        switch (scene_manager.getEditorAxisMode())
        {
            case EditorAxisMode::TranslateMode:
                drawTranslateAxis(axis_x, axis_red);
                drawTranslateAxis(axis_y, axis_green);
                drawTranslateAxis(axis_z, axis_blue);
                break;

            case EditorAxisMode::RotateMode:
                // UE: X=YZ ring (red), Y=XZ ring (green), Z=XY ring (blue).
                drawWorldRing(renderer, pivot, axis_y, axis_z, ring_radius, view_proj, rect, axis_red, k_rotate_thickness);
                drawWorldRing(renderer, pivot, axis_x, axis_z, ring_radius, view_proj, rect, axis_green, k_rotate_thickness);
                drawWorldRing(renderer, pivot, axis_x, axis_y, ring_radius, view_proj, rect, axis_blue, k_rotate_thickness);
                break;

            case EditorAxisMode::ScaleMode:
                drawScaleAxis(axis_x, local_scale.x, axis_red);
                drawScaleAxis(axis_y, local_scale.y, axis_green);
                drawScaleAxis(axis_z, local_scale.z, axis_blue);
                break;

            default:
                break;
        }
    }
}  // namespace

ZSlateSceneWindow::ZSlateSceneWindow(EditorUI* editor_ui)
    : PlayModeView(editor_ui, EditorLayoutWindowIds::kScene, ViewportType::scene)
{
    // MenuBar reserves the top strip so the composited scene (WorkRect) stays
    // below the ZSlate toolbar; NoBackground lets the scene show through.
    m_WindowFlags = EditorViewFlags_NoBackground | EditorViewFlags_MenuBar;
}

void ZSlateSceneWindow::OnViewportHidden()
{
    GET_SYSTEM(EditorInputManager)->setEngineWindowPos(Vector2(0.0f, 0.0f));
    GET_SYSTEM(EditorInputManager)->setEngineWindowSize(Vector2(0.0f, 0.0f));
    m_TrackContextClick = false;
    m_IsRotatingSceneView = false;
    m_IsPanningSceneView = false;
    m_IsAltLeftPanningSceneView = false;
    m_Popup.Close();
    m_CameraPanelOpen = false;
}

// ----------------------------------------------------------------------------
// Toolbar
// ----------------------------------------------------------------------------

std::shared_ptr<SWidget> ZSlateSceneWindow::MakeAxisButton(const char* label,
                                                           EditorAxisMode mode,
                                                           bool selected,
                                                           bool disabled,
                                                           float scale)
{
    auto btn = std::make_shared<SButton>();
    btn->Padding = FMargin(8.0f * scale, 3.0f * scale);
    btn->VAlign = EVerticalAlignment::Center;
    btn->NormalColor = selected ? kBtnSelected : kBtnNormal;
    btn->HoverColor = disabled ? (selected ? kBtnSelected : kBtnNormal)
                               : (selected ? kBtnSelectedHover : kBtnHover);
    btn->PressedColor = selected ? kBtnSelected : kBtnNormal;
    btn->SetContent(MakeText(label, 13.0f * scale, disabled ? kBtnTextDim : kBtnText));
    if (!disabled)
    {
        btn->OnClicked = [mode]() { GET_SYSTEM(EditorSceneManager)->setEditorAxisMode(mode); };
    }
    return btn;
}

void ZSlateSceneWindow::BuildToolbar(float scale)
{
    EditorSceneManager* scene_manager = GET_SYSTEM(EditorSceneManager);
    const EditorAxisMode mode = scene_manager != nullptr ? scene_manager->getEditorAxisMode() : EditorAxisMode::TranslateMode;
    const bool scene_view_2d = scene_manager != nullptr && scene_manager->IsSceneView2D();
    RenderSystem* render_system = GET_SYSTEM(RenderSystem);
    const bool skybox_visible =
        render_system != nullptr && render_system->IsSkyboxVisible(ViewportType::scene);

    auto bar = std::make_shared<SHorizontalBox>();
    const FMargin gap(3.0f * scale, 0.0f);

    bar->AddSlot(std::make_shared<SSpacer>(Vector2(8.0f * scale, 0.0f))).AutoSize();
    bar->AddSlot(MakeAxisButton("Trans", EditorAxisMode::TranslateMode, mode == EditorAxisMode::TranslateMode, false, scale))
        .AutoSize().SetVAlign(EVerticalAlignment::Center).SetPadding(gap);
    bar->AddSlot(MakeAxisButton("Rotate", EditorAxisMode::RotateMode, mode == EditorAxisMode::RotateMode, scene_view_2d, scale))
        .AutoSize().SetVAlign(EVerticalAlignment::Center).SetPadding(gap);
    bar->AddSlot(MakeAxisButton("Scale", EditorAxisMode::ScaleMode, mode == EditorAxisMode::ScaleMode, false, scale))
        .AutoSize().SetVAlign(EVerticalAlignment::Center).SetPadding(gap);

    // 2D toggle.
    {
        auto btn = std::make_shared<SButton>();
        btn->Padding = FMargin(8.0f * scale, 3.0f * scale);
        btn->VAlign = EVerticalAlignment::Center;
        btn->NormalColor = scene_view_2d ? kBtnSelected : kBtnNormal;
        btn->HoverColor = scene_view_2d ? kBtnSelectedHover : kBtnHover;
        btn->PressedColor = scene_view_2d ? kBtnSelected : kBtnNormal;
        btn->SetContent(MakeText("2D", 13.0f * scale, kBtnText));
        btn->OnClicked = [scene_view_2d]() {
            EditorSceneManager* sm = GET_SYSTEM(EditorSceneManager);
            if (sm != nullptr)
                sm->SetSceneView2D(!scene_view_2d);
        };
        bar->AddSlot(btn).AutoSize().SetVAlign(EVerticalAlignment::Center).SetPadding(gap);
    }

    // Skybox visibility (pressed = on, same affordance as the 2D toggle).
    {
        auto btn = std::make_shared<SButton>();
        btn->Padding = FMargin(8.0f * scale, 3.0f * scale);
        btn->VAlign = EVerticalAlignment::Center;
        btn->NormalColor = skybox_visible ? kBtnSelected : kBtnNormal;
        btn->HoverColor = skybox_visible ? kBtnSelectedHover : kBtnHover;
        btn->PressedColor = skybox_visible ? kBtnSelected : kBtnNormal;
        btn->SetContent(MakeText("Skybox", 13.0f * scale, kBtnText));
        btn->OnClicked = [skybox_visible]() {
            RenderSystem* rs = GET_SYSTEM(RenderSystem);
            if (rs != nullptr)
                rs->SetSkyboxVisible(ViewportType::scene, !skybox_visible);
        };
        bar->AddSlot(btn).AutoSize().SetVAlign(EVerticalAlignment::Center).SetPadding(gap);
    }

    // Stretch spacer pushes the right-hand tools to the far edge.
    bar->AddSlot(std::make_shared<SSpacer>(Vector2(0.0f, 0.0f))).Fill(1.0f);

    {
        auto cam = std::make_shared<SButton>();
        cam->Padding = FMargin(8.0f * scale, 3.0f * scale);
        cam->VAlign = EVerticalAlignment::Center;
        cam->NormalColor = kBtnNormal;
        cam->HoverColor = kBtnHover;
        cam->PressedColor = kBtnNormal;
        cam->SetContent(MakeText("Scene Camera", 13.0f * scale, kBtnText));
        cam->OnClicked = [this, scale]() {
            // Toggle the settings panel anchored below the button.
            if (m_CameraPanelOpen)
            {
                m_CameraPanelOpen = false;
                return;
            }
            m_Popup.Close();
            const FGeometry g = m_CameraButton ? m_CameraButton->GetCachedGeometry() : FGeometry();
            m_CameraPanelAnchor = Vector2(g.AbsolutePosition.x, g.AbsolutePosition.y + g.LocalSize.y + 2.0f);
            BuildCameraPanel(scale);
            m_CameraPanelInput.Reset();
            m_CameraPanelOpen = true;
        };
        m_CameraButton = cam;
        bar->AddSlot(cam).AutoSize().SetVAlign(EVerticalAlignment::Center).SetPadding(gap);
    }

    bar->AddSlot(std::make_shared<SSpacer>(Vector2(0.0f, 0.0f)))
        .AutoSize()
        .SetPadding(FMargin(0.0f, 0.0f, 5.0f * scale, 0.0f));

    auto bg = std::make_shared<SBorder>();
    bg->BackgroundColor = kToolbarBg;
    bg->HAlign = EHorizontalAlignment::Fill;
    bg->VAlign = EVerticalAlignment::Fill;
    bg->SetContent(bar);
    m_Toolbar = bg;
}

// ----------------------------------------------------------------------------
// Popups
// ----------------------------------------------------------------------------

void ZSlateSceneWindow::OpenContextMenu(const Vector2& anchor, float scale)
{
    m_CameraPanelOpen = false;
    m_Popup.Open(anchor, scale, [](SMenu& menu, float s) {
        menu.MinWidth = 180.0f * s;
        auto scene_manager = GET_SYSTEM(EditorSceneManager);
        const bool has_selection = scene_manager->getSelectedObjectID() != k_invalid_gobject_id;
        if (has_selection)
        {
            menu.AddItem("Focus", []() { GET_SYSTEM(EditorSceneManager)->FocusSelectedGObject(); }, s);
            menu.AddItem("Delete", []() { GET_SYSTEM(EditorSceneManager)->OnDeleteSelectedGObject(); }, s);
            menu.AddItem("Clear Selection", []() {
                GET_SYSTEM(EditorSceneManager)->OnGObjectSelected(k_invalid_gobject_id);
            }, s);
            menu.AddSeparator(s);
        }

        const bool show_skybox = GET_SYSTEM(RenderSystem)->IsSkyboxVisible(ViewportType::scene);
        menu.AddCheckItem("Skybox", show_skybox, []() {
            const bool cur = GET_SYSTEM(RenderSystem)->IsSkyboxVisible(ViewportType::scene);
            GET_SYSTEM(RenderSystem)->SetSkyboxVisible(ViewportType::scene, !cur);
        }, s);
        menu.AddSeparator(s);
        menu.AddItem("Save Scene    Ctrl+S", []() { GET_SYSTEM(WorldManager)->SaveCurrentLevel(); }, s);
        menu.AddItem("Save Scene As...", []() { GET_SYSTEM(EditorSceneManager)->SaveActiveSceneAsDialog(); }, s);
        menu.AddItem("Reload Current Level", []() {
            GET_SYSTEM(RenderSystem)->ClearForLevelReloading();
            GET_SYSTEM(WorldManager)->ReloadCurrentLevel();
            GET_SYSTEM(RenderSystem)->ResubmitActiveLevelRenderers();
            GET_SYSTEM(EditorSceneManager)->OnGObjectSelected(k_invalid_gobject_id);
        }, s);
    });
}

// ----------------------------------------------------------------------------
// Scene Camera settings panel (ZSlate floating panel)
// ----------------------------------------------------------------------------

void ZSlateSceneWindow::BuildCameraPanel(float scale)
{
    auto column = std::make_shared<SVerticalBox>();

    std::shared_ptr<RenderCamera> cam = GET_SYSTEM(EditorSceneManager)->getEditorCamera();
    EditorInputManager* input_manager = GET_SYSTEM(EditorInputManager);

    const float label_w = 130.0f * scale;
    const float row_pad = 4.0f * scale;
    const float font = 13.0f * scale;

    auto add_header = [&](const char* text) {
        column->AddSlot(MakeText(text, 14.0f * scale, kValueColor))
            .AutoSize()
            .SetPadding(FMargin(0.0f, row_pad, 0.0f, row_pad));
    };

    // A label + control row; `control` fills the remaining width.
    auto add_row = [&](const std::string& label, const std::shared_ptr<SWidget>& control) {
        auto row = std::make_shared<SHorizontalBox>();
        auto label_box = std::make_shared<SBox>();
        label_box->WidthOverride = label_w;
        label_box->VAlign = EVerticalAlignment::Center;
        label_box->SetContent(MakeText(label, font, kLabelColor));
        row->AddSlot(label_box).AutoSize().SetVAlign(EVerticalAlignment::Center);
        row->AddSlot(control).Fill(1.0f).SetVAlign(EVerticalAlignment::Center);
        column->AddSlot(row).AutoSize().SetPadding(FMargin(0.0f, row_pad * 0.5f, 0.0f, row_pad * 0.5f));
    };

    add_header("Scene Camera");

    // --- Field of View (slider + live value) --------------------------------
    if (cam)
    {
        const float fov = cam->getFOV().x;
        const float fov_min = RenderCamera::MIN_FOV;
        const float fov_max = RenderCamera::MAX_FOV;

        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.0f", fov);
        auto value_text = MakeText(buf, font, kValueColor);
        value_text->Alignment = TextAnchor::MiddleRight;

        auto slider = std::make_shared<SSlider>();
        slider->Height = 16.0f * scale;
        slider->MinDesiredWidth = 90.0f * scale;
        slider->Value = (fov_max > fov_min) ? (fov - fov_min) / (fov_max - fov_min) : 0.0f;
        std::weak_ptr<RenderCamera> cam_weak = cam;
        slider->OnValueChanged = [cam_weak, fov_min, fov_max, value_text](float t) {
            auto c = cam_weak.lock();
            if (!c)
                return;
            const float v = fov_min + t * (fov_max - fov_min);
            c->setFOVx(v);
            char b[32];
            std::snprintf(b, sizeof(b), "%.0f", v);
            value_text->Text = b;
        };

        auto hb = std::make_shared<SHorizontalBox>();
        hb->AddSlot(slider).Fill(1.0f).SetVAlign(EVerticalAlignment::Center);
        auto vbox = std::make_shared<SBox>();
        vbox->WidthOverride = 36.0f * scale;
        vbox->VAlign = EVerticalAlignment::Center;
        vbox->SetContent(value_text);
        hb->AddSlot(vbox).AutoSize().SetVAlign(EVerticalAlignment::Center).SetPadding(FMargin(6.0f * scale, 0.0f, 0.0f, 0.0f));
        add_row("Field of View", hb);
    }

    // --- Toggles -------------------------------------------------------------
    {
        auto cb = std::make_shared<SCheckBox>();
        cb->BoxSize = 16.0f * scale;
        cb->Checked = m_SceneCameraDynamicClipping;
        cb->OnCheckStateChanged = [this](bool v) { m_SceneCameraDynamicClipping = v; };
        add_row("Dynamic Clipping", cb);
    }

    // --- Clipping planes (numeric entry; ignored while Dynamic Clipping is on,
    // matching the old ImGui popup's BeginDisabled gate). Near -> m_Znear,
    // Far -> m_Zfar. Far is kept strictly greater than Near. -----------------
    if (cam)
    {
        const bool dim = m_SceneCameraDynamicClipping;
        const UIColor entry_text = dim ? UIColor(0.55f, 0.56f, 0.60f, 1.0f) : UIColor(0.92f, 0.93f, 0.96f, 1.0f);

        auto near_box = std::make_shared<SEditableTextBox>();
        auto far_box = std::make_shared<SEditableTextBox>();
        near_box->FontSize = font;
        far_box->FontSize = font;
        near_box->MinWidth = 90.0f * scale;
        far_box->MinWidth = 90.0f * scale;
        near_box->TextColor = entry_text;
        far_box->TextColor = entry_text;

        char nb[32];
        char fb[32];
        std::snprintf(nb, sizeof(nb), "%.4g", cam->m_Znear);
        std::snprintf(fb, sizeof(fb), "%.5g", cam->m_Zfar);
        near_box->Text = nb;
        far_box->Text = fb;

        std::weak_ptr<RenderCamera> cam_weak = cam;
        std::weak_ptr<SEditableTextBox> near_weak = near_box;
        std::weak_ptr<SEditableTextBox> far_weak = far_box;
        auto refresh = [cam_weak, near_weak, far_weak]() {
            auto c = cam_weak.lock();
            if (!c)
                return;
            if (auto n = near_weak.lock())
            {
                char b[32];
                std::snprintf(b, sizeof(b), "%.4g", c->m_Znear);
                n->Text = b;
            }
            if (auto f = far_weak.lock())
            {
                char b[32];
                std::snprintf(b, sizeof(b), "%.5g", c->m_Zfar);
                f->Text = b;
            }
        };

        near_box->OnTextCommitted = [this, cam_weak, refresh](const std::string& s) {
            auto c = cam_weak.lock();
            if (!c)
                return;
            char* end = nullptr;
            const float v = std::strtof(s.c_str(), &end);
            if (m_SceneCameraDynamicClipping || end == s.c_str())
            {
                refresh();  // snap stale / rejected text back to the live value
                return;
            }
            const float near_clip = std::max(v, 0.0001f);
            c->m_Znear = near_clip;
            c->m_Zfar = std::max(c->m_Zfar, near_clip + 0.0001f);
            refresh();
        };
        far_box->OnTextCommitted = [this, cam_weak, refresh](const std::string& s) {
            auto c = cam_weak.lock();
            if (!c)
                return;
            char* end = nullptr;
            const float v = std::strtof(s.c_str(), &end);
            if (m_SceneCameraDynamicClipping || end == s.c_str())
            {
                refresh();
                return;
            }
            const float near_clip = std::max(c->m_Znear, 0.0001f);
            c->m_Znear = near_clip;
            c->m_Zfar = std::max(v, near_clip + 0.0001f);
            refresh();
        };

        add_header("Clipping Planes");
        add_row("Near", near_box);
        add_row("Far", far_box);
    }

    {
        auto cb = std::make_shared<SCheckBox>();
        cb->BoxSize = 16.0f * scale;
        cb->Checked = m_SceneCameraOcclusionCulling;
        cb->OnCheckStateChanged = [this](bool v) { m_SceneCameraOcclusionCulling = v; };
        add_row("Occlusion Culling", cb);
    }

    add_header("Navigation");

    if (input_manager != nullptr)
    {
        {
            auto cb = std::make_shared<SCheckBox>();
            cb->BoxSize = 16.0f * scale;
            cb->Checked = input_manager->isCameraEasingEnabled();
            cb->OnCheckStateChanged = [](bool v) {
                EditorInputManager* im = GET_SYSTEM(EditorInputManager);
                if (im != nullptr)
                    im->setCameraEasingEnabled(v);
            };
            add_row("Camera Easing", cb);
        }
        {
            auto cb = std::make_shared<SCheckBox>();
            cb->BoxSize = 16.0f * scale;
            cb->Checked = input_manager->isCameraAccelerationEnabled();
            cb->OnCheckStateChanged = [](bool v) {
                EditorInputManager* im = GET_SYSTEM(EditorInputManager);
                if (im != nullptr)
                    im->setCameraAccelerationEnabled(v);
            };
            add_row("Camera Acceleration", cb);
        }

        // Camera speed slider + live value.
        {
            const float speed = input_manager->getCameraSpeed();
            const float smin = input_manager->getCameraSpeedMin();
            const float smax = input_manager->getCameraSpeedMax();

            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.3g", speed);
            auto value_text = MakeText(buf, font, kValueColor);
            value_text->Alignment = TextAnchor::MiddleRight;

            auto slider = std::make_shared<SSlider>();
            slider->Height = 16.0f * scale;
            slider->MinDesiredWidth = 90.0f * scale;
            slider->Value = (smax > smin) ? (speed - smin) / (smax - smin) : 0.0f;
            slider->OnValueChanged = [smin, smax, value_text](float t) {
                EditorInputManager* im = GET_SYSTEM(EditorInputManager);
                if (im == nullptr)
                    return;
                const float v = smin + t * (smax - smin);
                im->SetCameraSpeed(v);
                char b[32];
                std::snprintf(b, sizeof(b), "%.3g", v);
                value_text->Text = b;
            };

            auto hb = std::make_shared<SHorizontalBox>();
            hb->AddSlot(slider).Fill(1.0f).SetVAlign(EVerticalAlignment::Center);
            auto vbox = std::make_shared<SBox>();
            vbox->WidthOverride = 40.0f * scale;
            vbox->VAlign = EVerticalAlignment::Center;
            vbox->SetContent(value_text);
            hb->AddSlot(vbox).AutoSize().SetVAlign(EVerticalAlignment::Center).SetPadding(FMargin(6.0f * scale, 0.0f, 0.0f, 0.0f));
            add_row("Camera Speed", hb);
        }

        // Speed range (numeric entry). SetCameraSpeedRange clamps both ends and
        // keeps max > min, so each box commits with the live value of the other.
        {
            auto min_box = std::make_shared<SEditableTextBox>();
            auto max_box = std::make_shared<SEditableTextBox>();
            min_box->FontSize = font;
            max_box->FontSize = font;
            min_box->MinWidth = 90.0f * scale;
            max_box->MinWidth = 90.0f * scale;

            char mn[32];
            char mx[32];
            std::snprintf(mn, sizeof(mn), "%.3g", input_manager->getCameraSpeedMin());
            std::snprintf(mx, sizeof(mx), "%.3g", input_manager->getCameraSpeedMax());
            min_box->Text = mn;
            max_box->Text = mx;

            std::weak_ptr<SEditableTextBox> min_weak = min_box;
            std::weak_ptr<SEditableTextBox> max_weak = max_box;
            auto refresh = [min_weak, max_weak]() {
                EditorInputManager* im = GET_SYSTEM(EditorInputManager);
                if (im == nullptr)
                    return;
                if (auto n = min_weak.lock())
                {
                    char b[32];
                    std::snprintf(b, sizeof(b), "%.3g", im->getCameraSpeedMin());
                    n->Text = b;
                }
                if (auto x = max_weak.lock())
                {
                    char b[32];
                    std::snprintf(b, sizeof(b), "%.3g", im->getCameraSpeedMax());
                    x->Text = b;
                }
            };

            min_box->OnTextCommitted = [refresh](const std::string& s) {
                EditorInputManager* im = GET_SYSTEM(EditorInputManager);
                if (im == nullptr)
                    return;
                char* end = nullptr;
                const float v = std::strtof(s.c_str(), &end);
                if (end != s.c_str())
                    im->SetCameraSpeedRange(v, im->getCameraSpeedMax());
                refresh();
            };
            max_box->OnTextCommitted = [refresh](const std::string& s) {
                EditorInputManager* im = GET_SYSTEM(EditorInputManager);
                if (im == nullptr)
                    return;
                char* end = nullptr;
                const float v = std::strtof(s.c_str(), &end);
                if (end != s.c_str())
                    im->SetCameraSpeedRange(im->getCameraSpeedMin(), v);
                refresh();
            };

            add_row("Speed Min", min_box);
            add_row("Speed Max", max_box);
        }
    }

    auto panel = std::make_shared<SBox>();
    panel->WidthOverride = 320.0f * scale;
    auto border = std::make_shared<SBorder>();
    border->BackgroundColor = UIColor(0.16f, 0.16f, 0.18f, 0.98f);
    border->Padding = FMargin(10.0f * scale);
    border->HAlign = EHorizontalAlignment::Fill;
    border->VAlign = EVerticalAlignment::Top;
    border->SetContent(column);
    panel->SetContent(border);
    m_CameraPanel = panel;
}

// ----------------------------------------------------------------------------
// Camera preview overlay (native chrome + RenderSystem composite)
// ----------------------------------------------------------------------------

void ZSlateSceneWindow::UpdateCameraPreviewOverlay(const UIRect& work_rect,
                                                   float viewport_origin_x,
                                                   float viewport_origin_y)
{
    m_CameraPreviewVisible = false;

    if (work_rect.width <= 0.0f || work_rect.height <= 0.0f)
    {
        GET_SYSTEM(RenderSystem)->ClearCameraPreview();
        return;
    }

    std::shared_ptr<GameObject> selected_object = GET_SYSTEM(EditorSceneManager)->GetSelectedGObject().lock();
    CameraComponent* selected_camera_component = nullptr;
    if (selected_object)
    {
        for (auto component_ptr : selected_object->getComponents())
        {
            selected_camera_component = dynamic_cast<CameraComponent*>(static_cast<Component*>(component_ptr));
            if (selected_camera_component != nullptr)
                break;
        }
    }

    if (selected_camera_component == nullptr)
    {
        GET_SYSTEM(RenderSystem)->ClearCameraPreview();
        return;
    }

    EngineContentViewport game_viewport = GET_SYSTEM(RenderSystem)->GetViewport(ViewportType::game);
    const float preview_aspect = game_viewport.width > 0.0f && game_viewport.height > 0.0f
                                   ? game_viewport.width / game_viewport.height
                                   : 16.0f / 9.0f;
    const float preview_width = std::min(240.0f, work_rect.width * 0.38f);
    const float preview_height = preview_width / std::max(preview_aspect, 0.01f);
    const float padding = 10.0f;

    const float px = work_rect.x + work_rect.width - preview_width - padding;
    const float py = work_rect.y + padding;
    if (px < work_rect.x + padding || py + preview_height > work_rect.y + work_rect.height - padding)
    {
        GET_SYSTEM(RenderSystem)->ClearCameraPreview();
        return;
    }

    if (!m_SelectedCameraPreviewCamera)
        m_SelectedCameraPreviewCamera = std::make_shared<RenderCamera>();
    selected_camera_component->ApplyToGameRenderCamera(*m_SelectedCameraPreviewCamera);
    m_SelectedCameraPreviewCamera->SetAspect(preview_aspect);

    EngineContentViewport preview_viewport;
    preview_viewport.x = px - viewport_origin_x;
    preview_viewport.y = py - viewport_origin_y;
    preview_viewport.width = preview_width;
    preview_viewport.height = preview_height;
    GET_SYSTEM(RenderSystem)->SetCameraPreview(m_SelectedCameraPreviewCamera, preview_viewport,
                                               std::string(selected_object->name.c_str()));

    m_CameraPreviewFrame = UIRect(px, py, preview_width, preview_height);
    m_CameraPreviewVisible = true;
    m_CameraPreviewTitle = selected_object->name.empty() ? "Camera Preview" : selected_object->name.c_str();
}

void ZSlateSceneWindow::UpdateViewportForWorkRect(const UIRect& work_rect)
{
    if (work_rect.width <= 0.0f || work_rect.height <= 0.0f)
    {
        ClearViewport();
        return;
    }

    const ZSlate::EditorSlateHost& host = ZSlate::EditorSlateHost::Get();
    const Vector2 fb_scale = host.GetFramebufferScale();
    const Vector2 display_pos = host.GetDisplayPos();

    Vector2 render_target_window_pos;
    render_target_window_pos.x = (work_rect.x - display_pos.x) * fb_scale.x;
    render_target_window_pos.y = (work_rect.y - display_pos.y) * fb_scale.y;

    const float pixel_width = work_rect.width * fb_scale.x;
    const float pixel_height = work_rect.height * fb_scale.y;

    GET_SYSTEM(RenderSystem)
        ->UpdateViewport(m_ViewportId, render_target_window_pos.x, render_target_window_pos.y, pixel_width, pixel_height);
}

// ----------------------------------------------------------------------------
// Cross-window drag-drop consumption (asset placement / object reparent)
// ----------------------------------------------------------------------------

void ZSlateSceneWindow::HandleDragDropDrop(bool chrome_capturing, const UIRect& work_rect)
{
    // Cache the in-flight op while a drag is published. On the release frame the
    // source router may have already cleared the channel (draw-order dependent),
    // so we rely on this cached copy rather than re-reading the channel.
    if (auto active = ZSlate::GetActiveDragOperation())
        m_PendingDragOp = active;

    ZSlate::EditorSlateHost& host = ZSlate::EditorSlateHost::Get();
    const Vector2 mouse = host.GetPointerPos();
    const bool left_down = host.IsLeftDown();
    const bool release = host.WasLeftReleasedThisFrame();

    if (release && m_PendingDragOp != nullptr && !chrome_capturing && work_rect.width > 0.0f && work_rect.height > 0.0f &&
        work_rect.Contains(mouse))
    {
        // The object under the cursor becomes the drop parent (invalid = root).
        const Vector2 picked_uv((mouse.x - work_rect.x) / work_rect.width, (mouse.y - work_rect.y) / work_rect.height);
        const GObjectID parent_id = GET_SYSTEM(EditorSceneManager)->PickGObjectAtViewportUv(picked_uv);

        const std::string& type = m_PendingDragOp->PayloadType;
        if (type == EditorDragDrop::kZSlateAssetPayloadAssetPath)
        {
            auto* asset_op = dynamic_cast<FAssetDragDropOp*>(m_PendingDragOp.get());
            if (asset_op != nullptr && !asset_op->AssetPath.empty())
                EditorScenePlacement::RequestDrop(std::filesystem::path(asset_op->AssetPath), parent_id);
        }
        else if (type == EditorDragDrop::kZSlateAssetPayloadGObjectId)
        {
            const auto dragged = static_cast<GObjectID>(m_PendingDragOp->Id);
            if (dragged != parent_id)
            {
                Level* lvl = GET_SYSTEM(WorldManager)->getCurrentActiveLevel();
                if (lvl != nullptr)
                    EditorHierarchyReparent::Reparent(lvl, dragged, parent_id);
            }
        }
    }

    // Gesture finished (button up): drop the cache so a stale op can't fire later.
    if (!left_down)
        m_PendingDragOp = nullptr;
}

// ----------------------------------------------------------------------------
// Navigation / picking (input polling, unchanged behaviour)
// ----------------------------------------------------------------------------

void ZSlateSceneWindow::HandleSceneGizmoDrag(bool chrome_capturing, const UIRect& work_rect)
{
    EditorSceneManager* scene_manager = GET_SYSTEM(EditorSceneManager);
    if (scene_manager == nullptr || chrome_capturing || work_rect.width <= 0.0f || work_rect.height <= 0.0f)
    {
        return;
    }

    ZSlate::EditorSlateHost& host = ZSlate::EditorSlateHost::Get();
    const Vector2 mouse = host.GetPointerPos();
    const int surface_id = ZSlate::EditorSlateHost::HashId(m_Title);
    const bool is_hovered =
        work_rect.Contains(mouse) && host.IsSurfaceHovered(surface_id, mouse);

    const Vector2 viewport_pos(work_rect.x, work_rect.y);
    const Vector2 viewport_size(work_rect.width, work_rect.height);
    if (is_hovered && !m_IsDraggingGizmo)
    {
        const size_t axis_under_cursor =
            scene_manager->UpdateCursorOnAxis(mouse, viewport_pos, viewport_size);
        if (axis_under_cursor != 3)
        {
            GET_SYSTEM(RenderSystem)->SetSelectedAxis(axis_under_cursor);
        }
    }

    if (host.WasLeftPressedThisFrame() && is_hovered)
    {
        m_GizmoDragAxis = scene_manager->PickAxisAtViewportPixels(mouse, viewport_pos, viewport_size);
        if (m_GizmoDragAxis != 3)
        {
            m_IsDraggingGizmo = true;
            m_GizmoDragLastMouse = mouse;
        }
    }

    if (m_IsDraggingGizmo && host.IsLeftDown())
    {
        const Vector2 delta = mouse - m_GizmoDragLastMouse;
        if (delta.squaredLength() > 0.0f)
        {
            scene_manager->MoveEntity(mouse.x,
                                      mouse.y,
                                      m_GizmoDragLastMouse.x,
                                      m_GizmoDragLastMouse.y,
                                      viewport_pos,
                                      viewport_size,
                                      m_GizmoDragAxis,
                                      scene_manager->getSelectedObjectMatrix());
            m_GizmoDragLastMouse = mouse;
            scene_manager->DrawSelectedEntityAxis();
        }
    }

    if (!host.IsLeftDown())
    {
        m_IsDraggingGizmo = false;
        m_GizmoDragAxis = 3;
    }
}

void ZSlateSceneWindow::HandleSceneViewNavigation(bool chrome_capturing, const UIRect& work_rect)
{
    RHI* rhi = GET_SYSTEM(RHI);
    if (rhi == nullptr || rhi->getGraphicsAPI() != GraphicsAPI::DirectX12)
        return;

    if (work_rect.width <= 0.0f || work_rect.height <= 0.0f)
        return;

    ZSlate::EditorSlateHost& host = ZSlate::EditorSlateHost::Get();
    const Vector2 mouse = host.GetPointerPos();
    const int surface_id = ZSlate::EditorSlateHost::HashId(m_Title);

    const bool is_hovered = !chrome_capturing && work_rect.Contains(mouse) && host.IsSurfaceHovered(surface_id, mouse);

    if (host.IsAltDown())
    {
        if (auto* app = GET_SYSTEM(WindowSystem)->GetApplication())
            app->SetCursorMode(false);
    }

    const bool scene_view_2d =
        GET_SYSTEM(EditorSceneManager) != nullptr && GET_SYSTEM(EditorSceneManager)->IsSceneView2D();

    const bool right_pressed = host.IsRightDown() && !m_PrevRightDown;
    const bool middle_pressed = host.IsMiddleDown() && !m_PrevMiddleDown;

    if (is_hovered && !m_IsPanningSceneView && right_pressed && !scene_view_2d)
        m_IsRotatingSceneView = true;
    if (is_hovered && !m_IsPanningSceneView && right_pressed && scene_view_2d)
    {
        m_IsPanningSceneView = true;
        m_IsAltLeftPanningSceneView = false;
    }
    if (is_hovered && !m_IsRotatingSceneView && middle_pressed)
    {
        m_IsPanningSceneView = true;
        m_IsAltLeftPanningSceneView = false;
    }
    if (is_hovered && !m_IsRotatingSceneView && host.IsAltDown() && host.WasLeftPressedThisFrame())
    {
        m_IsPanningSceneView = true;
        m_IsAltLeftPanningSceneView = true;
    }

    const float mouse_wheel = host.GetWheelDelta();
    if (is_hovered && mouse_wheel != 0.0f)
        ZoomSceneViewCamera(GET_SYSTEM(EditorSceneManager)->getEditorCamera(), mouse_wheel);

    const Vector2 mouse_delta = host.GetPointerDelta();
    const bool is_alt_left_pan_down = m_IsAltLeftPanningSceneView && host.IsLeftDown();
    const bool is_middle_pan_down = !m_IsAltLeftPanningSceneView && host.IsMiddleDown();

    if ((mouse_delta.x != 0.0f || mouse_delta.y != 0.0f) && (m_IsRotatingSceneView || m_IsPanningSceneView))
    {
        std::shared_ptr<RenderCamera> editor_camera = GET_SYSTEM(EditorSceneManager)->getEditorCamera();
        if (editor_camera)
        {
            if (m_IsRotatingSceneView && host.IsRightDown())
            {
                const float angular_velocity = 180.0f / std::max(work_rect.width, work_rect.height);
                editor_camera->Rotate(Vector2(mouse_delta.y, mouse_delta.x) * angular_velocity);
                m_TrackContextClick = false;
            }
            else if (m_IsPanningSceneView && (is_middle_pan_down || is_alt_left_pan_down))
            {
                PanSceneViewCamera(editor_camera, mouse_delta, work_rect.height);
            }
        }
    }

    if (m_IsRotatingSceneView && !host.IsRightDown())
        m_IsRotatingSceneView = false;

    const bool is_keyboard_move_down = (m_IsRotatingSceneView && host.IsRightDown()) ||
                                       (is_hovered && (host.IsRightDown() || host.IsLeftDown()));
    if (is_keyboard_move_down)
        MoveSceneViewCameraWithKeyboard(GET_SYSTEM(EditorSceneManager)->getEditorCamera());

    if (m_IsPanningSceneView && !is_middle_pan_down && !is_alt_left_pan_down)
    {
        m_IsPanningSceneView = false;
        m_IsAltLeftPanningSceneView = false;
    }
}

void ZSlateSceneWindow::PanSceneViewCamera(const std::shared_ptr<RenderCamera>& editor_camera,
                                           const Vector2& mouse_delta,
                                           float viewport_height) const
{
    if (!editor_camera || viewport_height <= 1.0f)
        return;

    float world_per_pixel = 0.0f;
    if (editor_camera->IsOrthographic())
    {
        world_per_pixel = 2.0f * editor_camera->GetOrthoHalfHeight() / viewport_height;
    }
    else
    {
        const float camera_height = Math::max(Math::abs(editor_camera->position().z), 1.0f);
        const float fovy_radians = Math::DegreesToRadians(Math::max(editor_camera->getFOV().y, 1.0f));
        world_per_pixel = 2.0f * camera_height * std::tan(fovy_radians * 0.5f) / viewport_height;
    }

    const float speed_multiplier = ZSlate::EditorSlateHost::Get().IsShiftDown() ? 4.0f : 1.0f;
    Vector3 pan_delta = editor_camera->right() * (-mouse_delta.x * world_per_pixel * speed_multiplier) +
                        editor_camera->up() * (mouse_delta.y * world_per_pixel * speed_multiplier);
    editor_camera->move(pan_delta);
}

void ZSlateSceneWindow::ZoomSceneViewCamera(const std::shared_ptr<RenderCamera>& editor_camera, float wheel_delta) const
{
    if (!editor_camera)
        return;

    const float speed_multiplier = ZSlate::EditorSlateHost::Get().IsShiftDown() ? 4.0f : 1.0f;
    if (editor_camera->IsOrthographic())
    {
        editor_camera->AdjustOrthoHalfHeight(wheel_delta * speed_multiplier);
        return;
    }

    const float camera_height = Math::max(Math::abs(editor_camera->position().z), 1.0f);
    const float dolly_distance = wheel_delta * Math::max(camera_height * 0.08f, 0.2f) * speed_multiplier;
    editor_camera->move(editor_camera->forward() * dolly_distance);
}

void ZSlateSceneWindow::MoveSceneViewCameraWithKeyboard(const std::shared_ptr<RenderCamera>& editor_camera) const
{
    const ZSlate::EditorSlateHost& host = ZSlate::EditorSlateHost::Get();
    if (host.IsNativeTextInputActive() || host.IsForegroundCapturing())
        return;

    WindowSystem* window_system = GET_SYSTEM(WindowSystem);
    if (!editor_camera || window_system == nullptr || window_system->GetNativeWindowHandle() == nullptr)
        return;

    Vector3 camera_delta(0.0f, 0.0f, 0.0f);
    auto* app = window_system->GetApplication();
    if (!app) return;
    const bool scene_view_2d =
        GET_SYSTEM(EditorSceneManager) != nullptr && GET_SYSTEM(EditorSceneManager)->IsSceneView2D();
    if (scene_view_2d)
    {
        if (app->IsKeyDown(KeyCodes::KEY_W))
            camera_delta += editor_camera->up();
        if (app->IsKeyDown(KeyCodes::KEY_S))
            camera_delta -= editor_camera->up();
    }
    else
    {
        if (app->IsKeyDown(KeyCodes::KEY_W))
            camera_delta += editor_camera->forward();
        if (app->IsKeyDown(KeyCodes::KEY_S))
            camera_delta -= editor_camera->forward();
    }
    if (app->IsKeyDown(KeyCodes::KEY_A))
        camera_delta -= editor_camera->right();
    if (app->IsKeyDown(KeyCodes::KEY_D))
        camera_delta += editor_camera->right();

    if (camera_delta.squaredLength() <= 0.0f)
        return;

    const float speed_multiplier = ZSlate::EditorSlateHost::Get().IsShiftDown() ? 4.0f : 1.0f;
    editor_camera->move(camera_delta.normalisedCopy() *
                        GET_SYSTEM(EditorInputManager)->getCameraSpeed() * speed_multiplier);
}

void ZSlateSceneWindow::HandleContextMenu(bool chrome_capturing, const UIRect& work_rect, float scale)
{
    if (work_rect.width <= 0.0f || work_rect.height <= 0.0f)
        return;

    ZSlate::EditorSlateHost& host = ZSlate::EditorSlateHost::Get();
    const Vector2 mouse = host.GetPointerPos();
    const int surface_id = ZSlate::EditorSlateHost::HashId(m_Title);

    const bool is_hovered = !chrome_capturing && work_rect.Contains(mouse) && host.IsSurfaceHovered(surface_id, mouse);

    const bool right_pressed = host.IsRightDown() && !m_PrevRightDown;
    const bool right_released = !host.IsRightDown() && m_PrevRightDown;

    if (!m_IsRotatingSceneView && is_hovered && right_pressed)
    {
        m_TrackContextClick = true;
        m_ContextClickPos = mouse;
    }

    if (m_TrackContextClick && right_released)
    {
        const Vector2 delta(mouse.x - m_ContextClickPos.x, mouse.y - m_ContextClickPos.y);
        const bool is_click = (delta.x * delta.x + delta.y * delta.y) <= 16.0f;
        m_TrackContextClick = false;

        if (!m_IsRotatingSceneView && is_click && work_rect.Contains(m_ContextClickPos))
        {
            Vector2 picked_uv((m_ContextClickPos.x - work_rect.x) / work_rect.width,
                              (m_ContextClickPos.y - work_rect.y) / work_rect.height);
            const GObjectID picked_gobject_id = GET_SYSTEM(EditorSceneManager)->PickGObjectAtViewportUv(picked_uv);
            if (picked_gobject_id != k_invalid_gobject_id)
            {
                const GObjectSelectionOp selection_op =
                    host.IsCtrlDown() ? GObjectSelectionOp::Toggle : GObjectSelectionOp::Replace;
                GET_SYSTEM(EditorSceneManager)->OnGObjectSelected(picked_gobject_id, selection_op);
            }
            OpenContextMenu(m_ContextClickPos, scale);
        }
    }
    else if (m_TrackContextClick && !host.IsRightDown())
    {
        m_TrackContextClick = false;
    }
}

// ----------------------------------------------------------------------------
// OnGUI
// ----------------------------------------------------------------------------

void ZSlateSceneWindow::OnGUI()
{
    ZSlate::EditorSlateHost& host = ZSlate::EditorSlateHost::Get();
    float ui_scale = host.GetUiScale();
    if (ui_scale < 0.5f)
        ui_scale = 1.0f;

    // Rebuild the toolbar when scale / axis mode / 2D state changes.
    EditorSceneManager* scene_manager = GET_SYSTEM(EditorSceneManager);
    const int axis_mode = scene_manager != nullptr ? static_cast<int>(scene_manager->getEditorAxisMode()) : 0;
    const bool scene_view_2d = scene_manager != nullptr && scene_manager->IsSceneView2D();
    RenderSystem* render_system = GET_SYSTEM(RenderSystem);
    const bool skybox_visible =
        render_system != nullptr && render_system->IsSkyboxVisible(ViewportType::scene);
    if (m_Toolbar == nullptr || ui_scale != m_BuiltToolbarScale || axis_mode != m_BuiltAxisMode ||
        scene_view_2d != m_BuiltSceneView2D || skybox_visible != m_BuiltSkyboxVisible)
    {
        BuildToolbar(ui_scale);
        m_BuiltToolbarScale = ui_scale;
        m_BuiltAxisMode = axis_mode;
        m_BuiltSceneView2D = scene_view_2d;
        m_BuiltSkyboxVisible = skybox_visible;
        m_ToolbarInput.Reset();
    }

    // ---- Panel geometry -----------------------------------------------------
    float pos_x = 0.0f;
    float pos_y = 0.0f;
    float avail_w = 0.0f;
    float avail_h = 0.0f;
    // Native dock hosting is unconditional: ReconcileNativeTreeWithOpenWindows (run
    // before any panel OnGUI) guarantees an open window is in the dock tree, so the
    // leaf rect always comes from NativeRect().
    {
        const float* native_rect = NativeRect();
        pos_x = native_rect[0];
        pos_y = native_rect[1];
        avail_w = native_rect[2];
        avail_h = native_rect[3];
    }
    if (avail_w < 1.0f)
        avail_w = 1.0f;
    if (avail_h < 1.0f)
        avail_h = 1.0f;

    m_Toolbar->CacheDesiredSize();
    const float toolbar_h = std::min(avail_h, std::max(m_Toolbar->GetDesiredSize().y, 26.0f * ui_scale));

    UIRect toolbar_region {};
    UIRect work_rect {};
    UIRect panel_region(pos_x, pos_y, avail_w, avail_h);
    float viewport_origin_x = 0.0f;
    float viewport_origin_y = 0.0f;
    UIRect viewport_rect {};

    {
        toolbar_region = UIRect(pos_x, pos_y, avail_w, toolbar_h);
        work_rect = UIRect(pos_x, pos_y + toolbar_h, avail_w, std::max(1.0f, avail_h - toolbar_h));
        viewport_origin_x = host.GetDisplayPos().x;
        viewport_origin_y = host.GetDisplayPos().y;
        viewport_rect = UIRect(viewport_origin_x, viewport_origin_y, host.GetDisplaySize().x, host.GetDisplaySize().y);
    }

    // Report the scene viewport rect (composited under this panel, below toolbar).
    UpdateViewportForWorkRect(work_rect);

    UpdateCameraPreviewOverlay(work_rect, viewport_origin_x, viewport_origin_y);

    auto* viewport = GET_SYSTEM(RHI)->GetViewport(m_ViewportId);
    GET_SYSTEM(EditorInputManager)->setEngineWindowPos(Vector2(viewport->x, viewport->y));
    GET_SYSTEM(EditorInputManager)->setEngineWindowSize(Vector2(viewport->width, viewport->height));

    // ---- Input snapshot -----------------------------------------------------
    const Vector2 mouse = host.GetPointerPos();
    const bool left_down = host.IsLeftDown();
    const float wheel = host.GetWheelDelta();
    const bool over_toolbar = toolbar_region.width > 0.0f && toolbar_region.height > 0.0f && toolbar_region.Contains(mouse);

    const FGeometry toolbar_geom(Vector2(toolbar_region.x, toolbar_region.y),
                                 Vector2(toolbar_region.width, toolbar_region.height));

    // ---- Paint chrome through the native overlay (P9: the only renderer) ----
    auto& overlay = ZSlate::ZSlateEditorOverlay::Get();

    overlay.BeginWindowGroup(ZSlate::ZSlateEditorOverlay::kZPanel);
    UIRenderer* renderer = &overlay.GetRenderer();

    {
        // Editor grid + backdrop (composited under the toolbar; 3D scene renders beneath).
        if (work_rect.width > 1.0f && work_rect.height > 1.0f)
        {
            renderer->pushClipRect(work_rect, true);
            EditorSceneManager* scene_manager = GET_SYSTEM(EditorSceneManager);
            const std::shared_ptr<RenderCamera> editor_camera =
                scene_manager ? scene_manager->getEditorCamera() : nullptr;
            DrawEditorGridOverlay(*renderer, work_rect, editor_camera);
            if (scene_manager)
            {
                DrawTransformGizmoOverlay(*renderer, work_rect, editor_camera, *scene_manager);
            }
            renderer->popClipRect();
        }

        // Camera preview chrome (live texture is composited by RenderSystem).
        if (m_CameraPreviewVisible)
        {
            const UIRect& pr = m_CameraPreviewFrame;
            renderer->drawRect(pr, UIColor(1.0f, 1.0f, 1.0f, 0.71f), 2.0f);
            renderer->drawQuad(UIRect(pr.x, pr.y, pr.width, 22.0f), UIColor(0.0f, 0.0f, 0.0f, 0.67f));
            renderer->drawText(UIRect(pr.x + 6.0f, pr.y + 4.0f, pr.width - 12.0f, 18.0f), m_CameraPreviewTitle,
                               13.0f, UIColor(0.90f, 0.90f, 0.90f, 1.0f), TextAnchor::MiddleLeft);
        }

        // Toolbar.
        if (m_Toolbar)
        {
            m_Toolbar->CacheDesiredSize();
            FPaintContext ctx;
            ctx.Renderer = renderer;
            ctx.LayerId = 0;
            renderer->pushClipRect(toolbar_region, true);
            m_Toolbar->Paint(ctx, toolbar_geom);
            renderer->popClipRect();
        }

        // Scene Camera settings panel (clamped on-screen).
        if (m_CameraPanelOpen && m_CameraPanel)
        {
            m_CameraPanel->CacheDesiredSize();
            const Vector2 size = m_CameraPanel->GetDesiredSize();
            float px = std::min(m_CameraPanelAnchor.x, viewport_rect.x + viewport_rect.width - size.x);
            float py = std::min(m_CameraPanelAnchor.y, viewport_rect.y + viewport_rect.height - size.y);
            px = std::max(px, viewport_rect.x);
            py = std::max(py, viewport_rect.y);

            FPaintContext ctx;
            ctx.Renderer = renderer;
            ctx.LayerId = 1;
            m_CameraPanel->Paint(ctx, FGeometry(Vector2(px, py), size));
        }

        // Popup (context menu) on top of everything else.
        if (m_Popup.IsOpen())
            m_Popup.Render(*renderer, mouse, left_down, over_toolbar ? 0.0f : wheel, viewport_rect, 2);
    }

    // Register this panel for native hit-testing (after paint, like other native panels).
    const int surface_id = ZSlate::EditorSlateHost::HashId(m_Title);
    host.BeginSurface(surface_id, panel_region, ZSlate::ESurfaceLayer::Panels);

    // ---- Route input --------------------------------------------------------
    m_ToolbarInput.ProcessMouse(m_Toolbar, mouse, over_toolbar, left_down, 0.0f);

    bool panel_capturing = false;
    if (m_CameraPanelOpen && m_CameraPanel)
    {
        const FGeometry& g = m_CameraPanel->GetCachedGeometry();
        const bool over_panel = g.IsUnderLocation(mouse);
        panel_capturing = over_panel;
        m_CameraPanelInput.ProcessMouse(m_CameraPanel, mouse, over_panel, left_down, 0.0f);

        if (m_CameraPanelInput.HasKeyboardFocus())
        {
            for (unsigned int ch : host.GetCharsThisFrame())
                m_CameraPanelInput.ProcessChar(ch);
            // UE pattern: route ALL keys to the focused widget
            for (EKey key : host.GetKeysThisFrame())
                m_CameraPanelInput.ProcessKey(key);
        }

        const bool left_edge = host.WasLeftPressedThisFrame();
        const bool over_cam_btn = m_CameraButton && m_CameraButton->GetCachedGeometry().IsUnderLocation(mouse);
        if (left_edge && !over_panel && !over_cam_btn)
            m_CameraPanelOpen = false;
    }

    const bool popup_open = m_Popup.IsOpen();
    const bool popup_capturing = popup_open;
    const bool chrome_capturing = over_toolbar || popup_capturing || panel_capturing || m_CameraPanelOpen;

    HandleDragDropDrop(chrome_capturing, work_rect);
    HandleSceneGizmoDrag(chrome_capturing, work_rect);
    HandleSceneViewNavigation(chrome_capturing, work_rect);
    HandleContextMenu(chrome_capturing, work_rect, ui_scale);

    m_PrevLeftDown = left_down;
    m_PrevRightDown = host.IsRightDown();
    m_PrevMiddleDown = host.IsMiddleDown();
}
