// Bloom pass 2/4: 13-tap downsample (Jimenez, SIGGRAPH 2014).
// Ported from engine/shader/glsl/bloom_downsample.frag.

Texture2D in_color : register(t0, space0);
SamplerState in_color_sampler : register(s0, space0);

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

float3 Fetch(float2 uv, float dx, float dy)
{
    return in_color.Sample(in_color_sampler, uv + float2(dx, dy) * texel_size.xy).rgb;
}

float4 main(PSInput input) : SV_TARGET
{
    //  A B C
    //   D E
    //  F G H
    //   I J
    //  K L M
    const float3 a = Fetch(input.uv, -2.0, -2.0);
    const float3 b = Fetch(input.uv,  0.0, -2.0);
    const float3 c = Fetch(input.uv,  2.0, -2.0);
    const float3 d = Fetch(input.uv, -1.0, -1.0);
    const float3 e = Fetch(input.uv,  1.0, -1.0);
    const float3 f = Fetch(input.uv, -2.0,  0.0);
    const float3 g = Fetch(input.uv,  0.0,  0.0);
    const float3 h = Fetch(input.uv,  2.0,  0.0);
    const float3 i = Fetch(input.uv, -1.0,  1.0);
    const float3 j = Fetch(input.uv,  1.0,  1.0);
    const float3 k = Fetch(input.uv, -2.0,  2.0);
    const float3 l = Fetch(input.uv,  0.0,  2.0);
    const float3 m = Fetch(input.uv,  2.0,  2.0);

    const float3 result = (d + e + i + j) * 0.125 +
                          (a + b + g + f) * 0.03125 +
                          (b + c + h + g) * 0.03125 +
                          (f + g + l + k) * 0.03125 +
                          (g + h + m + l) * 0.03125;

    return float4(result, 1.0);
}
