#include "RenderSystem.h"

// 这三个放在 `#if` 外面：关卡重载 / 全局资源上传现在是**平台无关**的一份实现
// （文件末尾那段），两个平台都要这几个头。
//
// MainCameraRp1Pass.h 在所有平台都编译（见 Runtime/CMakeLists.txt 里
// "MainCameraRp1Pass / MainCameraFramebufferResources / ShadowPassShared 这些完全
// 平台无关的 TU" 那段注释），它自己也只 include 平台无关的 pass 头。
#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Core/Memory/MemoryManager.h"
#include "Runtime/Function/Framework/Component/Mesh/BaseRenderer.h"
#include "Runtime/Function/Framework/Level/Level.h"
#include "Runtime/Function/Framework/World/WorldManager.h"
#include "Runtime/Function/Render/Passes/MainCameraRp1Pass.h"

// Lightweight stub only on Metal (Apple) and WebGL2 (Emscripten). Windows
// DX12-only builds (ZENGINE_USE_VULKAN=OFF) must use the full implementation
// below so SwapLogicRenderData / ProcessSwapData / RenderScene mesh upload work.
// Do NOT gate the stub on !Z_HAS_VULKAN -- that left Win64 editor meshes invisible
// because AddDirtyGameObject never reached UpsertGameObject.
#if defined(__APPLE__) || defined(__EMSCRIPTEN__)

    #include "Runtime/Core/Base/Macro.h"
    #include "Runtime/Function/Render/Interface/RHI.h"
    #include "Runtime/Function/Render/RenderCamera.h"
    #include "Runtime/Function/Render/RenderPipeline.h"
    #include "Runtime/Function/Render/RenderResource.h"
    #include "Runtime/Function/Render/RenderScene.h"
    #include "Runtime/Resource/Asset/AssetManager.h"
    #include "Runtime/Resource/Config/ConfigManager.h"
    #include "Runtime/Resource/ResType/Global/GlobalRendering.h"

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

    // 场景渲染的地基：RenderScene（可见物体 / 光源）和 RenderResource（GPU 资源 +
    // per-frame UBO）。以前 Apple 上一个都没有，管线只能画 UI。
    //
    // GlobalRenderingRes 只在下面单独按需读一次（拿 IBL 的 12 张 HDR 和 LUT 路径），
    // 不走 UploadGlobalRenderResource 那条路 —— 那边要顺带建 texture streaming
    // manager / MegaLights / storage buffer，是另一件事。读不到的时候
    // DesktopRenderPipelineModule 的 EnsureFallbackIblTextures 会兜底，descriptor
    // 始终是合法的。
    m_RenderScene = std::make_shared<RenderScene>();
    m_RenderScene->SetUploadRhi(m_Rhi);
    m_RenderScene->SetVisibleNodesReference();

    auto render_resource = std::make_shared<RenderResource>();
    m_RenderResource = render_resource;

    // 必须在 RenderPipeline::Initialize 之前：RP1 的 SetupDescriptorSets 会直接
    // 绑上传 ringbuffer，晚一步 descriptor 就指向空 buffer。
    render_resource->CreateAndMapStorageBuffer(m_Rhi);

    // ---- IBL + 颜色分级 LUT ----
    //
    // 这条精简版 Initialize **从不调用 UploadGlobalRenderResource**，所以全局资源里
    // 的 IBL 和 LUT 必须单独补，否则：
    //   * IBL 由 DesktopRenderPipelineModule 的 EnsureFallbackIblTextures 兜底 ——
    //     一张 4x4 中性灰 cubemap + 1x1 BRDF。环境光恒等于一个常数，天光/反射全
    //     没了，日志里还挂着一条
    //     `[E] MainCameraRp1Pass::SetupDescriptorSets: IBL textures unavailable`
    //     （它是"没上传"不是"设备掉了"）。
    //   * LUT 除了兜底 texel 没有任何出路 —— RP2 的 binding 1 绑不到真图，颜色
    //     分级等于没接。
    //
    // 不把整个 UploadGlobalRenderResource 搬过来：那要顺带建 texture streaming
    // manager / MegaLights / storage buffer，是另一件事。
    //
    // 两个调用都必须排在 m_RenderPipeline->Initialize 之前：RP1 的
    // SetupDescriptorSets 在那儿就把 IBL descriptor 写死了。
    if (auto* config_manager = GET_SYSTEM(ConfigManager))
    {
        const eastl::string& global_rendering_url = config_manager->GetGlobalRenderingResUrl();
        if (auto* asset_manager = GET_SYSTEM(AssetManager); !global_rendering_url.empty())
        {
            if (auto* global_rendering_res = asset_manager->loadAsset<GlobalRenderingRes>(global_rendering_url))
            {
                LevelIBLResourceDesc ibl_desc;
                ibl_desc.m_SkyboxIrradianceMap = global_rendering_res->m_SkyboxIrradianceMap;
                ibl_desc.m_SkyboxSpecularMap   = global_rendering_res->m_SkyboxSpecularMap;
                ibl_desc.m_BrdfMap             = global_rendering_res->m_BrdfMap;
                render_resource->UploadIblResources(m_Rhi, ibl_desc);

                render_resource->UploadColorGradingLut(m_Rhi, global_rendering_res->m_ColorGradingMap);
                MemoryManager::DestroyObject(global_rendering_res);
            }
            else
            {
                LOG_WARNING(ZRender,
                            "RenderSystem(Apple): could not load '{}' -- IBL and color grading fall back "
                            "to neutral textures",
                            global_rendering_url.c_str());
            }
        }
    }

    RenderPipelineInitInfo pipeline_init_info;
    pipeline_init_info.render_resource = m_RenderResource;
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
        // Metal: acquire this frame's drawable, draw the UI straight into it, then
        // present. The ZSlate editor overlay is driven here, inside the swapchain
        // pass, through the pipeline's post-UI callbacks (EditorUIPass::Draw) --
        // the only place a Metal command buffer is open.
        //
        // 场景 pass 必须在 swapchain pass 打开**之前**录完：它们画的是
        // MainCameraFramebufferResources 自己的离屏 attachment，不占 swapchain。
        if (!m_Rhi->PrepareBeforePass([]() {}))
        {
            if (m_RenderPipeline && m_RenderScene && m_RenderResource)
            {
                ProcessSwapData();

                const auto render_resource = std::static_pointer_cast<RenderResource>(m_RenderResource);
                m_RenderScene->SetVisibleNodesReference();
                for (auto&& camera : m_Cameras)
                {
                    m_RenderResource->UpdatePerFrameBuffer(m_RenderScene, camera);
                    m_RenderScene->UpdateVisibleObjects(render_resource, camera);
                }

                m_RenderPipeline->PreparePassData(m_RenderResource);
                m_ActiveFrameDrawListSlot = 0;
                RHIDrawList& draw_list = GetFrameDrawList(m_ActiveFrameDrawListSlot);
                m_RenderPipeline->BuildDrawLists(m_RenderResource, draw_list);
                draw_list.ExecuteAll();

            }

            RHICommandBuffer* command_buffer = m_Rhi->GetCurrentCommandBuffer();
            RHIRenderPass*    render_pass    = m_Rhi->GetSwapchainRenderPass();
            RHIFramebuffer*   framebuffer    = m_Rhi->GetSwapchainFramebuffer();

            if (command_buffer != nullptr && render_pass != nullptr && framebuffer != nullptr)
            {
                const RHIExtent2D extent = m_Rhi->GetSwapchainInfo().extent;

                RHIClearValue clear_value {};
                clear_value.color.float32[0] = 0.0941f;
                clear_value.color.float32[1] = 0.0941f;
                clear_value.color.float32[2] = 0.1039f;
                clear_value.color.float32[3] = 1.0f;

                RHIRenderPassBeginInfo begin_info {};
                begin_info.sType          = RHI_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
                begin_info.renderPass     = render_pass;
                begin_info.framebuffer    = framebuffer;
                begin_info.renderArea     = {{0, 0}, extent};
                begin_info.clearValueCount = 1;
                begin_info.pClearValues   = &clear_value;

                m_Rhi->CmdBeginRenderPassPFN(command_buffer, &begin_info, RHI_SUBPASS_CONTENTS_INLINE);

                // 场景 → swapchain：一次全屏采样。Metal 没有持久 swapchain image，
                // 也没有 RP2，这是唯一能把 RP1 的离屏结果搬到屏幕上的路。
                // 必须画在 UI **之前**，否则 UI 会被场景盖掉。
                if (!m_SceneBlitAttempted)
                {
                    m_SceneBlitAttempted = true;
                    m_SceneBlit.Initialize(m_Rhi, render_pass);
                }
                if (m_SceneBlit.IsReady())
                {
                    // 源图和"要不要 tonemap"必须来自同一个判断：
                    //   RP2 这一帧跑了 —— 源是它的输出（已做过显示变换）⇒ 直通；
                    //   没有          —— 源是 RP1 的线性 HDR ⇒ 这里补 tonemap。
                    // 两边不一致的后果是二次 tonemap（画面发灰）或漏 tonemap
                    // （>1 死白、其余死黑），而两者都不会报任何错。
                    const bool already_tonemapped =
                        m_RenderPipeline != nullptr && m_RenderPipeline->IsSceneColorTonemapped();
                    m_SceneBlit.Record(command_buffer,
                                       m_RenderPipeline ? m_RenderPipeline->GetSceneColorView() : nullptr,
                                       !already_tonemapped);
                }

                if (m_RenderPipeline)
                {
                    for (const auto& callback : m_RenderPipeline->getPostUIRenderCallbacks())
                    {
                        if (callback)
                        {
                            callback();
                        }
                    }
                }
                m_Rhi->CmdEndRenderPassPFN(command_buffer);
            }

            m_Rhi->SubmitRendering([]() {});
        }
    }
    // On Metal this is skipped: DesktopRenderPipelineModule::BuildDrawLists
    // registers entries the swapchain pass above just ran (or, with no scene
    // passes, a single "PostUI"). Executing it here would re-record
    // EditorUIPass::Draw after SubmitRendering() committed the command buffer,
    // i.e. against a null command buffer where every draw is silently dropped.
    //
    // 场景那一串（ProcessSwapData → UpdatePerFrameBuffer → UpdateVisibleObjects →
    // PreparePassData → BuildDrawLists → ExecuteAll）已经上移到 swapchain pass
    // 之前，见上面。
#if defined(__APPLE__)
    // ---- Apple 真机验收的可观测性 ------------------------------------------
    // MSL 只在运行时编译、shader 创建失败往往是静默 nullptr，"窗口是黑的"根本
    // 定位不到是哪一段断了。这里每 ~120 帧打一条，把链路上的四个关键状态全说清：
    // 场景有没有物体 / 视锥里有没有可见网格 / 离屏颜色视图在不在 / blit 通不通。
    // 任何一项是 0 或 false 就直接指向那一段。
    {
        static uint32_t s_apple_frame_tick = 0;
        ++s_apple_frame_tick;
        if ((s_apple_frame_tick % 120) == 1)
        {
            const size_t entity_count  = m_RenderScene != nullptr ? m_RenderScene->m_RenderEntities.size() : 0u;
            const size_t visible_count = m_RenderScene != nullptr
                                             ? m_RenderScene->m_MainCameraVisibleMeshNodesPerViewport[0].size()
                                             : 0u;
            const RHIExtent2D extent = m_Rhi != nullptr ? m_Rhi->GetSwapchainInfo().extent : RHIExtent2D {};
            LOG_INFO(ZRender,
                     "AppleRender: frame={} entities={} visible={} scene_color_view={} blit_ready={} "
                     "rp2_tonemapped={} swapchain={}x{}",
                     s_apple_frame_tick,
                     entity_count,
                     visible_count,
                     (m_RenderPipeline != nullptr && m_RenderPipeline->GetSceneColorView() != nullptr) ? 1 : 0,
                     m_SceneBlit.IsReady() ? 1 : 0,
                     m_RenderPipeline != nullptr && m_RenderPipeline->IsSceneColorTonemapped() ? 1 : 0,
                     extent.width,
                     extent.height);
        }
    }
#endif
#if !defined(__APPLE__)
    if (m_RenderPipeline)
    {
        m_RenderPipeline->DeferredRender(m_Rhi, nullptr);
    }
#endif
}

void RenderSystem::Shutdown()
{
    if (m_RenderPipeline)
    {
        m_RenderPipeline->clear();
        m_RenderPipeline.reset();
    }
    if (m_RenderScene)
    {
        m_RenderScene->clear();
        m_RenderScene.reset();
    }
    if (m_RenderResource)
    {
        m_RenderResource->clear();
        m_RenderResource.reset();
    }
    m_Cameras.clear();
    m_Rhi = nullptr;
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
void RenderSystem::InitializeUIRenderBackend(WindowUI* window_ui)
{
    if (m_RenderPipeline)
    {
        m_RenderPipeline->InitializeUIRenderBackend(window_ui);
    }
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
bool RenderSystem::TryGetRenderSceneViewport(RHIViewport&, RHIRect2D&) const
{
    return false;
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
// ResubmitActiveLevelRenderers 不再有 Apple 专属的空桩：它的实现现在是平台无关
// 的一份（文件末尾），Apple 上也会真正跑起来 —— 顺带把全局资源上传排上。
void RenderSystem::SyncGameCameraFromMainCamera() {}
void RenderSystem::ProcessSwapData()
{
    // 场景物体的唯一入口：Level / GameObject 的增删改从 swap data 里取出来喂给
    // RenderScene。以前这里是空体，等于 Apple 上永远没有任何网格被上传 —— 就算
    // 管线接好了也只会画出背景色。
    //
    // 只处理三件真正影响"有没有东西可画"的事：全局资源（IBL）、GameObject 增删改。
    // 相机 / 粒子 / 视口那几段 Apple 上没有对应的消费者，先不动。
    if (!m_RenderScene || !m_RenderResource || m_Rhi == nullptr)
    {
        return;
    }

    RenderSwapData& swap_data = m_SwapContext.GetRenderSwapData();

    if (swap_data.m_LevelResourceDesc.has_value())
    {
        // Apple / Metal 走的是精简 Initialize，**从不**调 UploadGlobalRenderResource
        // （那要顺带建 texture streaming manager / MegaLights / storage buffer，是
        // 另一件事，而且在 Metal 上没验过），所以这里也只补它自己补的那两样：
        // IBL + 颜色分级 LUT。
        LOG_INFO(ZRender,
                 "ProcessSwapData: level resource swap data arrived -- re-uploading IBL + LUT and rebinding");
        const auto render_resource = std::static_pointer_cast<RenderResource>(m_RenderResource);
        render_resource->UploadIblResources(m_Rhi, swap_data.m_LevelResourceDesc->m_IblResourceDesc);
        render_resource->UploadColorGradingLut(
            m_Rhi, swap_data.m_LevelResourceDesc->m_ColorGradingResourceDesc.m_ColorGradingMap);

        // 上传完**必须重绑**：RP1 的 mesh_global descriptor 在 SetupDescriptorSets
        // 里就写死了，不重绑的话新 cubemap 永远进不了 shader —— 热重载前后画面
        // 完全一样，看起来就像"没生效"。
        if (m_RenderPipeline != nullptr)
        {
            if (auto* rp1 = dynamic_cast<MainCameraRp1Pass*>(m_RenderPipeline->GetMainCameraPass()))
            {
                rp1->RefreshMeshGlobalIblDescriptors();
            }
        }

        m_SwapContext.ResetLevelRsourceSwapData();
    }

    if (swap_data.m_GameObjectResourceDesc.has_value())
    {
        while (!swap_data.m_GameObjectResourceDesc->IsEmpty())
        {
            GameObjectDesc gobject = swap_data.m_GameObjectResourceDesc->GetNextProcessObject();
            m_RenderScene->UpsertGameObject(m_Rhi, *m_RenderResource, gobject);
            swap_data.m_GameObjectResourceDesc->pop();
        }
        m_SwapContext.ResetGameObjectResourceSwapData();
    }

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

    if (swap_data.m_VisibleAxisUpdatePending)
    {
        m_RenderScene->m_RenderAxis = swap_data.m_VisibleAxis;
        m_SwapContext.ResetVisibleAxisSwapData();
    }

    // ---- 下面这批字段 Apple 上没有消费者，但**必须**清掉 ----
    //
    // IsReadyToSwap() 是拿"render buffer 里还有没有待处理项"当门闩的：只要有一个
    // 字段一直挂着，swap() 就**永远**不会被调用 —— 于是游戏线程写进 logic buffer
    // 的任何东西都再也送不到渲染线程。
    //
    // 真机上就是这么死的：1-1.scene 里有个粒子发射器，ParticleManager 提交一次
    // m_ParticleSubmitRequest 之后 Apple 的 ProcessSwapData 从不碰它 ⇒ 第 N 帧之后
    // 整条 swap 通道永久死锁。表现是"关卡热重载完全没反应"：连
    // ResubmitActiveLevelRenderers 重新提交的 30 个 renderer 都进不来。
    //
    // 粒子这里**故意不走** m_RenderPipeline->ConsumeParticleSwapData()：那条路径
    // 会在 Metal 上建粒子的 GPU 资源，从来没验过；而 Apple 在此之前本来也从来没
    // 消费过粒子（只是以前连"没消费"都看不出来，因为整个 swap 已经死了）。
    m_SwapContext.ResetPartilceBatchSwapData();
    m_SwapContext.ResetEmitterTickSwapData();
    m_SwapContext.ResetEmitterTransformSwapData();

    // 相机 / 场景视口：Apple 上同样没有消费者（SyncGameCameraFromMainCamera 是
    // 空体），一样不能留着 —— 否则下一帧就又是死锁。
    if (swap_data.m_CameraSwapData.has_value())
    {
        m_SwapContext.ResetCameraSwapData();
    }
    if (swap_data.m_SceneViewportUpdatePending)
    {
        m_SwapContext.ResetSceneViewportSwapData();
    }
}
void RenderSystem::FlushRenderingCommands()
{
    // Apple / WebGL2 path: rendering runs synchronously on the main thread, so
    // there is no separate rendering/RHI thread queue to drain.
}

#else
    #include "Runtime/Function/Framework/Component/Camera/CameraComponent.h"
    #include "Runtime/Function/Framework/Component/Mesh/BaseRenderer.h"
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
        m_RenderScene->SetUploadRhi(m_Rhi);
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

        // NOTE: hoisting UploadGlobalRenderResource above RenderPipeline::Initialize
        // was tried and proven ineffective -- the upload still landed after the
        // pipeline ran. IBL binding is instead solved UE-style: fallback textures
        // are created before SetupDescriptorSets() so the descriptor set is always
        // valid, and the real cubemaps patch it in place once uploaded.
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
#if defined(_WIN32)
        if (auto* dx12_main_camera =
                dynamic_cast<DX12MainCameraPass*>(m_RenderPipeline->m_MainCameraPass.get()))
        {
            dx12_main_camera->OnGlobalRenderResourceUploaded();
        }
#endif

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
    m_RenderScene->SetUploadRhi(m_Rhi);
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

// AllocateFrameDrawListSlot / GetFrameDrawList 是**平台无关**的，所以它们住在这
// 个文件的公共尾巴上（见文件末尾），不跟着上面那两个大分支走。以前它们在这个
// #else 里，于是 Apple / Emscripten 上根本没有定义 —— ZRuntime 是静态库，缺符号
// 不会报错，但任何一个链它的可执行文件都会 undefined symbol。

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

    if (viewport_id == ViewportType::scene)
    {
        ViewportSwapEntry entry {};
        entry.viewport = viewport;
        entry.scissor.offset = {static_cast<int32_t>(offset_x), static_cast<int32_t>(offset_y)};
        entry.scissor.extent = {static_cast<uint32_t>(width > 0.0f ? width : 0.0f),
                                static_cast<uint32_t>(height > 0.0f ? height : 0.0f)};

        RenderSwapData& logic_swap = m_SwapContext.GetLogicSwapData();
        logic_swap.m_SceneViewportUpdate = entry;
        logic_swap.m_SceneViewportUpdatePending = true;
    }

    // Update camera aspect based on viewport_id
    // Scene view and Game view have their own cameras
    auto camera = GetCamera(viewport_id);
    if (camera && height > 0)
    {
        camera->SetAspect(width / height);
    }
}

bool RenderSystem::TryGetRenderSceneViewport(RHIViewport& out_viewport, RHIRect2D& out_scissor) const
{
    if (!m_HasRenderThreadSceneViewport)
    {
        return false;
    }

    out_viewport = m_RenderThreadSceneViewport;
    out_scissor = m_RenderThreadSceneScissor;
    return true;
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
    if (!m_RenderResource || !m_RenderScene)
    {
        return;
    }

    std::array<size_t, 3> mesh_asset_ids = {axis_entities[0].m_MeshAssetId,
                                            axis_entities[1].m_MeshAssetId,
                                            axis_entities[2].m_MeshAssetId};
    m_RenderScene->RegisterAxisMeshSources(mesh_asset_ids, mesh_datas);

    auto* render_resource = static_cast<RenderResource*>(m_RenderResource.get());
    for (size_t i = 0; i < axis_entities.size(); ++i)
    {
        render_resource->UploadGameObjectRenderResource(m_Rhi, axis_entities[i], mesh_datas[i]);
        if (!render_resource->HasValidMesh(axis_entities[i].m_MeshAssetId))
        {
            LOG_ERROR(ZRender,
                      "CreateAxis: failed to upload axis mesh {} (asset id {})",
                      i,
                      axis_entities[i].m_MeshAssetId);
        }
    }
}

void RenderSystem::SetVisibleAxis(std::optional<RenderEntity> axis)
{
    RenderSwapData& logic_swap = m_SwapContext.GetLogicSwapData();
    logic_swap.m_VisibleAxis = axis;
    logic_swap.m_VisibleAxisUpdatePending = true;

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

    // 交换数据里还压着的 GameObjectDesc 必须一起丢弃。ProcessSwapData 除了
    // 消费 render buffer，还会直接读游戏线程的 logic buffer（见其中
    // "apply mesh deletes/upserts from logic here" 那段），把那里的 desc 直接
    // 拷出来做 UpsertGameObject / DeleteEntityByGObjectID。这些 desc 持有的是
    // Play World 对象的引用，而调用方在清完之后马上就会卸载关卡 —— 于是
    // 渲染线程拿到的就是已销毁对象（ProcessSwapData 里的 0xc0000005）。
    // ClearForLevelReloading 只在即将丢弃关卡数据时调用（PIE 停止、编辑器
    // 重载场景），此时两个 buffer 里的待处理项都已无意义，直接清掉。
    // 交换数据里还压着的 GameObjectDesc 必须一起丢弃。ProcessSwapData 除了
    // 消费 render buffer，还会直接读游戏线程的 logic buffer（见其中
    // "apply mesh deletes/upserts from logic here" 那段），把那里的 desc 直接
    // 拷出来做 UpsertGameObject / DeleteEntityByGObjectID。这些 desc 持有的是
    // Play World 对象的引用，而调用方清完之后马上就会卸载关卡 —— 于是渲染
    // 线程拿到的就是已销毁对象（ProcessSwapData 里的 0xc0000005）。
    // ClearForLevelReloading 只在即将丢弃关卡数据时调用（PIE 停止、编辑器
    // 重载场景），此时两个 buffer 里的待处理项都已无意义，直接清掉。
    m_SwapContext.ResetGameObjectResourceSwapData();
    m_SwapContext.ResetGameObjectToDelete();
    m_SwapContext.ResetLogicGameObjectSwapData();
}

// ResubmitActiveLevelRenderers 搬到文件末尾的平台无关段了（Apple 也要跑）。

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

        // 平台无关的重绑：RP1（DX12 和 Metal 都用它）在 SetupDescriptorSets 里就把
        // mesh_global descriptor 写死了，上传完不重绑 ⇒ shader 里拿的还是旧
        // cubemap。以前只有 DX12 那条（OnGlobalRenderResourceUploaded），Apple /
        // Metal 上上传完没人管。
        if (m_RenderPipeline != nullptr)
        {
            if (auto* rp1 = dynamic_cast<MainCameraRp1Pass*>(m_RenderPipeline->GetMainCameraPass()))
            {
                rp1->RefreshMeshGlobalIblDescriptors();
            }
        }

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

    if (swap_data.m_VisibleAxisUpdatePending && m_RenderScene)
    {
        m_RenderScene->m_RenderAxis = swap_data.m_VisibleAxis;
        m_SwapContext.ResetVisibleAxisSwapData();
    }

    // Game-thread dirty state can sit on the logic buffer while the render buffer is
    // still draining a large upload queue (swap blocked). Mirror axis handling: apply
    // mesh deletes/upserts from logic here so level reload + Resubmit are not stuck
    // behind swap readiness.
    RenderSwapData& logic_swap = m_SwapContext.GetLogicSwapData();
    if (m_RenderScene)
    {
        if (logic_swap.m_GameObjectToDelete.has_value())
        {
            while (!logic_swap.m_GameObjectToDelete->IsEmpty())
            {
                GameObjectDesc gobject = logic_swap.m_GameObjectToDelete->GetNextProcessObject();
                m_RenderScene->DeleteEntityByGObjectID(gobject.getId());
                logic_swap.m_GameObjectToDelete->pop();
            }
            logic_swap.m_GameObjectToDelete.reset();
        }

        if (logic_swap.m_GameObjectResourceDesc.has_value())
        {
            while (!logic_swap.m_GameObjectResourceDesc->IsEmpty())
            {
                GameObjectDesc gobject = logic_swap.m_GameObjectResourceDesc->GetNextProcessObject();
                m_RenderScene->UpsertGameObject(m_Rhi, *m_RenderResource, gobject);
                logic_swap.m_GameObjectResourceDesc->pop();
            }
            logic_swap.m_GameObjectResourceDesc.reset();
        }
    }

    if (logic_swap.m_VisibleAxisUpdatePending && m_RenderScene)
    {
        m_RenderScene->m_RenderAxis = logic_swap.m_VisibleAxis;
        logic_swap.m_VisibleAxisUpdatePending = false;
    }

    if (swap_data.m_SceneViewportUpdatePending)
    {
        if (swap_data.m_SceneViewportUpdate.has_value())
        {
            const ViewportSwapEntry& entry = *swap_data.m_SceneViewportUpdate;
            m_RenderThreadSceneViewport = entry.viewport;
            m_RenderThreadSceneScissor = entry.scissor;
            m_HasRenderThreadSceneViewport = entry.viewport.width > 0.0f && entry.viewport.height > 0.0f;
        }
        else
        {
            m_HasRenderThreadSceneViewport = false;
        }

        m_SwapContext.ResetSceneViewportSwapData();
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

// ===========================================================================
// 平台无关的一小撮：帧绘制列表的槽位分配。
//
// 这两个函数以前住在 `#else`（非 Apple / 非 Emscripten）那个大分支里，于是
// Apple 与 Emscripten 上 **没有定义** —— Apple 的 Tick 现在要取 draw list
// （RenderSystem.cpp 里那条 Apple 分支），链接时就是 undefined symbol。
// 静态库不会暴露这个问题，只有真正链出可执行文件才会炸（ZEditor 就是这么红的）。
// ===========================================================================
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

// ===========================================================================
// RenderSwapData::m_LevelResourceDesc 的唯一生产者。
//
// 它以前**全库没有生产者**：两个 ProcessSwapData() 都读它、IsReadyToSwap() 拿它当
// 门闩，但没有任何地方赋值 ⇒ "关卡热重载刷新全局资源"在任何平台都不通。
//
// 必须由**游戏线程**调用（写 logic swap data），上传本身在渲染线程的
// ProcessSwapData() 里做。
// ===========================================================================
void RenderSystem::RequestGlobalResourceUpload()
{
    if (m_RenderResource == nullptr)
    {
        return;
    }

    auto* config_manager = GET_SYSTEM(ConfigManager);
    auto* asset_manager  = GET_SYSTEM(AssetManager);
    if (config_manager == nullptr || asset_manager == nullptr)
    {
        LOG_WARNING(ZRender,
                    "RequestGlobalResourceUpload: ConfigManager/AssetManager unavailable -- "
                    "global resources are not re-uploaded for this level");
        return;
    }

    const eastl::string& global_rendering_url = config_manager->GetGlobalRenderingResUrl();
    if (global_rendering_url.empty())
    {
        LOG_WARNING(ZRender,
                    "RequestGlobalResourceUpload: global rendering res url is empty -- "
                    "global resources are not re-uploaded for this level");
        return;
    }

    GlobalRenderingRes* global_rendering_res = asset_manager->loadAsset<GlobalRenderingRes>(global_rendering_url);
    if (global_rendering_res == nullptr)
    {
        LOG_WARNING(ZRender,
                    "RequestGlobalResourceUpload: could not load '{}' -- global resources stay as-is",
                    global_rendering_url.c_str());
        return;
    }

    LevelResourceDesc desc;
    desc.m_IblResourceDesc.m_SkyboxIrradianceMap         = global_rendering_res->m_SkyboxIrradianceMap;
    desc.m_IblResourceDesc.m_SkyboxSpecularMap           = global_rendering_res->m_SkyboxSpecularMap;
    desc.m_IblResourceDesc.m_BrdfMap                     = global_rendering_res->m_BrdfMap;
    desc.m_ColorGradingResourceDesc.m_ColorGradingMap    = global_rendering_res->m_ColorGradingMap;
    MemoryManager::DestroyObject(global_rendering_res);

    // 去重只针对"上一次请求还没被渲染线程消费"这种情况（SetLevelResourceDesc 是
    // 覆盖语义，本来也不会堆积）。
    //
    // 这里**故意不跟"上次已上传的那一份"比**：重载关卡要的不只是"资源内容变了要重
    // 传"，更是"上传完要重绑 descriptor"（ProcessSwapData 里的
    // RefreshMeshGlobalIblDescriptors）。若按内容去重，开场景时 desc 永远和启动时
    // 那份相同 ⇒ 一次都不会重绑，热重载后 shader 里还是旧 IBL。
    RenderSwapData& logic_swap_data = m_SwapContext.GetLogicSwapData();
    if (logic_swap_data.m_LevelResourceDesc.has_value() && *logic_swap_data.m_LevelResourceDesc == desc)
    {
        LOG_INFO(ZRender, "RequestGlobalResourceUpload: identical request still pending -- not re-queued");
        return;
    }

    logic_swap_data.SetLevelResourceDesc(desc);
    LOG_INFO(ZRender,
             "RequestGlobalResourceUpload: queued global resource upload (brdf={}, lut={})",
             desc.m_IblResourceDesc.m_BrdfMap.c_str(),
             desc.m_ColorGradingResourceDesc.m_ColorGradingMap.c_str());
}

// ===========================================================================
// 关卡重载 / 开场景之后的重建：重新提交关卡里的 BaseRenderer，并排一次全局资源
// 上传。
//
// 这份以前只存在于 `#else`（非 Apple）分支，Apple 上是空桩 ⇒ 编辑器在 Mac 上
// 重载场景时"什么都不做"。
// ===========================================================================
void RenderSystem::ResubmitActiveLevelRenderers()
{
    // 全局资源（IBL 的 12 张 HDR 面 + BRDF + LUT）不挂在关卡上，但它们上传完之后
    // 要重绑 descriptor —— 排一次上传，ProcessSwapData 里会顺带重绑。
    RequestGlobalResourceUpload();

    WorldManager* world_manager = GET_SYSTEM(WorldManager);
    if (world_manager == nullptr)
    {
        return;
    }

    Level* active_level = world_manager->getCurrentActiveLevel();
    if (active_level == nullptr)
    {
        return;
    }

    uint32_t resubmit_count = 0;
    for (const auto& id_object_pair : active_level->getAllGObjects())
    {
        const std::shared_ptr<GameObject>& object = id_object_pair.second;
        if (object == nullptr)
        {
            continue;
        }

        if (BaseRenderer* renderer = object->tryGetComponent(BaseRenderer))
        {
            renderer->RefreshRenderStateFromTransformDispatch();
            ++resubmit_count;
        }
    }

    LOG_INFO(ZRender, "ResubmitActiveLevelRenderers: {} renderer(s)", resubmit_count);
}
