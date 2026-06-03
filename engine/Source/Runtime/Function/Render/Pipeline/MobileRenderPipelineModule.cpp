#include "Runtime/Function/Render/Pipeline/MobileRenderPipelineModule.h"

#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Function/Render/DebugDraw/DebugDrawManager.h"
#include "Runtime/Function/Render/Pipeline/DesktopRenderPipelineModule.h"
#include "Runtime/Function/Render/RenderingThread/RHIDrawList.h"
#include "Runtime/Profiler/Profiler.h"
#if defined(Z_HAS_VULKAN)
    #include "Runtime/Function/Render/Interface/Vulkan/VulkanRHI.h"
    #include "Runtime/Function/Render/Passes/BindlessTonemapPass.h"
    #include "Runtime/Function/Render/Passes/ColorGradingPass.h"
    #include "Runtime/Function/Render/Passes/CombineUIPass.h"
    #include "Runtime/Function/Render/Passes/MainCameraPass.h"
    #include "Runtime/Function/Render/Passes/ParticlePass.h"
    #include "Runtime/Function/Render/Passes/UIPass.h"
    #include "Runtime/Function/Render/RenderPass.h"
#endif
#if defined(_WIN32)
    #include "Runtime/Function/Render/Passes/DX12MainCameraPass.h"
    #include "Runtime/Function/Render/Passes/DirectionalLightPass.h"
#endif
#if defined(Z_HAS_VULKAN)
    #include "Runtime/Function/Render/Passes/DirectionalLightPass.h"
#endif

MobileRenderPipelineModule::MobileRenderPipelineModule(RenderPipelineBase& host)
    : RenderPipelineModule(host), m_Shared(std::make_unique<DesktopRenderPipelineModule>(host))
{
}

MobileRenderPipelineModule::~MobileRenderPipelineModule() = default;

void MobileRenderPipelineModule::Setup(const RenderPipelineInitInfo& init_info)
{
    // Skeleton: reuse the shared main-camera assembly so the scene renders and the
    // runtime path switch is verifiable end-to-end. See class header for the TODOs to
    // replace this with a bespoke G-buffer-free forward chain.
    LOG_INFO(ZRender, "MobileRenderPipelineModule: forward-lite skeleton (shared main-camera assembly).");
    m_Shared->Setup(init_info);
}

void MobileRenderPipelineModule::Shutdown()
{
    m_Shared->Shutdown();
}

void MobileRenderPipelineModule::BuildDrawLists(std::shared_ptr<RenderResourceBase> render_resource,
                                                RHIDrawList& out_draw_list)
{
    out_draw_list.Clear();
    (void)render_resource;

#if defined(__APPLE__)
    const auto post_ui_callbacks = m_Pipeline.m_PostUiRenderCallbacks;
    out_draw_list.Add(
        "PostUI",
        [post_ui_callbacks]() {
            for (const auto& callback : post_ui_callbacks)
            {
                if (callback)
                {
                    callback();
                }
            }
        });
    return;
#else
    // Mobile forward-lite strip: directional shadow only (no point-light shadow,
    // no MegaLights), then the (shared) main-camera scene + post + UI draw.
    #if defined(_WIN32)
    if (m_Pipeline.m_Rhi && m_Pipeline.m_Rhi->getGraphicsAPI() == GraphicsAPI::DirectX12)
    {
        const auto post_ui_callbacks = m_Pipeline.m_PostUiRenderCallbacks;
        const auto show_skybox = m_Pipeline.m_IsShowSkybox;

        if (m_Pipeline.m_DirectionalLightPass)
        {
            out_draw_list.Add(
                "DirectionalShadow",
                [this]() {
                    static_cast<DirectionalLightShadowPass*>(m_Pipeline.m_DirectionalLightPass.get())->Draw();
                });
        }
        // NOTE (forward-lite strip): no "PointShadow" entry under Mobile.
        if (m_Pipeline.m_MainCameraPass)
        {
            out_draw_list.Add(
                "MainCamera",
                [this, post_ui_callbacks, show_skybox]() {
                    static_cast<DX12MainCameraPass*>(m_Pipeline.m_MainCameraPass.get())
                        ->Draw(post_ui_callbacks, show_skybox);
                });
        }
        return;
    }
    #endif
    #if defined(Z_HAS_VULKAN)
    MainCameraPass* main_camera_pass = static_cast<MainCameraPass*>(m_Pipeline.m_MainCameraPass.get());
    if (main_camera_pass)
    {
        main_camera_pass->m_IsShowAxis = m_Pipeline.m_IsShowAxis;
        main_camera_pass->m_IsShowSkybox = m_Pipeline.m_IsShowSkybox;
        main_camera_pass->m_SelectedAxis = m_Pipeline.m_SelectedAxis;
    }

    const auto post_ui_callbacks = m_Pipeline.m_PostUiRenderCallbacks;

    if (m_Pipeline.m_DirectionalLightPass)
    {
        out_draw_list.Add(
            "DirectionalShadow",
            [this]() { static_cast<DirectionalLightShadowPass*>(m_Pipeline.m_DirectionalLightPass.get())->Draw(); });
    }
    // NOTE (forward-lite strip): no "PointShadow" entry under Mobile.
    if (m_Pipeline.m_MainCameraPass)
    {
        out_draw_list.Add(
            "MainCamera",
            [this, post_ui_callbacks]() {
                ColorGradingPass& color_grading_pass =
                    *(static_cast<ColorGradingPass*>(m_Pipeline.m_ColorGradingPass.get()));
                FXAAPass& fxaa_pass = *(static_cast<FXAAPass*>(m_Pipeline.m_FxaaPass.get()));
                BindlessTonemapPass& tone_mapping_pass =
                    *(static_cast<BindlessTonemapPass*>(m_Pipeline.m_ToneMappingPass.get()));
                RenderPass& ui_pass = *(static_cast<RenderPass*>(m_Pipeline.m_UiPass.get()));
                CombineUIPass& combine_ui_pass = *(static_cast<CombineUIPass*>(m_Pipeline.m_CombineUiPass.get()));
                ParticlePass& particle_pass = *(static_cast<ParticlePass*>(m_Pipeline.m_ParticlePass.get()));
                MainCameraPass* main_camera_pass = static_cast<MainCameraPass*>(m_Pipeline.m_MainCameraPass.get());
                VulkanRHI* vulkan_rhi = static_cast<VulkanRHI*>(m_Pipeline.m_Rhi);

                main_camera_pass->Draw(color_grading_pass,
                                       fxaa_pass,
                                       tone_mapping_pass,
                                       ui_pass,
                                       combine_ui_pass,
                                       particle_pass,
                                       vulkan_rhi->m_CurrentSwapchainImageIndex,
                                       post_ui_callbacks);
            });
    }

    out_draw_list.Add(
        "DebugDraw",
        [this]() {
            VulkanRHI* vulkan_rhi = static_cast<VulkanRHI*>(m_Pipeline.m_Rhi);
            GET_SYSTEM(DebugDrawManager)->Draw(vulkan_rhi->m_CurrentSwapchainImageIndex);
        });
    #endif
#endif
}

void MobileRenderPipelineModule::SubmitDrawLists(RHI* rhi,
                                                 std::shared_ptr<RenderResourceBase> render_resource,
                                                 const RHIDrawList& draw_list)
{
    m_Shared->SubmitDrawLists(rhi, render_resource, draw_list);
}

void MobileRenderPipelineModule::UpdateAfterRecreate()
{
    m_Shared->UpdateAfterRecreate();
}

uint32_t MobileRenderPipelineModule::GetGuidOfPickedMesh(const Vector2& picked_uv)
{
    return m_Shared->GetGuidOfPickedMesh(picked_uv);
}

RHIRenderPass* MobileRenderPipelineModule::GetUIRenderPass() const
{
    return m_Shared->GetUIRenderPass();
}

RHIImageView* MobileRenderPipelineModule::GetUiLayerColorView() const
{
    return m_Shared->GetUiLayerColorView();
}

void MobileRenderPipelineModule::FinishDx12ShadowPassDescriptorSetup()
{
    m_Shared->FinishDx12ShadowPassDescriptorSetup();
}

void MobileRenderPipelineModule::ConsumeParticleSwapData(RenderSwapData& swap_data, RenderSwapContext& swap_context)
{
    m_Shared->ConsumeParticleSwapData(swap_data, swap_context);
}
