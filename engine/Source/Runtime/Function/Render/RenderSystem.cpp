#include "RenderSystem.h"

// 这三个放在 `#if` 外面：关卡重载 / 全局资源上传现在是**平台无关**的一份实现
// （文件末尾那段），两个平台都要这几个头。
//
// MainCameraRp1Pass.h 在所有平台都编译（见 Runtime/CMakeLists.txt 里
// "MainCameraRp1Pass / MainCameraFramebufferResources / ShadowPassShared 这些完全
// 平台无关的 TU" 那段注释），它自己也只 include 平台无关的 pass 头。
#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Core/Memory/MemoryManager.h"
// 心跳里的相机探针要用 CameraComponent::GetPosition()（Level.h 只是前向声明）。
#include "Runtime/Function/Framework/Component/Camera/CameraComponent.h"
// 心跳里的点光源探针要数关卡里带 LightComponent 的 GameObject（同样只是前向声明）。
#include "Runtime/Function/Framework/Component/Light/LightComponent.h"
#include "Runtime/Function/Framework/Component/Mesh/BaseRenderer.h"
#include "Runtime/Function/Framework/Level/Level.h"
#include "Runtime/Function/Framework/World/WorldManager.h"
// 心跳里的 Nanite 探针要读 NaniteRenderPass / NaniteRenderer（RenderPipelineBase.h
// 里的 GetNanitePass 只前向声明了这个类）。
#include "Runtime/Function/Render/Nanite/NaniteRenderer.h"
#include "Runtime/Function/Render/Passes/MainCameraRp1Pass.h"
#include "Runtime/Function/Render/Spine/SpineGpuRenderer.h"

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

    // ---- GlobalRenderingRes 必须在**建相机之前**读 ----
    //
    // 以前两台相机是硬编码的 `LookAt((0,-10,5), ZERO, UNIT_Z)` + 16:9 + 0.1/1000，
    // 完全不看 `GlobalRenderingRes::m_CameraConfig`。这份精简版 Initialize 当初
    // 只是为了把 UI 画出来，硬编码没有代价；场景管线打通之后就成了洞：
    //   * pose 对不上：配置里是 (-5,0,3) → (-4,0,3)，硬编码是 (0,-10,5) → 原点；
    //   * **aspect 对不上**：配置 1280x768 = 1.667，硬编码 16:9 = 1.778，而编辑器
    //     的 game 视口面板就是 1524x915 = 1.666 ⇒ 画面被横向拉伸，而且**不报任何
    //     错**（和 0x0 视口同一类）。
    // 同一份指针顺带给下面的 IBL / LUT 用，所以只 load 一次、只在最后销毁。
    GlobalRenderingRes* global_rendering_res = nullptr;
    if (auto* config_manager = GET_SYSTEM(ConfigManager))
    {
        const eastl::string& global_rendering_url = config_manager->GetGlobalRenderingResUrl();
        if (auto* asset_manager = GET_SYSTEM(AssetManager); !global_rendering_url.empty())
        {
            global_rendering_res = asset_manager->loadAsset<GlobalRenderingRes>(global_rendering_url);
        }
    }

    const bool has_camera_config = (global_rendering_res != nullptr);
    const float cam_znear = has_camera_config ? global_rendering_res->m_CameraConfig.m_ZNear : 0.1f;
    const float cam_zfar  = has_camera_config ? global_rendering_res->m_CameraConfig.m_ZFar : 1000.0f;
    const float cam_aspect =
        (has_camera_config && global_rendering_res->m_CameraConfig.m_Aspect.y > 0.0f)
            ? global_rendering_res->m_CameraConfig.m_Aspect.x / global_rendering_res->m_CameraConfig.m_Aspect.y
            : (16.0f / 9.0f);

    auto make_camera = [&](RenderCameraType type, const char* target_texture)
    {
        auto camera = std::make_shared<RenderCamera>();
        if (has_camera_config)
        {
            const CameraPose& pose = global_rendering_res->m_CameraConfig.m_Pose;
            camera->LookAt(pose.m_Position, pose.m_Target, pose.m_Up);
        }
        else
        {
            camera->LookAt(Vector3(0.0f, -10.0f, 5.0f), Vector3::ZERO, Vector3::UNIT_Z);
        }
        camera->m_Znear = cam_znear;
        camera->m_Zfar  = cam_zfar;
        camera->SetAspect(cam_aspect);
        camera->setTargetTexture(target_texture);
        camera->m_CurrentCameraType = type;
        return camera;
    };

    auto game_camera = make_camera(RenderCameraType::Game, "game");
    m_Cameras.push_back(game_camera);

    auto scene_camera = make_camera(RenderCameraType::Editor, "scene");
    m_Cameras.push_back(scene_camera);

    LOG_INFO(ZRender,
             "RenderSystem(Apple): cameras from {} -- pos=({},{},{}) znear={} zfar={} aspect={}",
             has_camera_config ? "GlobalRenderingRes::m_CameraConfig" : "hard-coded fallback",
             game_camera->position().x, game_camera->position().y, game_camera->position().z,
             cam_znear, cam_zfar, cam_aspect);

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

    // ---- 环境光 + 方向光 ----
    //
    // 这两样**只有非 Apple 那条 Initialize 设过**（`m_AmbientLight` /
    // `m_DirectionalLight`），Apple 这一条从来没设 ⇒ RenderScene 里一直是默认
    // 构造的 (0,0,0)。而 `RenderResourcePerFrame` 正是拿它们填每帧 UBO 的
    // （`scene_directional_light.color` / `ambient_light`），所以 Apple 上：
    //   * 方向光方向 = 颜色 = (0,0,0) ⇒ 主光**完全不参与**；
    //   * 环境光 = 0；
    //   * 画面只剩 IBL 在照，而且**不报任何错**（和 0x0 视口、16:9 相机同一类）。
    // 真机探针（修复前）：dir_light=(0,0,0) dir_color=(0,0,0) ambient=(0,0,0)，
    // 而 rendering.global.json 是方向 (0.5,-0.5,0.5)、颜色 (1,1,1)、环境光
    // (0.03,0.03,0.03)。
    if (global_rendering_res != nullptr)
    {
        m_RenderScene->m_AmbientLight = {global_rendering_res->m_AmbientLight.toVector3()};
        m_RenderScene->m_DirectionalLight.m_Direction =
            global_rendering_res->m_DirectionalLight.m_Direction.normalisedCopy();
        m_RenderScene->m_DirectionalLight.m_Color =
            global_rendering_res->m_DirectionalLight.m_Color.toVector3();
    }

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
    // 全局资源已经在建相机那一步 load 过一次了（同一份指针），这里直接用。
    if (global_rendering_res != nullptr)
    {
        LevelIBLResourceDesc ibl_desc;
        ibl_desc.m_SkyboxIrradianceMap = global_rendering_res->m_SkyboxIrradianceMap;
        ibl_desc.m_SkyboxSpecularMap   = global_rendering_res->m_SkyboxSpecularMap;
        ibl_desc.m_BrdfMap             = global_rendering_res->m_BrdfMap;
        render_resource->UploadIblResources(m_Rhi, ibl_desc);

        render_resource->UploadColorGradingLut(m_Rhi, global_rendering_res->m_ColorGradingMap);
        MemoryManager::DestroyObject(global_rendering_res);
        global_rendering_res = nullptr;
    }
    else
    {
        LOG_WARNING(ZRender,
                    "RenderSystem(Apple): GlobalRenderingRes unavailable -- IBL and color grading fall back "
                    "to neutral textures");
    }

    RenderPipelineInitInfo pipeline_init_info;
    pipeline_init_info.render_resource = m_RenderResource;
    m_RenderPipeline = std::make_shared<RenderPipeline>();
    m_RenderPipeline->m_Rhi = m_Rhi;
    m_RenderPipeline->Initialize(pipeline_init_info);
    return true;
}

void RenderSystem::Tick(float delta_time)
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
                // 可见节点列表是三槽环（RenderScene::kVisibleNodeSetCount）：先轮转到下一
                // 槽、填完**所有**相机、最后才发布。Apple 虽然同线程跑（没有那个"RHI 线程
                // 还在读旧帧"的竞态），但顺序必须跟非 Apple 一致 —— 否则这里永远写槽 0，
                // 环就退化成单槽，将来 Apple 一旦拆线程就会立刻踩到空材质。
                m_RenderScene->BeginVisibleNodeUpdate();

                // 点光源（阶段 55）：非 Apple 有两条路会同步点光源 —— TickRenderThread
                // 和 RenderUpdateSceneCmd::Execute —— Apple 这条精简路径从来没调过
                // `SyncPointLightsFromLevel`，于是 `m_PointLightList` 恒为空：场景里
                // 摆了 LightComponent 也一个都不亮，而且**不报任何错**（真机探针是
                // plights=0 而 gos=34 lcmp=2）。必须排在 UpdatePerFrameBuffer 之前，
                // 那一步才是把点光源写进每帧 UBO 的人。
                Level* active_level = nullptr;
                if (auto world_manager = GET_SYSTEM(WorldManager))
                {
                    active_level = world_manager->getCurrentActiveLevel();
                }
                m_RenderScene->SyncPointLightsFromLevel(active_level);

                for (auto&& camera : m_Cameras)
                {
                    m_RenderResource->UpdatePerFrameBuffer(m_RenderScene, camera);
                    m_RenderScene->UpdateVisibleObjects(render_resource, camera);
                }
                m_RenderScene->PublishVisibleNodes();

                // 阶段 58：Apple 这条精简 Tick 以前从来不推进 Nanite —— Nanite 的
                // 生产者只在"并行渲染 + DX12"那一支帧命令里，于是集群构建 / 裁剪 /
                // 批次组装在真机上一次都没跑过，而且**不报任何错**（心跳里
                // nanite=(has=1 inst=0 ...) 就是实锤）。位置跟非 Apple 一致：可见性
                // 更新之后、PreparePassData 之前。
                UpdateNaniteForFrame(delta_time);

                m_RenderPipeline->PreparePassData(m_RenderResource);
                m_ActiveFrameDrawListSlot = 0;
                RHIDrawList& draw_list = GetFrameDrawList(m_ActiveFrameDrawListSlot);

                // storage ring buffer 复位（阶段 69）：这个 buffer 的 `End` 是**只
                // 增**的，必须每帧拨回 `Begin`，否则所有 pass 往里写的数据会一帧一
                // 帧累积，直到撞上 `_GlobalUploadRingbuffersSize` 的上限 assert。
                // DX12 / Vulkan 两支都是在 `SubmitDrawLists()` 里、
                // `draw_list.ExecuteAll()` 之前复位的，而 Apple 这条精简 Tick **从不
                // 走 `SubmitDrawLists`**（那个函数的 Apple 分支压根没有调用者）⇒
                // 这个复位在 Apple 上一次都没发生过。以前只有主相机在往里写、每帧
                // 用量小，能撑几千帧不撞线；打开方向光 shadow 之后每帧用量变大，
                // frame=2641 就 assert 崩了（ips 里是
                // `__assert_rtn` ← `DirectionalLightShadowPass::DrawModel`）。
                // 必须排在 PrepareParticlesForDraw **之前** —— 粒子那一步也要往
                // 同一个 ring buffer 里写数据。
                render_resource->ResetRingBufferOffset(m_Rhi->GetCurrentFrameIndex());

                // 粒子的两步每帧推进必须夹在 draw list 录制的两头（顺序和
                // SubmitDrawLists 里那两条一模一样）。Apple 这条精简 Tick **不走
                // SubmitDrawLists**，所以这里必须自己叫一遍 —— 漏了的话粒子永远
                // 静止：compute 管线建好了、billboard 每帧在画、swap 数据也送到了，
                // 唯独 `Simulate()` 一次都不跑 ⇒ 存活数恒为 0，且不报任何错。
                m_RenderPipeline->PrepareParticlesForDraw(m_Rhi->GetCurrentCommandBuffer());
                m_RenderPipeline->BuildDrawLists(m_RenderResource, draw_list);
                draw_list.ExecuteAll();
                m_RenderPipeline->AdvanceParticlesAfterDraw();

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

                // GPU-Spine（控制台 `spine.load` 才有东西画）：场景之后、编辑器 UI 之前 ⇒ 面板盖在它上面，
                // 与 Vulkan（RP2 UI subpass 里画在 UI 之前）同一层次。swapchain pass 是 Apple 上唯一开着的 pass。
                SpineGpuRenderer::Get().DrawInEditorViewport(m_Rhi, render_pass, 0);

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
    // 只给 Apple 心跳里的 `particle=` 字段用。以前不能在这儿 include 是因为
    // ParticlePass.cpp 拉 `vulkan/vulkan.h`；解耦之后它是后端中立的。仍然放在
    // Apple 守卫里：Windows 非 Vulkan 分支不编 ParticlePass.cpp，无条件 include
    // 会让那里的链接去找不存在的符号。
    #include "Runtime/Function/Render/Passes/ParticlePass.h"

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
            // 可见节点列表已改三槽环（RenderScene::kVisibleNodeSetCount），那组
            // m_*PerViewport 数组进了 VisibleNodeSet 的私有存储，只剩这个 getter。
            // 真机探针证实主相机走的是 game 视口（type 0）。
            const size_t visible_count = m_RenderScene != nullptr
                                             ? m_RenderScene->GetMainCameraVisibleMeshNodes(ViewportType::game).size()
                                             : 0u;
            const RHIExtent2D extent = m_Rhi != nullptr ? m_Rhi->GetSwapchainInfo().extent : RHIExtent2D {};
            // 视口/裁剪矩形：RP1 的 SetViewportScissor 用完 TryGetRenderSceneViewport
            // 之后兜底用的就是这两个值。真机探针证实主相机走 **game**（type 0），
            // 所以两个都打。任何一个是 0x0 而它又被人用着，整块画面一格像素都画
            // 不出来，而且**不报任何错** —— 所以必须出现在心跳里。
            RHIViewport vp[2] {};
            RHIRect2D   sc[2] {};
            if (m_Rhi != nullptr)
            {
                for (int i = 0; i < 2; ++i)
                {
                    if (RHIViewport* v = m_Rhi->GetViewport(static_cast<ViewportType>(i)))
                    {
                        vp[i] = *v;
                    }
                    sc[i] = m_Rhi->GetSwapchainInfo().scissor[i];
                }
            }
            // 相机探针（阶段 53）：Apple 上 `SyncGameCameraFromMainCamera` 是空体，
            // 相机 swap data 也被直接丢弃 ⇒ game 相机可能从来没人更新过。这里把
            // game 相机和关卡主相机组件的位置都打出来：两者对不上（cam_sync=0）
            // 就说明这份同步确实缺了。0x0 视口那种"画不出来但不报错"的教训是：
            // 相机错了也照样出画面，只是画面是错的 —— 所以必须进心跳。
            Vector3   game_cam_pos {Vector3::ZERO};
            Vector2   game_cam_fov {0.0f, 0.0f};
            Vector3   main_cam_pos {Vector3::ZERO};
            int       main_cam_valid = 0;
            int       cam_sync = -1;  // -1 = 关卡里没有主相机组件，无从比较

            // 光照探针（阶段 54）：Apple 版 Initialize 从来没有设置过
            // `m_AmbientLight` / `m_DirectionalLight`（只有非 Apple 那条设），而
            // RenderResourcePerFrame 正是拿它们填每帧 UBO 的 ⇒ Apple 上方向光
            // 方向=颜色=(0,0,0)、环境光=0，画面只剩 IBL 在照，且**不报任何错**。
            Vector3 dir_light_dir {Vector3::ZERO};
            Vector3 dir_light_col {Vector3::ZERO};
            Vector3 ambient_light {Vector3::ZERO};
            size_t  point_lights = 0;
            // 点光源探针（阶段 55）：Apple 的 Tick 从不调 `SyncPointLightsFromLevel`
            // ⇒ `m_PointLightList` 恒为空。只看 plights 无法区分"关卡里本来就没灯"
            // 和"有灯但没同步"，所以同时把关卡里的 GameObject 总数和带
            // LightComponent 的数量打出来：gos=31 lcmp=2 plights=0 才是实锤。
            size_t gobject_count = 0;
            size_t light_components = 0;
            // Nanite 探针（阶段 58）：Nanite 以前只在"并行渲染 + DX12"那一支 Tick
            // 路径里被推进，Apple 这条单线程非 DX12 分支从来没调过 ⇒ 集群构建、
            // 裁剪、批次组装在真机上一次都没跑过，而且**不报任何错**。三个数一起
            // 打才能分辨"没 pass / 有 pass 但没实例 / 有实例但没可见簇"：
            // nhas=0 是根本没 Nanite pass，ninst=0 是场景里没认出实例，ncl=0 是
            // 认出来了但全被裁掉（相机没对着它，那就该是 0）。
            int    nanite_has_pass = 0;
            size_t nanite_instances = 0;
            size_t nanite_batches = 0;
            int    nanite_visible = 0;
            int    nanite_gpu = 0;
            // 粒子：has / emit / draw 分开打（见 ParticlePass::GetEmitterCount 的注释）。
            // Metal 上以前是**完全没有粒子 pass**（TU 被排除在 Apple 构建之外），
            // 画面上和"有 pass 但没 emitter"、"有 emitter 但没画"完全一样 —— 一片安静。
            int      particle_has_pass = 0;
            int      particle_emitters = 0;
            int      particle_drawn_emitters = 0;
            uint32_t particle_drawn_particles = 0;
            int      particle_drawn_viewport = -1;
            // 阴影探针（阶段 69）：方向光 shadow map 以前在 Metal 上是"全接好但一个
            // 物体都不画"，而且不报任何错 —— pass 建了、4096² 的 attachment 建了、
            // draw list 里每帧排了 `DirectionalShadow`、主相机也拿到了 view，唯独
            // `IsPointLightShadowEnabled()`（历史遗留名，实际是总开关）答了 false。
            // 三个数分开打才能分辨："开关没开" / "开了但可见节点是空的" / "真画了"：
            // ds_en=0 是门还关着，ds_en=1 而 ds_nodes=0 是可见性没喂东西，
            // ds_en=1 且 ds_nodes>0 才是真的往 shadow map 里画了。
            int    dir_shadow_enabled = 0;
            size_t dir_shadow_nodes = 0;
            int    point_shadow_enabled = 0;
            // CSM（阶段 71）：级数。0 = 这一帧根本没发布过级联数据（CPU 侧没跑），
            // 1 = 退回了单张（后端不支持选层 / 深度范围无效），4 = 级联真的生效。
            // 只有这个数能分辨"4 级全画到第 0 层"和"真的分了 4 层" —— 两者画面
            // 都可能看着像有阴影，但前者远处一定糊。
            uint32_t dir_shadow_cascades = 0;
            if (m_RenderPipeline != nullptr)
            {
                if (NaniteRenderPass* nanite_pass = m_RenderPipeline->GetNanitePass())
                {
                    nanite_has_pass = 1;
                    nanite_instances = nanite_pass->getNaniteRenderer()->GetInstances().size();
                    nanite_batches = nanite_pass->GetRenderBatches().size();
                    nanite_visible = nanite_pass->hasVisibleClusters() ? 1 : 0;
                    // ngpu（阶段 59）：Nanite 的 GPU 路径是**静默**退化到 CPU 的，
                    // inst/batches 全都照常产出 —— 只有这一个数能分辨这一帧的集群
                    // 裁决到底发生在 compute kernel 里还是 CPU 上。
                    nanite_gpu = nanite_pass->getNaniteRenderer()->IsGPUComputeReady() ? 1 : 0;
                }
                if (const ParticlePass* particle_pass =
                        dynamic_cast<const ParticlePass*>(m_RenderPipeline->GetParticlePass()))
                {
                    particle_has_pass       = 1;
                    particle_emitters       = particle_pass->GetEmitterCount();
                    particle_drawn_emitters = particle_pass->GetLastDrawnEmitters();
                    particle_drawn_particles = particle_pass->GetLastDrawnParticles();
                    particle_drawn_viewport  = particle_pass->GetLastDrawnViewport();
                }
            }
            if (m_RenderScene != nullptr)
            {
                dir_light_dir = m_RenderScene->m_DirectionalLight.m_Direction;
                dir_light_col = m_RenderScene->m_DirectionalLight.m_Color;
                ambient_light = m_RenderScene->m_AmbientLight.m_Irradiance;
                point_lights  = m_RenderScene->m_PointLightList.m_Lights.size();
                // 方向光 shadow map 画的就是这一份可见节点列表。
                dir_shadow_nodes = m_RenderScene->GetDirectionalLightVisibleMeshNodes().size();
            }
            if (m_Rhi != nullptr)
            {
                dir_shadow_enabled   = m_Rhi->IsDirectionalLightShadowEnabled() ? 1 : 0;
                point_shadow_enabled = m_Rhi->IsPointLightShadowEnabled() ? 1 : 0;
            }
            if (m_RenderResource != nullptr)
            {
                if (const auto* typed_resource = dynamic_cast<const RenderResource*>(m_RenderResource.get()))
                {
                    dir_shadow_cascades = typed_resource->m_DirectionalLightCascadeCount;
                }
            }
            if (const std::shared_ptr<RenderCamera> game_cam = GetCamera(ViewportType::game))
            {
                game_cam_pos = game_cam->position();
                game_cam_fov = game_cam->getFOV();
            }
            if (Level* probe_level = GET_SYSTEM(WorldManager) != nullptr
                                         ? GET_SYSTEM(WorldManager)->getCurrentActiveLevel()
                                         : nullptr)
            {
                if (const CameraComponent* main_cam = probe_level->GetMainCameraComponent())
                {
                    main_cam_pos = main_cam->GetPosition();
                    main_cam_valid = 1;
                    cam_sync = ((game_cam_pos - main_cam_pos).length() < 0.01f) ? 1 : 0;
                }
                gobject_count = probe_level->getAllGObjects().size();
                for (const auto& [go_id, game_object] : probe_level->getAllGObjects())
                {
                    (void)go_id;
                    if (game_object == nullptr)
                    {
                        continue;
                    }
                    if (const LightComponent* light = game_object->tryGetComponentConst(LightComponent);
                        light != nullptr && light->isEnabled())
                    {
                        ++light_components;
                    }
                }
            }

            LOG_INFO(ZRender,
                     "AppleRender: frame={} entities={} visible={} scene_color_view={} blit_ready={} "
                     "rp2_tonemapped={} swapchain={}x{} "
                     "game_cam=({},{},{}) fov={} main_cam_valid={} main_cam=({},{},{}) cam_sync={} "
                     "dir_light=({},{},{}) dir_color=({},{},{}) ambient=({},{},{}) plights={} gos={} lcmp={} "
                     "nanite=(has={} inst={} batches={} vis={} ngpu={}) "
                     "particle=(has={} emit={} draw={} parts={} vp={}) "
                     "shadow=(dir_en={} dir_nodes={} point_en={} cascades={}) "
                     "depth=(view={} fmt={}) "
                     "game_vp=({},{}) {}x{} game_sc=({},{}) {}x{} scene_vp=({},{}) {}x{} scene_sc=({},{}) {}x{}",
                     s_apple_frame_tick,
                     entity_count,
                     visible_count,
                     (m_RenderPipeline != nullptr && m_RenderPipeline->GetSceneColorView() != nullptr) ? 1 : 0,
                     m_SceneBlit.IsReady() ? 1 : 0,
                     m_RenderPipeline != nullptr && m_RenderPipeline->IsSceneColorTonemapped() ? 1 : 0,
                     extent.width,
                     extent.height,
                     // 实参顺序必须跟上面格式串里占位符的顺序一致（插在 swapchain 之后）。
                     game_cam_pos.x, game_cam_pos.y, game_cam_pos.z,
                     game_cam_fov.x, main_cam_valid,
                     main_cam_pos.x, main_cam_pos.y, main_cam_pos.z, cam_sync,
                     dir_light_dir.x, dir_light_dir.y, dir_light_dir.z,
                     dir_light_col.x, dir_light_col.y, dir_light_col.z,
                     ambient_light.x, ambient_light.y, ambient_light.z,
                     point_lights,
                     gobject_count,
                     light_components,
                     nanite_has_pass,
                     nanite_instances,
                     nanite_batches,
                     nanite_visible,
                     nanite_gpu,
                     particle_has_pass,
                     particle_emitters,
                     particle_drawn_emitters,
                     particle_drawn_particles,
                     particle_drawn_viewport,
                     // 实参顺序必须跟上面格式串里占位符的顺序一致（插在 particle 之后）。
                     dir_shadow_enabled,
                     dir_shadow_nodes,
                     point_shadow_enabled,
                     dir_shadow_cascades,
                     // 场景深度探针：以前 Metal 从不建深度图，view 恒 nullptr ⇒ 深度测试
                     // 与延迟光照反推世界坐标全是空的（洞 #16）。view=1 且 fmt 非 0 才是
                     // 真建出来了（建图时机在 swapchain 尺寸就绪之后，首帧可能是 0/0）。
                     (m_Rhi != nullptr && m_Rhi->GetDepthImageInfo().depth_image_view != nullptr) ? 1 : 0,
                     static_cast<int>(m_Rhi != nullptr ? m_Rhi->GetDepthImageInfo().depth_image_format
                                                       : RHI_FORMAT_UNDEFINED),
                     static_cast<int>(vp[0].x), static_cast<int>(vp[0].y),
                     static_cast<int>(vp[0].width), static_cast<int>(vp[0].height),
                     sc[0].offset.x, sc[0].offset.y, sc[0].extent.width, sc[0].extent.height,
                     static_cast<int>(vp[1].x), static_cast<int>(vp[1].y),
                     static_cast<int>(vp[1].width), static_cast<int>(vp[1].height),
                     sc[1].offset.x, sc[1].offset.y, sc[1].extent.width, sc[1].extent.height);
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

    // Apple 真机验收的可观测性：视口矩形到底有没有送到 RHI。以前
    // `TryGetRenderSceneViewport` 是 `return false` 的桩、Metal 的 UpdateViewport 也
    // 不动 scissor，这条链路出了问题是完全无声的。这里只在**值变化**时打一条。
    //
    // **两个都要记**：真机探针证实主相机 pass 在 Apple 上走的是 `game`（type 0）
    // 视口（编辑器场景面板的矩形由它承载），`scene`（type 1）在 macOS 上没有
    // 发布者（只有 PlayModeView 关窗时清成 0x0）—— 只看 scene 会被误导。
    const int vp_index = static_cast<int>(viewport_id);
    if (vp_index >= 0 && vp_index <= 1)
    {
        static float s_last_viewport_rect[2][4] = {{-1.0f, -1.0f, -1.0f, -1.0f}, {-1.0f, -1.0f, -1.0f, -1.0f}};
        float*      last    = s_last_viewport_rect[vp_index];
        const float rect[4] = {viewport.x, viewport.y, viewport.width, viewport.height};
        if (last[0] != rect[0] || last[1] != rect[1] || last[2] != rect[2] || last[3] != rect[3])
        {
            std::memcpy(last, rect, sizeof(rect));
            LOG_INFO(ZRender,
                     "AppleViewport[{}]: rect = ({}, {}) {}x{}",
                     vp_index,
                     static_cast<int>(viewport.x),
                     static_cast<int>(viewport.y),
                     static_cast<int>(viewport.width),
                     static_cast<int>(viewport.height));
        }
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
bool RenderSystem::TryGetRenderSceneViewport(RHIViewport& out_viewport, RHIRect2D& out_scissor) const
{
    // Apple / Metal 上**没有**渲染线程（`FlushRenderingCommands` 是空体，渲染就在
    // 主线程同步跑），所以 RHI 里的视口状态本身就是最新的 —— 不需要非 Apple 那条
    // "logic swap → render swap" 的快照往返。那条往返存在的唯一理由是游戏线程和
    // 渲染线程不能同时碰 `m_Rhi` 的状态，Apple 上这个前提不成立。
    //
    // 以前这里是 `return false;`，于是 5 个平台无关的消费者全走退化路径：
    //   - MainCameraRp1Pass::SetViewportScissor（5 处）：视口退化成
    //     `m_Rhi->GetViewport(viewport_type)` —— 这个其实是对的（Apple 的
    //     UpdateViewport 有把它设成编辑器给的矩形），但**裁剪矩形**退化成
    //     `GetSwapchainInfo().scissor[viewport_type]`，而 MetalRHI::UpdateViewport
    //     以前只更新 m_Viewports、不动 m_Scissors ⇒ scissor 永远停在 SyncLayerMetrics
    //     给它的整窗值（真机探针证实：修复前 game_sc=(0,0) 2560x1440，修复后
    //     game_sc=(389,104) 1524x915 跟着面板走）。现在 MetalRHI 也同步写 scissor 了
    //     （见那里的注释），这里再把它读出来，两边就和 DX12 一致。
    //   - RenderResourcePerFrame（2 处）：紧跟在后面有一段一模一样的 RHI 退化解，
    //     所以那两处以前也没差；现在走的是同一个值。
    //
    // 顺带一个真机事实：Apple 上主相机 pass 用的是 **game**（type 0）视口承载
    // 编辑器场景面板的矩形；`scene`（type 1）在 macOS 上没有发布者（只有
    // PlayModeView 关窗时清成 0x0）。所以这个函数在 Apple 上当前没有活跃消费者，
    // 修它是为了语义对齐 + 将来谁接上谁就对。
    if (m_Rhi == nullptr)
    {
        return false;
    }

    RHIViewport* viewport = m_Rhi->GetViewport(ViewportType::scene);
    if (viewport == nullptr || viewport->width <= 0.0f || viewport->height <= 0.0f)
    {
        return false;
    }

    out_viewport = *viewport;
    out_scissor  = m_Rhi->GetSwapchainInfo().scissor[static_cast<uint32_t>(ViewportType::scene)];
    return true;
}
// CreateAxis / SetVisibleAxis 以前在这里是**空桩 / 半桩**：CreateAxis 什么都不做
// （不注册 mesh source、不上传），SetVisibleAxis 只翻 pipeline 上的开关、不写 swap
// data（⇒ `m_VisibleAxisUpdatePending` 永远是 false，`m_RenderScene->m_RenderAxis`
// 永远没有值）。结果就是 Apple 上编辑器 gizmo 既没资源也没数据。
//
// 两份实现现在都搬到了文件末尾那段**平台无关**的公共尾巴上（和
// UpdateNaniteForFrame 同一个道理）：编辑器侧（EditorSceneManager::UploadAxisResource）
// 是平台无关的，它调进来拿到的却是空实现，这种不对称没有任何好处。
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
// ClearForLevelReloading 不再有 Apple 专属的空桩：它的实现现在是平台无关的一份
// （文件末尾），Apple 上也会真正清空渲染场景 —— 以前 PIE 停止 / 编辑器重载场景时
// Apple 上什么都不清（实体残留 + instance id 分配器只涨不回 + 交换数据里留着
// 已销毁对象的 desc）。
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
    // 相机 / 视口那几段 Apple 上没有对应的消费者（见下面各自的注释）。
    RenderSwapData& swap_data = m_SwapContext.GetRenderSwapData();

    // ---- 粒子：门闩字段，**必须**在下面的早退之前清 ----
    //
    // IsReadyToSwap() 是拿"render buffer 里还有没有待处理项"当门闩的：只要有一个
    // 字段一直挂着，swap() 就**永远**不会被调用 —— 于是游戏线程写进 logic buffer
    // 的任何东西都再也送不到渲染线程。以前这三个 Reset 写在函数末尾，而函数开头
    // 有一个 `!m_RenderScene || !m_RenderResource || m_Rhi == nullptr` 的早退 ——
    // 只要那个早退被触发一次（比如关卡切换的瞬间），粒子的门闩就永远没人清。
    //
    // 走 `RenderPipeline::ConsumeParticleSwapData()` 而不是在这里手写三个 Reset：
    // 平台无关，有没有粒子 pass 由那一层自己判断。
    //
    // 以前 Metal 上拿不到 ParticlePass，原因是**编译期**的：`ParticlePass.cpp` 经
    // 传递 include 拉到 `vulkan/vulkan.h`，被 `ZENGINE_APPLE_EXCLUDED_PASSES` 整个
    // 排除在 Apple 构建之外；而 `ConsumeParticleSwapData` 的消费代码又被
    // `#if defined(Z_HAS_VULKAN)` 门住 ⇒ Metal **和 DX12** 都落到"只清"分支。
    // 现已解耦：ParticlePass 那个 TU 不再碰 Vulkan 头，消费侧的 Vulkan 门也改成了
    // "有没有粒子 pass"这个真正的判据。
    if (m_RenderPipeline != nullptr)
    {
        m_RenderPipeline->ConsumeParticleSwapData(swap_data, m_SwapContext);
    }
    else
    {
        m_SwapContext.ResetPartilceBatchSwapData();
        m_SwapContext.ResetEmitterTickSwapData();
        m_SwapContext.ResetEmitterTransformSwapData();
    }

    // 相机 / 场景视口：Apple 上同样不走 swap 往返，"清掉即可"。
    //
    // 相机：Apple 是单线程，`SyncGameCameraFromMainCamera` 是空体（没有"游戏相机
    // → 渲染相机"这份拷贝要同步），swap data 里的 FOV / view matrix 对不上任何
    // 需要更新的东西。
    //
    // 场景视口：Apple 的 `TryGetRenderSceneViewport` 是**直接从 RHI 读**的（见那个
    // 函数的注释），不需要"logic swap → render swap"这份快照 —— 那份快照只是为
    // 了让渲染线程不碰游戏线程正在改的 RHI 状态。
    //
    // 注意这里**不消费**是有意的、也是安全的：正因为不依赖这条通道，
    // `TryGetRenderSceneViewport` 在 Apple 上永远拿得到当前帧的值，不存在"卡在
    // swap 里没送过来"的中间态。
    if (swap_data.m_CameraSwapData.has_value())
    {
        m_SwapContext.ResetCameraSwapData();
    }
    if (swap_data.m_SceneViewportUpdatePending)
    {
        m_SwapContext.ResetSceneViewportSwapData();
    }

    if (!m_RenderScene || !m_RenderResource || m_Rhi == nullptr)
    {
        return;
    }

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

    // 相机 / 场景视口这两个门闩字段已经和粒子一起上移到函数开头（见那里）。
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
            // 阶段 58：单线程路径以前从来不推进 Nanite（只有"并行 + DX12"那一支
            // 有），Nanite 在这条路上是死代码且不报错。
            UpdateNaniteForFrame(delta_time);
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

    // 阶段 58：Apple/Metal 走的就是这条非 DX12 单线程分支，而它以前从来不推进
    // Nanite —— 集群构建 / 裁剪 / 批次组装在 Apple 上一次都没跑过，且不报任何错。
    UpdateNaniteForFrame(delta_time);

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

// CreateAxis / SetVisibleAxis 的实现在文件末尾的平台无关尾巴上（见那里）。

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

// ResubmitActiveLevelRenderers 与 ClearForLevelReloading 都搬到文件末尾的平台无关
// 段了（Apple 也要跑）。

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

// 每帧推进 Nanite 的唯一入口。**必须**住在这个平台无关的尾巴上：四条 Tick 路径
// （并行的 DX12 / 非 DX12、单线程的 DX12 / 非 DX12）都要调它，其中 Apple 那条就在
// 上面 `#if defined(__APPLE__)` 的精简 Tick 里 —— 定义要是落在非 Apple 那个大分支
// 里，Apple 就是 undefined symbol（静态库看不出来，只有链 ZEditor 时才炸）。
// ===========================================================================
// 编辑器 gizmo（axis）的上传 + 数据通路。
//
// 这两个以前在 Apple 上是空桩 / 半桩（CreateAxis 什么都不做，SetVisibleAxis 只翻
// pipeline 的开关、不写 swap data），于是 Apple 编辑器里选中物体**没有坐标轴**，
// 而且不报任何错 —— 编辑器侧 `EditorSceneManager::UploadAxisResource()` 是平台无关
// 的，它每次启动都老老实实调进来，拿到的是一份什么都不做的实现。
//
// 和 UpdateNaniteForFrame 一样，**必须**住在这个平台无关的尾巴上。
// ===========================================================================
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
    size_t uploaded = 0;
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
        else
        {
            ++uploaded;
        }
    }

    // 成功也打一条：以前失败才有日志，而"根本没被调用"和"调了但都失败"在日志里
    // 长得一模一样（都是一片安静）。真机上靠这一行确认 gizmo 资源真的到了 GPU。
    LOG_INFO(ZRender,
             "CreateAxis: {}/3 axis meshes uploaded (asset ids {} / {} / {})",
             uploaded,
             mesh_asset_ids[0],
             mesh_asset_ids[1],
             mesh_asset_ids[2]);
}

void RenderSystem::SetVisibleAxis(std::optional<RenderEntity> axis)
{
    // swap data 这一半**不能省**：Apple 上以前只翻了 pipeline 的开关，于是
    // `m_VisibleAxisUpdatePending` 永远是 false ⇒ `m_RenderScene->m_RenderAxis`
    // 永远没有值 ⇒ 就算画的地方有了也没东西可画。
    RenderSwapData& logic_swap = m_SwapContext.GetLogicSwapData();
    logic_swap.m_VisibleAxis = axis;
    logic_swap.m_VisibleAxisUpdatePending = true;

    if (!m_RenderPipeline)
    {
        return;
    }

    m_RenderPipeline->SetAxisVisibleState(axis.has_value());
}

void RenderSystem::UpdateNaniteForFrame(float delta_time)
{
    if (m_RenderPipeline == nullptr || m_RenderScene == nullptr)
    {
        return;
    }
    // Nanite 的场景实例是从 RenderScene 认出来的，所以必须排在可见性更新**之后**；
    // 又要早于 PreparePassData，因为后者上传本帧的 GPU 资源。没有 Nanite pass 时
    // RenderPipelineBase::UpdateNanite 自己就是 no-op。
    m_RenderPipeline->UpdateNanite(m_RenderScene, GetRenderCamera(ViewportType::game), delta_time);
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
// 丢弃关卡数据**之前**的清理：清空渲染场景 + 丢掉交换数据里还压着的 desc。
//
// 这份以前只存在于 `#else`（非 Apple）分支，Apple 上是**空桩**。那时候 Apple 上
// 根本没有渲染实体（Apple 的 ProcessSwapData 曾经也是空体），空桩没有代价；场景
// 管线打通之后它就成了洞：
//   - `m_RenderEntities` / `m_MeshObjectIdMap` 从不清空 ⇒ 旧关卡的实体残留；
//   - `m_InstanceIdAllocator` 从不清空 ⇒ 切一次关卡就涨一截，永不回收；
//   - 交换数据里还压着的 GameObjectDesc 不清 ⇒ 它们持有的是 Play World 对象的
//     引用，而调用方（WorldManager::StopPIE）清完马上就卸载关卡 —— 于是之后
//     ProcessSwapData 拿到的是已销毁的对象。
//
// 调用方只在"即将丢弃关卡数据"时调它（PIE 停止、编辑器重载场景），此时两个
// buffer 里的待处理项都已无意义，直接清掉。Apple 上虽然是单线程、没有跨线程
// 竞态，但"desc 持有已销毁对象"这一点同样成立。
// ===========================================================================
void RenderSystem::ClearForLevelReloading()
{
    // 这一条以前在 Apple 上是空桩，什么都不清也不会有任何报错 —— 所以留一条日志，
    // 让"到底清了没有"在真机上可观测（清掉的实体数是唯一能证伪它的量）。
    const size_t dropped_entities = (m_RenderScene != nullptr) ? m_RenderScene->m_RenderEntities.size() : 0u;

    if (m_RenderScene != nullptr)
    {
        m_RenderScene->ClearForLevelReloading();
    }

    LOG_INFO(ZRender, "ClearForLevelReloading: dropped {} render entities and reset the GameObject swap data",
             dropped_entities);

    // ProcessSwapData 除了消费 render buffer，还会直接读游戏线程的 logic buffer
    // （见其中 "apply mesh deletes/upserts from logic here" 那段），把那里的 desc
    // 直接拷出来做 UpsertGameObject / DeleteEntityByGObjectID —— 所以 logic 这一侧
    // 也必须清。
    m_SwapContext.ResetGameObjectResourceSwapData();
    m_SwapContext.ResetGameObjectToDelete();
    m_SwapContext.ResetLogicGameObjectSwapData();
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
