// DX-B5: color grading pass — reads backup_odd (RP1 HDR output), writes to backup_even.
// Applies ACES approx tonemapping + sRGB gamma correction for display on LDR swapchain.
// LUT is bound but unused (will be activated later for color grading lookup).

Texture2D in_color : register(t0, space0);
SamplerState in_color_sampler : register(s0, space0);

Texture2D color_grading_lut : register(t1, space0);
SamplerState color_grading_lut_sampler : register(s1, space0);

struct PSInput
{
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
};

// ------------------------------------------------------------------
// ACES tonemapping approximation (Krzanowski / Unreal).
// Gentle shoulder — works well for both LDR-ish (0–3) and real HDR.
// ------------------------------------------------------------------
static const float a = 0.0245786;
static const float b = 0.000090537;
static const float c = 0.3539984;
static const float d = 0.2375826;
static const float e = 0.0746349;
static const float f = 0.971218;

static const float kExposure = 1.0;   // tweak if scene looks too bright/dark

float3 TonemapACES(float3 x)
{
    return (x * (a * x + b) + c) / (x * (d * x + e) + f);
}

float3 ApplySRGBGamma(float3 linearColor)
{
    // Fast pow(1/2.2) — good enough for real-time.
    // Production code should use exact sRGB piecewise transfer.
    return pow(linearColor, 1.0 / 2.2);
}

float4 main(PSInput input) : SV_TARGET
{
    float4 hdr = in_color.Sample(in_color_sampler, input.uv);

    // Exposure (tunable).
    float3 exposed = hdr.rgb * kExposure;

    // Tonemap -> [0, ~1].
    float3 ldr = TonemapACES(exposed);

    // Gamma correction for display.
    ldr = ApplySRGBGamma(ldr);

    return float4(ldr, hdr.a);
}
