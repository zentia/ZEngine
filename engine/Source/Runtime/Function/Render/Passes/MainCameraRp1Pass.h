#pragma once

#include "Runtime/Function/Render/Graph/RenderGraphResources.h"
#include "Runtime/Function/Render/Passes/MainCameraFramebufferResources.h"
#include "Runtime/Function/Render/Passes/MainCameraPassShaderCommon.h"
#include "Runtime/Function/Render/Passes/ParticlePass.h"
#include "Runtime/Function/Render/RenderPass.h"

#include <array>
#include <map>
#include <memory>

class RenderResourceBase;
class RenderResource;
class NaniteRenderer;
class AppleSsaoPass;

namespace MegaLights
{
class MegaLightsSystem;
}

// DX-B3: RP1 (gbuffer / deferred / forward) for DX12 and future Vulkan dedup.
class MainCameraRp1Pass : public RenderPass
{
public:
    enum LayoutType : uint8_t
    {
        _per_mesh = 0,
        _mesh_global,
        _mesh_per_material,
        _deferred_lighting,
        // 编辑器 gizmo（axis）：binding 0 = per-frame（dynamic），binding 1 = 那一个
        // axis 的 model 矩阵 + 选中轴。和 Vulkan 的 MainCameraPass 同一套布局。
        _axis,
        _layout_type_count
    };

    enum RenderPipelineType : uint8_t
    {
        _render_pipeline_type_mesh_gbuffer = 0,
        _render_pipeline_type_mesh_gbuffer_nocull,
        _render_pipeline_type_mesh_transparent,
        _render_pipeline_type_deferred_lighting,
        _render_pipeline_type_megalights_deferred,
        _render_pipeline_type_megalights_spatial,
        _render_pipeline_type_sky_procedural,
        _render_pipeline_type_sky_mesh,
        _render_pipeline_type_axis,
        _render_pipeline_type_count
    };

    void SetFramebufferResources(MainCameraFramebufferResources* resources) { m_FbResources = resources; }

    void SetShadowImageViews(RHIImageView* directional_shadow, RHIImageView* point_light_shadow)
    {
        m_DirectionalLightShadowColorImageView = directional_shadow;
        m_PointLightShadowColorImageView = point_light_shadow;
    }

    void SetPerMeshLayout(RHIDescriptorSetLayout* layout) { m_ExternalPerMeshLayout = layout; }

    // Nanite 的 GBuffer 注入。
    //
    // Nanite 的剔除跑在帧里更早的位置（RenderNaniteCmd → UpdateNanite），那时一个
    // pass 都还没开 —— 它只能把间接命令写好。真正的绘制必须发生在**某个 GBuffer
    // pass 开着的时候**，也就是 DrawGBufferPass 里。传进来的渲染器会在游戏视口的
    // 网格绘制之后，把它的集群追加进同一个 pass。
    //
    // nullptr（默认）= 这个 pass 不画 Nanite，Nanite 继续走自己的离屏 target。
    void SetNaniteRenderer(NaniteRenderer* renderer) { m_NaniteRenderer = renderer; }

    // 粒子的兄弟 pass（和 Vulkan 的 `MainCameraPass::SetParticlePass` 同一套契约）。
    //
    // 粒子是**两个** pass 拼起来的：`Simulate()` 是 compute（在帧末自己提交），
    // `Draw()` 画的是 billboard，必须发生在某个 graphics render pass 开着的时候 ——
    // 所以只能由持有 render pass 的人（RP1）在 forward 那一站里代调。这也是为什么
    // 它不能像 Nanite 那样只传一个裸指针：生命周期得跟 RP1 绑在一起。
    void SetParticlePass(std::shared_ptr<ParticlePass> pass) { m_ParticlePass = std::move(pass); }

    // 把 RP1 的深度图 / GBufferA（法线）/ forward render pass 交给粒子。
    // 必须在 `Initialize()` **之后**调：那时 framebuffer 资源才存在。
    void SetupParticlePass();

    // 把当前的 GBuffer render pass / framebuffer 重新交给 Nanite。
    //
    // DrawGBufferPass 每帧都会调，所以正常情况下不用手动调；这里留一个入口是为了
    // UpdateAfterFramebufferRecreate —— resize 会销毁重建 framebuffer，而 Nanite
    // 手里那一份是**跨帧保持**的（见 DrawGBufferPass 里的说明），不及时换掉就会
    // 拿着一个已销毁的句柄。
    void RefreshNaniteInjection();

    // Rebind mesh-global IBL + shadow textures after UploadGlobalRenderResource (level hot-reload).
    void RefreshMeshGlobalIblDescriptors();

    // Rebind the deferred-lighting G-buffer/depth input-attachment descriptors after the MainCamera
    // framebuffer + RHI depth target are recreated on a window resize (otherwise scene -> white).
    void RefreshDeferredLightingInputAttachments();

    bool Initialize();
    void Shutdown();

    void PreparePassData(std::shared_ptr<RenderResourceBase> render_resource) override;

    // Simplified: 3 independent render passes (no subpasses).
    void DrawGBufferPass(const std::array<bool, 2>& skybox_visible);
    void DrawDeferredLightingPass(const std::array<bool, 2>& skybox_visible);
    void DrawForwardLightingPass(const std::array<bool, 2>& skybox_visible);

    // ---- SSAO（只有 Metal 接了）----
    //
    // AppleSsaoPass 归 DesktopRenderPipelineModule 所有，RP1 只借用（和 Nanite 一样）。
    // nullptr = 不跑 SSAO，延迟光照的 AO 槽绑 1x1 白图 ⇒ 光照与没有 SSAO 时逐位一致。
    void SetAmbientOcclusionPass(AppleSsaoPass* pass) { m_AmbientOcclusionPass = pass; }

    // GBuffer 之后、延迟光照之前调：用和 DrawDeferredLighting **同一套**视口 / 裁剪
    // 矩形与 per-viewport 相机数据录 AO。非 Metal 后端是空操作。
    void DrawAmbientOcclusionPass();

    // 这一帧延迟光照的 AO 槽绑的是不是真 AO 图（心跳探针：开关=1 但绑的是兜底图时，
    // 画面和"没有 SSAO"完全一样，只有这个数能分辨）。
    bool IsAmbientOcclusionBound() const { return m_AmbientOcclusionBound; }

    /// True when RP1 is driven by the render graph (env ZENGINE_RG_RP1=1, DX12 only).
    /// Off by default: the hand-written chain is the one that has been shipping, and the
    /// graph path still needs a per-frame visual diff before it can become the default.
    bool IsRenderGraphPathEnabled() const { return m_UseRenderGraph; }

    /// Turns the graph path on / off at runtime (building the graph objects on first use).
    /// Returns false when it cannot be enabled (non-DX12 backend). Used by the in-frame
    /// A/B capture (DX12RenderGraphAbCapture), which forces each path once and restores.
    bool SetRenderGraphPathEnabled(bool enabled);

    /// Declares GBuffer -> DeferredLighting -> ForwardLighting into the graph
    /// (MainCameraRp1GraphDecl.h) and executes it. Returns false when the graph cannot be
    /// used, in which case the caller runs the three Draw*Pass() calls itself. Every
    /// rejection happens BEFORE the first command is recorded, so a false return never
    /// leaves a half-drawn frame behind (same contract as MainCameraRp2Pass).
    bool DrawRP1WithGraph(const std::array<bool, 2>& skybox_visible);

    RHIDescriptorSetLayout* GetPerMeshDescriptorSetLayout() const
    {
        return m_DescriptorInfos[_per_mesh].layout;
    }

    RHIDescriptorSetLayout* GetMaterialDescriptorSetLayout() const
    {
        return m_DescriptorInfos[_mesh_per_material].layout;
    }

    // Stable pointer for RenderResource::m_MaterialDescriptorSetLayout (DX12 init order).
    RHIDescriptorSetLayout*& GetMaterialDescriptorSetLayoutPtr() { return m_MaterialDescriptorSetLayoutPtr; }

    std::array<MainCameraPerFrame, 2> m_MainCameraPerFrameByViewport;
    MainCameraPerFrame m_MainCameraPerFrame;
    MegaLights::MegaLightsSystem* m_MegaLightsSystem {nullptr};

    // 编辑器 gizmo（axis）的开关与当前选中轴。
    //
    // 以前这两个数**根本传不进 RP1**：Apple 分支的 draw list lambda 只调了三个
    // Draw*Pass，从来没把 pipeline 上的 `m_IsShowAxis` / `m_SelectedAxis` 交给 pass
    // （对比 DX12 分支：那里每次都 `main_camera_pass->m_IsShowAxis = ...`）。于是
    // 就算 pass 里有 axis 绘制路径，它也会因为 `m_IsShowAxis` 恒为 false 而什么都不画。
    bool   m_IsShowAxis {false};
    size_t m_SelectedAxis {3};

private:
    /// Reads ZENGINE_RG_RP1 and, when it is on (and the backend is DX12), builds the
    /// executor / pool / graph. Called from Initialize(); harmless when the switch is off.
    void SetupRenderGraphPath();

    void SetupDescriptorSetLayouts();
    void SetupPipelines();
    void SetupAxisPipeline();
    void SetupDescriptorSets();
    void SetupAxisDescriptorSet();
    void EnsureFallbackIblTextures();

    void SetPerViewportData(ViewportType viewport_type);
    bool IsViewportValid(ViewportType viewport_type) const;
    void SetViewportScissor(ViewportType viewport_type);
    // SetViewportScissor 用的视口 / 裁剪矩形（Scene 视口走 RenderSystem 的 render rect）。
    // 拆出来是为了让 SSAO 和延迟光照用**同一个**判据，不写第二份近似。
    void ResolveViewportScissor(ViewportType viewport_type, RHIViewport& out_viewport, RHIRect2D& out_scissor);
    // Metal：延迟光照 set 1 / binding 4 的 AO 图（真 AO 或 1x1 白图兜底）。
    RHIImageView* ResolveAmbientOcclusionView();

    void DrawMeshGbuffer(ViewportType viewport_type);
    void DrawAxis(ViewportType viewport_type);
    void DrawSkyMeshPass(ViewportType viewport_type, bool skybox_visible);
    void DrawDeferredLighting(ViewportType viewport_type);
    void UpdateMegaLightsDescriptorSets(ViewportType viewport_type);
    void UpdateMegaLightsSpatialDescriptorSets(ViewportType viewport_type);
    void DrawMegaLightsSpatialDenoise(ViewportType viewport_type);
    void DrawMeshTransparent(ViewportType viewport_type);

    RHIPipeline* GetOrCreateMeshGBufferPipeline(const GpuPBRMaterial& material);
    RHIPipeline* GetOrCreateMeshTransparentPipeline(const GpuPBRMaterial& material);

    RHIShader* LoadBuiltinShader(const char* hlsl_relative_path, ShaderStage stage);

    MainCameraFramebufferResources* m_FbResources {nullptr};
    // 由 DesktopRenderPipelineModule 从 NaniteRenderPass 取来，只借不拥有。
    NaniteRenderer* m_NaniteRenderer {nullptr};
    // SSAO（Metal）。只借不拥有，见 SetAmbientOcclusionPass。
    AppleSsaoPass* m_AmbientOcclusionPass {nullptr};
    // AO 关掉 / 没跑时延迟光照采样的 1x1 白图（r=g=1 ⇒ 不遮蔽）。
    RHIImage*     m_AmbientOcclusionFallbackImage {nullptr};
    RHIImageView* m_AmbientOcclusionFallbackView {nullptr};
    bool          m_AmbientOcclusionBound {false};
    // 粒子。桌面管线模块创建并拥有它，RP1 持一份 shared_ptr（因为 `Draw()` 要在
    // forward pass 开着的时候被调用，只有 RP1 知道那一站是什么时候）。
    // nullptr = 这个 pass 不画粒子（老行为）。
    std::shared_ptr<ParticlePass> m_ParticlePass;
    // Live per-viewport UBO source (PreparePassData); avoids stale copies at Sky Pass upload.
    RenderResource* m_PerFrameSource {nullptr};
    RHIDescriptorSetLayout* m_ExternalPerMeshLayout {nullptr};
    RHIDescriptorSetLayout* m_MaterialDescriptorSetLayoutPtr {nullptr};

    RHIImageView* m_DirectionalLightShadowColorImageView {nullptr};
    RHIImageView* m_PointLightShadowColorImageView {nullptr};

    bool m_Initialized {false};

    // Fallback 1x1 IBL when UploadGlobalRenderResource has not populated real IBL yet (DX12 path).
    RHISampler* m_FallbackSampler {nullptr};
    RHIImageView* m_FallbackBrdfView {nullptr};
    RHIImageView* m_FallbackCubeView {nullptr};

    const std::vector<RenderMeshNode>* m_ActiveMainCameraVisibleMeshNodes {nullptr};

    std::map<MainCameraPassShaderCommon::MeshPipelineKey, RHIPipeline*> m_MeshGbufferMaterialPipelines;
    std::map<MainCameraPassShaderCommon::MeshPipelineKey, RHIPipeline*> m_MeshTransparentMaterialPipelines;

    AxisDrawStorage m_AxisDrawStorage;
    // "axis 一次都没画成功" 只打一次，别每帧刷屏。
    bool m_AxisSkippedLogged {false};
    bool m_AxisDrawnLogged {false};

    // ---- render graph path (ZENGINE_RG_RP1) -----------------------------------------
    // Nothing here allocates GPU memory: every RP1 texture is imported (the framebuffers
    // are bound to specific views), so the pool only exists because the executor needs one.
    bool m_UseRenderGraph {false};
    std::unique_ptr<RGRhiDeviceAdapter> m_RgDevice;
    std::unique_ptr<RenderGraphResourcePool> m_RgPool;
    std::unique_ptr<RenderGraphExecutor> m_RgExecutor;
    std::unique_ptr<RenderGraph> m_Rp1Graph;
    /// Rejections are logged once; the graph is rebuilt identically every frame, so a
    /// failure is permanent and repeating it would bury everything else in the log.
    bool m_RgFallbackLogged {false};
};
