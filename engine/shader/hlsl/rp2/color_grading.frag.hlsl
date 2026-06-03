// DX-B5: color grading subpass (Vulkan color_grading.frag passthrough; LUT bound but unused).

Texture2D in_color : register(t0, space0);
SamplerState in_color_sampler : register(s0, space0);

Texture2D color_grading_lut : register(t1, space0);
SamplerState color_grading_lut_sampler : register(s1, space0);

struct PSInput
{
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
};

float4 main(PSInput input) : SV_TARGET
{
    return in_color.Sample(in_color_sampler, input.uv);
}
