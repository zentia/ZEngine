// =====================================================================
// PR7: Bindless texture-blit pixel shader (DX12, SM 6.6 + HLSL 2021)
// ---------------------------------------------------------------------
// First production-path consumer of the bindless toolchain validated
// in PR5c's smoke-test. Samples a single Texture2D from the engine's
// global bindless CBV/SRV/UAV heap (owned by
// DX12BindlessTextureManager) using the SM 6.6 ResourceDescriptorHeap
// intrinsic, picking a static sampler from the root signature's
// 4-entry static-sampler array.
//
// Compilation contract:
//   - Compiled at runtime by DX12ShaderCompiler::compileFromFile with
//     target_profile = "ps_6_6" and hlsl_version = "2021" (the same
//     parameters the smoke-test exercises in PR5c phase 3).
//   - The owning C++ pipeline declares the descriptor-set binding with
//     RHI_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT, which
//     drives DX12RHI::createPipelineLayout (PR6) into reserving a
//     32-bit root constant at b0/space0 + setting
//     CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED on the root signature.
//   - The static-sampler array (s0..s3) is automatically baked into
//     the root signature by the same DX12RHI path. Layout:
//         s0 = LinearWrap, s1 = LinearClamp,
//         s2 = PointWrap,  s3 = PointClamp
//     This MUST stay in sync with the smoke-test's
//     createBindlessRootSignature() and -- starting in PR7 -- with
//     DX12RHI::createPipelineLayout's static-sampler emission. A drift
//     would silently swap filtering modes, not crash; AGENTS.md 2.9
//     pins this as an invariant.
//
// Index-pack contract:
//   - The host pushes `BindlessIndex::pack(texture_index, sampler_index)`
//     (declared in rhi.h) via RHI::cmdSetBindlessIndexPFN. Layout:
//         bits  [ 0..15] : texture_index  (slot in the bindless heap)
//         bits  [16..31] : sampler_index  (0..3, indexes s0..s3)
//   - The unpack code below MUST stay byte-identical to the smoke-test's
//     bindless_smoke.hlsl (same masks, same shift). PR5c's
//     static_assert block on BindlessIndex::kTextureIndexMask /
//     kTextureIndexBits guards drift -- if anyone widens the texture
//     half in rhi.h the smoke-test build breaks before this shader
//     can ship a wrong-slot sample at runtime.
//
// Inputs / outputs:
//   - Inputs: VSOutput from bindless_blit_vs.hlsl (SV_POSITION + uv).
//   - Output: a single R8G8B8A8_UNORM color, written to the off-screen
//     RT owned by TextureInspectorPanel.
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

float4 main(PSInput input) : SV_TARGET
{
    // ----- Index unpack -------------------------------------------------
    // Twin of BindlessIndex::unpackTexture / unpackSampler in rhi.h.
    // NonUniformResourceIndex is required for SM 6.6 dynamic indexing
    // even when the index is uniform across a draw -- HLSL 2021 makes
    // it cheap on uniform indices and mandatory on divergent ones, so
    // wrapping it is the safe default.
    const uint texture_index = g_packed_indices & 0xFFFFu;
    const uint sampler_index = (g_packed_indices >> 16) & 0xFFFFu;

    Texture2D bound_texture =
        ResourceDescriptorHeap[NonUniformResourceIndex(texture_index)];

    // ----- Sampler-array dispatch --------------------------------------
    // Static-sampler bindings can't be subscripted dynamically, so we
    // do a four-way switch. Modern drivers collapse this to predication.
    float4 sampled;
    switch (sampler_index)
    {
        case 1:  sampled = bound_texture.Sample(g_linear_clamp, input.uv); break;
        case 2:  sampled = bound_texture.Sample(g_point_wrap,   input.uv); break;
        case 3:  sampled = bound_texture.Sample(g_point_clamp,  input.uv); break;
        default: sampled = bound_texture.Sample(g_linear_wrap,  input.uv); break;
    }

    return sampled;
}
