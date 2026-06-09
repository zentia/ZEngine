// Scene sky-dome mesh (LightMode=Sky). Writes at far depth like UE SkyPass.

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
    float4 world = mul(mesh_instances[instance_id].model_matrix, float4(input.position, 1.0f));
    float4 clip_position = mul(proj_view_matrix, world);
    // Push to far depth (z = w in clip space = 1.0 in NDC after perspective division).
    // Use LEQUAL depth test to pass.
    clip_position.z = clip_position.w;
    
    VsOutput output;
    output.position = clip_position;
    output.world_direction = world.xyz;
    return output;
}
