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
struct NaniteCluster
{
    // 边界信息
    Vector3 center;
    float radius;
    AxisAlignedBox bounds;

    // 几何数据
    uint32_t vertex_count;
    uint32_t triangle_count;
    uint32_t vertex_offset;
    uint32_t index_offset;

    // LOD层级
    uint8_t lod_level;

    // 父级和子级索引
    uint32_t parent_cluster_index;
    std::vector<uint32_t> child_cluster_indices;

    // 材质信息
    uint32_t material_id;

    // 错误度量（用于LOD选择）
    float error_metric;

    // 可见性标志
    bool is_visible;
    bool is_loaded;
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

    // 几何数据（压缩格式）
    std::vector<uint8_t> vertex_data;
    std::vector<uint8_t> index_data;

    // 层次结构
    uint32_t root_cluster_index;
    uint8_t hierarchy_depth;

    // 边界信息
    AxisAlignedBox global_bounds;

    // 统计信息
    uint32_t total_vertex_count;
    uint32_t total_triangle_count;
    uint32_t total_cluster_count;
};

// 实例数据
struct NaniteInstance
{
    uint64_t instance_id;
    uint64_t mesh_id;

    // 变换矩阵
    Matrix4x4 transform;
    Matrix4x4 prev_transform;  // 用于运动模糊

    // 可见性信息
    bool is_visible;
    uint8_t current_lod_level;

    // 裁剪信息
    bool is_culled;
    uint32_t visible_cluster_count;
};

// 可见性结果
struct NaniteVisibilityResult
{
    uint32_t visible_instance_count;
    uint32_t visible_cluster_count;
    std::vector<uint32_t> visible_instance_indices;
    std::vector<uint32_t> visible_cluster_indices;
};

// 渲染批次
struct NaniteRenderBatch
{
    uint32_t batch_id;
    uint32_t instance_id;
    uint32_t cluster_start;
    uint32_t cluster_count;
    uint8_t lod_level;
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