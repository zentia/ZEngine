#pragma once

// ----------------------------------------------------------------------------
// ZSlateEditorOverlay -- native RHI backend for editor ZSlate (P1, M1).
//
// Owns a shared BatchedUIRenderer that editor ZSlate windows can paint into,
// plus the UI pipeline + GPU buffers (ported from the runtime UIPass) used to
// submit the recorded batch onto the editor's UI target. This replaces the
// per-window SlateImGuiRenderer (ImGui draw lists) with a direct RHI draw.
//
// Backend target differences are handled by the pipeline's render pass:
//   - DX12   : render_pass == nullptr  => PSO targets the swapchain format
//              (DX12RHI::ApplyPsoRenderTargetFormats default branch).
//   - Vulkan : the editor main-camera render pass + UI subpass.
// The draw itself is backend-agnostic (RHICommandBuffer* + Cmd*PFN).
//
// Gated behind the CVar r.ZSlate.NativeBackend (default 1). See
// doc/ui_system/ZSLATE_NATIVE_BACKEND_PLAN.md.
// ----------------------------------------------------------------------------

#include "Runtime/Function/Render/Interface/RHI.h"
#include "Runtime/UI/Render/BatchedUIRenderer.h"
#include "Runtime/UI/Render/UIRenderBatch.h"

#include "ZSlate/Backend/SlateUIRendererBackend.h"  // ISlateRenderer

#include "Runtime/UI/Core/UITypes.h"  // UIRect

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace ZSlate
{
class ZSlateEditorOverlay : public ISlateRenderer
{
public:
    static ZSlateEditorOverlay& Get();

    // Reads the r.ZSlate.NativeBackend CVar (registers it lazily on first call).
    bool IsNativeBackendEnabled();

    // r.ZSlate.NativeSelfTest CVar (default 0): records the M1 canary quad/label
    // into the shared batch each frame so the GPU path stays provable even when
    // no real ZSlate window is on screen.
    bool IsSelfTestEnabled();

    // r.ZSlate.NativeMenuBar CVar (default 1): render the editor's main menu bar
    // (File / Window / ...) as a ZSlate-native widget through the shared
    // BatchedUIRenderer instead of ImGui's BeginMenuBar/BeginMenu. M3 step.
    bool IsNativeMenuBarEnabled();

    // P8: the native ZSlate dock space (splitters / tab strips / panel frames) is no longer
    // a separate toggle -- it is the only dock whenever the native backend is on. Callers
    // key off IsNativeBackendEnabled(); the ImGui DockSpace remains only as the
    // r.ZSlate.NativeBackend 0 full-ImGui fallback. See doc/ui_system/ZSLATE_NATIVE_DOCK_PLAN.md.

    // Shared renderer editor ZSlate windows paint into when the native backend
    // is enabled (M2 wiring). Painted geometry is consumed by DrawBatch().
    // Returns the active renderer: the main overlay batch by default, or a
    // floating window's batch while one is pushed (editor tear-off). Panels call
    // this generically, so a torn-off panel paints into its own window's batch
    // without any per-panel change.
    BatchedUIRenderer& GetRenderer() { return m_CurrentRenderer != nullptr ? *m_CurrentRenderer : m_Renderer; }

    // Editor tear-off: redirect GetRenderer() to a floating window's renderer for
    // the duration of that window's panel paint. Push before the panel's OnGUI,
    // Pop after. Not nested in practice (one floating window painted at a time).
    void PushRenderer(BatchedUIRenderer* renderer) { m_CurrentRenderer = renderer; }
    void PopRenderer() { m_CurrentRenderer = nullptr; }

    // P10b z-order layers for BeginWindowGroup. Panels are tiled by the native
    // dock and never overlap, so they share one layer -- DrawBatch's stable_sort
    // then preserves their paint (registration) order, matching the ImGui
    // NoBringToFrontOnFocus creation order it replaced. Foreground chrome (menu
    // dropdowns, dock drag preview) composites strictly on top.
    // Dock panel-area backgrounds (the opaque fill behind each docked panel's
    // content) composite STRICTLY BELOW the panel widget trees. The dock chrome
    // is recorded later in the frame than the panels, so without a dedicated
    // sub-panel layer the (opaque) panel-area fill would sort on top of panel
    // content and blank every body. This mirrors UE Slate, where a panel paints
    // its background brush at a lower LayerId than its content within the same
    // widget tree.
    static constexpr int kZDockBackground = -100;
    static constexpr int kZPanel = 0;
    static constexpr int kZForeground = 1000;

    // M4 / P10b: open a per-window command group before a window paints its
    // widget tree (and its popups) into the shared renderer. z_order is an
    // explicit layer (kZPanel / kZForeground); DrawBatch submits groups sorted
    // ascending by z_order (stable, so equal-z groups keep insertion order). The
    // group implicitly ends at the next BeginWindowGroup (or the end of the
    // batch), so no explicit End is needed.
    void BeginWindowGroup(int z_order);

    // Clears the shared batch; call once before windows paint each frame.
    void ResetBatch();

    // Frame-start entry point used by EditorUIPass right before WindowUI::PreRender:
    // no-op unless the native backend is on, otherwise clears the batch, captures
    // ImGui's DisplaySize for the NDC mapping, and records the self-test if enabled.
    void BeginFrameIfEnabled();

    bool HasContent() const { return !m_Renderer.getBatch().empty(); }

    // Build the UI pipeline once. render_pass == nullptr => DX12 swapchain
    // overlay format; a real pass + subpass => Vulkan editor UI subpass.
    void EnsurePipeline(RHI* rhi, RHIRenderPass* render_pass, uint32_t subpass);

    // Upload + draw the shared batch onto the currently-bound editor UI target.
    void DrawBatch(RHI* rhi);

    // Editor tear-off: upload + draw an EXTERNAL batch (recorded by a floating
    // panel window's own BatchedUIRenderer) onto the currently-bound floating
    // surface RTV (the caller wraps this in DX12RHI::Begin/EndFloatingSurfaceDraw).
    // `key` is a caller-stable identity (the floating window) used to look up a
    // dedicated per-window GPU buffer ring, so multiple floating windows don't
    // clobber each other within a frame. The batch is drawn in natural order
    // (no window groups) into a (0,0)..(width,height) coordinate space.
    // ReleaseFloatingRing frees that ring when the window closes.
    void DrawExternalBatchToFloatingSurface(RHI* rhi,
                                            const void* key,
                                            const UIRenderBatch& batch,
                                            uint32_t width,
                                            uint32_t height);
    void ReleaseFloatingRing(RHI* rhi, const void* key);

    // Dev self-test (M1): records a fixed quad + label into the shared batch so
    // the GPU path can be validated before any real window is wired.
    void RecordSelfTest();

    void Destroy(RHI* rhi);

    // --- ISlateRenderer implementation (forward to m_Renderer with type conversion) ---
    void DrawQuad(const UIRect& rect, const ZSlate::UIColor& color) override;
    void DrawRect(const UIRect& rect, const ZSlate::UIColor& color, float thickness = 1.0f) override;
    void DrawConvexPoly(const Vector2* points, int count, const ZSlate::UIColor& color) override;
    void DrawRoundedRect(const UIRect& rect, float radius, const ZSlate::UIColor& color) override;
    void DrawTexturedQuad(const UIRect& rect, void* texture_handle, const ZSlate::UIColor& tint = Colors::White) override;
    void DrawBox(const UIRect& rect, const FMargin& margin, void* texture_handle, const ZSlate::UIColor& tint) override;
    void DrawBorder(const UIRect& rect, const FMargin& margin, void* texture_handle, const ZSlate::UIColor& tint) override;

    void DrawText(const UIRect& rect, const std::string& text, float font_size, const ZSlate::UIColor& color,
                 TextAnchor alignment = TextAnchor::MiddleLeft, TextWrapMode wrap = TextWrapMode::NoWrap,
                 void* font_handle = nullptr) override;
    void DrawText(const std::string& text, const Vector2& pos, float font_size, const ZSlate::UIColor& color) override;
    Vector2 MeasureText(const std::string& text, float font_size) const override;

    void PushClipRect(const UIRect& rect) override;
    void PopClipRect() override;

    void BeginFrame() override;
    void EndFrame() override;
    void Flush() override;

private:
    void EnsureGpuBuffers(RHI* rhi, size_t vertex_count, size_t index_count);
    void DestroyGpuBuffers(RHI* rhi);
    void UploadBatch(RHI* rhi, float display_width, float display_height, float display_pos_x,
                     float display_pos_y);

    BatchedUIRenderer m_Renderer;

    // Active renderer redirect for editor tear-off (see PushRenderer). nullptr =
    // main overlay batch (m_Renderer). Points at a floating window's batch while
    // that window's panel is being painted.
    BatchedUIRenderer* m_CurrentRenderer {nullptr};

    // ImGui DisplaySize captured at frame start. ZSlate windows record draws in
    // ImGui screen coordinates, so the NDC mapping must divide by the same space
    // (logical) rather than the physical framebuffer size, which differ under DPI
    // scaling. 0 => fall back to the renderer's framebuffer display size.
    float m_EditorDisplayWidth {0.0f};
    float m_EditorDisplayHeight {0.0f};

    // Per-window command groups (M4). first_command indexes the shared batch's
    // command vector; each group runs until the next group's first_command (or
    // the end of the batch). zorder_key is the host window's ImDrawList*.
    struct WindowGroup
    {
        int z_order {0};
        uint32_t first_command {0};
    };
    std::vector<WindowGroup> m_Groups;

    RHIPipelineLayout* m_Layout {nullptr};
    RHIPipeline* m_Pipeline {nullptr};
    bool m_PipelineReady {false};

    // The overlay's vertex/index buffers are HOST_VISIBLE and re-uploaded every
    // frame. With multiple frames in flight (DX12 keeps 3) a single buffer would be
    // overwritten by the CPU while a previous frame's GPU draw is still reading it --
    // invisible when the UI is static (identical bytes) but a visible flicker when the
    // scene-grid / text reproject (e.g. dragging the viewport). Ring one buffer set
    // per frame-in-flight, indexed by RHI::GetCurrentFrameIndex(); the RHI fences each
    // slot before reuse, so overwriting the current slot is race-free.
    static constexpr int kOverlayFrameRing = 3;
    RHIBuffer* m_VertexBuffer[kOverlayFrameRing] {};
    RHIDeviceMemory* m_VertexMemory[kOverlayFrameRing] {};
    RHIBuffer* m_IndexBuffer[kOverlayFrameRing] {};
    RHIDeviceMemory* m_IndexMemory[kOverlayFrameRing] {};
    size_t m_VertexCapacity[kOverlayFrameRing] {};
    size_t m_IndexCapacity[kOverlayFrameRing] {};
    int m_FrameSlot {0};

    std::vector<UIVertex> m_CpuVertices;

    // Per-floating-window GPU buffer rings (editor tear-off). Keyed by the
    // floating window identity; each entry is a 3-deep ring just like the main
    // overlay buffers (same frames-in-flight rationale -- see kOverlayFrameRing).
    struct FloatingRing
    {
        RHIBuffer* vertex_buffer[kOverlayFrameRing] {};
        RHIDeviceMemory* vertex_memory[kOverlayFrameRing] {};
        RHIBuffer* index_buffer[kOverlayFrameRing] {};
        RHIDeviceMemory* index_memory[kOverlayFrameRing] {};
        size_t vertex_capacity[kOverlayFrameRing] {};
        size_t index_capacity[kOverlayFrameRing] {};
    };
    std::unordered_map<const void*, FloatingRing> m_FloatingRings;
};
}  // namespace ZSlate
