// DX12: tonemap deferred HDR (backup_odd) inside RP2 color grading.
// Avoids bindless heap copy of transient framebuffer views between RP1 and tonemap.

Texture2D in_hdr : register(t0, space0);
SamplerState in_hdr_sampler : register(s0, space0);

Texture2D color_grading_lut : register(t1, space0);
SamplerState color_grading_lut_sampler : register(s1, space0);

struct PSInput
{
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
};

float3 Uncharted2Tonemap(float3 x)
{
    float A = 0.15;
    float B = 0.50;
    float C = 0.10;
    float D = 0.20;
    float E = 0.02;
    float F = 0.30;
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}

float4 main(PSInput input) : SV_TARGET
{
    float3 color = min(in_hdr.Sample(in_hdr_sampler, input.uv).rgb, float3(64.0, 64.0, 64.0));
    color = Uncharted2Tonemap(color * 1.5);
    color = color * (1.0 / Uncharted2Tonemap(float3(11.2, 11.2, 11.2)));
    color = float3(pow(color.x, 1.0 / 2.2), pow(color.y, 1.0 / 2.2), pow(color.z, 1.0 / 2.2));
    return float4(color, 1.0);
}
