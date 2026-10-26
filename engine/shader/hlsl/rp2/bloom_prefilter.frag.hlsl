// Bloom pass 1/4: threshold + soft knee + 4-tap half-res downsample.
// Ported from engine/shader/glsl/bloom_prefilter.frag.
// VS is hlsl/rp2/post_process.vert.hlsl (fullscreen triangle, supplies uv).

Texture2D in_color : register(t0, space0);
SamplerState in_color_sampler : register(s0, space0);

// Mirrors BloomPass::BloomUniformBuffer -- three float4s, 48 bytes, std140-compatible.
cbuffer BloomUB : register(b0, space0)
{
    // x: threshold, y: threshold - knee, z: knee * 2, w: 0.25 / knee
    float4 filter_params;
    // x: intensity, y: scatter, z: clamp_max, w: unused
    float4 combine_params;
    // xy: 1/size of the input texture, zw: 1/size of the output texture
    float4 texel_size;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
};

static const float3 kLumaWeights = float3(0.2126, 0.7152, 0.0722);

// Karis average: weight by 1/(1+luma) so one firefly pixel cannot dominate the
// downsample (see the GLSL version for the full rationale).
float3 KarisAverage(float3 color)
{
    const float luma = dot(color, kLumaWeights) * 0.25;
    return color * (1.0 / (1.0 + luma));
}

float4 main(PSInput input) : SV_TARGET
{
    const float2 ts = texel_size.xy;

    const float3 a = in_color.Sample(in_color_sampler, input.uv + float2(-1.0, -1.0) * ts).rgb;
    const float3 b = in_color.Sample(in_color_sampler, input.uv + float2( 1.0, -1.0) * ts).rgb;
    const float3 c = in_color.Sample(in_color_sampler, input.uv + float2(-1.0,  1.0) * ts).rgb;
    const float3 d = in_color.Sample(in_color_sampler, input.uv + float2( 1.0,  1.0) * ts).rgb;

    float3 color = (a + b + c + d) * 0.25;
    color = KarisAverage(color);

    const float brightness = max(color.r, max(color.g, color.b));
    float soft = clamp(brightness - filter_params.y, 0.0, filter_params.z);
    soft = filter_params.w * soft * soft;
    const float contribution = max(soft, brightness - filter_params.x) / max(brightness, 1e-5);

    return float4(color * contribution, 1.0);
}
