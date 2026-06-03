// =====================================================================
// PR-V3: VulkanBindlessTextureBlitPipeline implementation
// ---------------------------------------------------------------------
// Vulkan sibling of dx12/utility/bindless_texture_blit_pipeline.cpp.
// First production-path consumer of the Vulkan bindless toolchain.
// See bindless_texture_blit_pipeline.h for the full API contract; this
// TU is the Vulkan-only implementation. The `vulkan/utility/` location
// matches the DX12 sibling 1:1 so the cross-backend symmetry is
// obvious in the file tree.
//
// Key implementation notes (kept in the .cpp because they're
// implementation-specific and would otherwise pollute the public
// header):
//
//   1. Why we DO NOT call createDescriptorSetLayout ourselves.
//      The descriptor set we want to bind was allocated by
//      VulkanBindlessTextureManager AGAINST a specific
//      VkDescriptorSetLayout (with binding flags
//      UPDATE_AFTER_BIND | PARTIALLY_BOUND |
//      VARIABLE_DESCRIPTOR_COUNT). vkCmdBindDescriptorSets only
//      accepts a set if the pipeline layout's set-layout slot at the
//      same index is COMPATIBLE -- and "compatible" in Vulkan means
//      the set-layout objects either match by identity OR were
//      created with identical bindings (including flags and counts).
//      The cheapest, surest path is to reuse the manager's actual
//      VkDescriptorSetLayout. We wrap it in a fresh
//      VulkanDescriptorSetLayout so RHIPipelineLayoutCreateInfo can
//      reference it via its existing pointer field; the wrapper is
//      non-owning -- destroying it must NOT destroy the underlying
//      VkDescriptorSetLayout (still owned by the manager).
//
//   2. Why we explicitly publish the bindless push-constant range.
//      DX12RHI::CreatePipelineLayout (PR6) auto-injects the 32-bit
//      root constant when it sees
//      RHI_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT on a
//      binding. VulkanRHI::CreatePipelineLayout has no such auto-
//      detection -- it is a pure passthrough that copies whatever
//      pPushConstantRanges the caller supplies. So we MUST hand it
//      VulkanBindlessTextureManager::GetBindlessPushConstantRange()
//      explicitly, otherwise vkCmdPushConstants in
//      cmdSetBindlessIndexPFN fires VUID-vkCmdPushConstants-offset
//      on the very first draw.
//
//   3. recordBlit MUST bind the manager's descriptor set.
//      DX12 doesn't need this step (its bindless table lives in
//      cmdBindDescriptorSetsPFN's auto-bound CBV/SRV/UAV heap). On
//      Vulkan the descriptor set is a real object that has to be
//      explicitly bound to the pipeline layout's set 0. Without this
//      bind the shader's `u_bindless[idx]` resolves to UNBOUND and
//      validation fires VUID-vkCmdDraw-None-08114.
//
// Backend availability:
//   - PR-V3 lights up Vulkan only. dynamic_cast<VulkanRHI*> is the
//     runtime gate; on DX12 / Metal / WebGL2 the call returns false
//     and the pipeline stays in its "not ready" state (mirrors PR7's
//     DX12-only gate).
// =====================================================================

#include "Runtime/Function/Render/Interface/Vulkan/Utility/BindlessTextureBlitPipeline.h"

#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Function/Render/Interface/Vulkan/ShaderCompiler.h"
#include "Runtime/Function/Render/Interface/Vulkan/VulkanBindlessTextureManager.h"
#include "Runtime/Function/Render/Interface/Vulkan/VulkanRHI.h"
#include "Runtime/Function/Render/Interface/Vulkan/VulkanRHIResource.h"
#include "Runtime/Function/Render/RenderType.h"

namespace
{
    // GLSL sources kept INLINE rather than read from disk. Rationale
    // (deliberately different from the DX12 sibling, which probes the
    // file system for bindless_blit_*.hlsl):
    //
    //   - The DX12 sibling has to live with HLSL files because
    //     DX12ShaderCompiler currently only exposes a file-input API.
    //     glslang -- the Vulkan compiler ZRuntime already links --
    //     accepts source strings directly (see
    //     ShaderCompiler::CompileFromSource), no file I/O needed.
    //   - Inlining the shader removes a runtime dependency on the
    //     engine source tree being present at editor / smoke-test
    //     run-time; the smoke-test in particular is meant to work as
    //     a standalone executable copied anywhere on disk.
    //   - Both shaders are < 60 lines of GLSL each. The cost in TU
    //     size is trivial; the cost in deployment robustness is high.
    //
    // The on-disk .vert / .frag files at vulkan/utility/shaders/ are
    // kept as the human-edited source of truth (and for IDE syntax
    // highlighting) and MUST be kept in sync with the literals below.
    // A future refactor can move them to a generated .h via a
    // bin-to-c step, the same way the engine/shader CMakeLists.txt
    // does for the main pipeline shaders.
    constexpr const char* k_bindless_blit_vert = R"(#version 450
#extension GL_ARB_separate_shader_objects : enable

layout(location = 0) out vec2 v_uv;

void main()
{
    vec2 uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    v_uv = uv;
    gl_Position = vec4(uv * vec2(2.0, -2.0) + vec2(-1.0, 1.0), 0.0, 1.0);
}
)";

    constexpr const char* k_bindless_blit_frag = R"(#version 450
#extension GL_ARB_separate_shader_objects : enable
#extension GL_EXT_nonuniform_qualifier     : require

layout(set = 0, binding = 0) uniform sampler2D u_bindless[];

layout(push_constant) uniform BindlessPush
{
    uint g_packed_indices;
} u_push;

layout(location = 0) in  vec2 v_uv;
layout(location = 0) out vec4 o_color;

void main()
{
    uint texture_index = u_push.g_packed_indices & 0xFFFFu;
    o_color = texture(u_bindless[nonuniformEXT(texture_index)], v_uv);
}
)";
}  // namespace

bool VulkanBindlessTextureBlitPipeline::Initialize(RHI* rhi, RHIRenderPass* target_render_pass)
{
    if (m_Ready)
    {
        return true;
    }
    if (rhi == nullptr || target_render_pass == nullptr)
    {
        LOG_ERROR(ZRender, "VulkanBindlessTextureBlitPipeline::initialize: rhi or render_pass is null");
        return false;
    }

    // PR-V3 lights up Vulkan only. dynamic_cast keeps the runtime check
    // explicit -- mirrors PR7's DX12-only probe.
    VulkanRHI* vk_rhi = dynamic_cast<VulkanRHI*>(rhi);
    if (vk_rhi == nullptr)
    {
        LOG_WARNING(ZRender,
                    "VulkanBindlessTextureBlitPipeline currently supports Vulkan only; "
                    "current RHI is not a VulkanRHI. Initialization skipped.");
        return false;
    }
    if (!vk_rhi->supportsBindlessTextures())
    {
        LOG_WARNING(ZRender,
                    "VulkanBindlessTextureBlitPipeline: VulkanRHI does not support bindless "
                    "(VK_EXT_descriptor_indexing missing or required feature bits absent). "
                    "Initialization skipped.");
        return false;
    }
    RHIBindlessTextureManager* mgr = vk_rhi->getBindlessTextureManager();
    if (mgr == nullptr)
    {
        LOG_ERROR(ZRender,
                  "VulkanBindlessTextureBlitPipeline: supportsBindlessTextures() returned true "
                  "but getBindlessTextureManager() returned null -- inconsistent RHI state.");
        return false;
    }

    m_Rhi = rhi;
    m_VkRhi = vk_rhi;

    RHIShader* vs = nullptr;
    RHIShader* ps = nullptr;
    if (!CompileShaders(vs, ps))
    {
        m_Rhi = nullptr;
        m_VkRhi = nullptr;
        return false;
    }

    if (!BuildPipelineLayout())
    {
        m_Rhi->DestroyShaderModule(vs);
        m_Rhi->DestroyShaderModule(ps);
        m_SetLayoutView = nullptr;
        m_Rhi = nullptr;
        m_VkRhi = nullptr;
        return false;
    }

    if (!BuildPipeline(target_render_pass, vs, ps))
    {
        m_Rhi->DestroyShaderModule(vs);
        m_Rhi->DestroyShaderModule(ps);
        // The set-layout view is non-owning; the wrapper struct itself
        // leaks until Shutdown(). delete is safe (the wrapper does NOT
        // hold the underlying VkDescriptorSetLayout) but we keep the
        // pointer for symmetry with the success path -- Shutdown()
        // does the cleanup.
        m_PipelineLayout = nullptr;
        m_Rhi = nullptr;
        m_VkRhi = nullptr;
        return false;
    }

    // RHI has internalised the shader bytecode into the pipeline; we
    // can release the staging shader-module objects.
    m_Rhi->DestroyShaderModule(vs);
    m_Rhi->DestroyShaderModule(ps);

    m_Ready = true;
    LOG_INFO(ZRender,
             "VulkanBindlessTextureBlitPipeline initialized "
             "(layout={}, pipeline={}, target render-pass={})",
             static_cast<void*>(m_PipelineLayout),
             static_cast<void*>(m_Pipeline),
             static_cast<void*>(target_render_pass));
    return true;
}

void VulkanBindlessTextureBlitPipeline::Shutdown()
{
    // The set-layout wrapper is non-owning (it just held a borrowed
    // VkDescriptorSetLayout from the manager); deleting the wrapper
    // does NOT call vkDestroyDescriptorSetLayout. We delete it here
    // because nothing else owns the heap allocation we made in
    // buildPipelineLayout.
    if (m_SetLayoutView != nullptr)
    {
        delete m_SetLayoutView;
        m_SetLayoutView = nullptr;
    }
    // VulkanRHI does not currently expose explicit destroy*() for
    // pipelines / pipeline layouts (its resources are pooled and
    // released at RHI teardown -- same gap noted in the DX12
    // sibling's Shutdown()). We just drop our pointers so isReady()
    // flips to false; a future Initialize() rebuilds from scratch.
    m_Pipeline = nullptr;
    m_PipelineLayout = nullptr;
    m_Rhi = nullptr;
    m_VkRhi = nullptr;
    m_Ready = false;
}

void VulkanBindlessTextureBlitPipeline::RecordBlit(RHICommandBuffer* command_buffer,
                                                   uint32_t viewport_width,
                                                   uint32_t viewport_height,
                                                   uint32_t bindless_texture_index,
                                                   uint32_t sampler_index) const
{
    if (!m_Ready || command_buffer == nullptr)
    {
        return;
    }
    if (bindless_texture_index == RHIBindlessTextureManager::kInvalidBindlessIndex)
    {
        return;
    }
    if (viewport_width == 0 || viewport_height == 0)
    {
        return;
    }

    RHIBindlessTextureManager* mgr = m_VkRhi->getBindlessTextureManager();
    if (mgr == nullptr)
    {
        return;  // shouldn't happen post-init; defence in depth
    }

    // Bind pipeline first -- viewport / scissor and the bindless push
    // constant are graphics-pipeline-relative state, and the descriptor
    // set bind below is implicitly anchored to the just-bound pipeline's
    // layout (we pass our own layout to cmdBindDescriptorSetsPFN, but
    // the validation layer is happier when bind-pipeline runs first).
    m_Rhi->CmdBindPipelinePFN(command_buffer, RHI_PIPELINE_BIND_POINT_GRAPHICS, m_Pipeline);

    RHIViewport viewport = {0.0f,
                            0.0f,
                            static_cast<float>(viewport_width),
                            static_cast<float>(viewport_height),
                            0.0f,
                            1.0f};
    RHIRect2D scissor = {0, 0, viewport_width, viewport_height};
    m_Rhi->CmdSetViewportPFN(command_buffer, 0, 1, &viewport);
    m_Rhi->CmdSetScissorPFN(command_buffer, 0, 1, &scissor);

    // -----------------------------------------------------------------
    // Bind the bindless descriptor set at set 0.
    //
    // The manager's set was allocated against the same
    // VkDescriptorSetLayout that BuildPipelineLayout() embedded into
    // m_PipelineLayout, so the layout-compatibility check inside
    // vkCmdBindDescriptorSets passes. Without this step the shader's
    // u_bindless[] array resolves to UNBOUND and the first draw fires
    // VUID-vkCmdDraw-None-08114.
    //
    // We bind via the cross-backend RHI entry point so the call
    // mirrors how a future material pipeline would do it; the wrapper
    // just calls vkCmdBindDescriptorSets internally with no dynamic
    // offsets.
    // -----------------------------------------------------------------
    auto* vk_mgr = static_cast<VulkanBindlessTextureManager*>(mgr);
    VkDescriptorSet vk_set = vk_mgr->GetDescriptorSet();
    if (vk_set == VK_NULL_HANDLE)
    {
        return;
    }
    // Build a transient RHI wrapper around the raw VkDescriptorSet.
    // The wrapper itself is stack-allocated -- we just need its address
    // for the cmdBindDescriptorSetsPFN parameter shape; the call does
    // not retain a reference past the underlying vkCmdBindDescriptorSets
    // call, so a stack lifetime is sufficient.
    VulkanDescriptorSet wrapped_set;
    wrapped_set.setResource(vk_set);
    RHIDescriptorSet* set_arr[1] = {&wrapped_set};
    m_Rhi->CmdBindDescriptorSetsPFN(command_buffer,
                                    RHI_PIPELINE_BIND_POINT_GRAPHICS,
                                    m_PipelineLayout,
                                    /*firstSet*/ VulkanBindlessTextureManager::kBindlessDescriptorSet,
                                    /*descriptorSetCount*/ 1,
                                    set_arr,
                                    /*dynamicOffsetCount*/ 0,
                                    /*pDynamicOffsets*/ nullptr);

    // Push the packed bindless index. cmdSetBindlessIndexPFN's
    // 'commandBuffer' is honoured on Vulkan (vkCmdPushConstants needs
    // the cmd buffer); 'pipelineBindPoint' is unused on Vulkan because
    // push constants are bind-point-agnostic in the Vulkan model.
    const uint32_t packed = BindlessIndex::Pack(bindless_texture_index, sampler_index);
    m_Rhi->CmdSetBindlessIndexPFN(command_buffer,
                                  RHI_PIPELINE_BIND_POINT_GRAPHICS,
                                  m_PipelineLayout,
                                  packed);

    // Fullscreen oversized triangle: 3 verts, 1 instance. Matches
    // bindless_blit.vert (gl_VertexIndex 0..2 -> NDC corners).
    m_Rhi->CmdDraw(command_buffer, 3, 1, 0, 0);
}

bool VulkanBindlessTextureBlitPipeline::CompileShaders(RHIShader*& out_vs,
                                                       RHIShader*& out_ps) const
{
    out_vs = nullptr;
    out_ps = nullptr;

    // Vulkan's createShaderModuleFromSource accepts the
    // shader_compiler.h `enum class ShaderStage` integer values, NOT
    // RHI_SHADER_STAGE_*_BIT bitflags. Keep the static_cast<int>(...)
    // pattern that main_camera_pass.cpp et al use so future readers
    // notice the convention.
    out_vs = m_Rhi->CreateShaderModuleFromSource(k_bindless_blit_vert,
                                                 ShaderStage::Vertex,
                                                 "bindless_blit.vert",
                                                 /*include_paths*/ {},
                                                 /*macros*/ {});
    if (out_vs == nullptr)
    {
        LOG_ERROR(ZRender, "VulkanBindlessTextureBlitPipeline: failed to compile bindless_blit.vert");
        return false;
    }

    out_ps = m_Rhi->CreateShaderModuleFromSource(k_bindless_blit_frag,
                                                 ShaderStage::Fragment,
                                                 "bindless_blit.frag",
                                                 /*include_paths*/ {},
                                                 /*macros*/ {});
    if (out_ps == nullptr)
    {
        LOG_ERROR(ZRender,
                  "VulkanBindlessTextureBlitPipeline: failed to compile bindless_blit.frag "
                  "(check that glslang reports VK_EXT_descriptor_indexing / "
                  "GL_EXT_nonuniform_qualifier support).");
        m_Rhi->DestroyShaderModule(out_vs);
        out_vs = nullptr;
        return false;
    }
    return true;
}

bool VulkanBindlessTextureBlitPipeline::BuildPipelineLayout()
{
    // Wrap the manager's existing VkDescriptorSetLayout in a fresh
    // RHI wrapper. NON-OWNING -- destroying the wrapper must not
    // destroy the underlying layout (which the manager still owns
    // and re-uses for its own VkDescriptorSet allocation).
    auto* vk_mgr = static_cast<VulkanBindlessTextureManager*>(m_VkRhi->getBindlessTextureManager());
    VkDescriptorSetLayout vk_set_layout = vk_mgr->getDescriptorSetLayout();
    if (vk_set_layout == VK_NULL_HANDLE)
    {
        LOG_ERROR(ZRender,
                  "VulkanBindlessTextureBlitPipeline: bindless manager has no "
                  "VkDescriptorSetLayout (manager init failed earlier?).");
        return false;
    }

    auto* layout_view = new VulkanDescriptorSetLayout();
    layout_view->setResource(vk_set_layout);
    m_SetLayoutView = layout_view;

    // Push-constant range is mandatory: VulkanRHI::CreatePipelineLayout
    // is a pure passthrough, no auto-injection. Without this declaration
    // the validation layer fires VUID-vkCmdPushConstants-offset on the
    // first cmdSetBindlessIndexPFN.
    constexpr RHIPushConstantRange k_bindless_range =
        VulkanBindlessTextureManager::GetBindlessPushConstantRange();

    RHIDescriptorSetLayout* set_layouts[1] = {m_SetLayoutView};
    RHIPipelineLayoutCreateInfo info = {};
    info.sType = RHI_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    info.setLayoutCount = 1;
    info.pSetLayouts = set_layouts;
    info.pushConstantRangeCount = 1;
    info.pPushConstantRanges = &k_bindless_range;

    if (m_Rhi->CreatePipelineLayout(&info, m_PipelineLayout) != RHI_SUCCESS ||
        m_PipelineLayout == nullptr)
    {
        LOG_ERROR(ZRender, "VulkanBindlessTextureBlitPipeline: createPipelineLayout failed");
        m_PipelineLayout = nullptr;
        return false;
    }
    return true;
}

bool VulkanBindlessTextureBlitPipeline::BuildPipeline(RHIRenderPass* render_pass,
                                                      RHIShader* vs,
                                                      RHIShader* ps)
{
    // Pipeline state mirrors the DX12 sibling 1:1: empty IA (VS is
    // SV_VertexID-driven), no depth/stencil, no MSAA, no blend, no
    // cull, viewport+scissor dynamic. Re-using the DX12 shape keeps
    // the visual output byte-identical between backends, which the
    // smoke-test pixel-equality check below relies on.
    RHIPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = RHI_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = RHI_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1].sType = RHI_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = RHI_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = ps;
    stages[1].pName = "main";

    RHIPipelineVertexInputStateCreateInfo vi = {};
    vi.sType = RHI_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount = 0;
    vi.pVertexBindingDescriptions = nullptr;
    vi.vertexAttributeDescriptionCount = 0;
    vi.pVertexAttributeDescriptions = nullptr;

    RHIPipelineInputAssemblyStateCreateInfo ia = {};
    ia.sType = RHI_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = RHI_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    ia.primitiveRestartEnable = RHI_FALSE;

    RHIPipelineViewportStateCreateInfo vp = {};
    vp.sType = RHI_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.pViewports = nullptr;
    vp.scissorCount = 1;
    vp.pScissors = nullptr;

    RHIPipelineRasterizationStateCreateInfo rs = {};
    rs.sType = RHI_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.depthClampEnable = RHI_FALSE;
    rs.rasterizerDiscardEnable = RHI_FALSE;
    rs.polygonMode = RHI_POLYGON_MODE_FILL;
    rs.lineWidth = 1.0f;
    rs.cullMode = RHI_CULL_MODE_NONE;
    rs.frontFace = RHI_FRONT_FACE_CLOCKWISE;
    rs.depthBiasEnable = RHI_FALSE;
    rs.depthBiasConstantFactor = 0.0f;
    rs.depthBiasClamp = 0.0f;
    rs.depthBiasSlopeFactor = 0.0f;

    RHIPipelineMultisampleStateCreateInfo ms = {};
    ms.sType = RHI_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.sampleShadingEnable = RHI_FALSE;
    ms.rasterizationSamples = RHI_SAMPLE_COUNT_1_BIT;

    RHIPipelineColorBlendAttachmentState blend = {};
    blend.colorWriteMask =
        RHI_COLOR_COMPONENT_R_BIT | RHI_COLOR_COMPONENT_G_BIT |
        RHI_COLOR_COMPONENT_B_BIT | RHI_COLOR_COMPONENT_A_BIT;
    blend.blendEnable = RHI_FALSE;
    blend.srcColorBlendFactor = RHI_BLEND_FACTOR_ONE;
    blend.dstColorBlendFactor = RHI_BLEND_FACTOR_ZERO;
    blend.colorBlendOp = RHI_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = RHI_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = RHI_BLEND_FACTOR_ZERO;
    blend.alphaBlendOp = RHI_BLEND_OP_ADD;

    RHIPipelineColorBlendStateCreateInfo cb = {};
    cb.sType = RHI_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.logicOpEnable = RHI_FALSE;
    cb.logicOp = RHI_LOGIC_OP_COPY;
    cb.attachmentCount = 1;
    cb.pAttachments = &blend;
    cb.blendConstants[0] = 0.0f;
    cb.blendConstants[1] = 0.0f;
    cb.blendConstants[2] = 0.0f;
    cb.blendConstants[3] = 0.0f;

    RHIPipelineDepthStencilStateCreateInfo ds = {};
    ds.sType = RHI_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = RHI_FALSE;
    ds.depthWriteEnable = RHI_FALSE;
    ds.depthCompareOp = RHI_COMPARE_OP_ALWAYS;
    ds.depthBoundsTestEnable = RHI_FALSE;
    ds.stencilTestEnable = RHI_FALSE;

    RHIDynamicState dyn_states[2] = {RHI_DYNAMIC_STATE_VIEWPORT, RHI_DYNAMIC_STATE_SCISSOR};
    RHIPipelineDynamicStateCreateInfo dyn = {};
    dyn.sType = RHI_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dyn_states;

    RHIGraphicsPipelineCreateInfo info = {};
    info.sType = RHI_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vi;
    info.pInputAssemblyState = &ia;
    info.pViewportState = &vp;
    info.pRasterizationState = &rs;
    info.pMultisampleState = &ms;
    info.pColorBlendState = &cb;
    info.pDepthStencilState = &ds;
    info.pDynamicState = &dyn;
    info.layout = m_PipelineLayout;
    info.renderPass = render_pass;
    info.subpass = 0;
    info.basePipelineHandle = RHI_NULL_HANDLE;
    info.basePipelineIndex = -1;

    if (m_Rhi->CreateGraphicsPipelines(RHI_NULL_HANDLE, 1, &info, m_Pipeline) != RHI_SUCCESS ||
        m_Pipeline == nullptr)
    {
        LOG_ERROR(ZRender, "VulkanBindlessTextureBlitPipeline: createGraphicsPipelines failed");
        m_Pipeline = nullptr;
        return false;
    }
    return true;
}
