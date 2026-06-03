// =====================================================================
// PR-V4 part 1: VulkanBindlessTonemapPipeline implementation
// ---------------------------------------------------------------------
// See bindless_tonemap_pipeline.h for the API contract and the
// "why this lands before the standalone tonemap pass" rationale.
//
// Implementation strategy: the file is structurally identical to its
// sibling `bindless_texture_blit_pipeline.cpp` -- same pipeline state,
// same descriptor-set-layout reuse trick, same push-constant range,
// same compile-from-source path. The ONLY semantic difference is the
// fragment shader source string, which applies the Uncharted2 tone
// curve + Gamma 2.2 correction instead of a passthrough texture()
// fetch.
//
// We deliberately copy the sibling rather than refactor a shared base
// class. Reasons:
//   1. The pipeline-state struct count is high (~12 RHI structs per
//      pipeline) and refactoring them into a "GlslBindlessFullscreen
//      Pipeline" base would either (a) require a virtual hook for
//      per-shader pixel state -- premature when there are exactly
//      two consumers -- or (b) parameterise via a config struct, which
//      makes future per-pipeline tweaks (depth/stencil for a future
//      stencil-driven post-effect, blend mode for an additive bloom
//      pipeline, etc.) awkward.
//   2. The DX12 sibling at `dx12/utility/bindless_texture_blit_pipeline.{h,cpp}`
//      will eventually grow its own `BindlessTonemapPipeline`
//      counterpart in the same shape; keeping the Vulkan files
//      symmetrical with the DX12 layout (one .h+.cpp pair per
//      bindless pipeline kind) eases cross-backend review.
//   3. The duplication is ~250 lines of struct-init boilerplate that
//      has been validated by PR-V3's smoke test. A future cross-cutting
//      cleanup PR can extract a helper builder once a third bindless
//      pipeline arrives; doing it now would be premature
//      generalisation.
//
// Implementation notes worth preserving (kept here, not in the
// header, since they are implementation-specific):
//
//   1. Why we DO NOT call createDescriptorSetLayout ourselves.
//      The descriptor set we want to bind was allocated by
//      VulkanBindlessTextureManager AGAINST a specific
//      VkDescriptorSetLayout (UPDATE_AFTER_BIND | PARTIALLY_BOUND |
//      VARIABLE_DESCRIPTOR_COUNT). vkCmdBindDescriptorSets only
//      accepts a set if the pipeline layout's set-layout slot at the
//      same index is COMPATIBLE -- and "compatible" in Vulkan means
//      the set-layout objects either match by identity OR were
//      created with identical bindings (including flags and counts).
//      The cheapest, surest path is to reuse the manager's actual
//      VkDescriptorSetLayout -- exactly what the blit sibling does.
//
//   2. Why we explicitly publish the bindless push-constant range.
//      VulkanRHI::CreatePipelineLayout has no auto-detection -- it
//      is a pure passthrough. Without an explicit range,
//      vkCmdPushConstants in cmdSetBindlessIndexPFN fires
//      VUID-vkCmdPushConstants-offset on the very first draw.
//
//   3. recordTonemap MUST bind the manager's descriptor set.
//      Without this bind the shader's `u_bindless[idx]` resolves to
//      UNBOUND and validation fires VUID-vkCmdDraw-None-08114.
//
// Backend availability:
//   - PR-V4 part 1 lights up Vulkan only. dynamic_cast<VulkanRHI*>
//     is the runtime gate; on DX12 / Metal / WebGL2 the call
//     returns false and the pipeline stays in its "not ready" state
//     (mirrors PR-V3's gate).
// =====================================================================

#include "Runtime/Function/Render/Interface/Vulkan/Utility/BindlessTonemapPipeline.h"

#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Function/Render/Interface/Vulkan/ShaderCompiler.h"
#include "Runtime/Function/Render/Interface/Vulkan/VulkanBindlessTextureManager.h"
#include "Runtime/Function/Render/Interface/Vulkan/VulkanRHI.h"
#include "Runtime/Function/Render/Interface/Vulkan/VulkanRHIResource.h"
#include "Runtime/Function/Render/RenderType.h"

namespace
{
    // GLSL sources kept INLINE -- same rationale as the blit sibling
    // (no runtime file-system dependency, glslang accepts source
    // strings). See bindless_texture_blit_pipeline.cpp's anonymous
    // namespace for the full reasoning. The on-disk
    // `vulkan/utility/shaders/tone_mapping_bindless.frag` is the
    // human-edited source of truth + IDE syntax-highlighting source;
    // it MUST be kept in sync with the literal below.
    //
    // The vertex shader is identical to the blit sibling
    // (`bindless_blit.vert`): a single SV_VertexID-driven fullscreen
    // triangle. We re-declare the same source string here rather than
    // pulling it across translation units to keep this TU
    // self-contained -- if the blit sibling's vert ever drifts (e.g.
    // a future Y-flip change for headless capture), the tonemap
    // version stays pinned to its tested form.
    constexpr const char* k_bindless_tonemap_vert = R"(#version 450
#extension GL_ARB_separate_shader_objects : enable

layout(location = 0) out vec2 v_uv;

void main()
{
    vec2 uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    v_uv = uv;
    gl_Position = vec4(uv * vec2(2.0, -2.0) + vec2(-1.0, 1.0), 0.0, 1.0);
}
)";

    constexpr const char* k_bindless_tonemap_frag = R"(#version 450
#extension GL_ARB_separate_shader_objects : enable
#extension GL_EXT_nonuniform_qualifier     : require

layout(set = 0, binding = 0) uniform sampler2D u_bindless[];

layout(push_constant) uniform BindlessPush
{
    uint g_packed_indices;
} u_push;

layout(location = 0) in  vec2 v_uv;
layout(location = 0) out vec4 o_color;

vec3 Uncharted2Tonemap(vec3 x)
{
    float A = 0.15;
    float B = 0.50;
    float C = 0.10;
    float D = 0.20;
    float E = 0.02;
    float F = 0.30;
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}

void main()
{
    uint texture_index = u_push.g_packed_indices & 0xFFFFu;
    vec3 color = texture(u_bindless[nonuniformEXT(texture_index)], v_uv).rgb;

    color = Uncharted2Tonemap(color * 4.5);
    color = color * (1.0 / Uncharted2Tonemap(vec3(11.2)));

    color = vec3(pow(color.x, 1.0 / 2.2),
                 pow(color.y, 1.0 / 2.2),
                 pow(color.z, 1.0 / 2.2));

    o_color = vec4(color, 1.0);
}
)";
}  // namespace

bool VulkanBindlessTonemapPipeline::Initialize(RHI* rhi, RHIRenderPass* target_render_pass)
{
    if (m_Ready)
    {
        return true;
    }
    if (rhi == nullptr || target_render_pass == nullptr)
    {
        LOG_ERROR(ZRender, "VulkanBindlessTonemapPipeline::initialize: rhi or render_pass is null");
        return false;
    }

    // PR-V4 lights up Vulkan only; mirror PR-V3's runtime gate.
    VulkanRHI* vk_rhi = dynamic_cast<VulkanRHI*>(rhi);
    if (vk_rhi == nullptr)
    {
        LOG_WARNING(ZRender,
                    "VulkanBindlessTonemapPipeline currently supports Vulkan only; "
                    "current RHI is not a VulkanRHI. Initialization skipped.");
        return false;
    }
    if (!vk_rhi->supportsBindlessTextures())
    {
        LOG_WARNING(ZRender,
                    "VulkanBindlessTonemapPipeline: VulkanRHI does not support bindless "
                    "(VK_EXT_descriptor_indexing missing or required feature bits absent). "
                    "Initialization skipped.");
        return false;
    }
    RHIBindlessTextureManager* mgr = vk_rhi->getBindlessTextureManager();
    if (mgr == nullptr)
    {
        LOG_ERROR(ZRender,
                  "VulkanBindlessTonemapPipeline: supportsBindlessTextures() returned true "
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
        m_PipelineLayout = nullptr;
        m_Rhi = nullptr;
        m_VkRhi = nullptr;
        return false;
    }

    m_Rhi->DestroyShaderModule(vs);
    m_Rhi->DestroyShaderModule(ps);

    m_Ready = true;
    LOG_INFO(ZRender,
             "VulkanBindlessTonemapPipeline initialized "
             "(layout={}, pipeline={}, target render-pass={})",
             static_cast<void*>(m_PipelineLayout),
             static_cast<void*>(m_Pipeline),
             static_cast<void*>(target_render_pass));
    return true;
}

void VulkanBindlessTonemapPipeline::Shutdown()
{
    if (m_SetLayoutView != nullptr)
    {
        delete m_SetLayoutView;
        m_SetLayoutView = nullptr;
    }
    // VulkanRHI does not currently expose explicit destroy*() for
    // pipelines / pipeline layouts (its resources are pooled and
    // released at RHI teardown). We just drop our pointers so
    // isReady() flips to false; a future Initialize() rebuilds from
    // scratch. Same lifetime model as the blit sibling.
    m_Pipeline = nullptr;
    m_PipelineLayout = nullptr;
    m_Rhi = nullptr;
    m_VkRhi = nullptr;
    m_Ready = false;
}

void VulkanBindlessTonemapPipeline::RecordTonemap(RHICommandBuffer* command_buffer,
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

    // Bind the bindless descriptor set at set 0. Same compatibility
    // story as the blit sibling: the manager's set was allocated
    // against the same VkDescriptorSetLayout that BuildPipelineLayout()
    // embedded into m_PipelineLayout, so the layout-compat check
    // inside vkCmdBindDescriptorSets passes. Without this step the
    // shader's u_bindless[] array resolves to UNBOUND and the first
    // draw fires VUID-vkCmdDraw-None-08114.
    auto* vk_mgr = static_cast<VulkanBindlessTextureManager*>(mgr);
    VkDescriptorSet vk_set = vk_mgr->GetDescriptorSet();
    if (vk_set == VK_NULL_HANDLE)
    {
        return;
    }
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
    // 'commandBuffer' is honoured on Vulkan; 'pipelineBindPoint' is
    // unused on Vulkan because push constants are bind-point-agnostic
    // in the Vulkan model.
    const uint32_t packed = BindlessIndex::Pack(bindless_texture_index, sampler_index);
    m_Rhi->CmdSetBindlessIndexPFN(command_buffer,
                                  RHI_PIPELINE_BIND_POINT_GRAPHICS,
                                  m_PipelineLayout,
                                  packed);

    // Fullscreen oversized triangle: 3 verts, 1 instance. Matches
    // k_bindless_tonemap_vert (gl_VertexIndex 0..2 -> NDC corners).
    m_Rhi->CmdDraw(command_buffer, 3, 1, 0, 0);
}

bool VulkanBindlessTonemapPipeline::CompileShaders(RHIShader*& out_vs,
                                                   RHIShader*& out_ps) const
{
    out_vs = nullptr;
    out_ps = nullptr;

    // createShaderModuleFromSource takes `enum class ShaderStage`
    // integer values, NOT RHI_SHADER_STAGE_*_BIT bitflags. See PR-V3
    // semantic divergence note 5 in BINDLESS_TEXTURE_PATH.md for the
    // footgun history; we match the existing convention to stay
    // consistent with main_camera_pass.cpp et al.
    out_vs = m_Rhi->CreateShaderModuleFromSource(k_bindless_tonemap_vert,
                                                 ShaderStage::Vertex,
                                                 "tone_mapping_bindless.vert",
                                                 /*include_paths*/ {},
                                                 /*macros*/ {});
    if (out_vs == nullptr)
    {
        LOG_ERROR(ZRender, "VulkanBindlessTonemapPipeline: failed to compile tonemap_bindless.vert");
        return false;
    }

    out_ps = m_Rhi->CreateShaderModuleFromSource(k_bindless_tonemap_frag,
                                                 ShaderStage::Fragment,
                                                 "tone_mapping_bindless.frag",
                                                 /*include_paths*/ {},
                                                 /*macros*/ {});
    if (out_ps == nullptr)
    {
        LOG_ERROR(ZRender,
                  "VulkanBindlessTonemapPipeline: failed to compile tone_mapping_bindless.frag "
                  "(check that glslang reports VK_EXT_descriptor_indexing / "
                  "GL_EXT_nonuniform_qualifier support).");
        m_Rhi->DestroyShaderModule(out_vs);
        out_vs = nullptr;
        return false;
    }
    return true;
}

bool VulkanBindlessTonemapPipeline::BuildPipelineLayout()
{
    auto* vk_mgr = static_cast<VulkanBindlessTextureManager*>(m_VkRhi->getBindlessTextureManager());
    VkDescriptorSetLayout vk_set_layout = vk_mgr->getDescriptorSetLayout();
    if (vk_set_layout == VK_NULL_HANDLE)
    {
        LOG_ERROR(ZRender,
                  "VulkanBindlessTonemapPipeline: bindless manager has no "
                  "VkDescriptorSetLayout (manager init failed earlier?).");
        return false;
    }

    auto* layout_view = new VulkanDescriptorSetLayout();
    layout_view->setResource(vk_set_layout);
    m_SetLayoutView = layout_view;

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
        LOG_ERROR(ZRender, "VulkanBindlessTonemapPipeline: createPipelineLayout failed");
        m_PipelineLayout = nullptr;
        return false;
    }
    return true;
}

bool VulkanBindlessTonemapPipeline::BuildPipeline(RHIRenderPass* render_pass,
                                                  RHIShader* vs,
                                                  RHIShader* ps)
{
    // Pipeline state mirrors the blit sibling 1:1 (and therefore the
    // DX12 great-grand-sibling). Empty IA (VS is gl_VertexIndex-
    // driven), no depth/stencil, no MSAA, no blend, no cull,
    // viewport+scissor dynamic. Re-using the validated shape keeps
    // the visual output byte-identical between the future tonemap
    // standalone pass and the legacy in-subpass tonemap, isolating
    // any pixel delta to the algorithm change (which is zero, since
    // the GLSL math matches the legacy `tone_mapping.frag`).
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
        LOG_ERROR(ZRender, "VulkanBindlessTonemapPipeline: createGraphicsPipelines failed");
        m_Pipeline = nullptr;
        return false;
    }
    return true;
}
