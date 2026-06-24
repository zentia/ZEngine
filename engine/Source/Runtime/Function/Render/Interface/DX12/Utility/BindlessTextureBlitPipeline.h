// =====================================================================
// PR7: BindlessTextureBlitPipeline
// ---------------------------------------------------------------------
// First production-path consumer of the bindless toolchain validated
// in PR5c's smoke-test.
//
// Responsibility:
//   Build, once, the smallest possible RHI graphics pipeline that
//   draws a fullscreen triangle and samples one bindless texture from
//   the engine's global bindless CBV/SRV/UAV heap (managed by
//   BindlessTextureManager). Caller is responsible for:
//     - Owning an off-screen R8G8B8A8 RHIRenderPass + RHIFramebuffer
//       to render into.
//     - Having previously called BindlessTextureManager::allocate()
//       to register the source texture's view and obtain a slot.
//     - Calling RecordBlit() inside an active frame (between
//       RHI::BeginFrame and RHI::EndFrame), inside an active render
//       pass, with the framebuffer's color RT bound.
//
// Why a dedicated class instead of a `RenderPass` subclass:
//   ZEngine's `RenderPass` base hard-couples to the main render
//   pipeline's framebuffer lifecycle (m_Framebuffer.render_pass /
//   m_DescriptorInfos / etc., see fxaa_pass.cpp). PR7 wants to drive
//   editor-side previews that have no business inside that pipeline.
//   So this class stays free-standing -- it is to RenderPass what an
//   immediate-mode draw helper is to a deferred renderer.
//
// API contract:
//   - Initialize() builds layout + pipeline once. Idempotent: a second
//     call with isReady() == true is a no-op.
//   - Shutdown() must be called before the owning RHI is destroyed.
//   - RecordBlit() is called inside the per-frame editor tick, AFTER
//     RHI::BeginFrame and an outer cmdBeginRenderPass on the caller's
//     framebuffer (the caller knows the RT's clear color / load-op
//     semantics; this class doesn't).
//
// Bindless semantics:
//   The pipeline's only descriptor-set layout binding is declared with
//   RHI_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT. That is what
//   drives DX12RHI::CreatePipelineLayout (PR6+PR7) into:
//     - reserving a 32-bit root constant at b0/space0 for the packed
//       bindless index;
//     - setting CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED on the root sig;
//     - emitting the 4-entry static-sampler bank s0..s3
//       (LinearWrap / LinearClamp / PointWrap / PointClamp).
//   The HLSL side (bindless_blit_ps.hlsl) reads the packed index via
//   `BindlessIndex::unpack*` and samples
//   `ResourceDescriptorHeap[NonUniformResourceIndex(texture_index)]`.
//
// Backend support:
//   PR7 lights up DX12 only. On Vulkan, isReady() returns false after
//   Initialize() because BindlessTextureManager's Vulkan path predates
//   this pipeline's wiring. (PR8 is on the hook for Vulkan parity.)
// =====================================================================

#pragma once

#include "Runtime/Function/Render/Interface/RHI.h"
#include "Runtime/Function/Render/Interface/RHIStruct.h"

#include <cstdint>
#include <string>

// Sampler indices into the bindless static-sampler bank baked into
// every bindless-aware root signature by DX12RHI::createPipelineLayout.
// Order MUST match dx12_rhi.cpp's `bindless_static_samplers` AND
// bindless_blit_ps.hlsl's `g_*_*` declarations.
enum class BindlessBlitSampler : uint32_t
{
    LinearWrap = 0,
    LinearClamp = 1,
    PointWrap = 2,
    PointClamp = 3,
};

class BindlessTextureBlitPipeline
{
public:
    BindlessTextureBlitPipeline() = default;
    ~BindlessTextureBlitPipeline() = default;

    BindlessTextureBlitPipeline(const BindlessTextureBlitPipeline&) = delete;
    BindlessTextureBlitPipeline& operator=(const BindlessTextureBlitPipeline&) = delete;

    // Build the descriptor-set layout, pipeline layout, and graphics
    // pipeline. The caller-supplied `target_render_pass` MUST describe
    // a single R8G8B8A8_UNORM color attachment, no depth, no MSAA --
    // the pipeline state is hard-tuned to that. Returns true on
    // success, false on any RHI / shader-compile failure (in which
    // case isReady() stays false and RecordBlit() is a no-op).
    //
    // hlsl_search_root: directory containing bindless_blit_vs.hlsl /
    // bindless_blit_ps.hlsl. Empty string -> the in-engine default at
    // `<engine>/source/Runtime/Function/Render/Interface/dx12/utility/shaders`,
    // resolved by walking up from the working directory. Pass an
    // explicit path for tests that don't run with the engine layout
    // intact.
    bool Initialize(RHI* rhi,
                    RHIRenderPass* target_render_pass,
                    const std::string& hlsl_search_root = {});

    // Release every RHI-owned object. Safe to call multiple times.
    // After Shutdown() isReady() returns false.
    void Shutdown();

    // True iff Initialize() succeeded and Shutdown() has not been
    // called since.
    bool isReady() const { return m_Ready; }

    // Record one fullscreen-triangle drawcall into `command_buffer`.
    // PRECONDITIONS:
    //   - RHI::BeginFrame has been called this frame.
    //   - cmdBeginRenderPass on the same render pass passed to
    //     Initialize() has been called on `command_buffer`.
    //   - `bindless_texture_index` was returned by
    //     BindlessTextureManager::allocate() against the same RHI.
    //
    // Side effects:
    //   - Binds this pipeline,
    //   - sets viewport+scissor to (0,0,w,h),
    //   - pushes the packed bindless index via cmdSetBindlessIndexPFN,
    //   - issues CmdDraw(3, 1, 0, 0).
    //
    // Caller is responsible for the matching cmdEndRenderPass.
    void RecordBlit(RHICommandBuffer* command_buffer,
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
