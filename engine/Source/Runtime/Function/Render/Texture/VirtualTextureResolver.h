#pragma once

#include "VirtualTextureTypes.h"

#include <cstdint>
#include <vector>

class RHI;
class RHIBuffer;
class RHIDeviceMemory;
class RHIDescriptorSet;
class RHIDescriptorSetLayout;
class RHIPipeline;
class RHIPipelineLayout;
class RHISampler;
class VirtualTextureManager;

/// <summary>
/// One resolved virtual texel, as produced by vt_resolve.comp.
/// </summary>
struct VirtualTextureResolvedTexel
{
    uint32_t rgba;      // packUnorm4x8 of the sampled colour: R in the low byte, A in the high byte
    uint32_t resident;  // 1 = the page was resident and the colour came from the physical pool; 0 = fallback
};

/// <summary>
/// Runs vt_resolve.comp: the first shader that actually CONSUMES the page-table
/// texture.
///
/// For every texel of a virtual texture's mip 0 it does what a material shader
/// does at draw time -- virtual texel -> page table (texelFetch) -> physical slot
/// -> physical pool (texelFetch) -- and writes the result to a buffer that is read
/// back to the CPU. Used as the verification vehicle for the whole VT chain (page
/// upload + page table + invalidation) and as an on-demand debug dump of "what
/// would the GPU actually see".
///
/// The lookup itself lives in engine/shader/include/vt_common.h, shared with any
/// future material shader, so there is exactly one copy of the sampling contract.
///
/// Vulkan-only (ZENGINE_VT_GPU); Initialize() returns false elsewhere, and also
/// when the SPIR-V was not generated (no glslangValidator in the environment).
/// Every submission is a single-time command buffer that waits for the queue, so
/// calls are synchronous and must run on the thread that owns GPU submission.
/// </summary>
class VirtualTextureResolver
{
public:
    VirtualTextureResolver() = default;
    ~VirtualTextureResolver();

    VirtualTextureResolver(const VirtualTextureResolver&) = delete;
    VirtualTextureResolver& operator=(const VirtualTextureResolver&) = delete;

    bool Initialize(RHI* rhi);
    void Shutdown();
    bool isReady() const { return m_Ready; }

    enum class Mode : uint32_t
    {
        TexelFetch = 0,  // integer texel coordinate  (vt_common.h VTFetch)
        UV = 1,          // texel-centre UV           (vt_common.h VTSampleUV)
    };

    /// Resolve every mip-0 texel of `handle`. `out` gets virtual_width * virtual_height
    /// entries, row-major.
    ///
    /// A virtual texture that has never had a resident page has no page-table
    /// texture (lazy creation, see VirtualTextureResource::m_PageTableDirty) -- it
    /// is resolved entirely on the CPU as "nothing resident" rather than making the
    /// GPU bind a null image.
    ///
    /// Pages resolve through the texture's CURRENT GPU page table, so call
    /// VirtualTextureManager::FlushPageTables() first if residency changed through a
    /// path that does not flush (the batch entry points already do).
    bool Resolve(VirtualTextureManager& manager,
                 uint64_t handle,
                 Mode mode,
                 const float fallback_rgba[4],
                 std::vector<VirtualTextureResolvedTexel>& out);

private:
    bool EnsureOutputBuffer(uint64_t bytes);

    RHI* m_Rhi {nullptr};
    bool m_Ready {false};

    RHISampler* m_Sampler {nullptr};
    RHIBuffer* m_ParamsBuffer {nullptr};
    RHIDeviceMemory* m_ParamsMemory {nullptr};
    RHIBuffer* m_OutputBuffer {nullptr};
    RHIDeviceMemory* m_OutputMemory {nullptr};
    uint64_t m_OutputCapacity {0};

    // The RHI has no DestroyPipeline / DestroyPipelineLayout / DestroyDescriptorSetLayout
    // (same known gap as NaniteRenderer): these live until process exit. They are
    // created once, lazily, per Initialize.
    RHIDescriptorSetLayout* m_SetLayout {nullptr};
    RHIPipelineLayout* m_PipelineLayout {nullptr};
    RHIPipeline* m_Pipeline {nullptr};
    RHIDescriptorSet* m_DescriptorSet {nullptr};
};
