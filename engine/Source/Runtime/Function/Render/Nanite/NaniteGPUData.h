#pragma once

// ===========================================================================
// Nanite 的 GPU 端数据布局。
//
// 这是 CPU 与 shader 之间的**唯一契约**：GLSL 里那几个 block 的字段顺序、类型、
// 对齐必须和这里的 struct 逐字节一致，否则 kernel 读到的全是错位的数据，而
// 崩溃点会在很远的下游（画出来的东西不对，或者干脆什么都不画）。
//
// 所以每个 struct 都有 static_assert 钉住 sizeof 和 offsetof，并且
// NaniteGPUCullingSmokeTest 会真的把数据丢到 Metal 上跑一遍再读回来比对。
//
// 三条对齐规则（std430，SSBO 的默认规则）：
//   * vec4 / mat4 按 16 字节对齐 —— 所以每个 struct 都是 16 的倍数，字段全部用
//     vec4 / uvec4 / mat4 打包，不出现 float 和 uint 混排（那会留下 4 字节的洞，
//     而 GLSL 侧和 C++ 侧对洞在哪里的判断未必一致）。
//   * 数组元素步长 = 元素大小向上取整到 16。
//   * 矩阵一律按**列主序**存（见下面的 PackMatrix）。
// ===========================================================================

#include "NaniteCluster.h"
#include "NaniteTypes.h"
#include "Runtime/Core/Math/AxisAligned.h"
#include "Runtime/Core/Math/Matrix4.h"
#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Core/Math/Vector4.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

// ---------------------------------------------------------------------------
// 描述符绑定槽。
//
// 一个 descriptor set 同时给 compute（剔除）和 graphics（光栅）用：Metal 不在乎，
// 而少维护一份 layout 就少一处可能写错的地方。C++ 侧、GLSL 侧、冒烟测试三处
// 必须用同一组数字 —— 所以放在这里，别在调用点写字面量。
// ---------------------------------------------------------------------------
namespace NaniteGPUBinding
{
    constexpr uint32_t kCullConstants = 0;  // uniform：视锥平面 / LOD 参数 / 计数
    constexpr uint32_t kClusters      = 1;  // storage：NaniteGPUCluster[]
    constexpr uint32_t kInstances     = 2;  // storage：NaniteGPUInstance[]
    constexpr uint32_t kVisibleList   = 3;  // storage（写）：可见集群下标
    constexpr uint32_t kDrawCommands  = 4;  // storage（写）：RHIDrawIndexedIndirectCommand[]
    constexpr uint32_t kDrawCounter   = 5;  // storage（读写，原子）：可见计数
    constexpr uint32_t kPositions     = 6;  // storage：vec4[]，全局顶点位置
    constexpr uint32_t kHiZData       = 7;  // storage（只读，COMPUTE_BIT only）：HiZ 平铺金字塔
    constexpr uint32_t kBindingCount  = 8;
}  // namespace NaniteGPUBinding

// ---------------------------------------------------------------------------
// HiZ GPU 金字塔的描述符绑定槽。**独立于** NaniteGPUBinding 那套——HiZ 用自己的
// descriptor set / pipeline layout，不碰剔除那 7 个 storage binding（compute 与
// vertex 共用，动它会波及光栅）。三处（C++ / GLSL / 测试）必须用同一组数字。
// ---------------------------------------------------------------------------
namespace NaniteHiZBinding
{
    constexpr uint32_t kHeader  = 0;  // uniform：每级尺寸/偏移 + phase/dstLevel
    constexpr uint32_t kDepth   = 1;  // combined image sampler：D32 深度图
    constexpr uint32_t kData    = 2;  // storage（写）：平铺金字塔 float[]
    constexpr uint32_t kBindingCount = 3;
}  // namespace NaniteHiZBinding

// ---------------------------------------------------------------------------
// 与 GLSL 的 uvec4 对应。Vector4 是 float，这里需要的是整数槽位。
// ---------------------------------------------------------------------------
struct NaniteGPUUVec4
{
    uint32_t x {0};
    uint32_t y {0};
    uint32_t z {0};
    uint32_t w {0};
};

// 与 GLSL 侧 nanite_hiz.comp 的 _hiz_header 逐字节对应（std140：uvec4 数组）。
// 每级 mips[i] = (width, height, offset_texel, 保留)；params = (numMips, phase,
// dstLevel, 保留)；dims = (baseW, baseH, 0, 0)。
struct NaniteHiZGPUHeader
{
    NaniteGPUUVec4 mips[16];
    NaniteGPUUVec4 params;
    NaniteGPUUVec4 dims;
};
static_assert(sizeof(NaniteHiZGPUHeader) == 16 * 16 + 16 + 16, "HiZ header must match GLSL std140 layout");

// ---------------------------------------------------------------------------
// 一个集群在 GPU 上的样子。80 字节。
//
// bounds 是**模型空间**的（和 NaniteCluster::bounds 一致），要拿去和视锥比必须先
// 乘实例的 transform —— kernel 里就是这么做的。
// ---------------------------------------------------------------------------
struct NaniteGPUCluster
{
    Vector4 center_radius;  // xyz = 模型空间中心，w = 半径
    Vector4 bounds_min;     // xyz = 包围盒 min，w = error_metric
    Vector4 bounds_max;     // xyz = 包围盒 max，w = lod_level（按 float 存）
    NaniteGPUUVec4 geometry;  // x = vertex_offset, y = index_offset, z = index_count, w = instance_index
    NaniteGPUUVec4 flags;     // x = is_leaf, y = triangle_count, z/w = 保留
};

// ---------------------------------------------------------------------------
// 一个实例在 GPU 上的样子。176 字节。
// ---------------------------------------------------------------------------
struct NaniteGPUInstance
{
    Matrix4x4 transform;       // 列主序打包（见 PackMatrix）
    Matrix4x4 prev_transform;  // 同上，留给运动模糊
    Vector4 bounds_min;        // 模型空间包围盒
    Vector4 bounds_max;
    NaniteGPUUVec4 meta;  // x = mesh_id 低 32 位, y = mesh_id 高 32 位, z = cluster_offset, w = cluster_count
};

// ---------------------------------------------------------------------------
// 剔除 kernel 的 uniform。208 字节。
//
// 视锥平面在 CPU 侧算好传进来，而不是让 kernel 自己从 view_projection 推：
//   * 六个平面每帧只算一次，算在 CPU 上比每个集群算一遍便宜；
//   * 更重要的是，这样 GPU 与 NaniteCullingSystem 用的是**同一份**平面，两边对
//     "这个盒子在不在视锥里"的判断才会一致，冒烟测试的比对才有意义。
// ---------------------------------------------------------------------------
struct NaniteCullConstants
{
    Matrix4x4 view_projection;   // 列主序打包。kernel 目前不直接用它，但留着给后续遮挡剔除
    Vector4 frustum_planes[6];   // 顺序同 NaniteCullingSystem::ExtractFrustumPlanes
    Vector4 camera_position;     // xyz = 世界空间相机位置
    Vector4 lod_params;          // x = max_screen_error, y = lod_bias, z/w = 保留
    NaniteGPUUVec4 counts;       // x = cluster_count, y = instance_count, z = enable_lod, w = enable_culling
    // HiZ 遮挡剔除（切法二）参数。x = enable_hiz_occlusion（0/1），y/z/w = 金字塔 baseW/baseH/numMips。
    // numMips == 0 ⇒ kernel 里 SampleMaxDepth 直接返回 0（最远）⇒ 不遮挡，天然覆盖"还没有金字塔"
    // 的第一帧（时间性 HiZ：剔除在帧首，本帧深度还没画出来，只能用上一帧的金字塔）。
    Vector4 hiz;                 // x = enable_hiz_occlusion, y = baseW, z = baseH, w = numMips
};

// ---------------------------------------------------------------------------
// 剔除结果的计数器。32 字节（原子操作要求 4 字节对齐，整个 struct 按 16 对齐）。
// ---------------------------------------------------------------------------
struct NaniteCullCounter
{
    uint32_t visible_count {0};  // 通过剔除的集群数
    uint32_t draw_count {0};     // 写出去的非空绘制命令数（正常情况下等于 visible_count）
    uint32_t culled_count {0};   // 被剔除的集群数
    uint32_t reserved {0};
};

static_assert(sizeof(NaniteGPUUVec4) == 16, "NaniteGPUUVec4 must be 16 bytes");
static_assert(sizeof(NaniteGPUCluster) == 80, "NaniteGPUCluster must be 80 bytes");
static_assert(sizeof(NaniteGPUInstance) == 176, "NaniteGPUInstance must be 176 bytes");
static_assert(sizeof(NaniteCullConstants) == 224, "NaniteCullConstants must be 224 bytes (208 + hiz vec4)");
static_assert(sizeof(NaniteCullCounter) == 16, "NaniteCullCounter must be 16 bytes");

static_assert(offsetof(NaniteGPUCluster, center_radius) == 0, "cluster layout drifted");
static_assert(offsetof(NaniteGPUCluster, bounds_min) == 16, "cluster layout drifted");
static_assert(offsetof(NaniteGPUCluster, bounds_max) == 32, "cluster layout drifted");
static_assert(offsetof(NaniteGPUCluster, geometry) == 48, "cluster layout drifted");
static_assert(offsetof(NaniteGPUCluster, flags) == 64, "cluster layout drifted");

static_assert(offsetof(NaniteGPUInstance, transform) == 0, "instance layout drifted");
static_assert(offsetof(NaniteGPUInstance, prev_transform) == 64, "instance layout drifted");
static_assert(offsetof(NaniteGPUInstance, bounds_min) == 128, "instance layout drifted");
static_assert(offsetof(NaniteGPUInstance, bounds_max) == 144, "instance layout drifted");
static_assert(offsetof(NaniteGPUInstance, meta) == 160, "instance layout drifted");

static_assert(offsetof(NaniteCullConstants, view_projection) == 0, "constants layout drifted");
static_assert(offsetof(NaniteCullConstants, frustum_planes) == 64, "constants layout drifted");
static_assert(offsetof(NaniteCullConstants, camera_position) == 160, "constants layout drifted");
static_assert(offsetof(NaniteCullConstants, lod_params) == 176, "constants layout drifted");
static_assert(offsetof(NaniteCullConstants, counts) == 192, "constants layout drifted");

// ---------------------------------------------------------------------------
// 矩阵打包。
//
// **Matrix4x4 是行主序**（m_Mat[row][col]，clip = M * v，见 Matrix4.h 的注释），
// 而 GLSL 的 mat4 在存储缓冲里是**列主序**。直接把 m_Mat 的字节 memcpy 过去，
// shader 里写 `transform * vec4(p, 1.0)` 得到的是 Mᵀ·p，位置全错但不会报错 ——
// 所以这里显式转置，让 shader 侧可以一直用最自然的 `mat * vec`。
// ---------------------------------------------------------------------------
inline void NaniteGPUPackMatrix(const Matrix4x4& source, float out_column_major[16])
{
    for (int row = 0; row < 4; ++row)
    {
        for (int col = 0; col < 4; ++col)
        {
            out_column_major[col * 4 + row] = source.m_Mat[row][col];
        }
    }
}

// ---------------------------------------------------------------------------
// 打包后的 Nanite 场景。
//
// 关键决定：**顶点与索引是整个场景一份大缓冲**，不是每个网格一份。
// 集群的 index_offset / vertex_offset 在打包时加上该网格的基址，于是
//   * 索引值本身就是全局顶点下标 ⇒ 绘制时 vertexOffset(baseVertex) 恒为 0，
//     不需要依赖 gl_VertexIndex 是否叠加 baseVertex（这个语义四个后端不一致）；
//   * CmdBindIndexBuffer 一次绑到底，间接绘制的每个命令只是窗口不同。
//
// positions 存成 vec4 而不是 vec3：std430 下 vec3 的步长是 12 但对齐是 16，
// 一旦和 GLSL 侧的推断不一致就会整体错位。16 字节是唯一没有歧义的选择。
// ---------------------------------------------------------------------------
struct NaniteSceneGPUData
{
    std::vector<NaniteGPUCluster> clusters;
    std::vector<NaniteGPUInstance> instances;
    std::vector<Vector4> positions;
    std::vector<uint32_t> indices;

    bool empty() const { return clusters.empty() || instances.empty(); }

    void clear()
    {
        clusters.clear();
        instances.clear();
        positions.clear();
        indices.clear();
    }
};

// ---------------------------------------------------------------------------
// 把"实例列表 + 集群管理器里的网格资源"打包成 NaniteSceneGPUData。
//
// 每个实例的集群在 clusters 数组里占一段连续区间（写进 instance.meta 的
// cluster_offset / cluster_count），kernel 靠 cluster.instance_index 反查实例。
//
// 同一个网格被多个实例引用时，它的几何会被复制多份（每个实例一份集群记录 +
// 一份顶点/索引）。这不是最优（真正的 Nanite 是按网格去重、靠实例变换复用），
// 但它是**正确**的，而且把"集群属于哪个实例"这个信息直接固化在集群记录里，
// 不用在 shader 里做间接寻址。等有了真正的 GPU 剔除层次再优化。
// ---------------------------------------------------------------------------
inline void NaniteBuildSceneGPUData(const std::vector<NaniteInstance>& instances,
                                    const NaniteClusterManager& cluster_manager,
                                    NaniteSceneGPUData& out_data)
{
    out_data.clear();

    for (const NaniteInstance& instance : instances)
    {
        const NaniteMeshResource* mesh = cluster_manager.GetMeshResource(instance.mesh_id);
        if (mesh == nullptr || mesh->clusters.empty())
        {
            continue;
        }

        const uint32_t instance_index = static_cast<uint32_t>(out_data.instances.size());
        const uint32_t cluster_offset = static_cast<uint32_t>(out_data.clusters.size());
        const uint32_t vertex_base = static_cast<uint32_t>(out_data.positions.size());
        const uint32_t index_base = static_cast<uint32_t>(out_data.indices.size());

        NaniteGPUInstance gpu_instance {};
        NaniteGPUPackMatrix(instance.transform, &gpu_instance.transform.m_Mat[0][0]);
        NaniteGPUPackMatrix(instance.prev_transform, &gpu_instance.prev_transform.m_Mat[0][0]);
        gpu_instance.bounds_min = Vector4(instance.local_bounds.getMinCorner(), 0.0f);
        gpu_instance.bounds_max = Vector4(instance.local_bounds.getMaxCorner(), 0.0f);
        gpu_instance.meta.x = static_cast<uint32_t>(instance.mesh_id & 0xFFFFFFFFull);
        gpu_instance.meta.y = static_cast<uint32_t>(instance.mesh_id >> 32);
        gpu_instance.meta.z = cluster_offset;
        gpu_instance.meta.w = static_cast<uint32_t>(mesh->clusters.size());
        out_data.instances.push_back(gpu_instance);

        const uint32_t vertex_count = mesh->total_vertex_count;
        const uint32_t index_count = mesh->total_triangle_count * 3u;

        // 顶点：mesh->vertex_data 是 total_vertex_count 个 Vector3（12 字节），
        // 展开成 vec4。bytes 不够就说明 total_vertex_count 与实际数据不一致，
        // 宁可少拷也不要越界读。
        const uint32_t safe_vertex_count =
            std::min(vertex_count, static_cast<uint32_t>(mesh->vertex_data.size() / sizeof(Vector3)));
        out_data.positions.reserve(out_data.positions.size() + safe_vertex_count);
        const Vector3* vertices = reinterpret_cast<const Vector3*>(mesh->vertex_data.data());
        for (uint32_t v = 0; v < safe_vertex_count; ++v)
        {
            out_data.positions.emplace_back(vertices[v].x, vertices[v].y, vertices[v].z, 1.0f);
        }

        // 索引：值加上 vertex_base 变成全局下标。
        const uint32_t safe_index_count =
            std::min(index_count, static_cast<uint32_t>(mesh->index_data.size() / sizeof(uint32_t)));
        out_data.indices.reserve(out_data.indices.size() + safe_index_count);
        const uint32_t* src_indices = reinterpret_cast<const uint32_t*>(mesh->index_data.data());
        for (uint32_t i = 0; i < safe_index_count; ++i)
        {
            out_data.indices.push_back(src_indices[i] + vertex_base);
        }

        for (const NaniteCluster& cluster : mesh->clusters)
        {
            NaniteGPUCluster gpu_cluster {};
            gpu_cluster.center_radius =
                Vector4(cluster.center.x, cluster.center.y, cluster.center.z, cluster.radius);
            gpu_cluster.bounds_min = Vector4(cluster.bounds.getMinCorner(), cluster.error_metric);
            gpu_cluster.bounds_max = Vector4(cluster.bounds.getMaxCorner(),
                                             static_cast<float>(cluster.lod_level));
            gpu_cluster.geometry.x = cluster.vertex_offset + vertex_base;
            gpu_cluster.geometry.y = cluster.index_offset + index_base;
            gpu_cluster.geometry.z = cluster.triangle_count * 3u;
            gpu_cluster.geometry.w = instance_index;
            gpu_cluster.flags.x = cluster.is_leaf ? 1u : 0u;
            gpu_cluster.flags.y = cluster.triangle_count;
            out_data.clusters.push_back(gpu_cluster);
        }
    }
}
