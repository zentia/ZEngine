// Plan C mesh SkyPass: sample global IBL specular cubemap into HDR backup_odd.
// VS: sky_mesh.vert.hlsl (world position on TEXCOORD0).
// DEBUG: Output solid red to verify sky mesh rendering.

#include "../common/scene_lighting_structs.hlsli"

cbuffer PerFrame : register(b0)
{
    MainCameraPerFrame per_frame;
};

#include "../common/main_camera_per_frame_access.hlsli"

TextureCube specular_map : register(t5);
SamplerState specular_sampler : register(s5);

struct PsInput
{
    float4 position : SV_POSITION;
    float3 world_position : TEXCOORD0;
};

float4 main(PsInput input) : SV_Target0
{
    // DEBUG: Output solid red to verify sky mesh rendering.
    // If you see red in the sky area, the mesh is being drawn.
    return float4(1.0f, 0.0f, 0.0f, 1.0f);
}
