// Nanite 的 GLSL / C++ 共享结构定义。
//
// **这里每一个字段都必须和 C++ 的 NaniteGPUData.h 逐字节对应。**
// NaniteGPUData.h 里有一组 static_assert 钉住 C++ 侧的 sizeof / offsetof，
// NaniteGPUCullingSmokeTest 会真的把数据送上 GPU 跑一遍再读回来 —— 两侧任何一处
// 漂移都会让那个测试红掉。改这里之前先看那边的注释。
//
// 对齐规则是 std430（SSBO 默认）：vec4 / uvec4 / mat4 都是 16 字节对齐，所以每个
// struct 都是 16 的倍数，且不含 float/uint 混排的洞。

#ifndef ZENGINE_NANITE_COMMON_H
#define ZENGINE_NANITE_COMMON_H

#define NANITE_MAX_HIERARCHY_DEPTH 16

// 80 字节。bounds 是**模型空间**的，要拿去和视锥比必须先乘实例的 transform。
struct NaniteGPUCluster
{
    vec4 center_radius;  // xyz = 模型空间中心, w = 半径
    vec4 bounds_min;     // xyz = 包围盒 min,   w = error_metric
    vec4 bounds_max;     // xyz = 包围盒 max,   w = lod_level
    uvec4 geometry;      // x = vertex_offset, y = index_offset, z = index_count, w = instance_index
    uvec4 flags;         // x = is_leaf, y = triangle_count, z/w = 保留
};

// 176 字节。矩阵是**列主序**（C++ 侧 NaniteGPUPackMatrix 已转置），
// 所以 shader 里可以直接写 `transform * vec4(p, 1.0)`。
struct NaniteGPUInstance
{
    mat4 transform;
    mat4 prev_transform;
    vec4 bounds_min;
    vec4 bounds_max;
    uvec4 meta;  // x/y = mesh_id 低/高 32 位, z = cluster_offset, w = cluster_count
};

// 20 字节，必须和 RHIDrawIndexedIndirectCommand 一致（Vulkan 的
// VkDrawIndexedIndirectCommand / DX12 的 D3D12_DRAW_INDEXED_ARGUMENTS /
// Metal 的 MTLDrawIndexedPrimitivesIndirectArguments 都是这个布局）。
struct NaniteDrawCommand
{
    uint indexCount;
    uint instanceCount;
    uint firstIndex;
    int vertexOffset;
    uint firstInstance;
};

#endif  // ZENGINE_NANITE_COMMON_H
