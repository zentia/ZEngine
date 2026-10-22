#pragma once

#include "NaniteCluster.h"
#include "NaniteCulling.h"
#include "NaniteGPUData.h"
#include "NaniteTypes.h"
#include "Runtime/Function/Render/RenderPassBase.h"

#include <memory>
#include <vector>

class RHI;
class RenderResourceBase;
class RenderScene;
class RenderCamera;
class RenderEntity;
struct CpuMeshGeometry;

// ---------------------------------------------------------------------------
// Nanite 的 GPU 资源。
//
// 以前这些全是 `void*` 加一行 "TODO: 初始化GPU资源"，也就是说 Nanite 一行都不曾
// 跑在 GPU 上：集群构建、逐簇裁剪、批次组装全部在 CPU。
//
// 现在的分工是：
//   * CPU 负责"场景里有哪些实例、每个网格怎么切成集群"（NaniteClusterBuilder），
//     并把结果**打包**成 NaniteSceneGPUData；
//   * GPU 负责"这一帧哪些集群活着"（nanite_cluster_cull.comp），并直接把
//     RHIDrawIndexedIndirectCommand 写进间接缓冲；
//   * CPU 只发一次 dispatch 和一次 CmdDrawIndexedIndirect，**永远不读回计数**。
//
// 缓冲全部建在 host-visible 内存上（不是 device-local）：几何要每帧更新实例变换，
// 而这套 RHI 没有 staging 抽象，host-visible 是唯一能简单地反复写的选项。
// 等有了真正的流式加载再换成 device-local + blit。
// ---------------------------------------------------------------------------
struct NaniteGPUResources
{
    RHIBuffer* clusters {nullptr};
    RHIBuffer* instances {nullptr};
    RHIBuffer* positions {nullptr};
    RHIBuffer* indices {nullptr};
    RHIBuffer* visible_list {nullptr};
    RHIBuffer* draw_commands {nullptr};
    RHIBuffer* cull_counter {nullptr};
    RHIBuffer* cull_constants {nullptr};

    // CreateBuffer 还会吐一个 RHIDeviceMemory，得自己拿着（MapMemory 要用，
    // 析构时要 FreeMemory）。Metal 上它就是 buffer 的 host 指针包装。
    RHIDeviceMemory* clusters_memory {nullptr};
    RHIDeviceMemory* instances_memory {nullptr};
    RHIDeviceMemory* positions_memory {nullptr};
    RHIDeviceMemory* indices_memory {nullptr};
    RHIDeviceMemory* visible_list_memory {nullptr};
    RHIDeviceMemory* draw_commands_memory {nullptr};
    RHIDeviceMemory* cull_counter_memory {nullptr};
    RHIDeviceMemory* cull_constants_memory {nullptr};

    // 当前**容量**（元素个数）。数据变多就重建，变少只是少传一点 —— 避免每帧重建。
    uint32_t cluster_capacity {0};
    uint32_t instance_capacity {0};
    uint32_t position_capacity {0};
    uint32_t index_capacity {0};

    // 光栅阶段：管线照着 raster_render_pass 的附件格式建。
    //   * raster_framebuffer 非空 ⇒ 这个 pass 由 Nanite 自己开/关（离屏 target）；
    //   * 为空 ⇒ 调用方已经在这个 pass 里，Nanite 只追加一次间接绘制。
    RHIRenderPass* raster_render_pass {nullptr};
    RHIFramebuffer* raster_framebuffer {nullptr};

    // 外部注入的目标（主相机 GBuffer 之类）。render_pass 与 framebuffer **都**非空
    // 才生效，语义是"调用方已经在 pass 里"；任一为空就退回下面的离屏 target。
    RHIRenderPass* external_render_pass {nullptr};
    RHIFramebuffer* external_framebuffer {nullptr};

    // Nanite 自己的离屏目标：没有外部注入时用它，Nanite 自己开/关 pass。
    // headless、编辑器离屏更新、以及还没有场景 pass 的平台（macOS 的 Metal 就是）
    // 全都走这一条 —— 它是唯一能在本机被冒烟测试证明的通路。
    RHIImage* target_color {nullptr};
    RHIDeviceMemory* target_color_memory {nullptr};
    RHIImageView* target_color_view {nullptr};
    RHIImage* target_depth {nullptr};
    RHIDeviceMemory* target_depth_memory {nullptr};
    RHIImageView* target_depth_view {nullptr};
    RHIRenderPass* target_render_pass {nullptr};
    RHIFramebuffer* target_framebuffer {nullptr};
    uint32_t target_width {0};
    uint32_t target_height {0};

    RHIPipeline* cull_pipeline {nullptr};
    RHIPipeline* raster_pipeline {nullptr};
    RHIPipelineLayout* pipeline_layout {nullptr};
    RHIDescriptorSetLayout* descriptor_set_layout {nullptr};
    RHIDescriptorPool* descriptor_pool {nullptr};
    RHIDescriptorSet* descriptor_set {nullptr};

    bool compute_ready {false};
    bool raster_ready {false};

    // 上一帧排队的绘制命令上界（= 集群总数）。间接绘制要它做 drawCount。
    uint32_t dispatch_cluster_count {0};
};

// Nanite渲染器 - 负责Nanite几何的渲染
class NaniteRenderer
{
public:
    NaniteRenderer();
    ~NaniteRenderer();

    // 初始化
    bool Initialize(RHI* rhi,
                    std::shared_ptr<RenderResourceBase> render_resource,
                    const NaniteConfig& config);

    // 清理
    void clear();

    // 从渲染场景刷新实例与网格资源。
    //
    // 几何的唯一来源是 RenderResourceBase 的 CPU 网格缓存（CpuMeshGeometry），和 Lumen
    // 距离场同源 —— RenderMeshData 在上传完就被释放了，之后再也读不到顶点。
    //
    // 以前 NaniteRenderer **没有任何办法**拿到场景：实例只能靠外部手工 RegisterInstance，
    // 而全代码库一个调用者都没有，所以 Nanite 从来没在真实帧循环里跑过。
    //
    // 场景拓扑（实例 id → 网格资产 id 的集合）没变时只更新变换矩阵，不重建集群 ——
    // 集群构建是递归二分整个网格，每帧重跑毫无意义。
    // 返回是否有至少一个可用实例。
    bool UpdateFromScene(std::shared_ptr<RenderScene> render_scene, RenderResourceBase* render_resource);

    // 渲染一帧
    void Render(RHI* rhi,
                std::shared_ptr<RenderScene> render_scene,
                std::shared_ptr<RenderCamera> camera,
                const NaniteConfig& config);

    // 注册网格资源（集群构建器产出之后交给它）
    bool RegisterMeshResource(uint64_t mesh_id, const NaniteMeshResource& mesh_resource);

    // 注册实例。若调用方没填 local_bounds，这里从已注册的网格资源上取 ——
    // 否则视锥裁剪拿不到边界，只能保守地什么都不裁。
    uint64_t RegisterInstance(const NaniteInstance& instance);

    // 注销实例
    void UnregisterInstance(uint64_t instance_id);

    // 更新实例
    void UpdateInstance(uint64_t instance_id, const NaniteInstance& instance);

    // 获取配置
    const NaniteConfig& getConfig() const { return m_Config; }
    void SetConfig(const NaniteConfig& config) { m_Config = config; }

    // 上一帧的结果，供渲染通道（和测试）读取。
    const std::vector<NaniteRenderBatch>& GetRenderBatches() const { return m_RenderBatches; }
    const NaniteVisibilityResult& GetLastVisibilityResult() const { return m_LastVisibilityResult; }
    // 每个批次用 cluster_start/cluster_count 索引的就是这个数组（按实例分组、连续）。
    const std::vector<uint32_t>& GetVisibleClusters() const { return m_VisibleClusters; }
    const std::vector<NaniteInstance>& GetInstances() const { return m_Instances; }

    // 某个网格资产已经建好的集群数（0 = 还没建 / 建失败）。测试与统计用。
    uint32_t GetClusterCount(uint64_t mesh_id) const { return m_ClusterManager.GetClusterCount(mesh_id); }

    // ---- GPU 路径（真正的实现，以前的 "TODO: 初始化GPU资源"）--------------------

    // 把当前场景打包成 NaniteSceneGPUData，并保证 GPU 缓冲的容量够。
    // 返回是否有东西可画。
    bool PrepareGPUData(const std::shared_ptr<RenderCamera>& camera, const NaniteConfig& config);

    // 发一次 nanite_cluster_cull.comp。之后 draw_commands 缓冲里就是这一帧的完整
    // 间接绘制命令 —— CPU 不读回，也不知道有多少条是活的。
    //
    // 没有帧命令缓冲时（headless、编辑器离屏更新、还没 PrepareBeforePass）退到
    // BeginSingleTimeCommands：自己开、自己提交并等它跑完。返回之后缓冲的内容对
    // CPU 就是可见的 —— 冒烟测试的读回靠的正是这一点。
    void DispatchClusterCulling(const std::shared_ptr<RenderCamera>& camera, const NaniteConfig& config);

    // 绑好索引缓冲与描述符集后发 CmdDrawIndexedIndirect。drawCount 是集群总数
    // （上界），被剔除的集群贡献的是 indexCount == 0 的空命令。
    //
    // 光栅目标按这个顺序解析：
    //   1. SetRasterRenderPass(pass, framebuffer) 注入过 ⇒ 往那个 pass 里追加一次
    //      绘制（调用方已经开了 pass，Nanite 不碰 begin/end）；
    //   2. 否则用 Nanite 自己的离屏 target，并且由 Nanite 自己开/关这个 pass。
    //
    // 没有帧命令缓冲时（headless、编辑器离屏更新）退回 BeginSingleTimeCommands：
    // 自己开、自己提交并等它跑完 —— 冒烟测试把离屏 target 读回来靠的正是这一点。
    void DrawClustersIndirect();

    // 注入外部光栅目标。render_pass 与 framebuffer **都**非空才生效，语义是
    // "调用方已经在这个 pass 里"（DX12 主相机 GBuffer 就是这么接的）；任一为空
    // 则清掉外部目标，退回 Nanite 自己的离屏 target。
    void SetRasterRenderPass(RHIRenderPass* render_pass, RHIFramebuffer* framebuffer = nullptr);

    // 离屏 target 的尺寸。改了就在下一次 DrawClustersIndirect 里重建。
    // 默认值取自 swapchain extent，取不到（headless）时是 512×512。
    void SetRasterTargetSize(uint32_t width, uint32_t height);

    const NaniteGPUResources& GetGPUResources() const { return m_GPU; }
    const NaniteSceneGPUData& GetSceneGPUData() const { return m_SceneGPUData; }

    // 把当前打包好的数据按给定的相机参数填一份 NaniteCullConstants。
    // 非 const：视锥平面要问 NaniteCullingSystem（它的方法不是 const 的）。
    NaniteCullConstants BuildCullConstants(const std::shared_ptr<RenderCamera>& camera,
                                           const NaniteConfig& config);

private:
    // 一个可见实例在 m_VisibleClusters 里占的一段
    struct InstanceClusterRange
    {
        uint32_t instance_index {0};
        uint32_t start {0};
        uint32_t count {0};
        uint8_t lod_level {0};
    };

    RHI* m_Rhi;
    std::shared_ptr<RenderResourceBase> m_RenderResource;

    NaniteConfig m_Config;
    NaniteClusterBuilder m_ClusterBuilder;
    NaniteClusterManager m_ClusterManager;
    NaniteCullingSystem m_CullingSystem;

    // 实例管理
    std::vector<NaniteInstance> m_Instances;
    std::unordered_map<uint64_t, uint32_t> m_InstanceIdToIndex;

    // 场景跟踪：上一次 UpdateFromScene 看到的"实例 id → 网格资产 id"集合的哈希
    uint64_t m_SceneTopologyHash {0};

    // 渲染批次
    std::vector<NaniteRenderBatch> m_RenderBatches;

    // 本帧可见的集群下标（按实例分组，段内连续），批次里的 cluster_start/count 索引它
    std::vector<uint32_t> m_VisibleClusters;
    std::vector<InstanceClusterRange> m_InstanceRanges;
    NaniteVisibilityResult m_LastVisibilityResult;

    // GPU 资源（真实的 RHI 句柄，不再是 void* 占位）
    NaniteGPUResources m_GPU;
    NaniteSceneGPUData m_SceneGPUData;

    // SetRasterTargetSize 设过的离屏 target 尺寸（0 = 没设，走 swapchain extent）
    uint32_t m_RasterTargetWidth {0};
    uint32_t m_RasterTargetHeight {0};

    // 场景里"有哪些实例、各自用哪个网格"的指纹。变换矩阵**不**进哈希 ——
    // 它每帧都可能变，而变了不需要重建集群。
    uint64_t ComputeSceneTopologyHash(const std::vector<RenderEntity>& entities,
                                      RenderResourceBase& render_resource) const;

    // 拓扑变了：整份实例列表重建（集群按需构建一次，之后按网格资产 id 复用）。
    bool RebuildInstancesFromScene(const std::vector<RenderEntity>& entities, RenderResourceBase& render_resource);

    // 拓扑没变：只把变换矩阵推进一帧（prev_transform 留给运动模糊）。
    void UpdateInstanceTransforms(const std::vector<RenderEntity>& entities);

    // 保证 mesh_id 对应的 NaniteMeshResource 已注册。已注册就直接返回 ——
    // 集群构建是递归二分，同一个资产做第二次是纯浪费。
    bool EnsureMeshResource(uint64_t mesh_id, const CpuMeshGeometry& geometry);

    // 关掉 Nanite（或相机缺失）时把上一帧的产物清掉，否则下游会一直读到陈旧的批次。
    void ClearFrameResults();

    // 逐集群裁剪：把可见的叶子簇收集进 m_VisibleClusters，并按实例记下各自的段。
    // 这是 Nanite 真正的收益所在 —— 实例级裁剪之前是恒真的 TODO，集群级裁剪压根不存在。
    void PrepareRenderData(const std::shared_ptr<RenderCamera>& camera,
                           const NaniteVisibilityResult& visibility_result,
                           const NaniteConfig& config);

    // 把 m_InstanceRanges 变成批次。**不产出空批次**：cluster_count == 0 的批次以前
    // 是常态（旧实现的唯一产出），下游拿到它只会画出一堆空绘制。
    void BuildRenderBatches(std::vector<NaniteRenderBatch>& out_batches);

    // 更新GPU缓冲区：容量不够就重建，够就直接上传。
    void UpdateGPUBuffers();

    // 执行GPU裁剪
    NaniteVisibilityResult PerformGPUCulling(const std::shared_ptr<RenderCamera>& camera, const NaniteConfig& config);

private:
    // 容量不足就重建缓冲；返回是否全部就绪。
    bool EnsureGPUResources(uint32_t cluster_count, uint32_t instance_count, uint32_t position_count, uint32_t index_count);

    // 把 CPU 字节推到 GPU（MapMemory + memcpy + UnmapMemory）。
    bool UploadBytes(RHIBuffer* buffer, RHIDeviceMemory* memory, const void* data, size_t bytes);

    // 描述符集绑定。缓冲重建之后必须重调一次，否则 kernel 还指着旧的句柄。
    void UpdateDescriptorWrites();

    // 建剔除的 compute 管线（懒建：shader 不可用时失败，之后不再重试到每帧刷日志）。
    bool EnsureComputePipeline();

    // 建光栅管线。需要 raster_render_pass。
    bool EnsureRasterPipeline();

    // 保证离屏光栅目标（color + depth + render pass + framebuffer）存在且尺寸匹配。
    // 只有"没有外部注入目标"时才会被调用。
    bool EnsureRasterTarget();

    void DestroyRasterTarget();

    // 离屏 target 的尺寸：显式设过就用它，否则取 swapchain extent，再不行 512×512。
    uint32_t ResolveRasterTargetWidth() const;
    uint32_t ResolveRasterTargetHeight() const;

    void DestroyGPUResources();
};

// Nanite渲染通道
//
// 这是 Nanite 唯一的生产者：每帧在渲染线程上被 RenderPipelineBase::UpdateNanite 调用。
// 产出的渲染批次挂在 pass 上，等 GPU 路径（shader + 间接绘制）落地后由它消费。
class NaniteRenderPass : public RenderPassBase
{
public:
    NaniteRenderPass();
    ~NaniteRenderPass();

    virtual void Initialize(const RenderPassInitInfo* init_info) override;
    virtual void PreparePassData(std::shared_ptr<RenderResourceBase> render_resource) override;
    virtual void clear();

    // 执行一帧的 Nanite 计算（场景 → 实例/集群 → 裁剪 → 批次）
    void Render(std::shared_ptr<RenderScene> render_scene, std::shared_ptr<RenderCamera> camera, float delta_time);

    // 获取Nanite渲染器
    NaniteRenderer* getNaniteRenderer() { return &m_NaniteRenderer; }

    // 最近一次 Render 产出的批次（按实例分组，非空批次）
    const std::vector<NaniteRenderBatch>& GetRenderBatches() const { return m_NaniteRenderer.GetRenderBatches(); }

    // 最近一次 Render 有没有产出任何可见集群。给调试/编辑器用。
    bool hasVisibleClusters() const { return m_HasVisibleClusters; }

private:
    // 把 r.Nanite.* 的当前值推给渲染器（每帧都做，所以 CVar 是热生效的）
    void ApplyConsoleVariables();

    // Nanite渲染器
    NaniteRenderer m_NaniteRenderer;

    // Nanite配置
    NaniteConfig m_NaniteConfig;

    // 最近一次 Render 是否产出了可见集群
    bool m_HasVisibleClusters {false};

    // 子系统是否真的初始化过（需要 render_resource，而它在 Initialize 时经常还没有）
    bool m_RendererInitialized {false};
};
