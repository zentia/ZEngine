// Bloom pass 4/4: composite the finished bloom back onto the HDR scene.
// Ported from engine/shader/glsl/bloom_combine.frag.

Texture2D in_scene : register(t0, space0);
SamplerState in_scene_sampler : register(s0, space0);
Texture2D in_bloom : register(t1, space0);
SamplerState in_bloom_sampler : register(s1, space0);

cbuffer BloomUB : register(b0, space0)
{
    float4 filter_params;
    float4 combine_params;
    float4 texel_size;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
};

float4 main(PSInput input) : SV_TARGET
{
    const float3 scene = in_scene.Sample(in_scene_sampler, input.uv).rgb;

    const float3 tight = in_bloom.Sample(in_bloom_sampler, input.uv).rgb;

    const float2 ts = texel_size.zw * 4.0;
    const float3 wide = (in_bloom.Sample(in_bloom_sampler, input.uv + float2(-1.0, -1.0) * ts).rgb +
                         in_bloom.Sample(in_bloom_sampler, input.uv + float2( 1.0, -1.0) * ts).rgb +
                         in_bloom.Sample(in_bloom_sampler, input.uv + float2(-1.0,  1.0) * ts).rgb +
                         in_bloom.Sample(in_bloom_sampler, input.uv + float2( 1.0,  1.0) * ts).rgb) * 0.25;

    const float3 bloom = lerp(tight, wide, clamp(combine_params.y, 0.0, 1.0));

    // Additive in HDR, before tone mapping.
    return float4(scene + bloom * combine_params.x, 1.0);
}
