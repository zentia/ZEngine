// =====================================================================
// Metal Bindless Tone-Mapping Shader (MSL 2.0, Argument Buffer Tier2)
// ---------------------------------------------------------------------
// Metal sibling of tone_mapping_bindless.frag (Vulkan) and
// bindless_tonemap_ps.hlsl (DX12).
//
// Applies the Uncharted2 tone curve + Gamma 2.2 correction to an HDR
// input texture sampled from the engine's global bindless argument
// buffer. The tone-mapping math is byte-identical to the Vulkan / DX12
// siblings:
//   - Uncharted2 curve with A=0.15, B=0.50, C=0.10, D=0.20,
//     E=0.02, F=0.30
//   - Exposure multiplier: 4.5
//   - White point normalization: Uncharted2Tonemap(11.2)
//   - Gamma 2.2 correction (per-channel pow)
//
// Bindless wiring (identical to bindless_blit.metal):
//   - [[buffer(0)]] = device BindlessTable& bindless
//     (argument buffer with texture2d array + sampler array)
//   - [[buffer(1)]] = constant uint& packed_index
//     (4-byte packed index via setVertexBytes / setFragmentBytes)
//
// This shader differs from bindless_blit.metal ONLY in the tone-mapping
// math between the texture fetch and the return. The fullscreen-triangle
// VS, index unpack, and argument-buffer access are byte-identical.
// =====================================================================

#include <metal_stdlib>
using namespace metal;

#ifndef BINDLESS_CAPACITY
#define BINDLESS_CAPACITY 16384
#endif

struct BindlessTable
{
    array<texture2d<float, access::sample>, BINDLESS_CAPACITY> textures [[id(0)]];
    array<sampler, 4>                                          samplers [[id(BINDLESS_CAPACITY)]];
};

// =====================================================================
// Vertex shader: reuse the same fullscreen-triangle VS
// =====================================================================
struct BlitVSOutput
{
    float4 position [[position]];
    float2 uv;
};

vertex BlitVSOutput bindless_blit_vert(uint vid [[vertex_id]])
{
    BlitVSOutput out;
    float2 uv = float2((vid << 1) & 2, vid & 2);
    out.uv       = uv;
    out.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return out;
}

// =====================================================================
// Fragment shader: bindless tonemap
// =====================================================================

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

fragment float4 bindless_tonemap_frag(BlitVSOutput          in       [[stage_in]],
                                      device BindlessTable& bindless [[buffer(0)]],
                                      constant uint&        packed_index [[buffer(1)]])
{
    const uint tex_idx  = packed_index & 0xFFFFu;
    const uint samp_idx = (packed_index >> 16u) & 0xFFFFu;

    // Sample from the bindless argument buffer.
    float3 color = bindless.textures[tex_idx].sample(bindless.samplers[samp_idx], in.uv).rgb;

    // Uncharted2 tonemap -- same math as Vulkan / DX12 siblings.
    color = Uncharted2Tonemap(color * 4.5);
    color = color * (1.0 / Uncharted2Tonemap(float3(11.2, 11.2, 11.2)));

    // Gamma 2.2 correction.
    color = float3(pow(color.x, 1.0 / 2.2),
                   pow(color.y, 1.0 / 2.2),
                   pow(color.z, 1.0 / 2.2));

    return float4(color, 1.0);
}
