// Render SSBO/cbuffer layouts (C++ mirror: RenderCommon.h).
#include "scene_lighting_structs.hlsli"

struct MeshInstance
{
    float enable_vertex_blending;
    float _padding_enable_vertex_blending_1;
    float _padding_enable_vertex_blending_2;
    float _padding_enable_vertex_blending_3;
    float4x4 model_matrix;
};

struct MeshDrawPerDrawcall
{
    MeshInstance mesh_instances[64];
};

struct MeshDrawPerDrawcallVertexBlending
{
    float4x4 joint_matrices[1024 * 64];
};

// Per-vertex joint binding (C++ mirror: MeshVertex::JointBinding, 32 bytes).
// Uploaded into the per-mesh descriptor set (space1, binding 0) only for
// skinned meshes; rigid meshes get the 1-byte null buffer there.
struct MeshVertexJointBinding
{
    int4 indices;
    float4 weights;
};

// Must match s_MeshVertexBlendingMaxJointCount in RenderScene.cpp: the joint
// palette is a flat [max_joint_count * instance_id + joint_index] array.
static const uint kMeshVertexBlendingMaxJointCount = 1024;

struct AxisDrawStorage
{
    float4x4 model_matrix;
    uint selected_axis;
    uint3 _padding;
};

struct ParticleBillboardPerFrame
{
    float4x4 proj_view_matrix;
    float3 right_direction;
    float _pad_right;
    float3 up_direction;
    float _pad_up;
    float3 forward_direction;
    float _pad_forward;
};

struct ParticleCollisionPerFrame
{
    float4x4 view_matrix;
    float4x4 proj_view_matrix;
    float4x4 proj_inv_matrix;
};

// 布局必须与 CPU 侧 MeshMaterialUniform（Runtime/Function/Render/RenderCommon.h）
// 和 GLSL 版（engine/shader/glsl/mesh_gbuffer.frag）逐字节一致。
//
// padding 写成三个独立的 uint 而不是 uint3：HLSL cbuffer 的打包规则是"变量不能
// 跨越 16 字节寄存器边界"，一个 uint3 会被整体挪到下一个寄存器（offset 64），
// 于是 custom_params 落到 80 —— 和 GLSL 的 std140 布局就错开了。拆成三个 uint
// 它们能连续塞进寄存器 3（48/52/56/60），custom_params 才停在 64。
struct MeshMaterialUniform
{
    float4 base_color_factor;
    float metallic_factor;
    float roughness_factor;
    float normal_scale;
    float occlusion_strength;
    float3 emissive_factor;
    uint is_blend;
    uint is_double_sided;
    uint _padding_0;
    uint _padding_1;
    uint _padding_2;
    float4 custom_params[4];
};
