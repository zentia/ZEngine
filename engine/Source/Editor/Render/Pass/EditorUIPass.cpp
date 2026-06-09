#include "EditorUIPass.h"

#include <array>

#include "Editor/FloatingPanel/FloatingPanelManager.h"
#include "Editor/ZSlate/Backend/ZSlateEditorOverlay.h"
#include "Runtime/Resource/Config/ConfigManager.h"
#if defined(_WIN32)
    #include "Runtime/Function/Render/Interface/DX12/DX12RHI.h"
    #include "Runtime/Function/Render/Passes/DX12MainCameraPass.h"
#endif
#include "Runtime/Function/Render/RenderPipelineBase.h"
#include "Runtime/Function/Render/RenderSystem.h"
#include "Runtime/Function/Render/RenderType.h"
#include "Runtime/Function/Render/WindowSystem.h"
#include "Runtime/UI/Core/WindowUI.h"

#if defined(__APPLE__)
struct GLFWwindow;
class WindowUI;
bool EditorMetalUIInitialize(GLFWwindow* window);
bool EditorMetalUIRender(GLFWwindow* window, WindowUI* window_ui);
#endif

REGISTER_FACTORY(RenderPass, EditorUIPass, "EditorUIPass");
void EditorUIPass::Initialize(const RenderPassInitInfo* init_info)
{
    RenderPass::Initialize(nullptr);

    if (init_info != nullptr)
    {
        const auto* editor_init = static_cast<const EditorUIPassInitInfo*>(init_info);
        m_Framebuffer.render_pass = editor_init->render_pass;
        m_UiLayerColorView = editor_init->ui_layer_color_view;
    }
    else
    {
        m_Framebuffer.render_pass = nullptr;
        m_UiLayerColorView = nullptr;
    }
}

void EditorUIPass::InitializeUIRenderBackend(WindowUI* window_ui)
{
    m_WindowUi = window_ui;
    m_BackendInitialized = false;
    if (m_WindowUi == nullptr || m_Rhi == nullptr)
    {
        return;
    }

    // The editor UI is drawn entirely by the native ZSlate overlay (BatchedUIRenderer),
    // so there is no ImGui render backend to initialize here -- "backend ready" just means
    // the RHI device + command machinery the overlay's DrawBatch needs is up. Input flows
    // through EditorSlateHost (WindowSystem GLFW listeners), not ImGui's GLFW backend, so
    // no glfw callback install is needed either.
#if defined(_WIN32)
    if (m_Rhi->getGraphicsAPI() == GraphicsAPI::DirectX12)
    {
        DX12RHI* dx12_rhi = static_cast<DX12RHI*>(m_Rhi);
        if (dx12_rhi == nullptr || dx12_rhi->getDevice() == nullptr || dx12_rhi->getCommandQueue() == nullptr ||
            dx12_rhi->GetCbvSrvUavDescriptorHeap() == nullptr)
        {
            return;
        }
        m_BackendInitialized = true;
        return;
    }
#endif

#if defined(__APPLE__)
    m_BackendInitialized = EditorMetalUIInitialize(GET_SYSTEM(WindowSystem)->GetWindow());
    return;
#elif defined(Z_HAS_VULKAN)
    // Native overlay draws inside the main-camera render pass UI subpass (see Draw()).
    m_BackendInitialized = true;
#else
    // No compiled-in editor UI backend for this platform/config:
    //   - Windows: handled above by the DX12 branch (early-returned)
    //   - macOS:   handled above by the Metal branch (early-returned)
    //   - Vulkan:  not compiled in (Z_HAS_VULKAN off; only used for Android / HarmonyOS targets)
    // Leaving m_BackendInitialized = false; Draw() will be a no-op.
#endif
}

void EditorUIPass::RefreshUiLayerTarget(RHIImageView* ui_layer_color_view)
{
    // Only refresh the view pointer. backup_even is recreated on swapchain resize;
    // ImGui_ImplDX12 device objects do not depend on the RTV handle value.
    // Recreating them here often fails before the first frame and disabled the backend.
    m_UiLayerColorView = ui_layer_color_view;
}

void EditorUIPass::WaitForImGuiRenderComplete()
{
    std::unique_lock<std::mutex> lock(m_ImGuiRenderMutex);
    m_ImGuiRenderCv.wait(lock, [this] { return m_ImGuiFrameRendered.load(std::memory_order_acquire); });
}

bool EditorUIPass::IsImGuiFrameRendered() const
{
    return m_ImGuiFrameRendered.load(std::memory_order_acquire);
}

bool EditorUIPass::WaitForImGuiRenderCompleteFor(std::chrono::milliseconds timeout)
{
    std::unique_lock<std::mutex> lock(m_ImGuiRenderMutex);
    return m_ImGuiRenderCv.wait_for(lock, timeout, [this] { return m_ImGuiFrameRendered.load(std::memory_order_acquire); });
}

void EditorUIPass::SignalImGuiRenderComplete()
{
    m_ImGuiFrameRendered.store(true, std::memory_order_release);
    m_ImGuiRenderCv.notify_all();
}

void EditorUIPass::CompletePreparedImGuiFrameOnEarlyOut()
{
    if (!m_GameThreadFramePrepared)
    {
        return;
    }

    m_GameThreadFramePrepared = false;
    SignalImGuiRenderComplete();
}

void EditorUIPass::NotifySkippedRHIFrame()
{
    CompletePreparedImGuiFrameOnEarlyOut();
    if (!m_ImGuiFrameRendered.load(std::memory_order_acquire))
    {
        SignalImGuiRenderComplete();
    }
}

void EditorUIPass::PrepareGameThreadFrame()
{
    if (m_WindowUi == nullptr || !m_BackendInitialized)
    {
        return;
    }

#if defined(_WIN32)
    if (m_Rhi && m_Rhi->getGraphicsAPI() == GraphicsAPI::DirectX12)
    {
        ZSlate::ZSlateEditorOverlay::Get().BeginFrameIfEnabled();
        m_WindowUi->PreRender();
        // Editor tear-off: paint each floating panel into its own batch on this
        // (game) thread, right after the main overlay batch is built.
        FloatingPanelManager::Get().BuildBatches();
        m_GameThreadFramePrepared = true;
        return;
    }
#endif

#if defined(__APPLE__)
    return;
#elif defined(Z_HAS_VULKAN)
    ZSlate::ZSlateEditorOverlay::Get().BeginFrameIfEnabled();
    m_WindowUi->PreRender();
    FloatingPanelManager::Get().BuildBatches();
    m_GameThreadFramePrepared = true;
#endif
}

void EditorUIPass::Draw()
{
    if (m_WindowUi == nullptr || !m_BackendInitialized)
    {
        return;
    }

#if defined(_WIN32)
    if (m_Rhi && m_Rhi->getGraphicsAPI() == GraphicsAPI::DirectX12)
    {
        if (m_UiLayerColorView == nullptr)
        {
            if (auto render_system = GET_SYSTEM(RenderSystem))
            {
                if (auto render_pipeline = render_system->getRenderPipeline())
                {
                    m_UiLayerColorView = render_pipeline->GetUiLayerColorView();
                }
            }
        }

        DX12RHI* dx12_rhi = static_cast<DX12RHI*>(m_Rhi);
        if (dx12_rhi == nullptr || dx12_rhi->IsDeviceRemoved(" before editor UI draw"))
        {
            CompletePreparedImGuiFrameOnEarlyOut();
            return;
        }

        ID3D12GraphicsCommandList* command_list = dx12_rhi->getCurrentCommandList();
        if (command_list == nullptr)
        {
            CompletePreparedImGuiFrameOnEarlyOut();
            return;
        }

        if (m_GameThreadFramePrepared)
        {
            m_GameThreadFramePrepared = false;
        }
        else
        {
            // Non-parallel path: the game thread did not build the batch, so record it here.
            ZSlate::ZSlateEditorOverlay::Get().BeginFrameIfEnabled();
            m_WindowUi->PreRender();
            FloatingPanelManager::Get().BuildBatches();
        }

        float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        m_Rhi->PushEvent(m_Rhi->GetCurrentCommandBuffer(), "EditorUI", color);

        // After RP2 CmdEndRenderPass: composite is on the swapchain RTV.
        dx12_rhi->BeginSwapchainOverlayDraw();
        // Set the bindless heap as the active CBV/SRV/UAV heap (the native overlay's
        // textured quads sample through it). Falls back to the legacy m_CbvSrvUavHeap
        // on hardware without bindless support.
        if (!dx12_rhi->SetBindlessDescriptorHeaps())
        {
            ID3D12DescriptorHeap* descriptor_heap = dx12_rhi->GetCbvSrvUavDescriptorHeap();
            if (descriptor_heap != nullptr)
            {
                command_list->SetDescriptorHeaps(1, &descriptor_heap);
            }
        }

        // Native ZSlate overlay. On DX12 the swapchain RTV is bound by
        // BeginSwapchainOverlayDraw above; a null render pass yields a
        // swapchain-format PSO. The batch was recorded by ZSlate windows during
        // PreRender (BeginFrameIfEnabled cleared it).
        {
            auto& zslate_overlay = ZSlate::ZSlateEditorOverlay::Get();
            if (zslate_overlay.IsNativeBackendEnabled())
            {
                zslate_overlay.EnsurePipeline(m_Rhi, nullptr, 0);

                zslate_overlay.DrawBatch(m_Rhi);

                // Sky is composited in RP1 deferred (DX12MainCameraPass). A swapchain overlay
                // pass would reinhard-sample HDR again on top of the tonemapped combine output.

                // Editor tear-off: draw each floating panel onto its own swapchain.
                // Runs after the main DrawBatch (pipeline is ready) and is presented
                // by DX12RHI::SubmitRendering after the main swapchain present.
                FloatingPanelManager::Get().DrawSurfaces(m_Rhi);
            }
        }

        // Editor axis gizmo: draw last on the swapchain overlay, above ZSlate UI.
        if (auto render_system = GET_SYSTEM(RenderSystem))
        {
            if (auto render_pipeline = render_system->getRenderPipeline())
            {
                if (auto* dx12_pass = dynamic_cast<DX12MainCameraPass*>(render_pipeline->GetMainCameraPass()))
                {
                    dx12_pass->DrawAxis();
                }
            }
        }

        m_Rhi->PopEvent(m_Rhi->GetCurrentCommandBuffer());
        SignalImGuiRenderComplete();
        return;
    }
#endif

#if defined(__APPLE__)
    (void)EditorMetalUIRender(GET_SYSTEM(WindowSystem)->GetWindow(), m_WindowUi);
    return;
#elif defined(Z_HAS_VULKAN)
    if (m_GameThreadFramePrepared)
    {
        m_GameThreadFramePrepared = false;
    }
    else
    {
        // Non-parallel path: the game thread did not build the batch, so record it here.
        ZSlate::ZSlateEditorOverlay::Get().BeginFrameIfEnabled();
        m_WindowUi->PreRender();
        FloatingPanelManager::Get().BuildBatches();
    }

    float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    m_Rhi->PushEvent(m_Rhi->GetCurrentCommandBuffer(), "EditorUI", color);

    // Native ZSlate overlay. On Vulkan the editor UI draws inside the
    // main-camera render pass UI subpass, so the pipeline reuses that pass +
    // subpass (same as the runtime UIPass). The batch was recorded by ZSlate
    // windows during PreRender (BeginFrameIfEnabled cleared it).
    {
        auto& zslate_overlay = ZSlate::ZSlateEditorOverlay::Get();
        if (zslate_overlay.IsNativeBackendEnabled())
        {
            zslate_overlay.EnsurePipeline(m_Rhi, m_Framebuffer.render_pass, _main_camera_subpass_ui);
            zslate_overlay.DrawBatch(m_Rhi);
        }
    }

    m_Rhi->PopEvent(m_Rhi->GetCurrentCommandBuffer());
    SignalImGuiRenderComplete();
#else
    // No compiled-in UI backend for this platform/config.
    (void)m_WindowUi;
#endif
}
