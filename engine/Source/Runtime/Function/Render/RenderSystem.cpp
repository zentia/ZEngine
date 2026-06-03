#include "RenderSystem.h"

// Lightweight stub only on Metal (Apple) and WebGL2 (Emscripten). Windows
// DX12-only builds (ZENGINE_USE_VULKAN=OFF) must use the full implementation
// below so SwapLogicRenderData / ProcessSwapData / RenderScene mesh upload work.
// Do NOT gate the stub on !Z_HAS_VULKAN -- that left Win64 editor meshes invisible
// because AddDirtyGameObject never reached UpsertGameObject.
#if defined(__APPLE__) || defined(__EMSCRIPTEN__)

    #include "Runtime/Function/Render/Interface/RHI.h"
    #include "Runtime/Function/Render/RenderCamera.h"
    #include "Runtime/Function/Render/RenderPipeline.h"

std::vector<std::type_index> RenderSystem::GetDependencies() const
{
    return {GET_SYSTEM_TYPE(RHI)};
}

bool RenderSystem::Initialize()
{
    m_Rhi = GET_SYSTEM(RHI);
    if (m_Rhi == nullptr)
    {
        return false;
    }

    m_Cameras.clear();

    auto game_camera = std::make_shared<RenderCamera>();
    game_camera->LookAt(Vector3(0.0f, -10.0f, 5.0f), Vector3::ZERO, Vector3::UNIT_Z);
    game_camera->m_Znear = 0.1f;
    game_camera->m_Zfar = 1000.0f;
    game_camera->SetAspect(16.0f / 9.0f);
    game_camera->setTargetTexture("game");
    game_camera->m_CurrentCameraType = RenderCameraType::Game;
    m_Cameras.push_back(game_camera);

    auto scene_camera = std::make_shared<RenderCamera>();
    scene_camera->LookAt(Vector3(0.0f, -10.0f, 5.0f), Vector3::ZERO, Vector3::UNIT_Z);
    scene_camera->m_Znear = 0.1f;
    scene_camera->m_Zfar = 1000.0f;
    scene_camera->SetAspect(16.0f / 9.0f);
    scene_camera->setTargetTexture("scene");
    scene_camera->m_CurrentCameraType = RenderCameraType::Editor;
    m_Cameras.push_back(scene_camera);

    RenderPipelineInitInfo pipeline_init_info;
    m_RenderPipeline = std::make_shared<RenderPipeline>();
    m_RenderPipeline->m_Rhi = m_Rhi;
    m_RenderPipeline->Initialize(pipeline_init_info);
    return true;
}

void RenderSystem::Tick(float)
{
    if (m_Rhi)
    {
        m_Rhi->PrepareContext();
    }
    if (m_RenderPipeline)
    {
        m_RenderPipeline->DeferredRender(m_Rhi, nullptr);
    }
}

void RenderSystem::Shutdown()
{
    if (m_RenderPipeline)
    {
        m_RenderPipeline->clear();
        m_RenderPipeline.reset();
    }
    m_Cameras.clear();
    m_Rhi = nullptr;
}
void RenderSystem::SwapLogicRenderData() {}
RenderSwapContext& RenderSystem::GetSwapContext()
{
    return m_SwapContext;
}
std::shared_ptr<RenderCamera> RenderSystem::GetRenderCamera(ViewportType viewportType) const
{
    const size_t index = static_cast<size_t>(viewportType);
    return index < m_Cameras.size() ? m_Cameras[index] : nullptr;
}
std::shared_ptr<RenderCamera> RenderSystem::GetCamera(ViewportType camera_id) const
{
    const size_t index = static_cast<size_t>(camera_id);
    return index < m_Cameras.size() ? m_Cameras[index] : nullptr;
}
std::vector<std::shared_ptr<RenderCamera>> RenderSystem::GetAllCameras()
{
    return m_Cameras;
}
RHI* RenderSystem::GetRHI() const
{
    return m_Rhi;
}
void RenderSystem::InitializeUIRenderBackend(WindowUI*) {}
void RenderSystem::UpdateViewport(ViewportType viewport_id, float offset_x, float offset_y, float width, float height)
{
    RHIViewport viewport;
    viewport.x = offset_x;
    viewport.y = offset_y;
    viewport.width = width;
    viewport.height = height;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;

    if (m_Rhi)
    {
        m_Rhi->UpdateViewport(viewport_id, viewport);
    }

    auto camera = GetCamera(viewport_id);
    if (camera && height > 0.0f)
    {
        camera->SetAspect(width / height);
    }
}
uint32_t RenderSystem::GetGuidOfPickedMesh(const Vector2&)
{
    return 0;
}
GObjectID RenderSystem::GetGObjectIDByMeshID(uint32_t) const
{
    return {};
}
EngineContentViewport RenderSystem::GetViewport(ViewportType type) const
{
    if (!m_Rhi)
    {
        return {};
    }

    RHIViewport* viewport = m_Rhi->GetViewport(type);
    if (!viewport)
    {
        return {};
    }

    return {viewport->x, viewport->y, viewport->width, viewport->height};
}
void RenderSystem::CreateAxis(std::array<RenderEntity, 3>, std::array<RenderMeshData, 3>) {}
void RenderSystem::SetVisibleAxis(std::optional<RenderEntity> axis)
{
    if (!m_RenderPipeline)
    {
        return;
    }
    m_RenderPipeline->SetAxisVisibleState(axis.has_value());
}
void RenderSystem::SetSkyboxVisible(ViewportType viewport_type, bool visible)
{
    if (!m_RenderPipeline)
    {
        return;
    }
    m_RenderPipeline->SetSkyboxVisibleState(viewport_type, visible);
}
bool RenderSystem::IsSkyboxVisible(ViewportType viewport_type) const
{
    return m_RenderPipeline ? m_RenderPipeline->IsSkyboxVisibleState(viewport_type) : false;
}
void RenderSystem::SetSelectedAxis(size_t selected_axis)
{
    if (!m_RenderPipeline)
    {
        return;
    }
    m_RenderPipeline->SetSelectedAxis(selected_axis);
}
void RenderSystem::SetCameraPreview(std::shared_ptr<RenderCamera> camera,
                                    EngineContentViewport viewport,
                                    const std::string& title)
{
    m_CameraPreviewRequest.enabled = camera != nullptr && viewport.width > 0.0f && viewport.height > 0.0f;
    m_CameraPreviewRequest.camera = camera;
    m_CameraPreviewRequest.viewport = viewport;
    m_CameraPreviewRequest.title = title;
}
void RenderSystem::ClearCameraPreview()
{
    m_CameraPreviewRequest = {};
}
GuidAllocator<GameObjectPartId>& RenderSystem::GetGOInstanceIdAllocator()

{
    static GuidAllocator<GameObjectPartId> allocator;
    return allocator;
}
GuidAllocator<MeshSourceDesc>& RenderSystem::GetMeshAssetIdAllocator()
{
    static GuidAllocator<MeshSourceDesc> allocator;
    return allocator;
}
void RenderSystem::ClearForLevelReloading() {}
void RenderSystem::SyncGameCameraFromMainCamera() {}
void RenderSystem::ProcessSwapData() {}

#else
    #include "Runtime/Function/Framework/Component/Camera/CameraComponent.h"
    #include "Runtime/Function/Framework/Level/Level.h"
    #include "Runtime/Function/Framework/World/WorldManager.h"
    #include "Runtime/Function/Particle/ParticleManager.h"
    #include "Runtime/Function/Render/Interface/RHI.h"
    #include "Runtime/Resource/Config/ConfigManager.h"
    #if defined(Z_HAS_VULKAN)
        #include "Runtime/Function/Render/DebugDraw/DebugDrawManager.h"
        #include "Runtime/Function/Render/Passes/MainCameraPass.h"
    #endif
    #include "Runtime/Function/Render/Pipeline/RenderPipelineSettings.h"
    #include "Runtime/Function/Render/RenderCamera.h"
    #include "Runtime/Function/Render/RenderFramePipeline.h"
    #include "Runtime/Function/Render/RenderPass.h"
    #include "Runtime/Function/Render/RenderPipeline.h"
    #include "Runtime/Function/Render/RenderResource.h"
    #include "Runtime/Function/Render/RenderResourceBase.h"
    #include "Runtime/Function/Render/RenderScene.h"
    #include "Runtime/Function/Render/RenderSystemFrameCommands.h"
    #include "Runtime/Function/Render/RenderingThread/RenderThreadChecks.h"
    #include "Runtime/Function/Render/RenderingThread/RenderingThread.h"
    #include "Runtime/Function/Render/WindowSystem.h"
    #if defined(_WIN32)
        #include "Runtime/Function/Render/Passes/DX12MainCameraPass.h"
    #endif
    #include "Runtime/Profiler/Profiler.h"
    #include "Runtime/Resource/Asset/AssetManager.h"

    #include <algorithm>

std::vector<std::type_index> RenderSystem::GetDependencies() const
{
    return {GET_SYSTEM_TYPE(RHI), GET_SYSTEM_TYPE(WindowSystem), GET_SYSTEM_TYPE(ParticleManager)};
}

bool RenderSystem::Initialize()
{
    // global rendering resource

    const eastl::string& global_rendering_res_url = GET_SYSTEM(ConfigManager)->GetGlobalRenderingResUrl();
    GlobalRenderingRes* global_rendering_res =
        GET_SYSTEM(AssetManager)->loadAsset<GlobalRenderingRes>(global_rendering_res_url);

    // upload ibl, color grading textures
    LevelResourceDesc level_resource_desc;
    level_resource_desc.m_IblResourceDesc.m_SkyboxIrradianceMap = global_rendering_res->m_SkyboxIrradianceMap;
    level_resource_desc.m_IblResourceDesc.m_SkyboxSpecularMap = global_rendering_res->m_SkyboxSpecularMap;
    level_resource_desc.m_IblResourceDesc.m_BrdfMap = global_rendering_res->m_BrdfMap;
    level_resource_desc.m_ColorGradingResourceDesc.m_ColorGradingMap = global_rendering_res->m_ColorGradingMap;

    m_Rhi = GET_SYSTEM(RHI);
    if (m_Rhi == nullptr)
    {
        LOG_ERROR(ZRender, "RenderSystem failed to get registered RHI");
        return false;
    }

    if (m_Rhi->getGraphicsAPI() == GraphicsAPI::DirectX12)
    {
        const CameraPose& camera_pose = global_rendering_res->m_CameraConfig.m_Pose;

        auto game_camera = std::make_shared<RenderCamera>();
        game_camera->LookAt(camera_pose.m_Position, camera_pose.m_Target, camera_pose.m_Up);
        game_camera->m_Zfar = global_rendering_res->m_CameraConfig.m_ZFar;
        game_camera->m_Znear = global_rendering_res->m_CameraConfig.m_ZNear;
        game_camera->SetAspect(global_rendering_res->m_CameraConfig.m_Aspect.x /
                               global_rendering_res->m_CameraConfig.m_Aspect.y);
        game_camera->setTargetTexture("game");
        game_camera->m_CurrentCameraType = RenderCameraType::Game;
        m_Cameras.push_back(game_camera);

        auto scene_camera = std::make_shared<RenderCamera>();
        scene_camera->LookAt(camera_pose.m_Position, camera_pose.m_Target, camera_pose.m_Up);
        scene_camera->m_Zfar = global_rendering_res->m_CameraConfig.m_ZFar;
        scene_camera->m_Znear = global_rendering_res->m_CameraConfig.m_ZNear;
        scene_camera->SetAspect(global_rendering_res->m_CameraConfig.m_Aspect.x /
                                global_rendering_res->m_CameraConfig.m_Aspect.y);
        scene_camera->setTargetTexture("scene");
        scene_camera->m_CurrentCameraType = RenderCameraType::Editor;
        m_Cameras.push_back(scene_camera);

        m_RenderScene = std::make_shared<RenderScene>();
        m_RenderScene->m_AmbientLight = {global_rendering_res->m_AmbientLight.toVector3()};
        m_RenderScene->m_DirectionalLight.m_Direction =
            global_rendering_res->m_DirectionalLight.m_Direction.normalisedCopy();
        m_RenderScene->m_DirectionalLight.m_Color = global_rendering_res->m_DirectionalLight.m_Color.toVector3();
        m_RenderScene->SetVisibleNodesReference();

        auto render_resource = std::make_shared<RenderResource>();
        m_RenderResource = render_resource;

        // Upload ringbuffer must exist before MainCameraRp1Pass::SetupDescriptorSets runs
        // inside RenderPipeline::Initialize (otherwise descriptors point at null buffers).
        render_resource->CreateAndMapStorageBuffer(m_Rhi);

        RenderPipelineInitInfo pipeline_init_info;
        pipeline_init_info.enable_fxaa = global_rendering_res->m_EnableFxaa;
        pipeline_init_info.render_resource = m_RenderResource;

        m_RenderPipeline = std::make_shared<RenderPipeline>();
        m_RenderPipeline->m_Rhi = m_Rhi;
        m_RenderPipeline->Initialize(pipeline_init_info);

        render_resource->UploadGlobalRenderResource(m_Rhi, level_resource_desc);
        if (auto pipeline = dynamic_cast<RenderPipeline*>(m_RenderPipeline.get()))
        {
            pipeline->FinishDx12ShadowPassDescriptorSetup();
        }
        if (auto* dx12_main_camera =
                dynamic_cast<DX12MainCameraPass*>(m_RenderPipeline->m_MainCameraPass.get()))
        {
            dx12_main_camera->OnGlobalRenderResourceUploaded();
        }

        MemoryManager::DestroyObject(global_rendering_res);
        LOG_INFO(ZRender, "DX12 minimal main camera + UI RenderSystem path enabled");
        if (ShouldUseParallelRendering())
        {
            RenderFramePipeline::ConfigureMaxFramesInFlight(m_Rhi->GetMaxFramesInFlight());
            LOG_INFO(ZRender,
                     "RenderSystem: Game / Render / RHI parallel dispatch (max {} frames in flight)",
                     RenderFramePipeline::GetMaxFramesInFlight());
        }
        return true;
    }

    #if defined(Z_HAS_VULKAN)
    m_RenderResource = std::make_shared<RenderResource>();
    m_RenderResource->UploadGlobalRenderResource(m_Rhi, level_resource_desc);

    // setup render camera
    const CameraPose& camera_pose = global_rendering_res->m_CameraConfig.m_Pose;

    // Initialize cameras for Scene view and Game view
    // Game view camera
    auto game_camera = std::make_shared<RenderCamera>();
    game_camera->LookAt(camera_pose.m_Position, camera_pose.m_Target, camera_pose.m_Up);
    game_camera->m_Zfar = global_rendering_res->m_CameraConfig.m_ZFar;
    game_camera->m_Znear = global_rendering_res->m_CameraConfig.m_ZNear;
    game_camera->SetAspect(global_rendering_res->m_CameraConfig.m_Aspect.x /
                           global_rendering_res->m_CameraConfig.m_Aspect.y);
    game_camera->setTargetTexture("game");  // Render to game viewport RenderTexture
    game_camera->m_CurrentCameraType = RenderCameraType::Game;
    m_Cameras.push_back(game_camera);

    // Scene view camera
    auto scene_camera = std::make_shared<RenderCamera>();
    scene_camera->LookAt(camera_pose.m_Position, camera_pose.m_Target, camera_pose.m_Up);
    scene_camera->m_Zfar = global_rendering_res->m_CameraConfig.m_ZFar;
    scene_camera->m_Znear = global_rendering_res->m_CameraConfig.m_ZNear;
    scene_camera->SetAspect(global_rendering_res->m_CameraConfig.m_Aspect.x /
                            global_rendering_res->m_CameraConfig.m_Aspect.y);
    scene_camera->setTargetTexture("scene");  // Render to scene viewport RenderTexture
    scene_camera->m_CurrentCameraType = RenderCameraType::Editor;
    m_Cameras.push_back(scene_camera);

    // setup render scene
    m_RenderScene = std::make_shared<RenderScene>();
    m_RenderScene->m_AmbientLight = {global_rendering_res->m_AmbientLight.toVector3()};
    m_RenderScene->m_DirectionalLight.m_Direction =
        global_rendering_res->m_DirectionalLight.m_Direction.normalisedCopy();
    m_RenderScene->m_DirectionalLight.m_Color = global_rendering_res->m_DirectionalLight.m_Color.toVector3();
    m_RenderScene->SetVisibleNodesReference();

    // initialize render pipeline
    RenderPipelineInitInfo pipeline_init_info;
    pipeline_init_info.enable_fxaa = global_rendering_res->m_EnableFxaa;
    pipeline_init_info.render_resource = m_RenderResource;

    m_RenderPipeline = std::make_shared<RenderPipeline>();
    m_RenderPipeline->m_Rhi = m_Rhi;
    m_RenderPipeline->Initialize(pipeline_init_info);

    // descriptor set layout in main camera pass will be used when uploading resource
    static_cast<RenderResource*>(m_RenderResource.get())->m_MeshDescriptorSetLayout =
        &static_cast<RenderPass*>(m_RenderPipeline->m_MainCameraPass.get())
             ->m_DescriptorInfos[MainCameraPass::LayoutType::_per_mesh]
             .layout;
    static_cast<RenderResource*>(m_RenderResource.get())->m_MaterialDescriptorSetLayout =
        &static_cast<RenderPass*>(m_RenderPipeline->m_MainCameraPass.get())
             ->m_DescriptorInfos[MainCameraPass::LayoutType::_mesh_per_material]
             .layout;
    MemoryManager::DestroyObject(global_rendering_res);

    if (ShouldUseParallelRendering())
    {
        RenderFramePipeline::ConfigureMaxFramesInFlight(m_Rhi->GetMaxFramesInFlight());
        LOG_INFO(ZRender,
                 "RenderSystem: Game / Render / RHI parallel dispatch (max {} frames in flight)",
                 RenderFramePipeline::GetMaxFramesInFlight());
    }
    return true;
    #else
    LOG_ERROR(ZRender, "RenderSystem::Initialize: unsupported graphics API (Vulkan backend not compiled in)");
    return false;
    #endif
}

void RenderSystem::Tick(float delta_time)
{
    Z_PROFILE_FUNCTION();

    // Apply a pending r.RenderPath switch at a frame boundary (before any work for
    // this frame is enqueued), so the whole frame uses one coherent module.
    ApplyPendingRenderPathChange();

    if (!ShouldUseParallelRendering())
    {
        TickSingleThreaded(delta_time);
        return;
    }

    CHECK_GAME_THREAD();

    RenderFramePipeline::WaitForFrameSlot();

    const uint32_t frame_draw_list_slot = AllocateFrameDrawListSlot();
    m_ActiveFrameDrawListSlot = frame_draw_list_slot;

    BuildRenderSystemFrameCommands(*this, delta_time, frame_draw_list_slot);
    RenderingThread::SubmitRenderFrame();
    RenderFramePipeline::OnFrameSubmitted();
}

void RenderSystem::FlushRenderingCommands()
{
    if (!ShouldUseParallelRendering())
    {
        return;
    }

    RenderingThread::FlushRenderingCommands();
}

void RenderSystem::RunSynchronizedGpuReadback(std::function<void()> fn)
{
    if (!fn)
    {
        return;
    }

    if (ShouldUseParallelRendering())
    {
        FlushRenderingCommands();
        RenderingThread::ExecuteOnRHIThread(std::move(fn));
        return;
    }

    fn();
}

bool RenderSystem::ShouldUseParallelRendering()
{
    return RenderingThread::IsParallelRenderingEnabled();
}

void RenderSystem::ApplyPendingRenderPathChange()
{
    if (!RenderPipelineSettings::ConsumePathDirty())
    {
        return;
    }

    auto pipeline = dynamic_cast<RenderPipeline*>(m_RenderPipeline.get());
    if (!pipeline || !m_Rhi)
    {
        return;
    }

    const RenderPipelineSettings::RenderPath path = RenderPipelineSettings::GetEffectivePath();
    LOG_INFO(ZRender, "RenderSystem: switching render path to {}", RenderPipelineSettings::ToString(path));

    // Teardown + rebuild must run with the GPU idle and no in-flight frame touching
    // the old module. FlushRenderingCommands drains the render + RHI pipeline; the
    // actual swap then runs on the RHI thread (which owns the device) and blocks
    // until complete -- same contract RunSynchronizedGpuReadback relies on.
    auto switch_task = [this, pipeline, path]() {
        m_Rhi->WaitForFences();
        pipeline->SetActiveModule(path);
        RewireRenderResourceLayoutsAfterPathSwitch();
    };

    if (ShouldUseParallelRendering())
    {
        FlushRenderingCommands();
        RenderingThread::ExecuteOnRHIThread(switch_task);
    }
    else
    {
        switch_task();
    }
}

void RenderSystem::RewireRenderResourceLayoutsAfterPathSwitch()
{
    if (!m_RenderPipeline || !m_RenderResource)
    {
        return;
    }

#if defined(_WIN32)
    if (m_Rhi && m_Rhi->getGraphicsAPI() == GraphicsAPI::DirectX12)
    {
        // DX12 Setup already re-points the RenderResource layouts (it casts the same
        // render_resource); we only need to finish shadow descriptors + rebind the
        // already-uploaded global IBL/LUT/storage resources to the new main camera.
        if (auto pipeline = dynamic_cast<RenderPipeline*>(m_RenderPipeline.get()))
        {
            pipeline->FinishDx12ShadowPassDescriptorSetup();
        }
        if (auto* dx12_main_camera = dynamic_cast<DX12MainCameraPass*>(m_RenderPipeline->m_MainCameraPass.get()))
        {
            dx12_main_camera->OnGlobalRenderResourceUploaded();
        }
        return;
    }
#endif

#if defined(Z_HAS_VULKAN)
    if (m_RenderPipeline->m_MainCameraPass)
    {
        auto* main_camera_pass = static_cast<RenderPass*>(m_RenderPipeline->m_MainCameraPass.get());
        auto render_resource = static_cast<RenderResource*>(m_RenderResource.get());
        render_resource->m_MeshDescriptorSetLayout =
            &main_camera_pass->m_DescriptorInfos[MainCameraPass::LayoutType::_per_mesh].layout;
        render_resource->m_MaterialDescriptorSetLayout =
            &main_camera_pass->m_DescriptorInfos[MainCameraPass::LayoutType::_mesh_per_material].layout;
    }
#endif
}

void RenderSystem::TickSingleThreaded(float delta_time)
{
    Z_PROFILE_SCOPE("RenderSystem::TickSingleThreaded");
    m_ActiveFrameDrawListSlot = 0;
    TickRenderThread(delta_time);
    TickRHIThread();
}

uint32_t RenderSystem::AllocateFrameDrawListSlot()
{
    const uint32_t max_frames = std::max(1u, RenderFramePipeline::GetMaxFramesInFlight());
    return m_FrameDrawListSerial.fetch_add(1, std::memory_order_relaxed) % max_frames;
}

RHIDrawList& RenderSystem::GetFrameDrawList(uint32_t slot)
{
    const uint32_t max_frames = std::max(1u, RenderFramePipeline::GetMaxFramesInFlight());
    return m_FrameDrawLists[slot % max_frames];
}

void RenderSystem::TickRenderThread(float delta_time)
{
    CHECK_RENDER_THREAD();
    Z_PROFILE_SCOPE("RenderSystem::TickRenderThread");

    if (m_Rhi && m_Rhi->getGraphicsAPI() == GraphicsAPI::DirectX12)
    {
        SyncGameCameraFromMainCamera();

        if (m_RenderPipeline && m_RenderScene && m_RenderResource)
        {
            m_RenderScene->SetVisibleNodesReference();
            ProcessSwapData();

            Level* active_level = nullptr;
            if (auto world_manager = GET_SYSTEM(WorldManager))
            {
                active_level = world_manager->getCurrentActiveLevel();
            }
            m_RenderScene->SyncPointLightsFromLevel(active_level);

            auto render_resource = std::static_pointer_cast<RenderResource>(m_RenderResource);
            for (auto&& camera : m_Cameras)
            {
                m_RenderResource->UpdatePerFrameBuffer(m_RenderScene, camera);
                m_RenderScene->UpdateVisibleObjects(render_resource, camera);
            }
            m_RenderPipeline->PreparePassData(m_RenderResource);
            m_RenderPipeline->BuildDrawLists(m_RenderResource, GetFrameDrawList(m_ActiveFrameDrawListSlot));
        }
        return;
    }

    ProcessSwapData();
    SyncGameCameraFromMainCamera();

    if (m_RenderScene)
    {
        Level* active_level = nullptr;
        if (auto world_manager = GET_SYSTEM(WorldManager))
        {
            active_level = world_manager->getCurrentActiveLevel();
        }
        m_RenderScene->SyncPointLightsFromLevel(active_level);
    }

    auto render_resource = std::static_pointer_cast<RenderResource>(m_RenderResource);
    for (auto&& camera : m_Cameras)
    {
        m_RenderResource->UpdatePerFrameBuffer(m_RenderScene, camera);
        if (render_resource && render_resource->m_TextureStreamingManager)
        {
            auto streaming_manager = render_resource->m_TextureStreamingManager;
            Vector3 camera_pos = camera->position();
            Vector3 camera_forward = camera->forward();
            auto fov_pair = camera->getFOV();
            float fov = fov_pair.y;
            float aspect = camera->getAspect();

            auto swapchain_info = m_Rhi->GetSwapchainInfo();
            streaming_manager->UpdateStreaming(
                camera_pos, camera_forward, fov, aspect, swapchain_info.extent.width, swapchain_info.extent.height);
            streaming_manager->Tick(delta_time);
        }

        m_RenderScene->UpdateVisibleObjects(render_resource, camera);
    }

    m_RenderPipeline->PreparePassData(m_RenderResource);

    #if defined(Z_HAS_VULKAN)
    GET_SYSTEM(DebugDrawManager)->Tick(delta_time);
    #endif

    if (m_RenderPipeline && m_RenderResource)
    {
        m_RenderPipeline->BuildDrawLists(m_RenderResource, GetFrameDrawList(m_ActiveFrameDrawListSlot));
    }
}

void RenderSystem::TickRHIThread()
{
    CHECK_RHI_THREAD();
    Z_PROFILE_SCOPE("RenderSystem::TickRHIThread");

    if (m_Rhi == nullptr)
    {
        return;
    }

    m_Rhi->PrepareContext();

    if (m_RenderPipeline)
    {
        const RHIDrawList& draw_list = GetFrameDrawList(m_ActiveFrameDrawListSlot);
        m_RenderPipeline->SubmitDrawLists(m_Rhi, m_RenderResource, draw_list);
    }
}

void RenderSystem::Shutdown()
{
    if (ShouldUseParallelRendering())
    {
        FlushRenderingCommands();
    }

    m_Rhi = nullptr;

    if (m_RenderScene)
    {
        m_RenderScene->clear();
    }
    m_RenderScene.reset();
    RenderPass::m_VisiableNodes = {};

    if (m_RenderResource)
    {
        m_RenderResource->clear();
    }
    m_RenderResource.reset();

    if (m_RenderPipeline)
    {
        m_RenderPipeline->clear();
    }
    m_RenderPipeline.reset();
}

void RenderSystem::SwapLogicRenderData()
{
    m_SwapContext.SwapLogicRenderData();
}

RenderSwapContext& RenderSystem::GetSwapContext()
{
    return m_SwapContext;
}

std::shared_ptr<RenderCamera> RenderSystem::GetRenderCamera(ViewportType viewportType) const
{
    return m_Cameras[static_cast<int>(viewportType)];
}

std::shared_ptr<RenderCamera> RenderSystem::GetCamera(ViewportType camera_id) const
{
    return m_Cameras[static_cast<int>(camera_id)];
}

std::vector<std::shared_ptr<RenderCamera>> RenderSystem::GetAllCameras()
{
    return m_Cameras;
}

RHI* RenderSystem::GetRHI() const
{
    return m_Rhi;
}

void RenderSystem::UpdateViewport(ViewportType viewport_id, float offset_x, float offset_y, float width, float height)
{
    RHIViewport viewport;
    viewport.x = offset_x;
    viewport.y = offset_y;
    viewport.width = width;
    viewport.height = height;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;

    if (m_Rhi)
    {
        m_Rhi->UpdateViewport(viewport_id, viewport);
    }

    // Update camera aspect based on viewport_id
    // Scene view and Game view have their own cameras
    auto camera = GetCamera(viewport_id);
    if (camera && height > 0)
    {
        camera->SetAspect(width / height);
    }
}

EngineContentViewport RenderSystem::GetViewport(ViewportType type) const
{
    if (!m_Rhi)
    {
        return {};
    }

    RHIViewport* viewport = m_Rhi->GetViewport(type);
    if (!viewport)
    {
        return {};
    }

    return {viewport->x, viewport->y, viewport->width, viewport->height};
}

uint32_t RenderSystem::GetGuidOfPickedMesh(const Vector2& picked_uv)
{
    if (!m_RenderPipeline)
    {
        return 0;
    }

    uint32_t picked_mesh_id = 0;
    RunSynchronizedGpuReadback([&]() { picked_mesh_id = m_RenderPipeline->GetGuidOfPickedMesh(picked_uv); });
    return picked_mesh_id;
}

GObjectID RenderSystem::GetGObjectIDByMeshID(uint32_t mesh_id) const
{
    if (!m_RenderScene)
    {
        return k_invalid_gobject_id;
    }

    return m_RenderScene->GetGObjectIDByMeshID(mesh_id);
}

void RenderSystem::CreateAxis(std::array<RenderEntity, 3> axis_entities,
                              std::array<RenderMeshData, 3> mesh_datas)
{
    if (!m_RenderResource || (m_Rhi && m_Rhi->getGraphicsAPI() == GraphicsAPI::DirectX12))
    {
        return;
    }

    for (int i = 0; i < axis_entities.size(); i++)
    {
        m_RenderResource->UploadGameObjectRenderResource(m_Rhi, axis_entities[i], mesh_datas[i]);
    }
}

void RenderSystem::SetVisibleAxis(std::optional<RenderEntity> axis)
{
    if (m_RenderScene)
    {
        m_RenderScene->m_RenderAxis = axis;
    }

    if (!m_RenderPipeline)
    {
        return;
    }

    m_RenderPipeline->SetAxisVisibleState(axis.has_value());
}

void RenderSystem::SetSkyboxVisible(ViewportType viewport_type, bool visible)
{
    if (!m_RenderPipeline)
    {
        return;
    }

    m_RenderPipeline->SetSkyboxVisibleState(viewport_type, visible);
}

bool RenderSystem::IsSkyboxVisible(ViewportType viewport_type) const
{
    if (!m_RenderPipeline)
    {
        return false;
    }

    return m_RenderPipeline->IsSkyboxVisibleState(viewport_type);
}

void RenderSystem::SetSelectedAxis(size_t selected_axis)
{
    if (!m_RenderPipeline)
    {
        return;
    }

    m_RenderPipeline->SetSelectedAxis(selected_axis);
}

void RenderSystem::SetCameraPreview(std::shared_ptr<RenderCamera> camera,
                                    EngineContentViewport viewport,
                                    const std::string& title)
{
    m_CameraPreviewRequest.enabled = camera != nullptr && viewport.width > 0.0f && viewport.height > 0.0f;
    m_CameraPreviewRequest.camera = camera;
    m_CameraPreviewRequest.viewport = viewport;
    m_CameraPreviewRequest.title = title;
}

void RenderSystem::ClearCameraPreview()
{
    m_CameraPreviewRequest = {};
}

GuidAllocator<GameObjectPartId>& RenderSystem::GetGOInstanceIdAllocator()

{
    return m_RenderScene->GetInstanceIdAllocator();
}

GuidAllocator<MeshSourceDesc>& RenderSystem::GetMeshAssetIdAllocator()
{
    return m_RenderScene->GetMeshAssetIdAllocator();
}

void RenderSystem::ClearForLevelReloading()
{
    m_RenderScene->ClearForLevelReloading();
}

void RenderSystem::InitializeUIRenderBackend(WindowUI* window_ui)
{
    if (!m_RenderPipeline || window_ui == nullptr)
    {
        return;
    }

    m_RenderPipeline->InitializeUIRenderBackend(window_ui);
}

void RenderSystem::SyncGameCameraFromMainCamera()
{
    std::shared_ptr<RenderCamera> game_camera = GetCamera(ViewportType::game);
    if (!game_camera)
    {
        return;
    }

    Level* current_level = GET_SYSTEM(WorldManager)->getCurrentActiveLevel();
    if (current_level == nullptr)
    {
        return;
    }

    CameraComponent* main_camera_component = current_level->GetMainCameraComponent();
    if (main_camera_component == nullptr)
    {
        return;
    }

    main_camera_component->ApplyToGameRenderCamera(*game_camera);
}

void RenderSystem::ProcessSwapData()
{
    Z_PROFILE_SCOPE("RenderSystem::processSwapData");
    RenderSwapData& swap_data = m_SwapContext.GetRenderSwapData();

    // TODO: update global resources if needed
    if (swap_data.m_LevelResourceDesc.has_value())
    {
        m_RenderResource->UploadGlobalRenderResource(m_Rhi, *swap_data.m_LevelResourceDesc);

    #if defined(_WIN32)
        if (m_Rhi != nullptr && m_Rhi->getGraphicsAPI() == GraphicsAPI::DirectX12 && m_RenderPipeline != nullptr)
        {
            if (auto* dx12_pass = dynamic_cast<DX12MainCameraPass*>(m_RenderPipeline->m_MainCameraPass.get()))
            {
                dx12_pass->OnGlobalRenderResourceUploaded();
            }
        }
    #endif

        // reset level resource swap data to a clean state
        m_SwapContext.ResetLevelRsourceSwapData();
    }

    // update game object if needed
    if (swap_data.m_GameObjectResourceDesc.has_value())
    {
        while (!swap_data.m_GameObjectResourceDesc->IsEmpty())
        {
            GameObjectDesc gobject = swap_data.m_GameObjectResourceDesc->GetNextProcessObject();
            m_RenderScene->UpsertGameObject(m_Rhi, *m_RenderResource, gobject);
            swap_data.m_GameObjectResourceDesc->pop();
        }

        // reset game object swap data to a clean state
        m_SwapContext.ResetGameObjectResourceSwapData();
    }

    // remove deleted objects
    if (swap_data.m_GameObjectToDelete.has_value())
    {
        while (!swap_data.m_GameObjectToDelete->IsEmpty())
        {
            GameObjectDesc gobject = swap_data.m_GameObjectToDelete->GetNextProcessObject();
            m_RenderScene->DeleteEntityByGObjectID(gobject.getId());
            swap_data.m_GameObjectToDelete->pop();
        }

        m_SwapContext.ResetGameObjectToDelete();
    }

    for (auto&& camera : m_Cameras)
    {
        // process camera swap data
        if (swap_data.m_CameraSwapData.has_value())
        {
            if (swap_data.m_CameraSwapData->m_FovX.has_value())
            {
                camera->setFOVx(*swap_data.m_CameraSwapData->m_FovX);
            }

            if (swap_data.m_CameraSwapData->m_ViewMatrix.has_value())
            {
                camera->SetMainViewMatrix(*swap_data.m_CameraSwapData->m_ViewMatrix);
            }

            if (swap_data.m_CameraSwapData->m_CameraType.has_value())
            {
                camera->SetCurrentCameraType(*swap_data.m_CameraSwapData->m_CameraType);
            }

            m_SwapContext.ResetCameraSwapData();
        }
    }

    if (m_RenderPipeline)
    {
        m_RenderPipeline->ConsumeParticleSwapData(swap_data, m_SwapContext);
    }
}
#endif
