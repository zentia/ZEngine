// DX-B5: combine scene (backup_odd) + UI layer (backup_even) -> swapchain.

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
    float4 scene_color = in_scene_color.Sample(in_scene_sampler, input.uv);
    float4 ui_color = in_ui_color.Sample(in_ui_sampler, input.uv);

    ui_color.rgb = pow(max(ui_color.rgb, 0.0), 1.0 / 2.2);
    ui_color.a = pow(max(ui_color.a, 0.0), 1.0 / 2.2);

    // Standard over: transparent UI pixels show the graded scene underneath.
    float3 out_rgb = lerp(scene_color.rgb, ui_color.rgb, ui_color.a);
    return float4(out_rgb, 1.0);
}
