// =====================================================================
// PR-V3: VulkanBindlessTextureBlitPipeline
// ---------------------------------------------------------------------
// Vulkan sibling of dx12/utility/bindless_texture_blit_pipeline.{h,cpp}.
// First production-path consumer of the Vulkan bindless toolchain
// validated by the PR-V2 compile-time smoke test.
//
// Responsibility:
//   Build, once, the smallest possible RHI graphics pipeline that draws
//   a fullscreen triangle and samples one bindless texture from the
//   global bindless descriptor table owned by
//   VulkanBindlessTextureManager. Caller is responsible for:
//     - Owning an off-screen R8G8B8A8 RHIRenderPass + RHIFramebuffer to
//       render into.
//     - Having previously called RHIBindlessTextureManager::allocate()
//       to register the source texture's image view + sampler and to
//       obtain a slot.
//     - Calling RecordBlit() inside an active frame, inside an active
//       render pass on `command_buffer`.
//
// Why a dedicated class instead of a `RenderPass` subclass:
//   Same rationale as the DX12 sibling -- ZEngine's `RenderPass` base
//   hard-couples to the main pipeline's framebuffer lifecycle. PR-V3
//   wants to drive editor-side previews / a runtime smoke test that
//   have no business inside that pipeline.
//
// Differences from the DX12 sibling (intentional):
//   - DX12 reads its texture through SM 6.6's
//     `ResourceDescriptorHeap[NonUniformResourceIndex(idx)]` and a
//     4-entry static-sampler bank baked into the root signature. The
//     C++ side just declares one binding with
//     RHI_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT and lets
//     DX12RHI::CreatePipelineLayout (PR6) emit the rest.
//   - Vulkan has no equivalent "auto descriptor heap". The bindless
//     texture array IS a real descriptor set living inside
//     VulkanBindlessTextureManager (set=0, binding=0,
//     COMBINED_IMAGE_SAMPLER, count=capacity, UPDATE_AFTER_BIND |
//     PARTIALLY_BOUND | VARIABLE_DESCRIPTOR_COUNT). Pipelines that
//     consume the bindless path MUST:
//       a) build their pipeline layout from THAT exact
//          VkDescriptorSetLayout (manager->getDescriptorSetLayout()),
//          NOT a synthesised RHI marker layout -- otherwise the
//          allocated VkDescriptorSet is layout-incompatible at bind
//          time;
//       b) bind manager->GetDescriptorSet() at set 0 before drawing;
//       c) declare a 4-byte VK_SHADER_STAGE_ALL push-constant range at
//          offset 0, fed by VulkanRHI::cmdSetBindlessIndexPFN.
//   - Sampler index (high 16 bits of BindlessIndex::Pack) is IGNORED
//     in Vulkan. The COMBINED_IMAGE_SAMPLER slot already carries the
//     sampler that was passed to `manager->allocate(view, sampler)`,
//     so per-slot sampler choice is fixed at registration time. The
//     packed key shape stays cross-backend identical so the same
//     32-bit payload can flow through either backend; the sampler
//     half is reserved for a future SAMPLED_IMAGE + separate sampler
//     bank scheme if it ever lands.
//
// API contract:
//   - Initialize() builds layout + pipeline once. Idempotent: a
//     second call with isReady() == true is a no-op.
//   - Shutdown() must be called before the owning RHI is destroyed.
//   - RecordBlit() is called inside the per-frame editor / smoke-test
//     tick, AFTER `cmdBeginRenderPassPFN` on the same render pass
//     passed to Initialize() has been called on `command_buffer`.
//
// Backend availability:
//   - PR-V3 lights up Vulkan only. On any other backend isReady()
//     returns false after Initialize() (mirrors PR7's DX12-only gate).
// =====================================================================

#pragma once

#include "Runtime/Function/Render/Interface/RHI.h"
#include "Runtime/Function/Render/Interface/RHIStruct.h"

#include <cstdint>
#include <string>

class VulkanRHI;

class VulkanBindlessTextureBlitPipeline
{
public:
    VulkanBindlessTextureBlitPipeline() = default;
    ~VulkanBindlessTextureBlitPipeline() = default;

    VulkanBindlessTextureBlitPipeline(const VulkanBindlessTextureBlitPipeline&) = delete;
    VulkanBindlessTextureBlitPipeline& operator=(const VulkanBindlessTextureBlitPipeline&) = delete;

    // Build the pipeline-layout-from-bindless-set + graphics pipeline.
    // The caller-supplied `target_render_pass` MUST describe a single
    // R8G8B8A8_UNORM color attachment, no depth, no MSAA -- the
    // pipeline state is hard-tuned to that.
    //
    // Preconditions:
    //   - rhi != nullptr AND dynamic_cast<VulkanRHI*>(rhi) != nullptr;
    //   - rhi->supportsBindlessTextures() returns true (otherwise the
    //     manager is null and there is nothing to bind);
    //   - target_render_pass != nullptr.
    //
    // Returns true on success; false on any RHI / shader-compile
    // failure, in which case isReady() stays false and RecordBlit()
    // is a no-op.
    bool Initialize(RHI* rhi, RHIRenderPass* target_render_pass);

    // Release every RHI-owned object. Safe to call multiple times.
    // After Shutdown() isReady() returns false.
    void Shutdown();

    // True iff Initialize() succeeded and Shutdown() has not been
    // called since.
    bool isReady() const { return m_Ready; }

    // Record one fullscreen-triangle drawcall into `command_buffer`.
    // PRECONDITIONS:
    //   - cmdBeginRenderPassPFN on the same render pass passed to
    //     Initialize() has been called on `command_buffer`.
    //   - `bindless_texture_index` was returned by
    //     manager->allocate() against the same RHI.
    //
    // Side effects:
    //   - Binds this pipeline,
    //   - sets viewport+scissor to (0,0,w,h),
    //   - binds manager's descriptor set at set 0,
    //   - pushes the packed bindless index via cmdSetBindlessIndexPFN,
    //   - issues CmdDraw(3, 1, 0, 0).
    //
    // Caller is responsible for the matching cmdEndRenderPassPFN.
    //
    // Note: the `sampler` argument exists for ABI symmetry with the
    // DX12 sibling. On Vulkan the high 16 bits of the packed index are
    // NOT consulted by the shader -- the slot's sampler was bound at
    // manager->allocate() time. We still pack it so RenderDoc captures
    // and any future cross-backend log line shows the same packed key.
    void RecordBlit(RHICommandBuffer* command_buffer,
                    uint32_t viewport_width,
                    uint32_t viewport_height,
                    uint32_t bindless_texture_index,
                    uint32_t sampler_index = 0) const;

    // Accessors mostly useful for tests / RenderDoc captures.
    RHIPipeline* GetPipeline() const { return m_Pipeline; }
    RHIPipelineLayout* getPipelineLayout() const { return m_PipelineLayout; }

private:
    bool CompileShaders(RHIShader*& out_vs, RHIShader*& out_ps) const;
    bool BuildPipelineLayout();
    bool BuildPipeline(RHIRenderPass* render_pass, RHIShader* vs, RHIShader* ps);

    RHI* m_Rhi = nullptr;
    VulkanRHI* m_VkRhi = nullptr;
    // PR-V3: we deliberately do NOT own the descriptor-set layout.
    // The set we want to bind lives inside VulkanBindlessTextureManager
    // and was allocated against THAT layout; reusing the same
    // VkDescriptorSetLayout in the pipeline layout is the only way the
    // bind point stays compatible. We keep a lightweight RHI wrapper
    // around it so RHIPipelineLayoutCreateInfo can reference it like
    // any other descriptor-set layout.
    RHIDescriptorSetLayout* m_SetLayoutView = nullptr;  // non-owning RHI wrapper
    RHIPipelineLayout* m_PipelineLayout = nullptr;
    RHIPipeline* m_Pipeline = nullptr;
    bool m_Ready = false;
};
