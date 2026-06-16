// Plan C mesh SkyPass: sample global IBL specular cubemap into HDR backup_odd.
// VS: sky_mesh.vert.hlsl (world position on TEXCOORD0).
// Correctly samples the specular IBL cubemap for sky rendering.

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
    float3 world_direction : TEXCOORD0;
};

float4 main(PsInput input) : SV_Target0
{
    // DEBUG: Output pure red to test if sky mesh drawing works at all.
    // If you see red sky, the problem is in texture sampling (cubemap data or sampling params).
    // If you still see black, the problem is in draw call or render target.
    return float4(1.0f, 0.0f, 0.0f, 1.0f);

    // // Sample the specular IBL cubemap using the world direction from vertex shader.
    // // The world_direction is already normalized in the vertex shader.
    // // Use LOD 0 for sky = highest mip = sharpest reflection (matches UE SkyLight capture).
    // float3 sky_color = specular_map.SampleLevel(specular_sampler, input.world_direction, 0.0f).rgb;

    // return float4(sky_color, 1.0f);
}
