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
            system->m_RenderScene->SetVisibleNodesReference();

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

#if defined(Z_HAS_VULKAN)
        if (auto debug_draw = GET_SYSTEM(DebugDrawManager))
        {
            debug_draw->Tick(delta_time);
        }
#endif
    }

    const char* GetDebugName() const override { return "RenderUpdateScene"; }
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
        ENQUEUE_RENDER_COMMAND(RenderPreparePassDataCmd, &render_system);
        ENQUEUE_RENDER_COMMAND(RenderBuildDrawListsCmd, &render_system, frame_draw_list_slot);
        ENQUEUE_RENDER_COMMAND(RenderDispatchRHICommandsCmd, &render_system, frame_draw_list_slot);
        return;
    }
#endif

    ENQUEUE_RENDER_COMMAND(RenderProcessSwapDataCmd, &render_system);
    ENQUEUE_RENDER_COMMAND(RenderSyncGameCameraCmd, &render_system);
    ENQUEUE_RENDER_COMMAND(RenderUpdateSceneCmd, &render_system, delta_time);
    ENQUEUE_RENDER_COMMAND(RenderPreparePassDataCmd, &render_system);
    ENQUEUE_RENDER_COMMAND(RenderBuildDrawListsCmd, &render_system, frame_draw_list_slot);
    ENQUEUE_RENDER_COMMAND(RenderDispatchRHICommandsCmd, &render_system, frame_draw_list_slot);
}
