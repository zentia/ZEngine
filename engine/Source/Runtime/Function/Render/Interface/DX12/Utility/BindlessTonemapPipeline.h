// =====================================================================
// PR-DX2: DX12BindlessTonemapPipeline
// ---------------------------------------------------------------------
// DX12-side bindless tone-mapping graphics pipeline. Sibling of
// `BindlessTextureBlitPipeline` (PR7) -- same descriptor-set layout,
// same pipeline-layout shape, same fullscreen-triangle VS, same
// lifecycle contract. Only the pixel shader differs (this class applies
// the Uncharted2 curve + Gamma 2.2 correction; the blit pipeline just
// samples through).
//
// Why a dedicated class instead of a `RenderPass` subclass:
//   Same rationale as the bindless blit sibling -- ZEngine's
//   `RenderPass` base hard-couples to the main_camera framebuffer
//   lifecycle. The bindless tonemap is a framebuffer-agnostic utility
//   that a future standalone tonemap pass will drive.
//
// Why this lands as a pure-additive utility (no consumer wired):
//   The DX12 editor currently does not have a tonemap pass -- the
//   editor relies on the skybox's in-shader Reinhard + Gamma 2.2 and
//   ImGui's gamma-aware path. When a DX12 host needs a runtime tonemap
//   (e.g. swapchain change to RGB10A2 / FP16), this pipeline will be
//   the ready-made consumer. Landing it now closes the DX12/Vulkan
//   parity gap on the `BindlessTonemapPipeline` row.
//
// API contract:
//   - Initialize() builds layout + pipeline once. Idempotent.
//   - Shutdown() releases the RHI-owned objects. Safe to call
//     multiple times.
//   - RecordTonemap() is called inside an active render pass on
//     `command_buffer`. Caller owns the render pass / framebuffer.
//
// Backend availability:
//   PR-DX2 lights up DX12 only. dynamic_cast<DX12RHI*> is the runtime
//   gate; on Vulkan / Metal / WebGL2 the call returns false and the
//   pipeline stays in its "not ready" state. Vulkan parity is provided
//   by `VulkanBindlessTonemapPipeline` (PR-V4 part 1).
//
// Cross-references:
//   - sibling: dx12/utility/bindless_texture_blit_pipeline.{h,cpp}
//   - Vulkan sibling: vulkan/utility/bindless_tonemap_pipeline.{h,cpp}
//   - shader: dx12/utility/shaders/bindless_tonemap_ps.hlsl
//   - vertex shader: re-uses bindless_blit_vs.hlsl (same fullscreen-
//     triangle VS as the blit sibling).
// =====================================================================

#pragma once

#include "Runtime/Function/Render/Interface/DX12/Utility/BindlessTextureBlitPipeline.h"
#include "Runtime/Function/Render/Interface/RHI.h"
#include "Runtime/Function/Render/Interface/RHIStruct.h"

#include <cstdint>
#include <string>

class DX12BindlessTonemapPipeline
{
public:
    DX12BindlessTonemapPipeline() = default;
    ~DX12BindlessTonemapPipeline() = default;

    DX12BindlessTonemapPipeline(const DX12BindlessTonemapPipeline&) = delete;
    DX12BindlessTonemapPipeline& operator=(const DX12BindlessTonemapPipeline&) = delete;

    // Build the descriptor-set layout, pipeline layout, and graphics
    // pipeline. The caller-supplied `target_render_pass` MUST describe
    // a single R8G8B8A8_UNORM (or any 8-bit unorm) color attachment,
    // no depth, no MSAA -- the pipeline state is hard-tuned to that
    // shape, same as the blit sibling.
    //
    // hlsl_search_root: directory containing bindless_blit_vs.hlsl /
    // bindless_tonemap_ps.hlsl. Empty string -> the in-engine default
    // at `<engine>/source/Runtime/Function/Render/Interface/dx12/
    // utility/shaders`, resolved by walking up from the working
    // directory. Pass an explicit path for tests that don't run with
    // the engine layout intact.
    //
    // Preconditions:
    //   - rhi != nullptr AND dynamic_cast<DX12RHI*>(rhi) != nullptr;
    //   - rhi->supportsBindlessTextures() returns true;
    //   - target_render_pass != nullptr.
    //
    // Returns true on success; false on any RHI / shader-compile
    // failure, in which case isReady() stays false and RecordTonemap()
    // is a no-op.
    bool Initialize(RHI* rhi,
                    RHIRenderPass* target_render_pass,
                    const std::string& hlsl_search_root = {});

    // Release every RHI-owned object. Safe to call multiple times.
    // After Shutdown() isReady() returns false.
    void Shutdown();

    // True iff Initialize() succeeded and Shutdown() has not been
    // called since.
    bool isReady() const { return m_Ready; }

    // Record one fullscreen-triangle tonemap drawcall into
    // `command_buffer`.
    //
    // PRECONDITIONS:
    //   - RHI::beginFrame has been called this frame.
    //   - cmdBeginRenderPass on the same render pass passed to
    //     Initialize() has been called on `command_buffer`.
    //   - `bindless_texture_index` was returned by the bindless
    //     manager's allocate() against the same RHI, with the source
    //     HDR image's view.
    //
    // Side effects (identical shape to the blit sibling -- caller
    // glue can be reused 1:1):
    //   - Binds this pipeline,
    //   - sets viewport + scissor to (0,0,w,h),
    //   - pushes the packed bindless index via cmdSetBindlessIndexPFN,
    //   - issues CmdDraw(3, 1, 0, 0).
    //
    // Caller is responsible for the matching cmdEndRenderPass.
    void RecordTonemap(RHICommandBuffer* command_buffer,
                       uint32_t viewport_width,
                       uint32_t viewport_height,
                       uint32_t bindless_texture_index,
                       BindlessBlitSampler sampler = BindlessBlitSampler::LinearClamp) const;

    // Accessors mostly useful for tests / RenderDoc captures.
    RHIPipeline* GetPipeline() const { return m_Pipeline; }
    RHIPipelineLayout* getPipelineLayout() const { return m_PipelineLayout; }
    RHIDescriptorSetLayout* getDescriptorSetLayout() const { return m_SetLayout; }

private:
    bool CompileShaders(const std::string& hlsl_search_root,
                        RHIShader*& out_vs,
                        RHIShader*& out_ps) const;

    bool BuildDescriptorSetLayout();
    bool BuildPipelineLayout();
    bool BuildPipeline(RHIRenderPass* render_pass, RHIShader* vs, RHIShader* ps);

    RHI* m_Rhi = nullptr;
    RHIDescriptorSetLayout* m_SetLayout = nullptr;
    RHIPipelineLayout* m_PipelineLayout = nullptr;
    RHIPipeline* m_Pipeline = nullptr;
    bool m_Ready = false;
};
