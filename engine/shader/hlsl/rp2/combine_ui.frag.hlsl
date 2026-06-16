// DX-B5: combine scene (backup_even = CG output) + UI layer -> swapchain.

Texture2D in_scene_color : register(t0, space0);
SamplerState in_scene_sampler : register(s0, space0);

Texture2D in_ui_color : register(t1, space0);
SamplerState in_ui_sampler : register(s1, space0);

struct PSInput
{
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
};

float4 main(PSInput input) : SV_TARGET
{
    float4 scene = in_scene_color.Sample(in_scene_sampler, input.uv);
    float4 ui    = in_ui_color.Sample(in_ui_sampler, input.uv);
    // Alpha-composite UI over scene.
    float3 result = lerp(scene.rgb, ui.rgb, ui.a);
    return float4(result, 1.0f);
}
