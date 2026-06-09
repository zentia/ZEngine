// Procedural sky cube (36 verts). Matches Vulkan skybox.vert timing/depth.

#include "../common/scene_lighting_structs.hlsli"
#include "../common/main_camera_per_frame_access.hlsli"

cbuffer PerFrame : register(b0)
{
    MainCameraPerFrame per_frame;
};

struct VsOutput
{
    float4 position : SV_POSITION;
    float3 world_direction : TEXCOORD0;
};

VsOutput main(uint vertex_id : SV_VertexID)
{
    static const float3 k_cube_corners[8] = {
        float3(1.0f, 1.0f, 1.0f),   float3(1.0f, 1.0f, -1.0f),  float3(1.0f, -1.0f, -1.0f),
        float3(1.0f, -1.0f, 1.0f),  float3(-1.0f, 1.0f, 1.0f),  float3(-1.0f, 1.0f, -1.0f),
        float3(-1.0f, -1.0f, -1.0f), float3(-1.0f, -1.0f, 1.0f)};

    static const uint k_cube_indices[36] = {
        0, 1, 2, 2, 3, 0, 4, 5, 1, 1, 0, 4, 7, 6, 5, 5, 4, 7,
        3, 2, 6, 6, 7, 3, 4, 0, 3, 3, 7, 4, 1, 5, 6, 6, 2, 1};

    const float3 world_position = camera_position + k_cube_corners[k_cube_indices[vertex_id]];
    float4 clip_position = mul(proj_view_matrix, float4(world_position, 1.0f));
    clip_position.z = clip_position.w * 0.99999f;

    VsOutput output;
    output.position = clip_position;
    output.world_direction = normalize(world_position - camera_position);
    return output;
}
