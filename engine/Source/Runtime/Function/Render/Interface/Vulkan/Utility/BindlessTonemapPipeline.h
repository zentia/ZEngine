// =====================================================================
// PR-V4 part 1: VulkanBindlessTonemapPipeline
// ---------------------------------------------------------------------
// Vulkan-side bindless tone-mapping graphics pipeline. Sibling of
// `VulkanBindlessTextureBlitPipeline` (PR-V3) -- same set / binding /
// push-constant shape, same fullscreen-triangle VS, same lifecycle
// contract. Only the fragment shader differs (`tone_mapping_bindless.frag`
// applies the Uncharted2 curve + Gamma 2.2 correction; the blit shader
// just samples through).
//
// Why a dedicated pipeline class instead of a `RenderPass` subclass:
//   Same rationale as the bindless blit sibling -- ZEngine's
//   `RenderPass` base hard-couples to the main_camera framebuffer
//   lifecycle (FrameBuffer / attachments / per-frame layout
//   transitions). The bindless tonemap will eventually live in a
//   standalone render pass owned by a future `StandaloneToneMappingPass`
//   (PR-V4 part 2), which itself will hold its own framebuffer and
//   render-pass; this class is the framebuffer-agnostic core that
//   the future standalone pass will drive.
//
// Why this lands BEFORE the standalone tonemap pass:
//   The main_camera render pass (`main_camera_pass.cpp`) currently runs
//   tonemap as the 4th of 8 subpasses, with `backup_odd` as a
//   `subpassInput` and `backup_even` as the color attachment. Migrating
//   tonemap to bindless requires lifting it out of that subpass --
//   which forces splitting main_camera into two render passes around
//   the standalone tonemap (so `backup_even` can be `LOAD`'ed by the
//   later color_grading subpass with `initialLayout = SHADER_READ_ONLY`).
//   That split touches subpass-index references in
//   `color_grading_pass.cpp`, `fxaa_pass.cpp`, `combine_ui_pass.cpp`,
//   the `_main_camera_subpass_*` enum in `render_pass.h`, dependency
//   masks, and per-swapchain-image framebuffer setup -- a non-trivial
//   topology change that needs Adreno / Mali real-device regression
//   coverage before it touches the default render path.
//
//   Landing the bindless pipeline class FIRST as a pure-additive
//   utility (no consumer wired) means the future "split main_camera"
//   PR can focus exclusively on the topology change, with the
//   bindless side already validated by a link-and-shape smoke test.
//   Same staging strategy PR-V3 used (PR-V3 part 1 was the pipeline,
//   PR-V3 part 2 was the editor-side consumer).
//
// API contract:
//   - Initialize() builds layout + pipeline once. Idempotent.
//   - Shutdown() releases the wrapper objects (VkPipeline /
//     VkPipelineLayout are pooled by VulkanRHI, not destroyed
//     individually, mirroring the blit sibling's shutdown semantics).
//   - RecordTonemap() is called inside an active render pass on
//     `command_buffer`. Caller owns the render pass / framebuffer.
//
// Backend availability:
//   - PR-V4 part 1 lights up Vulkan only. dynamic_cast<VulkanRHI*> is
//     the runtime gate; on DX12 / Metal / WebGL2 the call returns
//     false and the pipeline stays in its "not ready" state.
//
// Cross-references:
//   - sibling: vulkan/utility/bindless_texture_blit_pipeline.{h,cpp}
//   - shader: vulkan/utility/shaders/tone_mapping_bindless.frag
//   - vertex shader: re-uses the same fullscreen-triangle VS as the
//     blit sibling (`bindless_blit.vert` / inlined `k_bindless_blit_vert`).
// =====================================================================

#pragma once

#include "Runtime/Function/Render/Interface/RHI.h"
#include "Runtime/Function/Render/Interface/RHIStruct.h"

#include <cstdint>

class VulkanRHI;

class VulkanBindlessTonemapPipeline
{
public:
    VulkanBindlessTonemapPipeline() = default;
    ~VulkanBindlessTonemapPipeline() = default;

    VulkanBindlessTonemapPipeline(const VulkanBindlessTonemapPipeline&) = delete;
    VulkanBindlessTonemapPipeline& operator=(const VulkanBindlessTonemapPipeline&) = delete;

    // Build the pipeline-layout-from-bindless-set + graphics pipeline.
    // The caller-supplied `target_render_pass` MUST describe a single
    // R8G8B8A8_UNORM (or any 8-bit unorm) color attachment, no depth,
    // no MSAA -- the pipeline state is hard-tuned to that shape, same
    // as the blit sibling.
    //
    // Preconditions:
    //   - rhi != nullptr AND dynamic_cast<VulkanRHI*>(rhi) != nullptr;
    //   - rhi->supportsBindlessTextures() returns true;
    //   - target_render_pass != nullptr.
    //
    // Returns true on success; false on any RHI / shader-compile
    // failure, in which case isReady() stays false and RecordTonemap()
    // is a no-op.
    bool Initialize(RHI* rhi, RHIRenderPass* target_render_pass);

    // Release every RHI-owned object reference. Safe to call multiple
    // times. After Shutdown() isReady() returns false.
    void Shutdown();

    // True iff Initialize() succeeded and Shutdown() has not been
    // called since.
    bool isReady() const { return m_Ready; }

    // Record one fullscreen-triangle tonemap drawcall into
    // `command_buffer`.
    //
    // PRECONDITIONS:
    //   - cmdBeginRenderPassPFN on the same render pass passed to
    //     Initialize() has been called on `command_buffer`.
    //   - `bindless_texture_index` was returned by the bindless
    //     manager's allocate() against the same RHI, with the source
    //     HDR image's view + sampler.
    //
    // Side effects (identical shape to the blit sibling -- caller
    // glue can be reused 1:1):
    //   - Binds this pipeline,
    //   - sets viewport + scissor to (0,0,w,h),
    //   - binds the manager's bindless descriptor set at set 0,
    //   - pushes the packed bindless index via cmdSetBindlessIndexPFN,
    //   - issues CmdDraw(3, 1, 0, 0).
    //
    // Caller is responsible for the matching cmdEndRenderPassPFN.
    //
    // The `sampler_index` argument exists for ABI symmetry with the
    // blit sibling and the DX12 path; on Vulkan the high 16 bits are
    // NOT consulted by the shader (the slot's sampler is fixed at
    // allocate() time). The packed key still flows through so
    // RenderDoc captures and any future cross-backend log line shows
    // the same packed value.
    void RecordTonemap(RHICommandBuffer* command_buffer,
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

    // Non-owning wrapper around the bindless manager's
    // VkDescriptorSetLayout. Same lifecycle rules as the blit sibling:
    // the wrapper is heap-allocated by BuildPipelineLayout() so that
    // RHIPipelineLayoutCreateInfo can reference it; deleting the
    // wrapper does NOT destroy the underlying VkDescriptorSetLayout
    // (the manager still owns and re-uses it).
    RHIDescriptorSetLayout* m_SetLayoutView = nullptr;
    RHIPipelineLayout* m_PipelineLayout = nullptr;
    RHIPipeline* m_Pipeline = nullptr;
    bool m_Ready = false;
};
