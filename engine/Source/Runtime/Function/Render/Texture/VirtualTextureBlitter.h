#pragma once

#include "VirtualTextureTypes.h"

#include <cstdint>
#include <vector>

class RHI;
class RHIBuffer;
class RHIDeviceMemory;
class RHIDescriptorSet;
class RHIDescriptorSetLayout;
class RHIFramebuffer;
class RHIImage;
class RHIImageView;
class RHIPipeline;
class RHIPipelineLayout;
class RHIRenderPass;
class RHISampler;
class VirtualTextureManager;

/// <summary>
/// Rasterizes a virtual texture onto an OFFSCREEN colour target with a full-screen
/// triangle, sampling it through the page table in a FRAGMENT shader
/// (vt_blit.vert / vt_blit.frag, which #include vt_common.h), then reads the image
/// back.
///
/// This is the first fragment-stage consumer of the page table, and deliberately
/// not wired into any camera pass: it owns its own render pass / framebuffer /
/// pipeline, so it cannot disturb MainCameraPass (which has a history of crashes).
/// vt_resolve.comp proves the lookup is right per integer texel in compute; this
/// proves it survives rasterization -- interpolated UVs at pixel centres, an output
/// size different from the virtual texture's, and the fragment-stage sampler path.
///
/// Vulkan-only (ZENGINE_VT_GPU); Initialize() returns false elsewhere or when the
/// SPIR-V was not generated. Every submission is a single-time command buffer that
/// waits for the queue, so calls are synchronous.
/// </summary>
class VirtualTextureBlitter
{
public:
    VirtualTextureBlitter() = default;
    ~VirtualTextureBlitter();

    VirtualTextureBlitter(const VirtualTextureBlitter&) = delete;
    VirtualTextureBlitter& operator=(const VirtualTextureBlitter&) = delete;

    bool Initialize(RHI* rhi);
    void Shutdown();
    bool isReady() const { return m_Ready; }

    /// Draw `handle`'s mip 0 into an out_width x out_height R8G8B8A8_UNORM target and
    /// read it back as packed RGBA (R in the low byte, A in the high byte),
    /// row-major, row 0 = top row of the virtual texture.
    ///
    /// A VT that never had a resident page has no page-table texture (lazy
    /// creation); it is filled with `fallback_rgba` on the CPU instead of binding a
    /// null image. Fails (returns false) if the pool image holds no uploaded page
    /// yet, because the pool would still be in the UNDEFINED layout.
    bool Render(VirtualTextureManager& manager,
                uint64_t handle,
                uint32_t out_width,
                uint32_t out_height,
                const float fallback_rgba[4],
                std::vector<uint32_t>& out_rgba);

private:
    bool EnsureTarget(uint32_t width, uint32_t height);
    void DestroyTarget();

    RHI* m_Rhi {nullptr};
    bool m_Ready {false};

    RHISampler* m_Sampler {nullptr};
    RHIBuffer* m_ParamsBuffer {nullptr};
    RHIDeviceMemory* m_ParamsMemory {nullptr};

    RHIRenderPass* m_RenderPass {nullptr};
    // The RHI has no DestroyPipeline / DestroyPipelineLayout / DestroyDescriptorSetLayout
    // (the same known gap as NaniteRenderer): these live until process exit.
    RHIDescriptorSetLayout* m_SetLayout {nullptr};
    RHIPipelineLayout* m_PipelineLayout {nullptr};
    RHIPipeline* m_Pipeline {nullptr};
    RHIDescriptorSet* m_DescriptorSet {nullptr};

    // Output target + readback buffer, rebuilt when the size changes.
    RHIImage* m_TargetImage {nullptr};
    RHIDeviceMemory* m_TargetMemory {nullptr};
    RHIImageView* m_TargetView {nullptr};
    RHIFramebuffer* m_Framebuffer {nullptr};
    RHIBuffer* m_ReadbackBuffer {nullptr};
    RHIDeviceMemory* m_ReadbackMemory {nullptr};
    uint32_t m_TargetWidth {0};
    uint32_t m_TargetHeight {0};
};
