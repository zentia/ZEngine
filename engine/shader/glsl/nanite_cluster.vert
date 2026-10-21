#version 450

// ===========================================================================
// Nanite 集群光栅化：一次绘制画**一个集群**。
//
// 没有顶点属性、没有顶点缓冲绑定。顶点位置直接从 storage buffer 里按
// gl_VertexIndex 取 —— 因为打包时每个网格的索引都加了它在全局顶点数组里的基址，
// 索引值本身就是全局顶点下标，于是间接绘制命令的 vertexOffset(baseVertex) 恒为 0，
// gl_VertexIndex 直接就是我们要的下标。
// （baseVertex 是否叠加进 gl_VertexIndex 这件事四个后端语义不一致，绕开它是最稳的。）
//
// 集群下标来自 gl_InstanceIndex：每条间接命令的 instanceCount = 1、
// firstInstance = 集群下标。Vulkan 下 gl_InstanceIndex = firstInstance + i，
// SPIRV-Cross 在 macOS 上把它映射到 Metal 的 [[instance_id]]（同样含 baseInstance），
// 所以这条通路在 Vulkan / Metal 上一致。
// DX12 的 SV_InstanceID **不**含 StartInstanceLocation，那个后端要自己加回来。
// ===========================================================================

#extension GL_GOOGLE_include_directive : enable

#include "nanite_common.h"

layout(set = 0, binding = 0) uniform _nanite_cull_constants
{
    mat4 view_projection;
    vec4 frustum_planes[6];
    vec4 camera_position;
    vec4 lod_params;
    uvec4 counts;
} u_constants;

layout(std430, set = 0, binding = 1) readonly buffer _nanite_clusters
{
    NaniteGPUCluster clusters[];
};

layout(std430, set = 0, binding = 2) readonly buffer _nanite_instances
{
    NaniteGPUInstance instances[];
};

layout(std430, set = 0, binding = 6) readonly buffer _nanite_positions
{
    vec4 positions[];
};

layout(location = 0) out vec3 out_world_position;
layout(location = 1) out vec3 out_cluster_color;

// 按 LOD 层级染个色，方便肉眼确认集群确实在被挑/被丢。
// 这只是调试着色 —— Nanite 真正的材质路径还没接。
vec3 LodDebugColor(uint lod_level)
{
    float t = float(lod_level) / float(NANITE_MAX_HIERARCHY_DEPTH);
    return mix(vec3(0.15, 0.75, 0.35), vec3(0.95, 0.45, 0.10), clamp(t, 0.0, 1.0));
}

void main()
{
    uint cluster_index = uint(gl_InstanceIndex);
    NaniteGPUCluster cluster = clusters[cluster_index];
    NaniteGPUInstance instance = instances[cluster.geometry.w];

    vec3 local_position = positions[uint(gl_VertexIndex)].xyz;
    vec4 world_position = instance.transform * vec4(local_position, 1.0);

    gl_Position = u_constants.view_projection * world_position;
    out_world_position = world_position.xyz;
    out_cluster_color = LodDebugColor(uint(cluster.bounds_max.w));
}
