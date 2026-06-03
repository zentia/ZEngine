#pragma once

// ----------------------------------------------------------------------------
// FloatingPanelManager -- editor tear-off (true OS multi-window).
//
// Detaches a docked editor panel into its own top-level OS window, rendered by a
// dedicated DX12 floating swapchain (DX12RHI::CreateFloatingSurface) and painted
// by the panel's normal OnGUI into a per-window BatchedUIRenderer batch. The main
// editor window is unaffected: the panel is removed from the native dock tree
// (DefaultLayout::SetPanelFloating) and re-docked when the floating window closes.
//
// Threading (mirrors the main overlay's game-build / render-draw split):
//   - TickMainThread()  : main/game thread. Processes detach requests (create
//                         child window + floating surface), polls close + resize.
//                         GLFW window ops MUST run here (main thread).
//   - BuildBatches()    : game thread. Paints each floating panel into its own
//                         batch. Call right after the main overlay PreRender.
//   - DrawSurfaces(rhi) : render thread. Draws each batch onto its swapchain
//                         (command list open). Call after the main DrawBatch.
//
// V1 is Windows / DX12 only (the floating swapchain lives in DX12RHI). On other
// platforms RequestFloat is a logged no-op.
// ----------------------------------------------------------------------------

#include "Runtime/Core/Math/Vector2.h"

#include <memory>
#include <string>
#include <vector>

class EditorUI;
class DefaultLayout;
class RHI;

class FloatingPanelManager
{
public:
    static FloatingPanelManager& Get();

    // Resolve the EditorUI / layout. Idempotent; call once during editor startup.
    void Initialize(EditorUI* editor_ui);
    // Tear down all floating windows + surfaces (call before EditorUI destruction).
    void Shutdown();

    // Detach the named docked panel into its own OS window on the next
    // TickMainThread. No-op if already floating or the title is unknown. The
    // window geometry is seeded from the panel's current dock rect.
    void RequestFloat(const std::string& title);

    // Same, but with an explicit spawn rect in DESKTOP coords (used by tab-drag
    // tear-off so the window appears under the cursor, and by session restore).
    void RequestFloatAt(const std::string& title, int x, int y, int width, int height);

    // Re-dock a floating panel back into the main window (closes its OS window).
    void RequestDock(const std::string& title);

    bool IsFloating(const std::string& title) const;
    bool HasFloatingPanels() const;

    void TickMainThread();
    void BuildBatches();
    void DrawSurfaces(RHI* rhi);

    FloatingPanelManager(const FloatingPanelManager&) = delete;
    FloatingPanelManager& operator=(const FloatingPanelManager&) = delete;

private:
    FloatingPanelManager();
    ~FloatingPanelManager();

    struct FloatingPanel;  // defined in the .cpp (keeps GLFW / DX12 out of the header)

    // A queued detach request; rect (desktop coords) is used when has_rect, else
    // the panel's current dock rect seeds the window.
    struct PendingFloat
    {
        std::string title;
        bool has_rect {false};
        int x {0};
        int y {0};
        int width {0};
        int height {0};
    };

    DefaultLayout* ResolveLayout() const;
    void ProcessPendingFloat();
    void ProcessClosedWindows();

    // UE-style window chrome built from real ZSlate widgets (SWindowFrame =
    // SWindowTitleBar + SResizeGrip). EnsureChrome lazily builds the widget tree
    // and wires its intent delegates the first time a panel is painted; the
    // delegates below turn user intent into GLFW window ops. Because BuildBatches
    // runs on the main thread (PrepareGameThreadFrame, guarded against the
    // render-thread Draw path) these may call GLFW directly -- no deferral.
    void EnsureChrome(FloatingPanel& panel);
    void BeginCaptionDrag(FloatingPanel& panel, const Vector2& pos_px);
    void UpdateCaptionDrag(FloatingPanel& panel, const Vector2& pos_px);
    void EndCaptionDrag(FloatingPanel& panel, const Vector2& pos_px);
    void UpdateResize(FloatingPanel& panel, const Vector2& pos_px);
    void ToggleMaximize(FloatingPanel& panel);
    void RestoreFromMaximize(FloatingPanel& panel);
    void DestroyPanel(FloatingPanel& panel, bool redock);

    // Persist / restore the floating set across sessions (rapidjson file under
    // <Project>/saved/config/floating_panels.json). SaveState snapshots live
    // window geometry; LoadState queues RequestFloatAt for each saved entry.
    void SaveState() const;
    void LoadState();

    EditorUI* m_EditorUi {nullptr};
    std::vector<std::unique_ptr<FloatingPanel>> m_Panels;
    std::vector<PendingFloat> m_PendingFloat;
    std::vector<std::string> m_PendingDock;
    bool m_StateLoaded {false};

    // Main editor window rect (desktop coords), captured at the top of
    // BuildBatches BEFORE any per-panel input override is pushed -- the override
    // would otherwise make EditorSlateHost::GetDisplayPos/Size report the
    // floating window instead. Used by the caption-drag re-dock hit-test.
    float m_MainRect[4] {0.0f, 0.0f, 0.0f, 0.0f};
};
