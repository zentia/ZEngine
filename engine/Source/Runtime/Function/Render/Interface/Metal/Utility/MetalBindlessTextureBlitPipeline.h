// =====================================================================
// MetalBindlessTextureBlitPipeline
// ---------------------------------------------------------------------
// Metal sibling of VulkanBindlessTextureBlitPipeline and
// BindlessTextureBlitPipeline (DX12). Compiles MSL shader source at
// runtime using [device newLibraryWithSource:], creates an
// MTLRenderPipelineState, and records fullscreen-triangle blit draws
// that sample a single texture from the engine's global bindless
// argument buffer (owned by MetalBindlessTextureManager).
//
// Architecture:
//   The Metal bindless path uses argument_buffer + texture2d<...> array:
//
//     struct BindlessTable {
//         array<texture2d<float, access::sample>, N> textures [[id(0)]];
//         array<sampler, 4>                          samplers [[id(N)]];
//     };
//
//   The argument buffer is bound at [[buffer(0)]] via
//   setVertexBuffer / setFragmentBuffer. The packed 32-bit bindless
//   index is delivered at [[buffer(1)]] via setVertexBytes /
//   setFragmentBytes (mirrors Vulkan push-constants and DX12 root
//   constants).
//
//   This is the Metal Tier2 path -- the fundamental bindless access
//   pattern for Apple GPUs since A11 / M1.
//
// Differences from Vulkan / DX12 siblings:
//   - Vulkan uses a GLSL sampler2D[] + push-constant path.
//   - DX12 uses HLSL SM 6.6 ResourceDescriptorHeap[] + root constant
//     + static sampler bank from the root signature.
//   - Metal uses MSL argument_buffer + texture2d array + sampler array,
//     with explicit useResource() residency management before draws.
//   - Metal does NOT go through the cross-backend RHI abstraction for
//     pipeline creation; it uses Metal APIs directly (id<MTLDevice>,
//     id<MTLRenderPipelineState>, etc.) because the MetalRHI stubs
//     don't support pipeline creation yet.
//
// API contract (mirrors Vulkan/DX12 siblings 1:1):
//   - Initialize() compiles MSL, creates pipeline state once.
//   - Shutdown() releases Metal resources.
//   - RecordBlit() records a fullscreen-triangle draw on the given
//     render command encoder.
//   - isReady() returns true iff Initialize() succeeded.
//
// Thread-safety:
//   - initialize / shutdown are NOT thread-safe (expected to be called
//     from the render thread during startup / teardown).
//   - RecordBlit() is const and safe to call from any thread that holds
//     a valid encoder reference.
// =====================================================================

#pragma once

#include "Runtime/Function/Render/Interface/RHI.h"
#include "Runtime/Function/Render/Interface/RHIStruct.h"

#include <cstdint>
#include <string>

#ifdef __APPLE__
    #define Component AppleComponent
    #include <Metal/Metal.h>
    #undef Component
#endif

class MetalRHI;
class MetalBindlessTextureManager;

class MetalBindlessTextureBlitPipeline
{
public:
    MetalBindlessTextureBlitPipeline() = default;
    ~MetalBindlessTextureBlitPipeline() = default;

    MetalBindlessTextureBlitPipeline(const MetalBindlessTextureBlitPipeline&) = delete;
    MetalBindlessTextureBlitPipeline& operator=(const MetalBindlessTextureBlitPipeline&) = delete;

    // Build the MTLRenderPipelineState for the bindless blit.
    //
    // Preconditions:
    //   - metal_rhi != nullptr AND metal_rhi->supportsBindlessTextures().
    //   - The MetalBindlessTextureManager has been initialized.
    //
    // Returns true on success; false on any Metal / shader-compile
    // failure, in which case isReady() stays false and RecordBlit()
    // is a no-op.
    bool Initialize(MetalRHI* metal_rhi);

    // Release all Metal resources. Safe to call multiple times.
    void Shutdown();

    // True iff Initialize() succeeded and Shutdown() has not been called.
    bool isReady() const { return m_Ready; }

    // Record one fullscreen-triangle drawcall that samples a bindless
    // texture into the current render pass.
    //
    // Preconditions:
    //   - isReady() == true.
    //   - encoder != nil (active MTLRenderCommandEncoder).
    //   - The encoder is inside a render pass with an RGBA8Unorm color
    //     attachment.
    //   - bindless_texture_index was returned by
    //     MetalBindlessTextureManager::allocate().
    //
    // Side effects on the encoder:
    //   1. setRenderPipelineState (binds this pipeline).
    //   2. setViewport (0, 0, width, height).
    //   3. setVertexBuffer / setFragmentBuffer (argument buffer at
    //      [[buffer(0)]]).
    //   4. setVertexBytes / setFragmentBytes (packed index at
    //      [[buffer(1)]]).
    //   5. useResource / useResources (residency for all allocated
    //      textures).
    //   6. drawPrimitives (3 vertices, fullscreen triangle).
    void RecordBlit(id<MTLRenderCommandEncoder> encoder,
                    uint32_t viewport_width,
                    uint32_t viewport_height,
                    uint32_t bindless_texture_index,
                    uint32_t sampler_index = 0) const;

    // Metal-specific accessors.
    id<MTLRenderPipelineState> getPipelineState() const { return m_PipelineState; }

private:
    // Compile the inline MSL source into a MTLLibrary.
    bool compileShader(id<MTLDevice> device, uint32_t capacity);

    // Create the MTLRenderPipelineState from the compiled library.
    bool createPipelineState(id<MTLDevice> device);

    MetalRHI* m_Rhi = nullptr;
    id<MTLLibrary> m_Library = nil;
    id<MTLRenderPipelineState> m_PipelineState = nil;
    bool m_Ready = false;
};
