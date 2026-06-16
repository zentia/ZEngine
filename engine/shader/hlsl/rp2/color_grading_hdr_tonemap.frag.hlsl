// DX12: tonemap deferred HDR (backup_odd) inside RP2 color grading.
// NOTE: Currently disabled in DX12MainCameraPass (RP2 color_grading.frag handles
// tonemapping).  Re-enable by changing `if (false && m_TonemapReady)` to
// `if (m_TonemapReady)` in DX12MainCameraPass.cpp.

Texture2D in_hdr : register(t0, space0);
SamplerState in_hdr_sampler : register(s0, space0);

Texture2D color_grading_lut : register(t1, space0);
SamplerState color_grading_lut_sampler : register(s1, space0);

struct PSInput
{
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
};

// ACES tonemapping approximation (same as color_grading.frag.hlsl).
static const float a = 0.0245786;
static const float b = 0.000090537;
static const float c = 0.3539984;
static const float d = 0.2375826;
static const float e = 0.0746349;
static const float f = 0.971218;

static const float kExposure = 1.0;

float3 TonemapACES(float3 x)
{
    return (x * (a * x + b) + c) / (x * (d * x + e) + f);
}

float4 main(PSInput input) : SV_TARGET
{
    float4 hdr = in_hdr.Sample(in_hdr_sampler, input.uv);
    float3 exposed = hdr.rgb * kExposure;
    float3 ldr = TonemapACES(exposed);
    ldr = pow(ldr, 1.0 / 2.2);   // fast sRGB gamma
    return float4(ldr, hdr.a);
}
