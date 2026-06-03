// DX-B3: rigid mesh vertex shader (matches MeshVertex 3-stream layout).

#include "../common/scene_lighting_structs.hlsli"
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
    float3 world_position : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float3 tangent : TEXCOORD2;
    float2 texcoord : TEXCOORD3;
};

VsOutput main(VsInput input, uint instance_id : SV_InstanceID)
{
    float4 world = mul(mesh_instances[instance_id].model_matrix, float4(input.position, 1.0f));
    float3 world_normal = normalize(mul((float3x3)mesh_instances[instance_id].model_matrix, input.normal));
    float3 world_tangent = normalize(mul((float3x3)mesh_instances[instance_id].model_matrix, input.tangent));

    VsOutput output;
    output.position = mul(proj_view_matrix, world);
    output.world_position = world.xyz;
    output.normal = world_normal;
    output.tangent = world_tangent;
    output.texcoord = input.texcoord;
    return output;
}
