#pragma once

// =====================================================================
// MetalBindlessTextureManager
// ---------------------------------------------------------------------
// Concrete Metal implementation of the cross-backend
// RHIBindlessTextureManager interface. Uses MTLArgumentBuffer with
// large texture/sampler arrays (requires Argument Buffer Tier 2,
// available since iOS 11.0 / macOS 10.13 on all Apple GPUs).
//
// Architecture:
//   - Single MTLBuffer backing an MTLArgumentEncoder layout that
//     contains:
//       * array<texture2d<float>, capacity> at argument index 0
//       * array<sampler, kNumStaticSamplers> at argument index 1
//   - Static sampler bank (4 entries: linear-wrap, linear-clamp,
//     point-wrap, point-clamp) mirrors the DX12 root-signature
//     static sampler convention. sampler_index in the packed
//     BindlessIndex selects among these.
//   - Slot allocation: same LIFO free-list + high-water mark as
//     Vulkan/DX12; slot 0 reserved for 1x1 white placeholder.
//   - Index delivery: setVertexBytes / setFragmentBytes (4 bytes,
//     same packed uint32 format as Vulkan push-constants and DX12
//     root constants).
//   - Resource residency: useResource / useResources on the active
//     render/compute command encoder before any draw that reads the
//     bindless table.
//
// Thread-safety:
//   - allocate / free / update guarded by std::mutex, same as Vulkan.
//   - Metal argument buffers are NOT update-while-in-flight safe
//     (no UPDATE_AFTER_BIND equivalent). Callers MUST only modify
//     the table between frames, never while a command buffer that
//     reads it is in flight.
//
// MSL shader-side contract:
//   struct BindlessTable {
//       array<texture2d<float, access::sample>, N> textures  [[id(0)]];
//       array<sampler, 4>                           samplers  [[id(N)]];
//   };
//   Fragment: device BindlessTable& bindless [[buffer(0)]],
//             constant uint& packed_index   [[buffer(1)]]
//
//   NOTE: [[id(N)]] is a FLAT index space in MSL argument buffers.
//   The texture array occupies ids 0..N-1, so the sampler array
//   must start at id N (capacity), NOT id 1. This is different
//   from Vulkan/DX12 where descriptor types have separate index
//   spaces.
//
// References:
//   - Apple: "Argument Buffers" WWDC sessions (2019-2022)
//   - UnrealEngine: FMetalBindlessDescriptorManager
//   - Unity 2023.1 Metal backend: MetalArgumentBuffer texture array
// =====================================================================

#include "Runtime/Function/Render/Interface/RHI.h"
#include "Runtime/Function/Render/Interface/RHIStruct.h"

#ifdef __APPLE__
#define Component AppleComponent
#include <Metal/Metal.h>
#undef Component
#endif

#include <mutex>
#include <vector>

class MetalBindlessTextureManager final : public RHIBindlessTextureManager
{
public:
    // Number of static samplers in the argument buffer.
    // Indices: 0=linear-wrap, 1=linear-clamp, 2=point-wrap, 3=point-clamp
    // Must match DX12's kBindlessStaticSamplerCount convention.
    static constexpr uint32_t kNumStaticSamplers = 4;

    // Metal buffer table index for the argument buffer when bound
    // via setVertexBuffer / setFragmentBuffer.
    static constexpr uint32_t kBindlessBufferIndex = 0;

    // Metal buffer table index for the packed bindless index constant.
    static constexpr uint32_t kBindlessIndexBufferIndex = 1;

    // Argument indices inside the argument buffer layout.
    // NOTE: Metal argument buffers use a FLAT [[id]] index space.
    // The texture array occupies ids 0..capacity-1, so the sampler
    // array's starting id equals the capacity (NOT 1).
    static constexpr uint32_t kTextureArrayArgIndex = 0;
    // kSamplerArrayArgIndex is dynamic -- it equals m_Capacity.
    // Use getSamplerArrayArgIndex() to query it at runtime.
    uint32_t getSamplerArrayArgIndex() const { return m_Capacity; }

    MetalBindlessTextureManager()           = default;
    ~MetalBindlessTextureManager() override = default;

    // Two-phase init. Returns false on any failure (Tier1 device, OOM,
    // etc.); the owning RHI should treat the manager as unusable.
    bool Initialize(id<MTLDevice> device, uint32_t capacity);

    // Releases all Metal resources. Safe to call multiple times.
    void Shutdown();

    // ------- RHIBindlessTextureManager overrides -------------------
    uint32_t capacity() const override { return m_Capacity; }
    uint32_t allocate(RHIImageView* image_view, RHISampler* sampler) override;
    void     free(uint32_t index) override;
    void     Update(uint32_t index, RHIImageView* image_view, RHISampler* sampler) override;
    // ---------------------------------------------------------------

    // Metal-specific accessors for the rest of the Metal backend.

    // The backing argument buffer. Bind via setVertexBuffer / setFragmentBuffer
    // at kBindlessBufferIndex before any draw that reads the bindless table.
    id<MTLBuffer> getArgumentBuffer() const { return m_ArgumentBuffer; }

    // The argument encoder used to write texture/sampler entries.
    // Retain for the manager's lifetime; needed for allocate / update.
    id<MTLArgumentEncoder> getArgumentEncoder() const { return m_ArgumentEncoder; }

    // Ensure all currently-allocated textures and samplers are resident
    // on the given render command encoder. Call once before any draw
    // that reads the bindless table (typically at the start of a render
    // pass or once per frame).
    void ensureResidency(id<MTLRenderCommandEncoder> encoder) const;

    // Overload for compute command encoder.
    void ensureResidency(id<MTLComputeCommandEncoder> encoder) const;

private:
    // Encode a single texture at the given slot. Caller must hold m_Mutex.
    void writeTextureLocked(uint32_t index, id<MTLTexture> texture);

    // Create the 4 static samplers and encode them into the argument buffer.
    // Called once during Initialize(). Returns false on failure.
    bool createAndEncodeStaticSamplers();

    // Create a 1x1 white placeholder texture, upload data, encode at slot 0.
    // Returns false on failure.
    bool CreateAndUploadPlaceholder();

    void DestroyPlaceholder();

    id<MTLDevice>           m_Device              = nil;
    id<MTLBuffer>           m_ArgumentBuffer     = nil;
    id<MTLArgumentEncoder>  m_ArgumentEncoder    = nil;

    // Static sampler objects (owned, encoded into the argument buffer once).
    id<MTLSamplerState>     m_StaticSamplers[kNumStaticSamplers] = {nil, nil, nil, nil};

    // Slot-0 placeholder texture (1x1 white RGBA8).
    id<MTLTexture>          m_PlaceholderTexture = nil;

    // All currently-allocated textures (excluding slot 0 placeholder)
    // for residency tracking. Updated under m_Mutex.
    std::vector<id<MTLTexture>> m_AllocatedTextures;

    uint32_t m_Capacity {0};

    // Slot bookkeeping (guarded by m_Mutex):
    std::mutex            m_Mutex;
    uint32_t              m_HighWaterMark {0};
    std::vector<uint32_t> m_FreeList;

    // Argument buffer layout offsets (queried from the encoder).
    NSUInteger m_TexturesOffset  {0};
    NSUInteger m_SamplersOffset  {0};
};
