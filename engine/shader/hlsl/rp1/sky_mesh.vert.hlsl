// Scene sky-dome mesh (LightMode=Sky). Writes at far depth like UE SkyPass.
// Correctly computes world_direction as a normalized direction vector for cubemap sampling.

#include "../common/render_storage_structs.hlsli"
#include "../common/main_camera_per_frame_access.hlsli"

cbuffer PerFrame : register(b0)
{
    MainCameraPerFrame per_frame;
};

StructuredBuffer<MeshInstance> mesh_instances : register(t1);

struct VsInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float3 tangent : TANGENT;
    float2 texcoord : TEXCOORD0;
};

struct VsOutput
{
    float4 position : SV_POSITION;
    float3 world_direction : TEXCOORD0;
};

VsOutput main(VsInput input, uint instance_id : SV_InstanceID)
{
    // For skybox, we need a direction vector from camera to the sky dome vertex.
    // The sky dome is typically centered at the camera, so we can use the vertex position
    // in model space directly as the direction vector (normalized).
    // The model matrix for skybox is usually just a translation to the camera position,
    // so world.xyz ≈ input.position (if model matrix is identity or translation only).
    float4 world = mul(mesh_instances[instance_id].model_matrix, float4(input.position, 1.0f));
    
    // Use proj_view_matrix (macro from main_camera_per_frame_access.hlsli)
    float4 clip_position = mul(proj_view_matrix, world);
    
    // Push to far depth (z = w * 0.99999 in clip space ≈ 1.0 in NDC after perspective division).
    // Use LEQUAL depth test to pass. Slight offset from w avoids depth precision issues at far plane.
    clip_position.z = clip_position.w * 0.99999f;
    
    VsOutput output;
    output.position = clip_position;
    // Normalize to get a unit direction vector for cubemap sampling.
    // This ensures correct cubemap sampling regardless of the sky mesh scale.
    output.world_direction = normalize(world.xyz);
    return output;
}
