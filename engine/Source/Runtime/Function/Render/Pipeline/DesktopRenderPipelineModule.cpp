#include "Runtime/Function/Render/Pipeline/DesktopRenderPipelineModule.h"

#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Function/Render/DebugDraw/DebugDrawManager.h"
#include "Runtime/Function/Render/RenderingThread/RHIDrawList.h"
#include "Runtime/Profiler/Profiler.h"
#if defined(__APPLE__)
    #include <TargetConditionals.h>
#endif
#if defined(Z_HAS_VULKAN)
    #include "Runtime/Function/Render/Interface/Vulkan/VulkanRHI.h"
    #include "Runtime/Function/Render/Lumen/LumenRenderPass.h"
    #include "Runtime/Function/Render/Passes/BindlessTonemapPass.h"
    #include "Runtime/Function/Render/Passes/ColorGradingPass.h"
    #include "Runtime/Function/Render/Passes/CombineUIPass.h"
    #include "Runtime/Function/Render/Passes/DirectionalLightPass.h"
    #include "Runtime/Function/Render/Passes/MainCameraPass.h"
    #include "Runtime/Function/Render/Passes/ParticlePass.h"
    #include "Runtime/Function/Render/Passes/PickPass.h"
    #include "Runtime/Function/Render/Passes/PointLightPass.h"
    #include "Runtime/Function/Render/Passes/UIPass.h"
    #include "Runtime/Function/Render/MegaLights/MegaLightsSettings.h"
#endif
#if defined(_WIN32)
    #include "Runtime/Function/Render/Passes/DX12MainCameraPass.h"
    #include "Runtime/Function/Render/Passes/DirectionalLightPass.h"
    #include "Runtime/Function/Render/Passes/PointLightPass.h"
    #include "Runtime/Function/Render/Passes/ShadowPassShared.h"
    #include "Runtime/Function/Render/Passes/UIPass.h"
    #include "Runtime/Function/Render/MegaLights/MegaLightsSettings.h"
#endif
#if defined(_WIN32)
    #include "Runtime/Function/Render/Interface/DX12/DX12RHI.h"
#endif
#include "Runtime/Function/Render/RenderPass.h"
#include "Runtime/Function/Render/RenderResource.h"
#include "Runtime/Function/Render/RenderSwapContext.h"
#include "Runtime/Function/Render/RenderSystem.h"

#if defined(_WIN32)
namespace
{
    bool LogDx12DeviceLost(RHI* rhi, const char* step)
    {
        if (rhi == nullptr || rhi->getGraphicsAPI() != GraphicsAPI::DirectX12)
        {
            return false;
        }
        auto* dx12_rhi = static_cast<DX12RHI*>(rhi);
        if (dx12_rhi->IsDeviceRemoved(step))
        {
            LOG_ERROR(ZRender, "DesktopRenderPipelineModule(DX12): aborting init after {}", step);
            return true;
        }
        return false;
    }
}  // namespace
#endif

void DesktopRenderPipelineModule::Setup(const RenderPipelineInitInfo& init_info)
{
#if defined(__APPLE__)
    (void)init_info;
    return;
#else
    #if defined(_WIN32)
    if (m_Pipeline.m_Rhi && m_Pipeline.m_Rhi->getGraphicsAPI() == GraphicsAPI::DirectX12)
    {
        // DX-B8: post/UI passes live inside DX12MainCameraPass (Rp1 + BindlessTonemap + Rp2),
        // not as separate m_ColorGradingPass / m_FxaaPass / m_UiPass / m_CombineUiPass objects.
        // Shadow passes + main camera mirror the Vulkan pipeline slot layout for DeferredRender.
        RHIDescriptorSetLayout* per_mesh_layout = nullptr;
        if (!ShadowPassShared::CreatePerMeshDescriptorSetLayout(m_Pipeline.m_Rhi, per_mesh_layout))
        {
            LOG_ERROR(ZRender, "DesktopRenderPipelineModule(DX12): failed to create shadow per-mesh descriptor layout");
            return;
        }

        if (auto* render_resource = dynamic_cast<RenderResource*>(init_info.render_resource.get()))
        {
            render_resource->m_MeshDescriptorSetLayout = &ShadowPassShared::GetPerMeshLayoutPtr();
        }

        m_Pipeline.m_PointLightShadowPass = std::make_shared<PointLightShadowPass>();
        m_Pipeline.m_DirectionalLightPass = std::make_shared<DirectionalLightShadowPass>();
        m_Pipeline.m_MainCameraPass = std::make_shared<DX12MainCameraPass>();

        RenderPassCommonInfo pass_common_info;
        pass_common_info.rhi = m_Pipeline.m_Rhi;
        pass_common_info.render_resource = init_info.render_resource;

        m_Pipeline.m_PointLightShadowPass->SetCommonInfo(pass_common_info);
        m_Pipeline.m_DirectionalLightPass->SetCommonInfo(pass_common_info);
        m_Pipeline.m_MainCameraPass->SetCommonInfo(pass_common_info);

        m_Pipeline.m_PointLightShadowPass->Initialize(nullptr);
        m_Pipeline.m_DirectionalLightPass->Initialize(nullptr);

        #if defined(_WIN32)
        if (LogDx12DeviceLost(m_Pipeline.m_Rhi, " PointLightShadowPass::Initialize"))
        {
            return;
        }
        #endif

        static_cast<PointLightShadowPass*>(m_Pipeline.m_PointLightShadowPass.get())
            ->setPerMeshLayout(per_mesh_layout);
        static_cast<DirectionalLightShadowPass*>(m_Pipeline.m_DirectionalLightPass.get())
            ->setPerMeshLayout(per_mesh_layout);

        m_Pipeline.m_PointLightShadowPass->PostInitialize();
        #if defined(_WIN32)
        if (LogDx12DeviceLost(m_Pipeline.m_Rhi, " PointLightShadowPass::PostInitialize"))
        {
            return;
        }
        #endif
        m_Pipeline.m_DirectionalLightPass->PostInitialize();
        #if defined(_WIN32)
        if (LogDx12DeviceLost(m_Pipeline.m_Rhi, " DirectionalLightShadowPass::PostInitialize"))
        {
            return;
        }
        #endif

        auto dx12_main_camera = static_cast<DX12MainCameraPass*>(m_Pipeline.m_MainCameraPass.get());
        dx12_main_camera->m_PointLightShadowColorImageView =
            static_cast<RenderPass*>(m_Pipeline.m_PointLightShadowPass.get())->GetFramebufferImageViews()[0];
        dx12_main_camera->m_DirectionalLightShadowColorImageView =
            static_cast<RenderPass*>(m_Pipeline.m_DirectionalLightPass.get())->m_Framebuffer.attachments[0].view;

        MainCameraPassInitInfo camera_init_info {};
        camera_init_info.enble_fxaa = init_info.enable_fxaa;
        m_Pipeline.m_MainCameraPass->Initialize(&camera_init_info);

        if (auto* render_resource = dynamic_cast<RenderResource*>(init_info.render_resource.get()))
        {
            render_resource->m_MaterialDescriptorSetLayout =
                &dx12_main_camera->getRp1Pass().GetMaterialDescriptorSetLayoutPtr();
        }

        m_Pipeline.m_UiPass = std::make_shared<UIPass>();
        m_Pipeline.m_UiPass->SetCommonInfo(pass_common_info);
        UIPassInitInfo ui_init_info {};
        ui_init_info.render_pass = dx12_main_camera->getRp2RenderPass();
        if (ui_init_info.render_pass != nullptr)
        {
            m_Pipeline.m_UiPass->Initialize(&ui_init_info);
            dx12_main_camera->getRp2Pass().SetUiPass(static_cast<RenderPass*>(m_Pipeline.m_UiPass.get()));
        }

        LOG_INFO(ZRender, "DesktopRenderPipelineModule(DX12): shadow passes + main camera initialized");
        return;
    }
    #endif
    #if defined(Z_HAS_VULKAN)
    m_Pipeline.m_PointLightShadowPass = std::make_shared<PointLightShadowPass>();
    m_Pipeline.m_DirectionalLightPass = std::make_shared<DirectionalLightShadowPass>();
    m_Pipeline.m_MainCameraPass = std::make_shared<MainCameraPass>();
    m_Pipeline.m_LumenPass = std::make_shared<LumenRenderPass>();
    m_Pipeline.m_ToneMappingPass = std::make_shared<BindlessTonemapPass>();
    m_Pipeline.m_ColorGradingPass = std::make_shared<ColorGradingPass>();
    m_Pipeline.m_UiPass = std::make_shared<UIPass>();
    m_Pipeline.m_CombineUiPass = std::make_shared<CombineUIPass>();
    m_Pipeline.m_PickPass = std::make_shared<PickPass>();
    m_Pipeline.m_FxaaPass = std::make_shared<FXAAPass>();
    m_Pipeline.m_ParticlePass = std::make_shared<ParticlePass>();

    RenderPassCommonInfo pass_common_info;
    pass_common_info.rhi = m_Pipeline.m_Rhi;
    pass_common_info.render_resource = init_info.render_resource;

    m_Pipeline.m_PointLightShadowPass->SetCommonInfo(pass_common_info);
    m_Pipeline.m_DirectionalLightPass->SetCommonInfo(pass_common_info);
    m_Pipeline.m_MainCameraPass->SetCommonInfo(pass_common_info);
    m_Pipeline.m_LumenPass->SetCommonInfo(pass_common_info);
    m_Pipeline.m_ToneMappingPass->SetCommonInfo(pass_common_info);
    m_Pipeline.m_ColorGradingPass->SetCommonInfo(pass_common_info);
    m_Pipeline.m_UiPass->SetCommonInfo(pass_common_info);
    m_Pipeline.m_CombineUiPass->SetCommonInfo(pass_common_info);
    m_Pipeline.m_PickPass->SetCommonInfo(pass_common_info);
    m_Pipeline.m_FxaaPass->SetCommonInfo(pass_common_info);
    m_Pipeline.m_ParticlePass->SetCommonInfo(pass_common_info);

    m_Pipeline.m_PointLightShadowPass->Initialize(nullptr);
    m_Pipeline.m_DirectionalLightPass->Initialize(nullptr);
    m_Pipeline.m_LumenPass->Initialize(nullptr);

    MainCameraPass* main_camera_pass =
        static_cast<MainCameraPass*>(m_Pipeline.m_MainCameraPass.get());
    RenderPass* _main_camera_pass =
        static_cast<RenderPass*>(m_Pipeline.m_MainCameraPass.get());
    std::shared_ptr<ParticlePass> particle_pass =
        std::static_pointer_cast<ParticlePass>(m_Pipeline.m_ParticlePass);

    ParticlePassInitInfo particle_init_info {};
    particle_init_info.m_ParticleManager = GET_SYSTEM(ParticleManager);
    m_Pipeline.m_ParticlePass->Initialize(&particle_init_info);

    main_camera_pass->m_PointLightShadowColorImageView =
        static_cast<RenderPass*>(m_Pipeline.m_PointLightShadowPass.get())->GetFramebufferImageViews()[0];
    main_camera_pass->m_DirectionalLightShadowColorImageView =
        static_cast<RenderPass*>(m_Pipeline.m_DirectionalLightPass.get())->m_Framebuffer.attachments[0].view;

    MainCameraPassInitInfo main_camera_init_info;
    main_camera_init_info.enble_fxaa = init_info.enable_fxaa;
    main_camera_pass->SetParticlePass(particle_pass);
    m_Pipeline.m_MainCameraPass->Initialize(&main_camera_init_info);

    static_cast<ParticlePass*>(m_Pipeline.m_ParticlePass.get())->SetupParticlePass();

    std::vector<RHIDescriptorSetLayout*> descriptor_layouts = _main_camera_pass->GetDescriptorSetLayouts();
    static_cast<PointLightShadowPass*>(m_Pipeline.m_PointLightShadowPass.get())
        ->setPerMeshLayout(descriptor_layouts[MainCameraPass::LayoutType::_per_mesh]);
    static_cast<DirectionalLightShadowPass*>(m_Pipeline.m_DirectionalLightPass.get())
        ->setPerMeshLayout(descriptor_layouts[MainCameraPass::LayoutType::_per_mesh]);

    m_Pipeline.m_PointLightShadowPass->PostInitialize();
    m_Pipeline.m_DirectionalLightPass->PostInitialize();

    // BindlessTonemapPass owns its own RP/FB; it just needs the source
    // (RP1's backup_odd) and target (RP2's backup_even) image views plus
    // the swapchain extent. The init render_pass field is unused here
    // (legacy ToneMappingPass needed it to bind into main_camera's RP).
    BindlessTonemapPassInitInfo tone_mapping_init_info;
    tone_mapping_init_info.source_hdr_view =
        _main_camera_pass->GetFramebufferImageViews()[_main_camera_pass_backup_buffer_odd];
    tone_mapping_init_info.target_ldr_view =
        _main_camera_pass->GetFramebufferImageViews()[_main_camera_pass_backup_buffer_even];
    tone_mapping_init_info.target_ldr_format = RHI_FORMAT_R16G16B16A16_SFLOAT;
    tone_mapping_init_info.width = m_Pipeline.m_Rhi->GetSwapchainInfo().extent.width;
    tone_mapping_init_info.height = m_Pipeline.m_Rhi->GetSwapchainInfo().extent.height;
    m_Pipeline.m_ToneMappingPass->Initialize(&tone_mapping_init_info);
    // Allocate / refresh the bindless slot for the source HDR view so
    // that the very first Draw() doesn't sample an unbound descriptor.
    static_cast<BindlessTonemapPass*>(m_Pipeline.m_ToneMappingPass.get())
        ->UpdateAfterFramebufferRecreate(tone_mapping_init_info.source_hdr_view,
                                         tone_mapping_init_info.target_ldr_view,
                                         tone_mapping_init_info.width,
                                         tone_mapping_init_info.height);

    ColorGradingPassInitInfo color_grading_init_info;
    color_grading_init_info.render_pass = _main_camera_pass->GetRenderPass();
    color_grading_init_info.input_attachment =
        _main_camera_pass->GetFramebufferImageViews()[_main_camera_pass_backup_buffer_even];
    m_Pipeline.m_ColorGradingPass->Initialize(&color_grading_init_info);

    UIPassInitInfo ui_init_info;
    ui_init_info.render_pass = _main_camera_pass->GetRenderPass();
    m_Pipeline.m_UiPass->Initialize(&ui_init_info);

    CombineUIPassInitInfo combine_ui_init_info;
    combine_ui_init_info.render_pass = _main_camera_pass->GetRenderPass();
    combine_ui_init_info.scene_input_attachment =
        _main_camera_pass->GetFramebufferImageViews()[_main_camera_pass_backup_buffer_odd];
    combine_ui_init_info.ui_input_attachment =
        _main_camera_pass->GetFramebufferImageViews()[_main_camera_pass_backup_buffer_even];
    m_Pipeline.m_CombineUiPass->Initialize(&combine_ui_init_info);

    PickPassInitInfo pick_init_info;
    pick_init_info.per_mesh_layout = descriptor_layouts[MainCameraPass::LayoutType::_per_mesh];
    m_Pipeline.m_PickPass->Initialize(&pick_init_info);

    FXAAPassInitInfo fxaa_init_info;
    fxaa_init_info.render_pass = _main_camera_pass->GetRenderPass();
    fxaa_init_info.input_attachment =
        _main_camera_pass->GetFramebufferImageViews()[_main_camera_pass_post_process_buffer_odd];
    m_Pipeline.m_FxaaPass->Initialize(&fxaa_init_info);
    #endif  // Z_HAS_VULKAN
#endif      // __APPLE__
}

void DesktopRenderPipelineModule::Shutdown()
{
    // Release the path's passes (their dtors free GPU resources). The caller
    // (RenderPipeline::SetActiveModule via RenderSystem) guarantees the GPU is idle
    // and no in-flight frame references these passes. Reset the composite/post passes
    // before the shadow + main-camera passes they read from.
    m_Pipeline.m_CombineUiPass.reset();
    m_Pipeline.m_UiPass.reset();
    m_Pipeline.m_FxaaPass.reset();
    m_Pipeline.m_ColorGradingPass.reset();
    m_Pipeline.m_ToneMappingPass.reset();
    m_Pipeline.m_PickPass.reset();
    m_Pipeline.m_ParticlePass.reset();
    m_Pipeline.m_MainCameraPass.reset();
    m_Pipeline.m_DirectionalLightPass.reset();
    m_Pipeline.m_PointLightShadowPass.reset();
    m_Pipeline.m_LumenPass.reset();
}

void DesktopRenderPipelineModule::BuildDrawLists(std::shared_ptr<RenderResourceBase> render_resource,
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
        if (m_Pipeline.m_PointLightShadowPass && !MegaLights::IsEnabled())
        {
            out_draw_list.Add(
                "PointShadow",
                [this]() {
                    static_cast<PointLightShadowPass*>(m_Pipeline.m_PointLightShadowPass.get())->Draw();
                });
        }
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
    if (m_Pipeline.m_PointLightShadowPass && !MegaLights::IsEnabled())
    {
        out_draw_list.Add(
            "PointShadow",
            [this]() { static_cast<PointLightShadowPass*>(m_Pipeline.m_PointLightShadowPass.get())->Draw(); });
    }
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

void DesktopRenderPipelineModule::SubmitDrawLists(RHI* rhi,
                                                  std::shared_ptr<RenderResourceBase> render_resource,
                                                  const RHIDrawList& draw_list)
{
#if defined(__APPLE__)
    (void)rhi;
    (void)render_resource;
    draw_list.ExecuteAll();
    return;
#else
    #if defined(_WIN32)
    if (rhi && rhi->getGraphicsAPI() == GraphicsAPI::DirectX12)
    {
        if (!render_resource)
        {
            render_resource = m_Pipeline.m_RenderResource;
        }
        if (auto* dx12_resource = dynamic_cast<RenderResource*>(render_resource.get()))
        {
            dx12_resource->ResetRingBufferOffset(rhi->GetCurrentFrameIndex());
        }

        rhi->WaitForFences();
        rhi->ResetCommandPool();
        const bool skip_frame = rhi->PrepareBeforePass([this]() { UpdateAfterRecreate(); });
        if (skip_frame)
        {
            m_Pipeline.notifySkippedFrameRender();
            return;
        }

        draw_list.ExecuteAll();
        rhi->SubmitRendering([this]() { UpdateAfterRecreate(); });
        return;
    }
    #endif
    #if defined(Z_HAS_VULKAN)
    VulkanRHI* vulkan_rhi = static_cast<VulkanRHI*>(rhi);
    RenderResource* vulkan_resource = static_cast<RenderResource*>(render_resource.get());

    vulkan_resource->ResetRingBufferOffset(vulkan_rhi->m_CurrentFrameIndex);

    vulkan_rhi->WaitForFences();
    vulkan_rhi->ResetCommandPool();

    const bool recreate_swapchain = vulkan_rhi->PrepareBeforePass([this]() { UpdateAfterRecreate(); });
    if (recreate_swapchain)
    {
        m_Pipeline.notifySkippedFrameRender();
        return;
    }

    static_cast<ParticlePass*>(m_Pipeline.m_ParticlePass.get())
        ->SetRenderCommandBufferHandle(
            static_cast<MainCameraPass*>(m_Pipeline.m_MainCameraPass.get())->GetRenderCommandBuffer());

    draw_list.ExecuteAll();

    vulkan_rhi->SubmitRendering([this]() { UpdateAfterRecreate(); });
    static_cast<ParticlePass*>(m_Pipeline.m_ParticlePass.get())->CopyNormalAndDepthImage();
    static_cast<ParticlePass*>(m_Pipeline.m_ParticlePass.get())->Simulate();
    #else
    (void)rhi;
    (void)render_resource;
    (void)draw_list;
    #endif
#endif
}

void DesktopRenderPipelineModule::UpdateAfterRecreate()
{
#if defined(__APPLE__)
    return;
#else
    #if defined(_WIN32)
    if (m_Pipeline.m_Rhi && m_Pipeline.m_Rhi->getGraphicsAPI() == GraphicsAPI::DirectX12)
    {
        if (m_Pipeline.m_MainCameraPass)
        {
            static_cast<DX12MainCameraPass*>(m_Pipeline.m_MainCameraPass.get())->UpdateAfterFramebufferRecreate();
        }
        for (const auto& callback : m_Pipeline.m_FramebufferRecreateCallbacks)
        {
            if (callback)
            {
                callback();
            }
        }
        return;
    }
    #endif
    #if defined(Z_HAS_VULKAN)
    if (!m_Pipeline.m_MainCameraPass || !m_Pipeline.m_ToneMappingPass || !m_Pipeline.m_ColorGradingPass ||
        !m_Pipeline.m_FxaaPass || !m_Pipeline.m_CombineUiPass || !m_Pipeline.m_PickPass || !m_Pipeline.m_ParticlePass)
    {
        return;
    }

    MainCameraPass& main_camera_pass = *(static_cast<MainCameraPass*>(m_Pipeline.m_MainCameraPass.get()));
    ColorGradingPass& color_grading_pass = *(static_cast<ColorGradingPass*>(m_Pipeline.m_ColorGradingPass.get()));
    FXAAPass& fxaa_pass = *(static_cast<FXAAPass*>(m_Pipeline.m_FxaaPass.get()));
    BindlessTonemapPass& tone_mapping_pass = *(static_cast<BindlessTonemapPass*>(m_Pipeline.m_ToneMappingPass.get()));
    CombineUIPass& combine_ui_pass = *(static_cast<CombineUIPass*>(m_Pipeline.m_CombineUiPass.get()));
    PickPass& pick_pass = *(static_cast<PickPass*>(m_Pipeline.m_PickPass.get()));
    ParticlePass& particle_pass = *(static_cast<ParticlePass*>(m_Pipeline.m_ParticlePass.get()));

    main_camera_pass.UpdateAfterFramebufferRecreate();
    tone_mapping_pass.UpdateAfterFramebufferRecreate(
        main_camera_pass.GetFramebufferImageViews()[_main_camera_pass_backup_buffer_odd],
        main_camera_pass.GetFramebufferImageViews()[_main_camera_pass_backup_buffer_even],
        m_Pipeline.m_Rhi->GetSwapchainInfo().extent.width,
        m_Pipeline.m_Rhi->GetSwapchainInfo().extent.height);
    color_grading_pass.UpdateAfterFramebufferRecreate(
        main_camera_pass.GetFramebufferImageViews()[_main_camera_pass_backup_buffer_even]);
    fxaa_pass.UpdateAfterFramebufferRecreate(
        main_camera_pass.GetFramebufferImageViews()[_main_camera_pass_post_process_buffer_odd]);
    combine_ui_pass.UpdateAfterFramebufferRecreate(
        main_camera_pass.GetFramebufferImageViews()[_main_camera_pass_backup_buffer_odd],
        main_camera_pass.GetFramebufferImageViews()[_main_camera_pass_backup_buffer_even]);
    pick_pass.RecreateFramebuffer();
    particle_pass.UpdateAfterFramebufferRecreate();
    GET_SYSTEM(DebugDrawManager)->UpdateAfterRecreateSwapchain();
    #endif  // Z_HAS_VULKAN
#endif      // __APPLE__
}

uint32_t DesktopRenderPipelineModule::GetGuidOfPickedMesh(const Vector2& picked_uv)
{
#if defined(__APPLE__)
    (void)picked_uv;
    return 0;
#else
    #if defined(Z_HAS_VULKAN)
    if (m_Pipeline.m_PickPass == nullptr)
    {
        return 0;
    }

    PickPass& pick_pass = *(static_cast<PickPass*>(m_Pipeline.m_PickPass.get()));
    return pick_pass.Pick(picked_uv);
    #else
    (void)picked_uv;
    return 0;
    #endif  // Z_HAS_VULKAN
#endif      // __APPLE__
}

RHIRenderPass* DesktopRenderPipelineModule::GetUIRenderPass() const
{
#if defined(__APPLE__)
    return nullptr;
#else
    #if defined(_WIN32)
    if (m_Pipeline.m_Rhi && m_Pipeline.m_Rhi->getGraphicsAPI() == GraphicsAPI::DirectX12)
    {
        if (m_Pipeline.m_MainCameraPass == nullptr)
        {
            return nullptr;
        }
        return static_cast<DX12MainCameraPass*>(m_Pipeline.m_MainCameraPass.get())->getRp2RenderPass();
    }
    #endif
    #if defined(Z_HAS_VULKAN)
    if (m_Pipeline.m_MainCameraPass == nullptr)
    {
        return nullptr;
    }

    return static_cast<RenderPass*>(m_Pipeline.m_MainCameraPass.get())->GetRenderPass();
    #else
    return nullptr;
    #endif  // Z_HAS_VULKAN
#endif      // __APPLE__
}

RHIImageView* DesktopRenderPipelineModule::GetUiLayerColorView() const
{
#if defined(__APPLE__)
    return nullptr;
#else
    #if defined(_WIN32)
    if (m_Pipeline.m_Rhi && m_Pipeline.m_Rhi->getGraphicsAPI() == GraphicsAPI::DirectX12 && m_Pipeline.m_MainCameraPass)
    {
        return static_cast<DX12MainCameraPass*>(m_Pipeline.m_MainCameraPass.get())->getUiLayerColorView();
    }
    #endif
    #if defined(Z_HAS_VULKAN)
    if (m_Pipeline.m_MainCameraPass == nullptr)
    {
        return nullptr;
    }
    const auto views = static_cast<RenderPass*>(m_Pipeline.m_MainCameraPass.get())->GetFramebufferImageViews();
    if (views.size() > _main_camera_pass_backup_buffer_even)
    {
        return views[_main_camera_pass_backup_buffer_even];
    }
    #endif
    return nullptr;
#endif  // __APPLE__
}

void DesktopRenderPipelineModule::FinishDx12ShadowPassDescriptorSetup()
{
#if defined(_WIN32)
    if (m_Pipeline.m_Rhi == nullptr || m_Pipeline.m_Rhi->getGraphicsAPI() != GraphicsAPI::DirectX12)
    {
        return;
    }

    if (m_Pipeline.m_PointLightShadowPass)
    {
        static_cast<PointLightShadowPass*>(m_Pipeline.m_PointLightShadowPass.get())->FinishDescriptorSetup();
    }
    if (m_Pipeline.m_DirectionalLightPass)
    {
        static_cast<DirectionalLightShadowPass*>(m_Pipeline.m_DirectionalLightPass.get())->FinishDescriptorSetup();
    }
#else
    (void)0;
#endif
}

void DesktopRenderPipelineModule::ConsumeParticleSwapData(RenderSwapData& swap_data, RenderSwapContext& swap_context)
{
#if defined(Z_HAS_VULKAN)
    if (!m_Pipeline.m_ParticlePass)
    {
        RenderPipelineModule::ConsumeParticleSwapData(swap_data, swap_context);
        return;
    }

    if (swap_data.m_ParticleSubmitRequest.has_value())
    {
        ParticlePass* particle_pass = static_cast<ParticlePass*>(m_Pipeline.m_ParticlePass.get());

        int emitter_count = swap_data.m_ParticleSubmitRequest->GetEmitterCount();
        particle_pass->SetEmitterCount(emitter_count);

        for (int index = 0; index < emitter_count; ++index)
        {
            const ParticleEmitterDesc& desc = swap_data.m_ParticleSubmitRequest->GetEmitterDesc(index);
            particle_pass->CreateEmitter(index, desc);
        }

        particle_pass->InitializeEmitters();

        swap_context.ResetPartilceBatchSwapData();
    }
    if (swap_data.m_EmitterTickRequest.has_value())
    {
        static_cast<ParticlePass*>(m_Pipeline.m_ParticlePass.get())
            ->SetTickIndices(swap_data.m_EmitterTickRequest->m_EmitterIndices);
        swap_context.ResetEmitterTickSwapData();
    }

    if (swap_data.m_EmitterTransformRequest.has_value())
    {
        static_cast<ParticlePass*>(m_Pipeline.m_ParticlePass.get())
            ->SetTransformIndices(swap_data.m_EmitterTransformRequest->m_TransformDescs);
        swap_context.ResetEmitterTransformSwapData();
    }
#else
    RenderPipelineModule::ConsumeParticleSwapData(swap_data, swap_context);
#endif
}
