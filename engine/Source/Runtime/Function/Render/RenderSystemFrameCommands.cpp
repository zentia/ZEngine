#include "RenderSystemFrameCommands.h"

#include "Runtime/Function/Framework/Level/Level.h"
#include "Runtime/Function/Framework/World/WorldManager.h"
#include "Runtime/Function/Render/Interface/RHI.h"
#include "Runtime/Function/Render/RenderPipeline.h"
#include "Runtime/Function/Render/RenderResource.h"
#include "Runtime/Function/Render/RenderScene.h"
#include "Runtime/Function/Render/RenderSystem.h"
#include "Runtime/Function/Render/RenderingThread/RenderingThread.h"

#if defined(Z_HAS_VULKAN)
    #include "Runtime/Function/Render/DebugDraw/DebugDrawManager.h"
#endif

struct RenderSyncGameCameraCmd final : IRenderCommand
{
    RenderSystem* system {nullptr};

    explicit RenderSyncGameCameraCmd(RenderSystem* in_system)
        : system(in_system) {}

    void Execute() override
    {
        if (system != nullptr)
        {
            system->SyncGameCameraFromMainCamera();
        }
    }

    const char* GetDebugName() const override { return "RenderSyncGameCamera"; }
};

struct RenderProcessSwapDataCmd final : IRenderCommand
{
    RenderSystem* system {nullptr};

    explicit RenderProcessSwapDataCmd(RenderSystem* in_system)
        : system(in_system) {}

    void Execute() override
    {
        if (system != nullptr)
        {
            system->ProcessSwapData();
        }
    }

    const char* GetDebugName() const override { return "RenderProcessSwapData"; }
};

struct RenderUpdateSceneCmd final : IRenderCommand
{
    RenderSystem* system {nullptr};
    float delta_time {0.0f};

    RenderUpdateSceneCmd(RenderSystem* in_system, float in_delta_time)
        : system(in_system), delta_time(in_delta_time)
    {
    }

    void Execute() override
    {
        if (system == nullptr || system->m_RenderPipeline == nullptr || system->m_RenderScene == nullptr ||
            system->m_RenderResource == nullptr)
        {
            return;
        }

#if defined(_WIN32)
        if (system->m_Rhi && system->m_Rhi->getGraphicsAPI() == GraphicsAPI::DirectX12)
        {
            // Rotate **once per frame**, not once per camera: the scene is filled
            // for every camera in turn and only the last one's result would
            // survive a per-call rotation.
            system->m_RenderScene->BeginVisibleNodeUpdate();

            Level* active_level = nullptr;
            if (auto world_manager = GET_SYSTEM(WorldManager))
            {
                active_level = world_manager->getCurrentActiveLevel();
            }
            system->m_RenderScene->SyncPointLightsFromLevel(active_level);

            auto render_resource = std::static_pointer_cast<RenderResource>(system->m_RenderResource);
            for (auto&& camera : system->m_Cameras)
            {
                system->m_RenderResource->UpdatePerFrameBuffer(system->m_RenderScene, camera);
                system->m_RenderScene->UpdateVisibleObjects(render_resource, camera);
            }

            // Publish only now that the set is complete -- the RHI thread may still
            // be drawing an earlier frame out of one of the other slots.
            system->m_RenderScene->PublishVisibleNodes();
            return;
        }
#endif

        if (system->m_RenderScene)
        {
            Level* active_level = nullptr;
            if (auto world_manager = GET_SYSTEM(WorldManager))
            {
                active_level = world_manager->getCurrentActiveLevel();
            }
            system->m_RenderScene->SyncPointLightsFromLevel(active_level);
        }

        // See the DX12 branch above: rotate once per frame, publish once the whole
        // set is complete.
        system->m_RenderScene->BeginVisibleNodeUpdate();

        auto render_resource = std::static_pointer_cast<RenderResource>(system->m_RenderResource);
        for (auto&& camera : system->m_Cameras)
        {
            system->m_RenderResource->UpdatePerFrameBuffer(system->m_RenderScene, camera);
            if (render_resource && render_resource->m_TextureStreamingManager)
            {
                auto streaming_manager = render_resource->m_TextureStreamingManager;
                Vector3 camera_pos = camera->position();
                Vector3 camera_forward = camera->forward();
                auto fov_pair = camera->getFOV();
                float fov = fov_pair.y;
                float aspect = camera->getAspect();

                auto swapchain_info = system->m_Rhi->GetSwapchainInfo();
                streaming_manager->UpdateStreaming(
                    camera_pos, camera_forward, fov, aspect, swapchain_info.extent.width, swapchain_info.extent.height);
                streaming_manager->Tick(delta_time);
            }

            system->m_RenderScene->UpdateVisibleObjects(render_resource, camera);
        }

        system->m_RenderScene->PublishVisibleNodes();

#if defined(Z_HAS_VULKAN)
        if (auto debug_draw = GET_SYSTEM(DebugDrawManager))
        {
            debug_draw->Tick(delta_time);
        }
#endif
    }

    const char* GetDebugName() const override { return "RenderUpdateScene"; }
};

// Lumen 是纯 CPU 的（距离场体素化 + 表面缓存 + 辐照度 probe 网格），所以它在帧里
// 的位置是"场景更新之后、PreparePassData 之前"：GI 读的是场景实体，而它算出来的
// 间接光要在本帧的 GPU 资源上传之前写好，才能进 per-frame uniform。
struct RenderLumenCmd final : IRenderCommand
{
    RenderSystem* system {nullptr};
    float delta_time {0.0f};

    RenderLumenCmd(RenderSystem* in_system, float in_delta_time)
        : system(in_system), delta_time(in_delta_time)
    {
    }

    void Execute() override
    {
        if (system == nullptr || system->m_RenderPipeline == nullptr)
        {
            return;
        }

        auto render_scene = system->m_RenderScene;
        auto camera = system->GetRenderCamera(ViewportType::game);
        if (render_scene == nullptr || camera == nullptr)
        {
            return;
        }

        system->m_RenderPipeline->UpdateLumen(render_scene, camera, delta_time);
    }

    const char* GetDebugName() const override { return "RenderLumen"; }
};

// Nanite 也是纯 CPU 的（集群构建 + 逐簇裁剪 + 批次组装），所以它和 Lumen 站在帧里同一
// 个位置：读场景实体，必须在可见性更新之后；产出的批次要在 PreparePassData 之前写好。
struct RenderNaniteCmd final : IRenderCommand
{
    RenderSystem* system {nullptr};
    float delta_time {0.0f};

    RenderNaniteCmd(RenderSystem* in_system, float in_delta_time)
        : system(in_system), delta_time(in_delta_time)
    {
    }

    void Execute() override
    {
        // 走 RenderSystem 上那个唯一的入口，别在这里再抄一遍判空：阶段 58 之前
        // Nanite 的推进只在"并行 + DX12"这一支里，别的 Tick 路径各抄各的（或者干脆
        // 没抄），于是漏了也没人发现。
        if (system != nullptr)
        {
            system->UpdateNaniteForFrame(delta_time);
        }
    }

    const char* GetDebugName() const override { return "RenderNanite"; }
};

struct RenderPreparePassDataCmd final : IRenderCommand
{
    RenderSystem* system {nullptr};

    explicit RenderPreparePassDataCmd(RenderSystem* in_system)
        : system(in_system) {}

    void Execute() override
    {
        if (system != nullptr && system->m_RenderPipeline != nullptr && system->m_RenderResource != nullptr)
        {
            system->m_RenderPipeline->PreparePassData(system->m_RenderResource);
        }
    }

    const char* GetDebugName() const override { return "RenderPreparePassData"; }
};

struct RenderBuildDrawListsCmd final : IRenderCommand
{
    RenderSystem* system {nullptr};
    uint32_t frame_draw_list_slot {0};

    RenderBuildDrawListsCmd(RenderSystem* in_system, uint32_t in_frame_draw_list_slot)
        : system(in_system), frame_draw_list_slot(in_frame_draw_list_slot)
    {
    }

    void Execute() override
    {
        if (system != nullptr && system->m_RenderPipeline != nullptr && system->m_RenderResource != nullptr)
        {
            RHIDrawList& draw_list = system->GetFrameDrawList(frame_draw_list_slot);
            system->m_RenderPipeline->BuildDrawLists(system->m_RenderResource, draw_list);
        }
    }

    const char* GetDebugName() const override { return "RenderBuildDrawLists"; }
};

struct RHIPrepareContextCmd final : IRenderCommand
{
    RenderSystem* system {nullptr};

    explicit RHIPrepareContextCmd(RenderSystem* in_system)
        : system(in_system) {}

    void Execute() override
    {
        if (system != nullptr && system->m_Rhi != nullptr)
        {
            system->m_Rhi->PrepareContext();
        }
    }

    const char* GetDebugName() const override { return "RHIPrepareContext"; }
};

// ---------------------------------------------------------------------------------------------
// VirtualTexture 的两条命令（并行路径：Vulkan 分支与 DX12 分支都排；单线程路径走
// UpdateVirtualTextureForFrame）。
//
// 为什么拆成两条、分别排在两个线程上：VT 的 GPU 工作（页上传、页表刷新、建池、销毁页表纹理）全是
// "一次性命令缓冲 + vkQueueSubmit + vkQueueWaitIdle"，而 Vulkan 的图形队列和"当前命令缓冲"归
// **RHI 线程**所有。把它放到渲染线程会和 RHI 线程并发提交同一个队列——这正是下面那段注释里
// Nanite 在 Vulkan 上第一帧崩溃的同一类问题。所以：
//   * 渲染线程命令：读 CVar / 相机、加载源图（磁盘 IO，不该堵 RHI 线程）、注册纹理、请求页——纯 CPU；
//   * RHI 线程命令：建池、销毁退役纹理、页上传 / 页表刷新——所有 GPU 工作，和帧提交在同一线程上
//     天然串行，不存在并发提交。
// ---------------------------------------------------------------------------------------------
struct RenderVirtualTextureCpuCmd final : IRenderCommand
{
    RenderSystem* system {nullptr};
    float delta_time {0.0f};

    RenderVirtualTextureCpuCmd(RenderSystem* in_system, float in_delta_time)
        : system(in_system), delta_time(in_delta_time)
    {
    }

    void Execute() override
    {
        if (system != nullptr)
        {
            system->UpdateVirtualTextureCpu(delta_time);
        }
    }

    const char* GetDebugName() const override { return "RenderVirtualTextureCpu"; }
};

struct RHIVirtualTextureGpuCmd final : IRenderCommand
{
    RenderSystem* system {nullptr};

    explicit RHIVirtualTextureGpuCmd(RenderSystem* in_system)
        : system(in_system) {}

    void Execute() override
    {
        if (system != nullptr)
        {
            system->UpdateVirtualTextureGpu();
        }
    }

    const char* GetDebugName() const override { return "RHIVirtualTextureGpu"; }
};

struct RHISubmitDrawListsCmd final : IRenderCommand
{
    RenderSystem* system {nullptr};
    uint32_t frame_draw_list_slot {0};

    RHISubmitDrawListsCmd(RenderSystem* in_system, uint32_t in_frame_draw_list_slot)
        : system(in_system), frame_draw_list_slot(in_frame_draw_list_slot)
    {
    }

    void Execute() override
    {
        if (system != nullptr && system->m_Rhi != nullptr && system->m_RenderPipeline != nullptr)
        {
            const RHIDrawList& draw_list = system->GetFrameDrawList(frame_draw_list_slot);
            system->m_RenderPipeline->SubmitDrawLists(system->m_Rhi, system->m_RenderResource, draw_list);
        }
    }

    const char* GetDebugName() const override { return "RHISubmitDrawLists"; }
};

struct RenderDispatchRHICommandsCmd final : IRenderCommand
{
    RenderSystem* system {nullptr};
    uint32_t frame_draw_list_slot {0};

    RenderDispatchRHICommandsCmd(RenderSystem* in_system, uint32_t in_frame_draw_list_slot)
        : system(in_system), frame_draw_list_slot(in_frame_draw_list_slot)
    {
    }

    void Execute() override
    {
        if (system != nullptr)
        {
            RenderingThread::EnqueueRHICommand(std::make_unique<RHIPrepareContextCmd>(system));
#if defined(Z_HAS_VULKAN)
            // VT 的 GPU 工作：排在 PrepareContext 之后、SubmitDrawLists 之前。它用自己的一次性命令
            // 缓冲，不碰"当前命令缓冲"，所以不依赖 PrepareContext；放在这里只是为了和帧提交同在
            // RHI 线程、同一个批里串行执行（不会和 SubmitDrawLists 的提交交错）。DX12 的上传走
            // 自己专用的 copy 命令列表（DX12RHI::UploadImageRegionImmediate），同样不碰帧命令列表。
            RenderingThread::EnqueueRHICommand(std::make_unique<RHIVirtualTextureGpuCmd>(system));
#endif
            RenderingThread::EnqueueRHICommand(
                std::make_unique<RHISubmitDrawListsCmd>(system, frame_draw_list_slot));
            RenderingThread::SubmitRHICommandBatch();
        }
    }

    const char* GetDebugName() const override { return "RenderDispatchRHICommands"; }
};

void BuildRenderSystemFrameCommands(RenderSystem& render_system, float delta_time, uint32_t frame_draw_list_slot)
{
#if defined(_WIN32)
    if (auto rhi = render_system.GetRHI();
        rhi && rhi->getGraphicsAPI() == GraphicsAPI::DirectX12)
    {
        ENQUEUE_RENDER_COMMAND(RenderSyncGameCameraCmd, &render_system);
        ENQUEUE_RENDER_COMMAND(RenderProcessSwapDataCmd, &render_system);
        ENQUEUE_RENDER_COMMAND(RenderUpdateSceneCmd, &render_system, delta_time);
        ENQUEUE_RENDER_COMMAND(RenderLumenCmd, &render_system, delta_time);
        ENQUEUE_RENDER_COMMAND(RenderNaniteCmd, &render_system, delta_time);
#if defined(Z_HAS_VULKAN)
        // VT 的 CPU 半边（渲染线程）。DX12 分支此前没有它——只有 RHI 线程那一半
        // （RHIVirtualTextureGpuCmd，见 RenderDispatchRHICommandsCmd）每帧在跑，生产者从没被创建，
        // 所以 DX12 下 r.VirtualTexture.Enable 1 是"看起来开了、其实什么都没发生"。
        ENQUEUE_RENDER_COMMAND(RenderVirtualTextureCpuCmd, &render_system, delta_time);
#endif
        ENQUEUE_RENDER_COMMAND(RenderPreparePassDataCmd, &render_system);
        ENQUEUE_RENDER_COMMAND(RenderBuildDrawListsCmd, &render_system, frame_draw_list_slot);
        ENQUEUE_RENDER_COMMAND(RenderDispatchRHICommandsCmd, &render_system, frame_draw_list_slot);
        return;
    }
#endif

    ENQUEUE_RENDER_COMMAND(RenderProcessSwapDataCmd, &render_system);
    ENQUEUE_RENDER_COMMAND(RenderSyncGameCameraCmd, &render_system);
    ENQUEUE_RENDER_COMMAND(RenderUpdateSceneCmd, &render_system, delta_time);
    ENQUEUE_RENDER_COMMAND(RenderLumenCmd, &render_system, delta_time);
    // ⚠️ 这一支（并行 + 非 DX12 = Vulkan）**不能**在渲染线程推进 Nanite：Nanite 的 GPU 剔除
    // 往 `GetCurrentCommandBuffer()` 里录，而 Vulkan 的"当前命令缓冲"归 **RHI 线程**所有，
    // 要到 RHI 线程的 RHIPrepareContextCmd 才设上 ——
    //   * 第一帧它的 VkCommandBuffer 还是空 ⇒ vkCmdBindPipeline(NULL) 直接 AV（实测 0x28，
    //     NaniteRenderer::DispatchClusterCulling → hooked_vkCmdBindPipeline）；
    //   * 之后每帧拿到的是 RHI 线程正在录 / 已提交的那一条 ⇒ 两个线程同时录同一条命令缓冲。
    // 阶段 58（3d41c982）把它补进来时 Vulkan 编辑器一启动就崩。要在 Vulkan 并行路径上跑
    // Nanite，得把它挪成 RHI 命令（排在 PrepareContext 之后、且命令缓冲已 Begin），
    // 那是另一件事；在那之前这里保持阶段 58 之前的样子。
    //
    // VT 不同：它不用"当前命令缓冲"，GPU 工作全是自带的一次性命令缓冲，所以可以安全地放到 RHI
    // 线程（RHIVirtualTextureGpuCmd，见 RenderDispatchRHICommandsCmd）。这里只排它的 CPU 半边。
#if defined(Z_HAS_VULKAN)
    ENQUEUE_RENDER_COMMAND(RenderVirtualTextureCpuCmd, &render_system, delta_time);
#endif
    ENQUEUE_RENDER_COMMAND(RenderPreparePassDataCmd, &render_system);
    ENQUEUE_RENDER_COMMAND(RenderBuildDrawListsCmd, &render_system, frame_draw_list_slot);
    ENQUEUE_RENDER_COMMAND(RenderDispatchRHICommandsCmd, &render_system, frame_draw_list_slot);
}
