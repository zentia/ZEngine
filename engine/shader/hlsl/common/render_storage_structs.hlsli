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
};
