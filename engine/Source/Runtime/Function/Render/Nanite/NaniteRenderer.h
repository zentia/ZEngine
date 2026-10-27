#pragma once

#include "NaniteCluster.h"
#include "NaniteCulling.h"
#include "NaniteGPUData.h"
#include "NanitePageSlotTable.h"
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

// Nanite 的光栅目标有**且只有三个颜色附件**：GBufferA / B / C。
//
// 这个数字必须和 engine/shader/include/gbuffer.h 里那句 "EXACTLY THREE" 一致：
// shader 声明三个 out，管线就得声明三个 blend 附件，render pass 就得有三个颜色
// 附件。任何一边少一个，Vulkan 会直接拒绝建管线，Metal 会在开 pass 时炸。
constexpr uint32_t kNaniteRasterColorAttachmentCount = 3;

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

    // ---- 持久页缓冲（页池启用时用）----------------------------------------
    // 与上面的稠密整场景 positions/indices **并存**：页池路径把 admitted 页的几何按
    // 槽位偏移驻留进这里、evicted 页的槽位归还。容量 = 页池预算换算的元素数（不是整
    // 场景）。注意：draw 路径目前仍读稠密 positions/indices（shader 页表间接寻址是下一
    // 步），所以这两个缓冲现在是"几何已物理驻留、但还没被 draw 消费"的桥接状态。
    RHIBuffer* page_positions {nullptr};
    RHIDeviceMemory* page_positions_memory {nullptr};
    uint32_t page_position_capacity {0};
    RHIBuffer* page_indices {nullptr};
    RHIDeviceMemory* page_indices_memory {nullptr};
    uint32_t page_index_capacity {0};

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
    //
    // 它刻意是**GBuffer 形状的**（三张颜色图 + 一张深度），和主相机的 GBuffer 逐
    // 格式一致。这样离屏这条通路跑的就是真实 GBuffer 那条代码路径：shader 写三个
    // target、管线声明三个 blend 附件，换到注入的主相机 pass 上时没有任何一处会
    // 悄悄改变行为。
    RHIImage* target_gbuffer[kNaniteRasterColorAttachmentCount] {};
    RHIDeviceMemory* target_gbuffer_memory[kNaniteRasterColorAttachmentCount] {};
    RHIImageView* target_gbuffer_view[kNaniteRasterColorAttachmentCount] {};
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

    // Latched once the GPU path is known to be unavailable, so we stop retrying
    // (and logging) the same failure on every frame. See EnsureComputePipeline().
    bool gpu_pipelines_unsupported {false};

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
    // 这条是**离屏**路径：Nanite 自己的 target，pass 的开与关也归它。没有帧命令
    // 缓冲时（headless、编辑器离屏更新）退回 BeginSingleTimeCommands：自己开、自己
    // 提交并等它跑完 —— 冒烟测试把离屏 target 读回来靠的正是这一点。
    //
    // 已经用 SetRasterRenderPass 注入过目标时这里是 no-op：那个 pass 属于调用方，
    // 绘制要发生在它开着的时候，离屏这里再画一遍就是没人要的第二份。
    void DrawClustersIndirect();

    // **注入**路径：调用方已经把 SetRasterRenderPass 给的那个 pass 开好了，把它
    // 正在用的命令缓冲一起传进来，Nanite 只往里追加一条间接绘制。
    //
    // 不碰 pass 的 begin/end，也不设 viewport / scissor —— 那都是调用方的管线状态，
    // 抢过来改会把同一个 pass 里排在后面的绘制一起改坏。
    //
    // 命令缓冲要显式传，是因为 Nanite 判断不了"调用方是不是真的在 pass 里"：
    // GetCurrentCommandBuffer() 在无头、编辑器离屏、还没 PrepareBeforePass 的情况
    // 下都是空的，拿它当判据会把"没在 pass 里"误判成"在 pass 里"。给了缓冲就等于
    // 调用方声明了"我在里面"；传 nullptr 就什么都不画。
    void DrawClustersIndirectInPass(RHICommandBuffer* command_buffer);

    // 注入外部光栅目标。render_pass 与 framebuffer **都**非空才生效，语义是
    // "调用方已经在这个 pass 里"（DX12 主相机 GBuffer 就是这么接的）；任一为空
    // 则清掉外部目标，退回 Nanite 自己的离屏 target。
    //
    // 注入生效期间 DrawClustersIndirect()（离屏）不再画，绘制由调用方在 pass 里
    // 调 DrawClustersIndirectInPass 触发。
    void SetRasterRenderPass(RHIRenderPass* render_pass, RHIFramebuffer* framebuffer = nullptr);

    // 当前注入的 render pass（没注入就是 nullptr）。给调用方判断"该不该由我来画"。
    RHIRenderPass* GetInjectedRasterRenderPass() const { return m_GPU.external_render_pass; }

    // 离屏 target 的尺寸。改了就在下一次 DrawClustersIndirect 里重建。
    // 默认值取自 swapchain extent，取不到（headless）时是 512×512。
    void SetRasterTargetSize(uint32_t width, uint32_t height);

    const NaniteGPUResources& GetGPUResources() const { return m_GPU; }
    const NaniteSceneGPUData& GetSceneGPUData() const { return m_SceneGPUData; }

    // GPU 路径到底成没成（心跳探针用）。它**不报任何错**地退化到 CPU，所以必须能
    // 被读出来，否则"GPU 剔除上线了没有"在真机上无法证伪。
    bool IsGPUComputeReady() const { return m_GPU.compute_ready; }

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

    // 页池启用时的持久页槽位表（page_id → GPU 槽位）。容量按页池预算换算的元素数建。
    // UpdateStreaming 之后由 ExecuteStreamingUploads 驱动：admitted 认领槽位并拷几何、
    // evicted 归还槽位。未启用页池时它闲置（draw 仍走稠密 positions/indices）。
    nanite_pages::PageSlotTable m_PageSlots{1, 1};

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

    // GPU 路径的一次性诊断（阶段 59）：它静默退化、不报任何错，所以"卡在哪一步"
    // 只能靠这个 flag + 一条日志说清楚。只打一次，否则每帧刷屏。
    bool m_GpuDiagLogged {false};
    // "管线建成了"不等于"真的发了 dispatch" —— 这是"历史空桩"最爱藏的地方，所以
    // dispatch 自己也要有一条一次性确认。
    bool m_DispatchDiagLogged {false};

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

    // 执行本帧的页池增量上传计划：admitted 页按 dst slot_base 把几何切片拷进持久页缓冲、
    // evicted 页归还槽位。页池未启用时是 no-op。真正把几何拷进 GPU 是渲染绑定（MapMemory
    // 到槽位子段 + memcpy），本地只能编译 + 静态检查，无法 headless 验证。
    void ExecuteStreamingUploads();

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

    // 绑管线 / 描述符集 / 索引缓冲，然后发那唯一的一条 CmdDrawIndexedIndirect。
    // 离屏路径和注入路径共用这一段：它们的差别只在"谁开 pass、谁设视口"。
    void RecordClusterDraw(RHICommandBuffer* command_buffer);

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
