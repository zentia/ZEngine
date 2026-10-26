// Bloom pass 3/4: 9-tap tent upsample, non-additive variant.
// Ported from engine/shader/glsl/bloom_upsample.frag.
//
// Reads the smaller mip AND the same-size mip from the downsample chain and writes
// their sum, so the pass needs no additive blend state (see the GLSL version).

Texture2D in_lower : register(t0, space0);
SamplerState in_lower_sampler : register(s0, space0);
Texture2D in_same  : register(t1, space0);
SamplerState in_same_sampler  : register(s1, space0);

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

float3 TentFetch(float2 uv, float dx, float dy)
{
    return in_lower.Sample(in_lower_sampler, uv + float2(dx, dy) * texel_size.xy).rgb;
}

float4 main(PSInput input) : SV_TARGET
{
    float3 lower = TentFetch(input.uv, -1.0, -1.0) * 1.0 +
                   TentFetch(input.uv,  0.0, -1.0) * 2.0 +
                   TentFetch(input.uv,  1.0, -1.0) * 1.0 +
                   TentFetch(input.uv, -1.0,  0.0) * 2.0 +
                   TentFetch(input.uv,  0.0,  0.0) * 4.0 +
                   TentFetch(input.uv,  1.0,  0.0) * 2.0 +
                   TentFetch(input.uv, -1.0,  1.0) * 1.0 +
                   TentFetch(input.uv,  0.0,  1.0) * 2.0 +
                   TentFetch(input.uv,  1.0,  1.0) * 1.0;
    lower *= (1.0 / 16.0);

    const float3 same = in_same.Sample(in_same_sampler, input.uv).rgb;

    const float3 sum = min(lower + same, float3(combine_params.z, combine_params.z, combine_params.z));
    return float4(sum, 1.0);
}
