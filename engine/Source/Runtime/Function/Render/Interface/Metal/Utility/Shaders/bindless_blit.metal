// =====================================================================
// Metal Bindless Texture Blit Shaders (MSL 2.0, Argument Buffer Tier2)
// ---------------------------------------------------------------------
// Metal sibling of the Vulkan bindless_blit.vert/.frag and DX12
// bindless_blit_vs.hlsl / bindless_blit_ps.hlsl.
//
// Architecture:
//   - Single MSL file containing both vertex and fragment functions
//     (Metal convention; one MTLLibrary per file, multiple entry points).
//   - Bindless textures accessed through an argument buffer whose layout
//     matches MetalBindlessTextureManager's MTLArgumentEncoder:
//       struct BindlessTable {
//           array<texture2d<float, access::sample>, N> textures [[id(0)]];
//           array<sampler, 4>                          samplers [[id(1)]];
//       };
//   - The argument buffer is bound at [[buffer(0)]] (=
//     MetalBindlessTextureManager::kBindlessBufferIndex).
//   - The packed 32-bit index is delivered as a 4-byte constant at
//     [[buffer(1)]] (= kBindlessIndexBufferIndex) via
//     setVertexBytes / setFragmentBytes, mirroring Vulkan push-constants
//     and DX12 root constants.
//
// Index-pack contract (identical to Vulkan / DX12 siblings):
//   bits  [ 0..15] : texture_index  -- slot in the bindless texture array
//   bits  [16..31] : sampler_index  -- 0..3, indexes the 4 static samplers
//
// Static sampler bank (matches MetalBindlessTextureManager and DX12):
//   0 = linear-wrap    (LinearMinMag, Repeat)
//   1 = linear-clamp   (LinearMinMag, ClampToEdge)
//   2 = point-wrap     (NearestMinMag, Repeat)
//   3 = point-clamp    (NearestMinMag, ClampToEdge)
//
// BINDLESS_CAPACITY:
//   Must be defined before compilation to match the runtime capacity
//   passed to MetalBindlessTextureManager::initialize(). Defaults to
//   16384 (desktop). The C++ pipeline prepends the actual value via
//   newLibraryWithSource:options: preprocessor macros.
//
// References:
//   - Apple: "Argument Buffers" WWDC sessions (2019-2022)
//   - UnrealEngine: FMetalBindlessDescriptorManager
//   - Unity 2023.1: MetalArgumentBuffer texture array
// =====================================================================

#include <metal_stdlib>
using namespace metal;

#ifndef BINDLESS_CAPACITY
#define BINDLESS_CAPACITY 16384
#endif

// =====================================================================
// Argument buffer struct -- MUST match MetalBindlessTextureManager's
// MTLArgumentEncoder layout exactly:
//   - textures at argument index 0 (kTextureArrayArgIndex)
//   - samplers at argument index 1 (kSamplerArrayArgIndex)
// =====================================================================
struct BindlessTable
{
    array<texture2d<float, access::sample>, BINDLESS_CAPACITY> textures [[id(0)]];
    array<sampler, 4>                                          samplers [[id(BINDLESS_CAPACITY)]];
};

// =====================================================================
// Vertex shader: fullscreen oversized triangle
// ---------------------------------------------------------------------
// Same idiom as Vulkan/DX12 siblings:
//   vid=0 -> (-1, -1)  uv=(0, 0)
//   vid=1 -> ( 3, -1)  uv=(2, 0)
//   vid=2 -> (-1,  3)  uv=(0, 2)
// After clipping to NDC [-1..1], the visible portion has uv in [0..1].
// No vertex buffer, no input layout -- driven solely by vertex_id.
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
// Fragment shader: bindless texture sample
// ---------------------------------------------------------------------
// Accesses a single texture from the argument buffer's texture array,
// sampled with one of the 4 static samplers. The texture and sampler
// indices are unpacked from the packed 32-bit constant.
//
// This is the Metal Tier2 argument_buffer + texture2d array path --
// the fundamental bindless access pattern for Metal GPUs.
// =====================================================================
fragment float4 bindless_blit_frag(BlitVSOutput          in       [[stage_in]],
                                   device BindlessTable& bindless [[buffer(0)]],
                                   constant uint&        packed_index [[buffer(1)]])
{
    // Unpack indices -- twin of BindlessIndex::unpackTexture / unpackSampler.
    const uint tex_idx  = packed_index & 0xFFFFu;
    const uint samp_idx = (packed_index >> 16u) & 0xFFFFu;

    // Sample the bindless texture using the argument buffer's
    // texture2d array + sampler array. This is the core of the
    // Metal bindless path:
    //   bindless.textures[tex_idx]  -> texture2d from the argument buffer
    //   bindless.samplers[samp_idx] -> sampler from the argument buffer
    //   .sample(sampler, coord)     -> standard MSL sampling operation
    return bindless.textures[tex_idx].sample(bindless.samplers[samp_idx], in.uv);
}
