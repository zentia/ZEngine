#pragma once
#include "Runtime/Function/Render/RenderPass.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>

class WindowUI;

struct EditorUIPassInitInfo : RenderPassInitInfo
{
    RHIRenderPass* render_pass {nullptr};
    RHIImageView* ui_layer_color_view {nullptr};
};

class EditorUIPass : public RenderPass
{
public:
    void Initialize(const RenderPassInitInfo* init_info) override final;
    void InitializeUIRenderBackend(WindowUI* window_ui) override final;
    // Call on the game thread before Application::TickOneFrame when parallel rendering is active.
    void PrepareGameThreadFrame();

    void Draw() override final;
    bool isBackendInitialized() const { return m_BackendInitialized; }

    /// True only if the most recent Draw() finished recording the native overlay; the
    /// game-thread frame handshake (FinalizePendingImGuiPlatformFrame) waits on this.
    bool ConsumeImGuiFrameRendered() { return m_ImGuiFrameRendered.exchange(false); }

    // Game thread: block until the RHI worker finishes recording the editor UI for the frame.
    void WaitForImGuiRenderComplete();
    bool IsImGuiFrameRendered() const;
    bool WaitForImGuiRenderCompleteFor(std::chrono::milliseconds timeout);

    // RHI thread: signal that the editor UI draw finished (including early-out paths).
    void SignalImGuiRenderComplete();

    // Parallel rendering: RHI skipped SubmitDrawLists (swapchain not ready).
    void NotifySkippedRHIFrame();

    // RP2 UI subpass target (backup_even). Refreshed when framebuffers are first ready / resized.
    void RefreshUiLayerTarget(RHIImageView* ui_layer_color_view);

private:
    // RHI thread, inside Draw(): make sure there is a batch to draw. Consumes the
    // game thread's prepared frame if there is one; otherwise records the UI here --
    // but ONLY when parallel rendering is off. With parallel rendering on, the game
    // thread owns PreRender(), so "not prepared" means "the game thread is (about to
    // be) recording it", and recording here too would run two PreRender()s on the
    // same UIRenderBatch at once.
    void ConsumeOrRecordFrameOnRhiThread();

    WindowUI* m_WindowUi {nullptr};
    RHIImageView* m_UiLayerColorView {nullptr};
    bool m_BackendInitialized {false};
    // Set by the game thread (PrepareGameThreadFrame), consumed by the RHI thread
    // (Draw) and cleared by the game thread on a wait timeout -- three writers on
    // two threads, so it must be atomic.
    std::atomic<bool> m_GameThreadFramePrepared {false};
    std::atomic<bool> m_ImGuiFrameRendered {false};
    std::mutex m_ImGuiRenderMutex;
    std::condition_variable m_ImGuiRenderCv;

    void CompletePreparedImGuiFrameOnEarlyOut();
};
