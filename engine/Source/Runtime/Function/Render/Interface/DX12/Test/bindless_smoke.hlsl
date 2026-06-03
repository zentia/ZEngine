// =====================================================================
// PR5c: Bindless smoke-test pixel shader
// ---------------------------------------------------------------------
// Minimal SM 6.6 + HLSL 2021 pixel shader that demonstrates the
// `ResourceDescriptorHeap[]` indexing path the engine's bindless
// pipeline is built around. NOT linked into ZRuntime / ZEditor; only
// consumed by `dx12_bindless_smoke_test.cpp`.
//
// Contract being validated by the smoke-test:
//   - DXC accepts SM 6.6 (`ps_6_6`) + `-HV 2021` via the new
//     per-compile `target_profile` / `hlsl_version` parameters added
//     in PR5b.
//   - The resulting DXIL references `ResourceDescriptorHeap` (the
//     SM 6.6 bindless intrinsic) and binds samplers from the root
//     signature's static-sampler array, NOT from a sampler-bindless
//     table. This matches the engine's deliberate choice (see
//     AGENTS.md 2.9 + DX12RHI::getBindlessRootSignatureFlags doc):
//     SAMPLER_HEAP_DIRECTLY_INDEXED stays OFF, samplers come from
//     the root signature, only CBV/SRV/UAV go through dynamic
//     indexing.
//   - When fed to `ID3D12Device::CreateGraphicsPipelineState` with
//     a root signature whose flags include
//     `D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED`
//     (sourced from `DX12RHI::getBindlessRootSignatureFlags()`), PSO
//     creation succeeds. PSO success is the smoke-test's pass
//     criterion -- we don't actually draw anything.
//
// Layout assumption (must match smoke-test root signature):
//   - Root parameter 0: 32-bit root constant `g_packed_indices`
//     carrying `(texture_index, sampler_index)` in a single uint,
//     packed as:
//         texture_index = root_constant & 0xFFFF;
//         sampler_index = (root_constant >> 16) & 0xFFFF;
//     This unpack MUST stay in sync with the host-side helper
//     `BindlessIndex::pack / unpackTexture / unpackSampler` declared
//     in `runtime/function/render/interface/rhi.h`. The smoke-test
//     pins the half-widths via `static_assert` -- a width drift on
//     either side breaks the build before it can ship.
//   - Static sampler array s0..s3: linear-wrap, linear-clamp,
//     point-wrap, point-clamp respectively. The shader picks one
//     by `sampler_index` via a four-way switch (HLSL has no array
//     subscript on SamplerState declared via static samplers).
//   - Texture2D resources are indexed via ResourceDescriptorHeap
//     (the bindless table maintained by DX12BindlessTextureManager),
//     guarded with NonUniformResourceIndex per HLSL 2021 best
//     practices.
// =====================================================================

// Root constant slot bound at root parameter 0. The smoke-test never
// actually draws -- it only checks that the PSO can be built -- but
// keeping the cbuffer here makes the DXIL reference the root
// constant, which forces the root signature to be a non-empty match.
cbuffer BindlessIndices : register(b0)
{
    uint g_packed_indices;
};

// Static samplers declared in the root signature land at these
// register slots. Declared individually (not as `SamplerState[4]`)
// because static-sampler arrays in DX12 root signatures are still
// modelled as four discrete static-sampler entries with sequential
// shader registers, and HLSL static-sampler bindings can't be
// indexed dynamically.
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
    // Unpack indices. NonUniformResourceIndex is required when the
    // index could vary across a wave (e.g. different draw calls
    // sampling different textures via the same shader); harmless and
    // free when the index is uniform, so we always wrap it.
    //
    // These two lines are the HLSL twin of `BindlessIndex::
    // unpackTexture / unpackSampler` in rhi.h. The smoke-test's
    // `static_assert(BindlessIndex::kTextureIndexMask == 0xFFFFu, ...)`
    // pins the literals to the host helper.
    const uint texture_index = g_packed_indices & 0xFFFFu;
    const uint sampler_index = (g_packed_indices >> 16) & 0xFFFFu;

    // SM 6.6 dynamic-resource indexing. ResourceDescriptorHeap[i]
    // resolves to the i-th descriptor in the currently bound
    // CBV/SRV/UAV heap (which is DX12BindlessTextureManager's
    // dedicated shader-visible heap on the engine's hot path).
    Texture2D bound_texture = ResourceDescriptorHeap[NonUniformResourceIndex(texture_index)];

    // Sampler-side: pick from the static-sampler array. A four-way
    // branch is the textbook idiom on HLSL when samplers come from
    // root signature statics. The branch collapses to predication
    // on every modern driver and is irrelevant for the smoke-test
    // anyway (we never execute the shader).
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
