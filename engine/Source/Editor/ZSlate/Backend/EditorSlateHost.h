#pragma once

// ----------------------------------------------------------------------------
// EditorSlateHost -- native editor windowing + input layer (P10).
//
// A WindowSystem(GLFW)-sourced input bus + native display metrics that lets
// fully-native ZSlate editor panels read pointer / keyboard / scale / scale
// WITHOUT ImGui::GetIO(). Mirrors the runtime UISystem input path
// (engine/Source/Runtime/UI/UISystem.cpp). It coexists with ImGui -- which
// still hosts the remaining ImGui-widget islands (Scene grid, shader editor,
// preview controls, modal popups) and still consumes GLFW via ImGui_ImplGlfw --
// until the ImGui backend is torn out in a later phase.
//
// Coordinate space: GetPointerPos() returns ABSOLUTE screen coordinates
// (window-client origin + cursor offset). This matches ImGui's io.MousePos
// under ImGuiConfigFlags_ViewportsEnable (which the editor enables), so the
// native pointer aligns with panel geometry cached in ImGui screen space and
// with the overlay's DisplayPos-relative NDC mapping in ZSlateEditorOverlay.
//
// P10c: native input is now unconditional for native panels (the transitional
// r.ZSlate.NativeInput CVar was retired). See
// doc/ui_system/ZSLATE_NATIVE_DOCK_PLAN.md (P10).
// ----------------------------------------------------------------------------

#include "Runtime/Core/Math/Vector2.h"
#include "Runtime/Slate/Core/SlateKeys.h"
#include "Runtime/UI/Core/UITypes.h"  // UIRect

#include "Runtime/Function/Render/Platform/Generic/GenericWindow.h"

#include <vector>

namespace ZSlate
{
enum class EMouseCursor
{
    Default,
    Hand,
    ResizeEW,
    ResizeNS,
    ResizeNWSE,  // diagonal (top-left <-> bottom-right), used by the resize grip
};

// Surface stack layers (P10b). Higher layer occludes lower for hit-testing, so a
// Foreground dropdown/popup over a Panel makes the panel report not-hovered --
// the native equivalent of ImGui's popup/window focus eating the click.
enum class ESurfaceLayer
{
    Panels = 0,      // tiled dock leaves; never overlap each other
    Floating = 1,    // (reserved) future floating/torn-off panels
    Foreground = 2,  // menu dropdowns, dock drag preview, context popups
};

// Editor tear-off (P5): a self-contained snapshot of one floating window's input
// + display metrics for a single frame. While the FloatingPanelManager paints a
// floating panel it pushes one of these (PushInputOverride) so every panel input
// accessor below returns the FLOATING window's pointer/buttons/keys/scale instead
// of the main window's -- with zero per-panel change. Coordinates are the floating
// window's CLIENT-LOCAL space (origin 0,0), matching how the panel is placed
// (DefaultLayout::BeginFloatingPanelRender gives it a (0,0,w,h) rect). The struct
// is owned by the manager and lives for the duration of the panel's paint.
struct FloatingInputState
{
    GenericWindow* window {nullptr};  // for SetMouseCursor routing
    Vector2 pointer {0.0f, 0.0f};
    Vector2 pointer_delta {0.0f, 0.0f};
    bool left_down {false};
    bool right_down {false};
    bool middle_down {false};
    bool ctrl {false};
    bool shift {false};
    bool alt {false};
    bool left_pressed {false};
    bool left_double {false};
    bool left_released {false};
    float wheel {0.0f};
    std::vector<unsigned int> chars;
    std::vector<EKey> keys;
    float display_w {0.0f};
    float display_h {0.0f};
    float display_x {0.0f};
    float display_y {0.0f};
    float ui_scale {1.0f};
};

class EditorSlateHost
{
public:
    static EditorSlateHost& Get();

    // Subscribe to WindowSystem GLFW callbacks + capture the display content
    // scale. Idempotent; call once on the main thread during editor startup
    // (after WindowSystem is up). Registering the callbacks off the main thread
    // would race the GLFW fan-out vectors, so this is NOT done lazily.
    void Initialize();

    // P10c: always true now (the transitional r.ZSlate.NativeInput CVar was
    // retired). Native ZSlate editor panels unconditionally read input / scale /
    // metrics / hit-testing from this host and are hosted without ImGui::Begin.
    // Kept as a method so the remaining coexistence gates read intentionally.
    bool IsNativeInputEnabled();

    // Snapshot the per-frame event accumulators (wheel / chars / keys). Call
    // once per UI frame AFTER glfwPollEvents and BEFORE panels paint -- driven
    // by ZSlateEditorOverlay::BeginFrameIfEnabled so ordering + thread match the
    // panel paint. Pointer / button / modifier state is level-tracked live in
    // the GLFW callbacks and needs no snapshot.
    void NewFrame();

    // --- Input accessors (stable for the duration of the frame) ---
    // Each transparently returns the active floating window's input when an override
    // is pushed (editor tear-off, P5), otherwise the main window's.
    const Vector2& GetPointerPos() const { return m_Override != nullptr ? m_Override->pointer : m_PointerScreen; }
    bool IsLeftDown() const { return m_Override != nullptr ? m_Override->left_down : m_LeftDown; }
    bool IsRightDown() const { return m_Override != nullptr ? m_Override->right_down : m_RightDown; }
    bool IsMiddleDown() const { return m_Override != nullptr ? m_Override->middle_down : m_MiddleDown; }
    float GetWheelDelta() const { return m_Override != nullptr ? m_Override->wheel : m_FrameWheel; }
    const Vector2& GetPointerDelta() const { return m_Override != nullptr ? m_Override->pointer_delta : m_PointerDelta; }

    // Left-button press edge this frame (up->down transition), the native
    // replacement for ImGui::IsMouseClicked(Left). Computed in NewFrame from PRESS
    // events accumulated in the GLFW mouse-button callback, so a click is caught
    // even if released the same frame (matching ImGui's event-queue semantics).
    bool WasLeftPressedThisFrame() const { return m_Override != nullptr ? m_Override->left_pressed : m_FrameLeftPressed; }
    // Left-button double-click this frame, the native replacement for
    // ImGui::IsMouseDoubleClicked(Left): two PRESS events within kDoubleClickTime
    // seconds and kDoubleClickMaxDist px of each other (ImGui's defaults).
    bool WasLeftDoubleClickedThisFrame() const { return m_Override != nullptr ? m_Override->left_double : m_FrameLeftDoubleClicked; }
    // Left-button release edge this frame (down->up transition).
    bool WasLeftReleasedThisFrame() const { return m_Override != nullptr ? m_Override->left_released : m_FrameLeftReleased; }
    bool IsCtrlDown() const { return m_Override != nullptr ? m_Override->ctrl : m_CtrlDown; }
    bool IsShiftDown() const { return m_Override != nullptr ? m_Override->shift : m_ShiftDown; }
    bool IsAltDown() const { return m_Override != nullptr ? m_Override->alt : m_AltDown; }
    const std::vector<unsigned int>& GetCharsThisFrame() const { return m_Override != nullptr ? m_Override->chars : m_FrameChars; }
    const std::vector<EKey>& GetKeysThisFrame() const { return m_Override != nullptr ? m_Override->keys : m_FrameKeys; }

    // Native display metrics (replaces ImGui io.DisplaySize / GetFontSize()/16 and
    // GetMainViewport()->WorkPos/WorkSize). Display size is the logical window size
    // (screen coords), matching ImGui's DisplaySize under GLFW. Display pos is the
    // window client origin in desktop coords, matching the main viewport Pos under
    // ViewportsEnable -- popup rects clamp against (pos, size). UI scale matches
    // EditorUI's content_scale.
    Vector2 GetDisplaySize() const
    {
        return m_Override != nullptr ? Vector2(m_Override->display_w, m_Override->display_h) : Vector2(m_WindowW, m_WindowH);
    }
    Vector2 GetDisplayPos() const
    {
        return m_Override != nullptr ? Vector2(m_Override->display_x, m_Override->display_y) : Vector2(m_WindowX, m_WindowY);
    }
    float GetUiScale() const { return m_Override != nullptr ? m_Override->ui_scale : m_UiScale; }

    // Logical window size -> framebuffer pixel scale (for composited viewports).
    Vector2 GetFramebufferScale() const;

    // Monotonic seconds (glfwGetTime). Replaces ImGui::GetTime for native UI.
    static double GetTime();

    // Native mouse cursor (replaces ImGui::SetMouseCursor on the dock chrome path).
    void SetMouseCursor(EMouseCursor cursor);

    // --- Surface stack (P10b) ---
    // Stable surface id from a panel title (FNV-1a 32). Panels register + query
    // with HashId(m_Title) so each tiled dock leaf gets a distinct id.
    static int HashId(const char* name);

    // Register a surface for this frame. `id` is a caller-stable identity (e.g. a
    // window id); rects are in the same absolute screen space as GetPointerPos.
    // Cleared each NewFrame. Registration order within a layer is the stacking
    // order (later registered occludes earlier), matching paint order.
    void BeginSurface(int id, const UIRect& rect, ESurfaceLayer layer);

    // Editor tear-off (P5): redirect every input accessor + the surface hit-test
    // stack to a floating window's per-frame snapshot for the duration of that
    // window's panel paint. While pushed, BeginSurface registers into a separate
    // override stack (so the main window's hit-test is untouched) and HoveredSurface
    // / IsForegroundCapturing query that stack. Push before the panel's BeginGUI,
    // Pop after. `state` must outlive the scope. Not nested in practice (one
    // floating window painted at a time).
    void PushInputOverride(const FloatingInputState* state);
    void PopInputOverride();
    bool IsInputOverridden() const { return m_Override != nullptr; }

    // Topmost surface id whose rect contains `point` (highest layer wins; ties
    // broken by latest registration). Returns -1 when no surface is hit.
    int HoveredSurface(const Vector2& point) const;

    // Same query, but against the PREVIOUS frame's surface registrations. The
    // editor draws the dock chrome (MenuController, which runs HandleNativeDockInput)
    // BEFORE the panels/islands register their surfaces for the current frame, so
    // the chrome must hit-test the last fully-populated stack. The 1-frame lag is
    // harmless (panel rects barely move frame-to-frame) and matches ImGui's own
    // previous-frame hover model that the old IsWindowHovered gate relied on.
    int HoveredSurfacePrev(const Vector2& point) const;

    // Convenience: is `id` the topmost surface under `point`? This is the native
    // replacement for ImGui::IsItemHovered() on a panel canvas -- it returns
    // false when a Foreground surface (open dropdown / popup) occludes the panel.
    bool IsSurfaceHovered(int id, const Vector2& point) const { return HoveredSurface(point) == id; }

    // True if any Foreground-layer surface (menu dropdown / popup / drag preview)
    // is registered this frame. This is the native input-capture flag (P10c): it
    // replaces the old "##ZSlateMenuCapture" full-viewport modal ImGui window that
    // ate click-through while an overlay-drawn dropdown was open. Native panels are
    // already occluded by the Foreground surface via IsSurfaceHovered; this query
    // lets any remaining ImGui-island consumer skip input while a menu is open.
    bool IsForegroundCapturing() const;

    // True while any native SEditableTextBox (Console command bar, Inspector fields,
    // etc.) owns keyboard focus this frame. Reset in NewFrame(); panels call
    // NotifyNativeTextInputActive() from OnGUI when SlateInputRouter::HasKeyboardFocus().
    void NotifyNativeTextInputActive() { m_NativeTextInputActive = true; }
    bool IsNativeTextInputActive() const { return m_NativeTextInputActive; }

private:
    EditorSlateHost() = default;

    bool m_Initialized {false};
    bool m_NativeTextInputActive {false};

    // Live pointer / button / modifier state. Written in GLFW callbacks on the
    // main thread, read by panels (possibly on the render thread). Lock-free,
    // matching the runtime UISystem -- a torn float read is harmless for input.
    Vector2 m_PointerScreen {0.0f, 0.0f};
    bool m_LeftDown {false};
    bool m_RightDown {false};
    bool m_MiddleDown {false};
    bool m_CtrlDown {false};
    bool m_ShiftDown {false};
    bool m_AltDown {false};

    // Event accumulators (filled in callbacks) + per-frame snapshot (NewFrame).
    float m_PendingWheel {0.0f};
    std::vector<unsigned int> m_PendingChars;
    std::vector<EKey> m_PendingKeys;
    float m_FrameWheel {0.0f};
    std::vector<unsigned int> m_FrameChars;
    std::vector<EKey> m_FrameKeys;

    // Left-button click-edge + double-click accumulation (filled in the GLFW
    // mouse-button callback, snapshotted in NewFrame). m_LastLeftPress* persist
    // across frames for the double-click time/distance test.
    int m_PendingLeftPresses {0};
    int m_PendingLeftDoubleClicks {0};
    bool m_FrameLeftPressed {false};
    bool m_FrameLeftDoubleClicked {false};
    bool m_FrameLeftReleased {false};
    int m_PendingLeftReleases {0};
    double m_LastLeftPressTime {-1.0};
    Vector2 m_LastLeftPressPos {0.0f, 0.0f};
    Vector2 m_PointerDelta {0.0f, 0.0f};
    Vector2 m_PrevPointerScreen {0.0f, 0.0f};

    // Cached logical window size + client origin (desktop coords), refreshed from
    // the window-size / cursor callbacks. m_UiScale (the DPI content scale) is
    // seeded in Initialize() and then re-polled every frame in NewFrame() so it
    // tracks monitor/maximize/DPI changes (see NewFrame for the rationale).
    float m_WindowW {0.0f};
    float m_WindowH {0.0f};
    float m_WindowX {0.0f};
    float m_WindowY {0.0f};
    float m_UiScale {1.0f};

    // Per-frame surface registrations (cleared in NewFrame).
    struct Surface
    {
        int id {-1};
        UIRect rect {};
        ESurfaceLayer layer {ESurfaceLayer::Panels};
    };
    std::vector<Surface> m_Surfaces;
    // Last frame's registrations, snapshotted in NewFrame for HoveredSurfacePrev.
    std::vector<Surface> m_SurfacesPrev;

    // Editor tear-off (P5): active floating-window input override (nullptr = main
    // window). m_OverrideSurfaces is the floating panel's own hit-test stack,
    // cleared on each PushInputOverride and filled by BeginSurface while pushed.
    const FloatingInputState* m_Override {nullptr};
    std::vector<Surface> m_OverrideSurfaces;

    int HoveredIn(const std::vector<Surface>& surfaces, const Vector2& point) const;

    void* m_CursorArrow  {nullptr};
    void* m_CursorHand {nullptr};
    void* m_CursorResizeEw {nullptr};
    void* m_CursorResizeNs {nullptr};
    void* m_CursorResizeNwse {nullptr};
};
}  // namespace ZSlate
