// =====================================================================
// PR-DX2: Bindless tonemap pixel shader (DX12, SM 6.6 + HLSL 2021)
// ---------------------------------------------------------------------
// DX12 sibling of `tone_mapping_bindless.frag` (Vulkan, PR-V4 part 1).
// Applies the Uncharted2 tone curve + Gamma 2.2 correction to an HDR
// input texture sampled from the engine's global bindless CBV/SRV/UAV
// heap.
//
// The tonemap math is byte-identical to the Vulkan GLSL version:
//   - Uncharted2 curve with A=0.15, B=0.50, C=0.10, D=0.20,
//     E=0.02, F=0.30
//   - Exposure multiplier: 4.5
//   - White point normalization: Uncharted2Tonemap(11.2)
//   - Gamma 2.2 correction (per-channel pow)
//
// Compilation contract (same as bindless_blit_ps.hlsl):
//   - Compiled at runtime by DX12ShaderCompiler::compileFromFile with
//     target_profile = "ps_6_6" and hlsl_version = "2021".
//   - The owning C++ pipeline declares the descriptor-set binding with
//     RHI_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT, which
//     drives DX12RHI::createPipelineLayout into reserving a 32-bit
//     root constant at b0/space0 + setting
//     CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED on the root signature.
//   - The static-sampler array (s0..s3) is automatically baked into
//     the root signature by the same DX12RHI path.
//
// Index-pack contract (same as bindless_blit_ps.hlsl):
//   - bits [ 0..15] : texture_index  (slot in the bindless heap)
//   - bits [16..31] : sampler_index  (0..3, indexes s0..s3)
//
// This shader differs from bindless_blit_ps.hlsl ONLY in the tone-
// mapping math between the texture fetch and the return. The
// fullscreen-triangle VS, index unpack, and sampler dispatch are
// byte-identical.
// =====================================================================

cbuffer BindlessIndices : register(b0)
{
    uint g_packed_indices;
};

// Static samplers from the root signature. Order MUST match the host
// emission in DX12RHI::createPipelineLayout (and the smoke-test's
// createBindlessRootSignature).
SamplerState g_linear_wrap   : register(s0);
SamplerState g_linear_clamp  : register(s1);
SamplerState g_point_wrap    : register(s2);
SamplerState g_point_clamp   : register(s3);

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
    // ----- Index unpack -------------------------------------------------
    const uint texture_index = g_packed_indices & 0xFFFFu;
    const uint sampler_index = (g_packed_indices >> 16) & 0xFFFFu;

    Texture2D bound_texture =
        ResourceDescriptorHeap[NonUniformResourceIndex(texture_index)];

    // ----- Sampler-array dispatch --------------------------------------
    float4 sampled;
    switch (sampler_index)
    {
        case 1:  sampled = bound_texture.Sample(g_linear_clamp, input.uv); break;
        case 2:  sampled = bound_texture.Sample(g_point_wrap,   input.uv); break;
        case 3:  sampled = bound_texture.Sample(g_point_clamp,  input.uv); break;
        default: sampled = bound_texture.Sample(g_linear_wrap,  input.uv); break;
    }

    // ----- Uncharted2 tonemap + Gamma 2.2 ------------------------------
    // Same math as the Vulkan sibling (tone_mapping_bindless.frag) and
    // the legacy tone_mapping.frag. Kept in sync so swapping the
    // shader between bindless and legacy paths is a pixel-equivalent
    // change.
    float3 color = sampled.rgb;

    color = Uncharted2Tonemap(color * 4.5);
    color = color * (1.0 / Uncharted2Tonemap(float3(11.2, 11.2, 11.2)));

    color = float3(pow(color.x, 1.0 / 2.2),
                   pow(color.y, 1.0 / 2.2),
                   pow(color.z, 1.0 / 2.2));

    return float4(color, 1.0);
}
