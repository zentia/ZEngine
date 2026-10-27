#pragma once

#include "Runtime/Core/Math/AxisAligned.h"
#include "Runtime/Core/Math/Matrix4.h"
#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Core/Math/Vector4.h"

#include <cstdint>
#include <memory>
#include <vector>

// Nanite常量定义
namespace NaniteConstants
{
    // 集群索引的"空"值。以前代码里到处写字面量 UINT32_MAX，读起来要对照上下文才知道
    // 那是"没有父节点"还是"越界"。
    constexpr uint32_t kInvalidClusterIndex = UINT32_MAX;

    constexpr uint32_t kMaxClustersPerMesh = 65536;
    constexpr uint32_t kMaxVerticesPerCluster = 128;
    constexpr uint32_t kMaxTrianglesPerCluster = 64;
    constexpr uint32_t kClusterGroupSize = 128;
    constexpr uint32_t kMaxHierarchyDepth = 16;
    constexpr uint32_t kPageSize = 64 * 1024;  // 64KB
    constexpr float kMinClusterSize = 0.1f;
    constexpr float kMaxClusterSize = 1000.0f;
}  // namespace NaniteConstants

// 集群边界类型
enum class NaniteClusterBoundType : uint8_t
{
    Sphere,
    OBB,  // Oriented Bounding Box
    AABB  // Axis-Aligned Bounding Box
};

// 集群数据结构
//
// 两类节点都装在这个结构里：
//   * 叶子集群（is_leaf == true）—— 真正可渲染的单元，三角形数不超过
//     kMaxTrianglesPerCluster。
//   * 内部节点（is_leaf == false）—— 只把它子树里所有叶子的几何区间**聚合**
//     成一个 LOD 节点。没有网格简化器，所以"粗一档"并不会少画三角形；内部节点
//     存在的意义是让层次结构与逐层误差可查询，渲染时只画叶子。
struct NaniteCluster
{
    // 边界信息
    Vector3 center {0.0f, 0.0f, 0.0f};
    float radius {0.0f};
    AxisAlignedBox bounds;

    // 几何数据（顶点数/索引数都是**元素个数**，不是字节数）
    uint32_t vertex_count {0};
    uint32_t triangle_count {0};
    uint32_t vertex_offset {0};
    uint32_t index_offset {0};

    // LOD层级：0 = 根（覆盖整个网格、最粗），数值越大越细
    uint8_t lod_level {0};

    // 父级和子级索引
    uint32_t parent_cluster_index {NaniteConstants::kInvalidClusterIndex};
    std::vector<uint32_t> child_cluster_indices;

    // 材质信息
    uint32_t material_id {0};

    // 错误度量（用于LOD选择）。单位是世界/模型空间长度，父节点恒 >= 子节点。
    // 以前这个字段**从来没有人赋值**，而 NaniteCluster 也没有默认初始化 ⇒
    // SelectLOD 读到的是未初始化的栈内存。
    float error_metric {0.0f};

    // 可见性标志
    bool is_visible {false};
    bool is_loaded {false};

    // 见上面的说明：只有叶子集群能被提交渲染。
    bool is_leaf {false};
};

// 集群组（用于批量处理）
struct NaniteClusterGroup
{
    uint32_t group_id;
    std::vector<uint32_t> cluster_indices;
    AxisAlignedBox group_bounds;
    uint8_t min_lod_level;
    uint8_t max_lod_level;
};

// 几何编码格式（version: zengine-nanite-geom/1）。
//
// **先说清楚一件事**：任意 32-bit 浮点位置**无法无损压进更少的比特**。真正的 Nanite
// 几何压缩是**有界有损**的定点量化 —— 用一点可控的精度换显著的体积。所以这里没有
// "无损压缩"，只有"带误差上界的量化"。
enum class GeometryCodec : uint8_t
{
    // 未压缩的原始布局（默认，保持既有字节契约）：
    //   vertex_data : total_vertex_count 个 Vector3，每个 12 字节
    //   index_data  : total_triangle_count * 3 个 uint32_t，每个 4 字节
    Raw = 0,

    // 有界有损量化：
    //   vertex_data : [头 24B: quant_min(3×float) + quant_scale(3×float)]
    //                 + total_vertex_count 个 (3×int16) 量化位置，每个 6 字节。
    //                 解码：pos ≈ quant_min + q * quant_scale，每分量误差 ≤ quant_scale/2。
    //   index_data  : total_triangle_count * 3 个 uint16_t，每个 2 字节
    //                 （集群顶点数远小于 65536，16-bit 足够）。
    Quantized = 1,
};

// Nanite网格资源
struct NaniteMeshResource
{
    // 唯一标识
    uint64_t mesh_id;

    // 集群数据
    std::vector<NaniteCluster> clusters;
    std::vector<NaniteClusterGroup> cluster_groups;

    // 几何数据。布局由 geometry_codec 决定（见 GeometryCodec）。
    // 每个集群用 vertex_offset/vertex_count 与 index_offset/triangle_count 在这两段里
    // 取一段连续的窗口 —— 深度优先的分割顺序保证了同一子树的窗口是连续的。
    //
    // 注意：**层次结构、边界、error_metric 都在量化之前用全精度位置算好**（见
    // NaniteCluster.cpp 的 RecursiveClusterSplit），所以量化只影响存盘的字节，不影响
    // 裁剪/LOD 用的边界与误差 —— 那些量永远是全精度的。
    GeometryCodec geometry_codec {GeometryCodec::Raw};
    std::vector<uint8_t> vertex_data;
    std::vector<uint8_t> index_data;

    // Quantized 编码的解码头（Raw 时忽略，恒为 0）：
    //   解码位置 ≈ quant_min + q * quant_scale，其中 q 是存储的 int16 三元组。
    //   每分量最大误差 ≤ quant_scale 的一半。
    Vector3 quant_min {0.0f, 0.0f, 0.0f};
    Vector3 quant_scale {0.0f, 0.0f, 0.0f};

    // 层次结构
    uint32_t root_cluster_index {NaniteConstants::kInvalidClusterIndex};
    uint8_t hierarchy_depth {0};

    // 边界信息
    AxisAlignedBox global_bounds;

    // 统计信息
    uint32_t total_vertex_count {0};
    uint32_t total_triangle_count {0};
    uint32_t total_cluster_count {0};
};

// 实例数据
struct NaniteInstance
{
    // 这四个以前**没有默认初始化**：一个默认构造出来的 NaniteInstance 带着未初始化的
    // id 和矩阵，除了"手工 RegisterInstance"那条（全代码库零调用者）之外，任何走默认
    // 构造的路径都是在读垃圾。
    uint64_t instance_id {0};
    uint64_t mesh_id {0};

    // 变换矩阵
    Matrix4x4 transform {Matrix4x4::IDENTITY};
    Matrix4x4 prev_transform {Matrix4x4::IDENTITY};  // 用于运动模糊

    // **模型空间**包围盒。视锥裁剪拿它乘 transform 得到世界盒。
    // 以前裁剪用的是一个默认构造的空盒（代码里写着"需要从mesh资源获取"的 TODO），
    // 于是"视锥裁剪"其实什么都没裁掉 —— 而 FrustumCull 本身又恒返回 true，两层都没干活。
    // NaniteRenderer::RegisterInstance 会在注册时从网格资源替你填上。
    AxisAlignedBox local_bounds;

    // 可见性信息
    bool is_visible {false};
    uint8_t current_lod_level {0};

    // 裁剪信息
    bool is_culled {false};
    uint32_t visible_cluster_count {0};
};

// 可见性结果
struct NaniteVisibilityResult
{
    uint32_t visible_instance_count {0};
    uint32_t visible_cluster_count {0};
    std::vector<uint32_t> visible_instance_indices;
    std::vector<uint32_t> visible_cluster_indices;
};

// 渲染批次
struct NaniteRenderBatch
{
    uint32_t batch_id {0};
    uint32_t instance_id {0};
    uint32_t cluster_start {0};
    uint32_t cluster_count {0};
    uint8_t lod_level {0};
};

// GPU缓冲区布局
struct NaniteGPUBufferLayout
{
    // 集群数据缓冲区
    uint32_t cluster_data_buffer_size;
    uint32_t cluster_data_buffer_offset;

    // 顶点数据缓冲区
    uint32_t vertex_data_buffer_size;
    uint32_t vertex_data_buffer_offset;

    // 索引数据缓冲区
    uint32_t index_data_buffer_size;
    uint32_t index_data_buffer_offset;

    // 实例数据缓冲区
    uint32_t instance_data_buffer_size;
    uint32_t instance_data_buffer_offset;

    // 可见性缓冲区
    uint32_t visibility_buffer_size;
    uint32_t visibility_buffer_offset;
};

// Nanite配置
struct NaniteConfig
{
    bool enabled = true;
    bool enable_lod = true;
    bool enable_culling = true;
    bool enable_streaming = true;

    // HiZ 遮挡剔除（切法二）。开启后 cluster-cull kernel 读上一帧的金字塔判遮挡，被完全
    // 遮挡的簇写空命令。保守判据（拿不准绝不剔除），误剔比不剔更糟，所以正确性底线是"无破洞"。
    // 时间性 HiZ：剔除在帧首，本帧深度还没画出来，只能用上一帧末尾生成的金字塔——第一帧
    // （还没有金字塔）天然不遮挡。需要 enable_culling 也开（遮挡是视锥之后的第二步）。
    bool enable_hiz_occlusion = false;

    // 页池字节预算（step 2.5 后续：真正的按页驻留）。0 = 关闭页池，退回纯距离裁剪
    // （UpdateStreaming 只标 cluster_loaded_flags，不约束总驻留字节）。> 0 时启用
    // NanitePagePool：叶子页按"距离优先 + LRU 破平"装进该字节预算，装不下的远页被
    // 驱逐——语义从"标不标"升级为"装不装得进预算池"。结构页(internal)永不驱逐。
    // 注意：这只约束**簿记驻留位**；把换入/换出队列真正拷进/释放显存仍是渲染绑定。
    uint64_t streaming_pool_budget_bytes = 0;

    float lod_bias = 0.0f;
    float max_screen_error = 1.0f;
    uint32_t max_visible_clusters = 100000;
    uint32_t max_streaming_distance = 10000;

    // 性能设置
    uint32_t culling_thread_count = 4;
    bool use_gpu_culling = true;
    bool use_async_streaming = true;

    // 几何编码格式（见 GeometryCodec）。默认 Raw 保持既有字节契约；设 Quantized 启用
    // 有界有损量化（更小的 vertex/index 字节段，代价是每分量 ≤ quant_scale/2 的位置误差）。
    GeometryCodec geometry_codec {GeometryCodec::Raw};
};