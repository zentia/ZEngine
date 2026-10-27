#pragma once

#include "Runtime/Core/Base/EngineSystem.h"
#include "Runtime/Function/Render/RenderEntity.h"
#include "Runtime/Function/Render/RenderFramePipeline.h"
#include "Runtime/Function/Render/RenderGuidAllocator.h"
#include "Runtime/Function/Render/RenderSwapContext.h"
#include "Runtime/Function/Render/RenderType.h"
#include "Runtime/Function/Render/RenderingThread/RHIDrawList.h"

#include "Runtime/Function/Render/AppleSceneBlit.h"
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
class VirtualTextureProducer;
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
    // Also re-queues the global render resources (IBL + LUT) so the render thread
    // re-uploads AND rebinds them -- see RequestGlobalResourceUpload().
    void ResubmitActiveLevelRenderers();

    /// `RenderSwapData::m_LevelResourceDesc` 的**唯一生产者**。
    ///
    /// 从 GlobalRenderingRes 读出 IBL 的 12 张 HDR cubemap 面 + BRDF + 颜色分级
    /// LUT，排进 logic swap data；渲染线程的 `ProcessSwapData()` 会上传它并**重绑**
    /// descriptor（见那里的 `RefreshMeshGlobalIblDescriptors`）。
    ///
    /// 这个字段以前全库没有生产者 —— 两个 `ProcessSwapData()` 都读它、
    /// `IsReadyToSwap()` 拿它当门闩，但没有任何地方赋值 ⇒ "关卡热重载刷新全局
    /// 资源"在任何平台都不通。
    void RequestGlobalResourceUpload();

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
    // VT 最小生产者。用 unique_ptr + 不完整类型：VirtualTextureProducer 只在 Vulkan 路径编译，
    // 头里不能 include 它。析构发生在 RenderSystem 销毁时，但**真正的拆除**在 Shutdown() 里
    // 显式做（RHI 此时还活着）——不能指望析构，那时 RHI 可能已经没了。
    // 自定义删除器是为了让不完整类型的 unique_ptr 在这个头里合法；定义在 .cpp。
    struct VirtualTextureProducerDeleter
    {
        void operator()(VirtualTextureProducer* producer) const;
    };
    std::unique_ptr<VirtualTextureProducer, VirtualTextureProducerDeleter> m_VirtualTextureProducer;
    CameraPreviewRequest m_CameraPreviewRequest;

    RHIViewport m_RenderThreadSceneViewport {};
    RHIRect2D m_RenderThreadSceneScissor {};
    bool m_HasRenderThreadSceneViewport {false};

#if defined(__APPLE__)
    // Metal 上没有 RP2，RP1 的结果是离屏的，得由它采样到 swapchain 上。
    // m_SceneBlitAttempted 是为了"初始化失败也只报一次错"，不然每帧刷一条日志。
    AppleSceneBlit m_SceneBlit;
    bool           m_SceneBlitAttempted {false};
#endif

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

    // 每帧推进 Nanite 的唯一入口（平台无关、线程模型无关）。
    //
    // 以前"推进 Nanite"这句话散在帧命令里，而且只有**并行渲染 + DX12** 那一支有
    // （`BuildRenderSystemFrameCommands` 的 DX12 分支）：单线程的两条路
    // （`TickRenderThread` 的 DX12 / 非 DX12 分支）和并行的非 DX12 分支都没有，于是
    // Nanite 在 Apple/Metal 上从来没跑过一次 —— 集群构建、裁剪、批次组装全是死代码，
    // 而且**不报任何错**。把四条路径统一到这一个函数上，将来再加 Tick 路径也不会漏。
    void UpdateNaniteForFrame(float delta_time);

    // 与 UpdateNaniteForFrame 同理：把 VT 流送推进统一到一个平台无关的入口，避免
    // 漏接某条 Tick 路径。函数本身无条件声明；**定义**里用 Z_HAS_VULKAN 包住对
    // VirtualTextureManager 的引用（VT 只在 Vulkan 路径编译），非 Vulkan 构建下是
    // no-op。默认由 r.VirtualTexture.Enable 门控（默认关），所以即便 VT 编译进来，
    // 不显式打开也零开销、零回归。
    void UpdateVirtualTextureForFrame(float delta_time);

    // r.VirtualTexture.Source 指向的图片 -> RGBA8 TextureData（VirtualTextureProducer 的 loader 回调）。
    // 静态函数而非 lambda：Producer 的 TextureLoader 是裸函数指针，这样它不依赖 <functional>
    // 也不捕获状态。
    static std::shared_ptr<TextureData> LoadVirtualTextureSource(void* render_system, const std::string& path);

    // 释放 VT 生产者（卸掉纹理、拆它拉起来的管理器）。必须在 RHI 还活着时调用——
    // RenderSystem 依赖 RHI，所以在 RenderSystem::Shutdown 里调用是安全的。
    // 同样无条件声明、定义里用 Z_HAS_VULKAN 包住，非 Vulkan 构建下是 no-op。
    void ShutdownVirtualTexture();

    static constexpr uint32_t kMaxFrameDrawLists = 3;

    uint32_t AllocateFrameDrawListSlot();
    RHIDrawList& GetFrameDrawList(uint32_t slot);

    std::array<RHIDrawList, kMaxFrameDrawLists> m_FrameDrawLists {};
    std::atomic<uint32_t> m_FrameDrawListSerial {0};
    uint32_t m_ActiveFrameDrawListSlot {0};

    friend struct RenderSyncGameCameraCmd;
    friend struct RenderProcessSwapDataCmd;
    friend struct RenderUpdateSceneCmd;
    friend struct RenderLumenCmd;
    friend struct RenderNaniteCmd;
    friend struct RenderPreparePassDataCmd;
    friend struct RenderBuildDrawListsCmd;
    friend struct RenderDispatchRHICommandsCmd;
    friend struct RHIPrepareContextCmd;
    friend struct RHISubmitDrawListsCmd;
};
