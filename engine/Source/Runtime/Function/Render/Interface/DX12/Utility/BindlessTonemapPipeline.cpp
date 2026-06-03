// =====================================================================
// PR-DX2: DX12BindlessTonemapPipeline implementation
// ---------------------------------------------------------------------
// See bindless_tonemap_pipeline.h for the API contract and the "why
// this lands as a pure-additive utility" rationale.
//
// Implementation strategy: the file is structurally identical to its
// sibling `bindless_texture_blit_pipeline.cpp` -- same pipeline state,
// same descriptor-set-layout trick (VARIABLE_DESCRIPTOR_COUNT binding
// that triggers DX12RHI::createPipelineLayout's bindless path), same
// compile-from-file path. The ONLY semantic difference is the pixel
// shader source file, which applies the Uncharted2 tone curve +
// Gamma 2.2 correction instead of a passthrough texture() fetch.
//
// We deliberately copy the sibling rather than refactor a shared base
// class. Reasons (mirrored from VulkanBindlessTonemapPipeline):
//   1. The pipeline-state struct count is high (~12 RHI structs per
//      pipeline) and refactoring them into a shared base would either
//      (a) require a virtual hook for per-shader pixel state --
//      premature when there are exactly two consumers -- or (b)
//      parameterise via a config struct, which makes future
//      per-pipeline tweaks (depth/stencil for a future stencil-driven
//      post-effect, blend mode for an additive bloom pipeline, etc.)
//      awkward.
//   2. Keeping the DX12 files symmetrical with the Vulkan layout (one
//      .h+.cpp pair per bindless pipeline kind) eases cross-backend
//      review.
//   3. The duplication is ~250 lines of struct-init boilerplate that
//      has been validated by PR7's smoke test. A future cross-cutting
//      cleanup PR can extract a helper builder once a third bindless
//      pipeline arrives; doing it now would be premature
//      generalisation.
// =====================================================================

#include "Runtime/Function/Render/Interface/DX12/Utility/BindlessTonemapPipeline.h"

#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Function/Render/Interface/DX12/DX12RHI.h"
#include "Runtime/Function/Render/Interface/DX12/DX12ShaderCompiler.h"
#include "Runtime/Function/Render/Interface/RHI.h"

#include <filesystem>
#include <string>
#include <vector>

namespace
{
    // Resolve bindless_blit_vs.hlsl + bindless_tonemap_ps.hlsl (CWD-independent).
    std::filesystem::path resolveShadersRoot(const std::string& override_root)
    {
        namespace fs = std::filesystem;

        auto try_root = [](const fs::path& root) -> fs::path {
            if (fs::exists(root / "bindless_blit_vs.hlsl") && fs::exists(root / "bindless_tonemap_ps.hlsl"))
            {
                return fs::absolute(root);
            }
            return {};
        };

#ifdef ZENGINE_DX12_UTILITY_SHADER_ROOT
        if (const fs::path from_cmake = try_root(fs::path(ZENGINE_DX12_UTILITY_SHADER_ROOT)); !from_cmake.empty())
        {
            return from_cmake;
        }
#endif

        if (!override_root.empty())
        {
            if (const fs::path from_override = try_root(fs::path(override_root)); !from_override.empty())
            {
                return from_override;
            }
        }

        const fs::path rel_paths[] = {
            "engine/Source/Runtime/Function/Render/Interface/DX12/Utility/Shaders",
            "engine/source/Runtime/Function/Render/Interface/dx12/utility/shaders",
        };

        fs::path cursor = fs::current_path();
        for (int i = 0; i < 8; ++i)
        {
            for (const fs::path& rel : rel_paths)
            {
                if (const fs::path found = try_root(cursor / rel); !found.empty())
                {
                    return found;
                }
            }
            if (!cursor.has_parent_path())
            {
                break;
            }
            cursor = cursor.parent_path();
        }

        return {};
    }
}  // namespace

bool DX12BindlessTonemapPipeline::Initialize(RHI* rhi,
                                             RHIRenderPass* target_render_pass,
                                             const std::string& hlsl_search_root)
{
    if (m_Ready)
    {
        return true;
    }
    if (rhi == nullptr || target_render_pass == nullptr)
    {
        LOG_ERROR(ZRender, "DX12BindlessTonemapPipeline::initialize: rhi or render_pass is null");
        return false;
    }

    // PR-DX2 lights up DX12 only.
    auto* dx12_rhi = dynamic_cast<DX12RHI*>(rhi);
    if (dx12_rhi == nullptr)
    {
        LOG_WARNING(ZRender,
                    "DX12BindlessTonemapPipeline currently supports DX12 only; "
                    "current RHI is not a DX12RHI. Initialization skipped.");
        return false;
    }
    if (!dx12_rhi->supportsBindlessTextures())
    {
        LOG_WARNING(ZRender,
                    "DX12BindlessTonemapPipeline: DX12RHI does not support bindless "
                    "textures. Initialization skipped.");
        return false;
    }

    m_Rhi = rhi;

    RHIShader* vs = nullptr;
    RHIShader* ps = nullptr;
    if (!CompileShaders(hlsl_search_root, vs, ps))
    {
        m_Rhi = nullptr;
        return false;
    }

    if (!BuildDescriptorSetLayout())
    {
        m_Rhi->DestroyShaderModule(vs);
        m_Rhi->DestroyShaderModule(ps);
        m_Rhi = nullptr;
        return false;
    }

    if (!BuildPipelineLayout())
    {
        m_Rhi->DestroyShaderModule(vs);
        m_Rhi->DestroyShaderModule(ps);
        m_SetLayout = nullptr;
        m_Rhi = nullptr;
        return false;
    }

    if (!BuildPipeline(target_render_pass, vs, ps))
    {
        m_Rhi->DestroyShaderModule(vs);
        m_Rhi->DestroyShaderModule(ps);
        m_SetLayout = nullptr;
        m_PipelineLayout = nullptr;
        m_Rhi = nullptr;
        return false;
    }

    // RHI has internalised the shader bytecode into the pipeline; we
    // can release the staging shader-module objects.
    m_Rhi->DestroyShaderModule(vs);
    m_Rhi->DestroyShaderModule(ps);

    m_Ready = true;
    LOG_INFO(ZRender,
             "DX12BindlessTonemapPipeline initialized "
             "(layout={}, pipeline={}, target render-pass={})",
             static_cast<void*>(m_PipelineLayout),
             static_cast<void*>(m_Pipeline),
             static_cast<void*>(target_render_pass));
    return true;
}

void DX12BindlessTonemapPipeline::Shutdown()
{
    // The DX12 RHI does not currently expose explicit destroy*() for
    // pipelines / pipeline layouts / descriptor-set layouts (its
    // resources are pooled and released at RHI teardown). We just drop
    // our pointers so isReady() flips to false and a future
    // Initialize() rebuilds from scratch. Same lifetime model as the
    // blit sibling.
    m_Pipeline = nullptr;
    m_PipelineLayout = nullptr;
    m_SetLayout = nullptr;
    m_Rhi = nullptr;
    m_Ready = false;
}

void DX12BindlessTonemapPipeline::RecordTonemap(RHICommandBuffer* command_buffer,
                                                uint32_t viewport_width,
                                                uint32_t viewport_height,
                                                uint32_t bindless_texture_index,
                                                BindlessBlitSampler sampler) const
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

    // Bind pipeline first -- viewport / scissor and the bindless root
    // constant are graphics-pipeline-relative state.
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

    // Push the packed bindless index. Same packing as the blit sibling.
    const uint32_t packed = BindlessIndex::Pack(bindless_texture_index, static_cast<uint32_t>(sampler));
    m_Rhi->CmdSetBindlessIndexPFN(command_buffer,
                                  RHI_PIPELINE_BIND_POINT_GRAPHICS,
                                  m_PipelineLayout,
                                  packed);

    // Fullscreen oversized triangle: 3 verts, 1 instance. Matches
    // bindless_blit_vs.hlsl (SV_VertexID 0..2 -> NDC corners).
    m_Rhi->CmdDraw(command_buffer, 3, 1, 0, 0);
}

bool DX12BindlessTonemapPipeline::CompileShaders(const std::string& hlsl_search_root,
                                                 RHIShader*& out_vs,
                                                 RHIShader*& out_ps) const
{
    out_vs = nullptr;
    out_ps = nullptr;

    const std::filesystem::path shaders_dir = resolveShadersRoot(hlsl_search_root);
    if (shaders_dir.empty())
    {
        LOG_ERROR(ZRender,
                  "DX12BindlessTonemapPipeline: cannot locate bindless tonemap shaders. "
                  "CWD='{}'. Pass an explicit hlsl_search_root or run the editor "
                  "from the engine root.",
                  std::filesystem::current_path().string());
        return false;
    }

    const std::string vs_path = (shaders_dir / "bindless_blit_vs.hlsl").string();
    const std::string ps_path = (shaders_dir / "bindless_tonemap_ps.hlsl").string();

    // ---- VS: SM 6.0 default is fine; no ResourceDescriptorHeap on VS.
    //         Reuses the same VS as the blit sibling (bindless_blit_vs.hlsl).
    DX12ShaderCompiler vs_compiler;
    const DX12ShaderCompileResult vs_result =
        vs_compiler.CompileFromFile(vs_path,
                                    /*shader_stage=*/ShaderStage::Vertex,
                                    /*include_paths=*/ {},
                                    /*macros=*/ {},
                                    /*entry_point=*/"main",
                                    /*target_profile=*/"",  // -> vs_6_0 default
                                    /*hlsl_version=*/"");   // -> HV 2018 default
    if (!vs_result.success)
    {
        LOG_ERROR(ZRender,
                  "DX12BindlessTonemapPipeline: failed to compile VS at {}: {}",
                  vs_path,
                  vs_result.error_message);
        return false;
    }

    // ---- PS: SM 6.6 + HLSL 2021 are MANDATORY for ResourceDescriptorHeap[]
    //         + NonUniformResourceIndex. Same compilation contract as the
    //         blit sibling's PS.
    DX12ShaderCompiler ps_compiler;
    const DX12ShaderCompileResult ps_result =
        ps_compiler.CompileFromFile(ps_path,
                                    /*shader_stage=*/ShaderStage::Fragment,
                                    /*include_paths=*/ {},
                                    /*macros=*/ {},
                                    /*entry_point=*/"main",
                                    /*target_profile=*/"ps_6_6",
                                    /*hlsl_version=*/"2021");
    if (!ps_result.success)
    {
        LOG_ERROR(ZRender,
                  "DX12BindlessTonemapPipeline: failed to compile tonemap PS at {} "
                  "(ps_6_6 / HV 2021): {}",
                  ps_path,
                  ps_result.error_message);
        return false;
    }

    out_vs = m_Rhi->CreateShaderModule(vs_result.dxil_code);
    out_ps = m_Rhi->CreateShaderModule(ps_result.dxil_code);
    if (out_vs == nullptr || out_ps == nullptr)
    {
        LOG_ERROR(ZRender, "DX12BindlessTonemapPipeline: createShaderModule returned null");
        if (out_vs)
            m_Rhi->DestroyShaderModule(out_vs);
        if (out_ps)
            m_Rhi->DestroyShaderModule(out_ps);
        out_vs = nullptr;
        out_ps = nullptr;
        return false;
    }
    return true;
}

bool DX12BindlessTonemapPipeline::BuildDescriptorSetLayout()
{
    // Single binding at (set=0, binding=0), descriptorCount=1,
    // VARIABLE_DESCRIPTOR_COUNT. Same layout as the blit sibling --
    // the marker flag tells DX12RHI::CreatePipelineLayout that this
    // set is "bindless" and triggers the 32-bit root constant +
    // static sampler bank emission.
    RHIDescriptorSetLayoutBinding binding = {};
    binding.binding = 0;
    binding.descriptorType = RHI_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    binding.descriptorCount = 1;
    binding.stageFlags = RHI_SHADER_STAGE_FRAGMENT_BIT;
    binding.bindingFlags = RHI_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT |
                           RHI_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;

    RHIDescriptorSetLayoutCreateInfo create_info = {};
    create_info.sType = RHI_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    create_info.pNext = nullptr;
    create_info.flags = 0;
    create_info.bindingCount = 1;
    create_info.pBindings = &binding;

    if (m_Rhi->CreateDescriptorSetLayout(&create_info, m_SetLayout) != RHI_SUCCESS ||
        m_SetLayout == nullptr)
    {
        LOG_ERROR(ZRender, "DX12BindlessTonemapPipeline: createDescriptorSetLayout failed");
        m_SetLayout = nullptr;
        return false;
    }
    return true;
}

bool DX12BindlessTonemapPipeline::BuildPipelineLayout()
{
    RHIDescriptorSetLayout* set_layouts[1] = {m_SetLayout};
    RHIPipelineLayoutCreateInfo info = {};
    info.sType = RHI_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    info.setLayoutCount = 1;
    info.pSetLayouts = set_layouts;
    info.pushConstantRangeCount = 0;
    info.pPushConstantRanges = nullptr;

    if (m_Rhi->CreatePipelineLayout(&info, m_PipelineLayout) != RHI_SUCCESS ||
        m_PipelineLayout == nullptr)
    {
        LOG_ERROR(ZRender, "DX12BindlessTonemapPipeline: createPipelineLayout failed");
        m_PipelineLayout = nullptr;
        return false;
    }
    return true;
}

bool DX12BindlessTonemapPipeline::BuildPipeline(RHIRenderPass* render_pass,
                                                RHIShader* vs,
                                                RHIShader* ps)
{
    // Pipeline state mirrors the blit sibling 1:1. Empty IA (VS is
    // SV_VertexID-driven), no depth/stencil, no MSAA, no blend, no
    // cull, viewport+scissor dynamic.
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
        LOG_ERROR(ZRender, "DX12BindlessTonemapPipeline: createGraphicsPipelines failed");
        m_Pipeline = nullptr;
        return false;
    }
    return true;
}
