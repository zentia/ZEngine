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

// Nanite网格资源
struct NaniteMeshResource
{
    // 唯一标识
    uint64_t mesh_id;

    // 集群数据
    std::vector<NaniteCluster> clusters;
    std::vector<NaniteClusterGroup> cluster_groups;

    // 几何数据。目前**未压缩**（压缩仍是 TODO），布局固定为：
    //   vertex_data : total_vertex_count 个 Vector3，每个 12 字节
    //   index_data  : total_triangle_count * 3 个 uint32_t，每个 4 字节
    // 每个集群用 vertex_offset/vertex_count 与 index_offset/triangle_count 在这两段里
    // 取一段连续的窗口 —— 深度优先的分割顺序保证了同一子树的窗口是连续的。
    std::vector<uint8_t> vertex_data;
    std::vector<uint8_t> index_data;

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
    uint64_t instance_id;
    uint64_t mesh_id;

    // 变换矩阵
    Matrix4x4 transform;
    Matrix4x4 prev_transform;  // 用于运动模糊

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

    float lod_bias = 0.0f;
    float max_screen_error = 1.0f;
    uint32_t max_visible_clusters = 100000;
    uint32_t max_streaming_distance = 10000;

    // 性能设置
    uint32_t culling_thread_count = 4;
    bool use_gpu_culling = true;
    bool use_async_streaming = true;
};