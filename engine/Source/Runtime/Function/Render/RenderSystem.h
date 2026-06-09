#pragma once

#include "Runtime/Core/Base/EngineSystem.h"
#include "Runtime/Function/Render/RenderEntity.h"
#include "Runtime/Function/Render/RenderFramePipeline.h"
#include "Runtime/Function/Render/RenderGuidAllocator.h"
#include "Runtime/Function/Render/RenderSwapContext.h"
#include "Runtime/Function/Render/RenderType.h"
#include "Runtime/Function/Render/RenderingThread/RHIDrawList.h"

#include "Runtime/Function/Render/Interface/RHIStruct.h"

#include <array>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class WindowSystem;
class RHI;
class RenderResourceBase;
class RenderPipelineBase;
class RenderScene;
class RenderCamera;
class WindowUI;
class DebugDrawManager;

struct EngineContentViewport
{
    float x {0.f};
    float y {0.f};
    float width {0.f};
    float height {0.f};
};

struct CameraPreviewRequest
{
    bool enabled {false};
    EngineContentViewport viewport {};
    std::shared_ptr<RenderCamera> camera {nullptr};
    std::string title;
};

class RenderSystemInitInfo

{
public:
    std::string ui_pass_name;
    WindowSystem* window_system;
    RHI* rhi;
};
class RenderSystem : public IEngineSystem
{
public:
    std::string GetName() const override { return "RenderSystem"; }
    SystemInitPhase GetInitPhase() const override { return SystemInitPhase::Rendering; }
    std::vector<std::type_index> GetDependencies() const override;

    RenderSystem() = default;
    bool Initialize() override;
    virtual void Tick(float delta_time);

    // Game-thread barrier: drain render/RHI workers and wait for pipelined frames.
    // Required before synchronous GPU readback (mesh picking, buffer Map, etc.).
    void FlushRenderingCommands();
    void Shutdown() override;

    void SwapLogicRenderData();
    RenderSwapContext& GetSwapContext();
    std::shared_ptr<RenderCamera>
    GetRenderCamera(ViewportType viewportType) const;  // Legacy: returns first camera
    std::shared_ptr<RenderCamera> GetCamera(ViewportType camera_id) const;
    std::vector<std::shared_ptr<RenderCamera>> GetAllCameras();
    RHI* GetRHI() const;

    void InitializeUIRenderBackend(WindowUI* window_ui);

    void
    UpdateViewport(ViewportType viewport_id, float offset_x, float offset_y, float width, float height);
    uint32_t GetGuidOfPickedMesh(const Vector2& picked_uv);
    GObjectID GetGObjectIDByMeshID(uint32_t mesh_id) const;

    EngineContentViewport GetViewport(ViewportType type) const;
    // Scene-panel viewport published with the current frame's swap data (render thread).
    bool TryGetRenderSceneViewport(RHIViewport& out_viewport, RHIRect2D& out_scissor) const;
    std::shared_ptr<RenderScene> getRenderScene() const { return m_RenderScene; }

    void CreateAxis(std::array<RenderEntity, 3> axis_entities, std::array<RenderMeshData, 3> mesh_datas);

    void SetVisibleAxis(std::optional<RenderEntity> axis);
    void SetSkyboxVisible(ViewportType viewport_type, bool visible);
    bool IsSkyboxVisible(ViewportType viewport_type) const;
    void SetSelectedAxis(size_t selected_axis);
    void SetCameraPreview(std::shared_ptr<RenderCamera> camera,
                          EngineContentViewport viewport,
                          const std::string& title);
    void ClearCameraPreview();
    CameraPreviewRequest getCameraPreviewRequest() const { return m_CameraPreviewRequest; }
    GuidAllocator<GameObjectPartId>& GetGOInstanceIdAllocator();

    GuidAllocator<MeshSourceDesc>& GetMeshAssetIdAllocator();

    void ClearForLevelReloading();

    // Re-queue every BaseRenderer in the active level (after scene open / reload).
    void ResubmitActiveLevelRenderers();

    // 获取渲染管线（供 Editor 注册回调使用）
    std::shared_ptr<RenderPipelineBase> getRenderPipeline() const { return m_RenderPipeline; }

    std::shared_ptr<RenderResourceBase> getRenderResource() const { return m_RenderResource; }

    // Flush + marshal fn onto the RHI thread when parallel rendering is active
    // (runs inline when single-threaded). The sanctioned way for game-thread /
    // editor UI code to perform synchronous GPU work (readbacks, offscreen
    // previews) without racing the RHI worker. See doc/THREADING_GUIDE.md.
    void RunSynchronizedGpuReadback(std::function<void()> fn);

    // 为独立工具（如 TexPreview）设置最小渲染管线
    // 只配置 UIPass，不加载 3D 渲染通道
    void SetupMinimalPipeline();

private:
    RenderSwapContext m_SwapContext;

    RHI* m_Rhi;
    // Unity-style: Multiple cameras support
    std::vector<std::shared_ptr<RenderCamera>> m_Cameras;  // Camera ID -> Camera mapping
    std::shared_ptr<RenderScene> m_RenderScene;
    std::shared_ptr<RenderResourceBase> m_RenderResource;
    std::shared_ptr<RenderPipelineBase> m_RenderPipeline;
    CameraPreviewRequest m_CameraPreviewRequest;

    RHIViewport m_RenderThreadSceneViewport {};
    RHIRect2D m_RenderThreadSceneScissor {};
    bool m_HasRenderThreadSceneViewport {false};

    void SyncGameCameraFromMainCamera();

    void ProcessSwapData();

    // If r.RenderPath changed (editor toggle / console), swap the active render-path
    // module at a safe boundary: drain the render+RHI pipeline, switch on the RHI
    // thread (GPU idle), and re-point the RenderResource descriptor-set layouts that
    // reference the (now-recreated) main-camera pass. Called at the top of Tick.
    void ApplyPendingRenderPathChange();
    void RewireRenderResourceLayoutsAfterPathSwitch();

    void TickSingleThreaded(float delta_time);
    void TickRenderThread(float delta_time);
    void TickRHIThread();
    static bool ShouldUseParallelRendering();

    static constexpr uint32_t kMaxFrameDrawLists = 3;

    uint32_t AllocateFrameDrawListSlot();
    RHIDrawList& GetFrameDrawList(uint32_t slot);

    std::array<RHIDrawList, kMaxFrameDrawLists> m_FrameDrawLists {};
    std::atomic<uint32_t> m_FrameDrawListSerial {0};
    uint32_t m_ActiveFrameDrawListSlot {0};

    friend struct RenderSyncGameCameraCmd;
    friend struct RenderProcessSwapDataCmd;
    friend struct RenderUpdateSceneCmd;
    friend struct RenderPreparePassDataCmd;
    friend struct RenderBuildDrawListsCmd;
    friend struct RenderDispatchRHICommandsCmd;
    friend struct RHIPrepareContextCmd;
    friend struct RHISubmitDrawListsCmd;
};
