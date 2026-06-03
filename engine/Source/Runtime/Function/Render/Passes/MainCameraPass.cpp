#include "Runtime/Function/Render/Passes/MainCameraPass.h"

#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Function/Render/Interface/Vulkan/VulkanRHI.h"
#include "Runtime/Function/Render/Interface/Vulkan/VulkanRenderResource.h"
#include "Runtime/Function/Render/Interface/Vulkan/VulkanUtil.h"
#include "Runtime/Function/Render/RenderCamera.h"
#include "Runtime/Function/Render/RenderHelper.h"
#include "Runtime/Function/Render/RenderMesh.h"
#include "Runtime/Function/Render/RenderResource.h"
#include "Runtime/Function/Render/RenderScene.h"
#include "Runtime/Function/Render/RenderSystem.h"
#include "Runtime/Function/Render/RenderType.h"
#include "Runtime/Utility/Utility.h"

#include <algorithm>
#include <array>
#include <axis_frag.h>
#include <axis_vert.h>
#include <cctype>
#include <deferred_lighting_frag.h>
#include <deferred_lighting_vert.h>
#include "Runtime/Function/Render/MegaLights/MegaLightsSettings.h"
#include <megalights_deferred_frag.h>
#include <megalights_deferred_vert.h>
#include <megalights_spatial_frag.h>
#include <map>
#include <mesh_frag.h>
#include <mesh_gbuffer_frag.h>
#include <mesh_vert.h>
#include <skybox_frag.h>
#include <skybox_vert.h>
#include <stdexcept>

namespace
{
    std::string ToUpperCopy(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
            return static_cast<char>(std::toupper(character));
        });
        return value;
    }

    bool EqualsIgnoreCase(const std::string& lhs, const std::string& rhs)
    {
        return ToUpperCopy(lhs) == ToUpperCopy(rhs);
    }

    bool ContainsIgnoreCase(const std::string& text, const std::string& token)
    {
        return ToUpperCopy(text).find(ToUpperCopy(token)) != std::string::npos;
    }

    bool IsBlendModeEnabled(const std::string& blend)
    {
        const std::string normalized_blend = ToUpperCopy(blend);
        return !(normalized_blend.empty() || normalized_blend == "OFF" || normalized_blend == "OPAQUE");
    }

    RHICullModeFlags ParseCullMode(const std::string& cull)
    {
        if (EqualsIgnoreCase(cull, "OFF") || EqualsIgnoreCase(cull, "NONE"))
        {
            return RHI_CULL_MODE_NONE;
        }
        if (EqualsIgnoreCase(cull, "FRONT"))
        {
            return RHI_CULL_MODE_FRONT_BIT;
        }
        return RHI_CULL_MODE_BACK_BIT;
    }

    RHICompareOp ParseCompareOp(const std::string& ztest)
    {
        if (EqualsIgnoreCase(ztest, "NEVER"))
        {
            return RHI_COMPARE_OP_NEVER;
        }
        if (EqualsIgnoreCase(ztest, "LESS"))
        {
            return RHI_COMPARE_OP_LESS;
        }
        if (EqualsIgnoreCase(ztest, "EQUAL"))
        {
            return RHI_COMPARE_OP_EQUAL;
        }
        if (EqualsIgnoreCase(ztest, "LEQUAL"))
        {
            return RHI_COMPARE_OP_LESS_OR_EQUAL;
        }
        if (EqualsIgnoreCase(ztest, "GREATER"))
        {
            return RHI_COMPARE_OP_GREATER;
        }
        if (EqualsIgnoreCase(ztest, "NOTEQUAL"))
        {
            return RHI_COMPARE_OP_NOT_EQUAL;
        }
        if (EqualsIgnoreCase(ztest, "GEQUAL"))
        {
            return RHI_COMPARE_OP_GREATER_OR_EQUAL;
        }
        if (EqualsIgnoreCase(ztest, "ALWAYS"))
        {
            return RHI_COMPARE_OP_ALWAYS;
        }
        return RHI_COMPARE_OP_LESS_OR_EQUAL;
    }

    template<size_t AttachmentCount>
    void ApplyBlendMode(const std::string& blend, std::array<RHIPipelineColorBlendAttachmentState, AttachmentCount>& attachments)
    {
        const std::string normalized_blend = ToUpperCopy(blend);
        const bool enable_alpha_blend = normalized_blend == "ALPHA" || normalized_blend == "TRANSPARENT" ||
                                        normalized_blend == "BLEND";
        const bool enable_additive_blend = normalized_blend == "ADDITIVE" || normalized_blend == "ADD";
        const bool enable_premultiply_blend = normalized_blend == "PREMULTIPLY" || normalized_blend == "PREMULTIPLIED";

        for (RHIPipelineColorBlendAttachmentState& attachment : attachments)
        {
            attachment.colorWriteMask = RHI_COLOR_COMPONENT_R_BIT | RHI_COLOR_COMPONENT_G_BIT | RHI_COLOR_COMPONENT_B_BIT |
                                        RHI_COLOR_COMPONENT_A_BIT;
            attachment.blendEnable = RHI_FALSE;
            attachment.srcColorBlendFactor = RHI_BLEND_FACTOR_ONE;
            attachment.dstColorBlendFactor = RHI_BLEND_FACTOR_ZERO;
            attachment.colorBlendOp = RHI_BLEND_OP_ADD;
            attachment.srcAlphaBlendFactor = RHI_BLEND_FACTOR_ONE;
            attachment.dstAlphaBlendFactor = RHI_BLEND_FACTOR_ZERO;
            attachment.alphaBlendOp = RHI_BLEND_OP_ADD;

            if (enable_alpha_blend)
            {
                attachment.blendEnable = RHI_TRUE;
                attachment.srcColorBlendFactor = RHI_BLEND_FACTOR_SRC_ALPHA;
                attachment.dstColorBlendFactor = RHI_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                attachment.srcAlphaBlendFactor = RHI_BLEND_FACTOR_ONE;
                attachment.dstAlphaBlendFactor = RHI_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            }
            else if (enable_additive_blend)
            {
                attachment.blendEnable = RHI_TRUE;
                attachment.srcColorBlendFactor = RHI_BLEND_FACTOR_SRC_ALPHA;
                attachment.dstColorBlendFactor = RHI_BLEND_FACTOR_ONE;
                attachment.srcAlphaBlendFactor = RHI_BLEND_FACTOR_ONE;
                attachment.dstAlphaBlendFactor = RHI_BLEND_FACTOR_ONE;
            }
            else if (enable_premultiply_blend)
            {
                attachment.blendEnable = RHI_TRUE;
                attachment.srcColorBlendFactor = RHI_BLEND_FACTOR_ONE;
                attachment.dstColorBlendFactor = RHI_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                attachment.srcAlphaBlendFactor = RHI_BLEND_FACTOR_ONE;
                attachment.dstAlphaBlendFactor = RHI_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            }
        }
    }

    const VulkanShaderPassData* FindShaderPassByLightMode(const VulkanPBRMaterial& material, const char* desired_light_mode)
    {
        for (const VulkanShaderPassData& shader_pass : material.shader_passes)
        {
            if (EqualsIgnoreCase(shader_pass.light_mode, desired_light_mode != nullptr ? desired_light_mode : ""))
            {
                return &shader_pass;
            }
        }
        return nullptr;
    }

    const VulkanShaderPassData* FindForwardShaderPass(const VulkanPBRMaterial& material)
    {
        if (const VulkanShaderPassData* shader_pass = FindShaderPassByLightMode(material, "ForwardBase"))
        {
            return shader_pass;
        }
        return FindShaderPassByLightMode(material, "Forward");
    }

    const VulkanShaderPassData* FindTransparentShaderPass(const VulkanPBRMaterial& material)
    {
        if (const VulkanShaderPassData* shader_pass = FindShaderPassByLightMode(material, "Transparent"))
        {
            return shader_pass;
        }

        for (const VulkanShaderPassData& shader_pass : material.shader_passes)
        {
            if (IsBlendModeEnabled(shader_pass.blend))
            {
                return &shader_pass;
            }
        }

        return nullptr;
    }

    bool CanUseRuntimePrimaryShaderPass(const std::shared_ptr<RHI>& rhi, const VulkanPBRMaterial& material)
    {
        if (material.vertex_shader_file.empty() || material.fragment_shader_file.empty())
        {
            return false;
        }

        if (!material.light_mode.empty() && !EqualsIgnoreCase(material.light_mode, "GBUFFER"))
        {
            return false;
        }

        if (rhi == nullptr)
        {
            return false;
        }

        const GraphicsAPI graphics_api = rhi->getGraphicsAPI();
        if (graphics_api == GraphicsAPI::Vulkan)
        {
            if (!material.enable_vulkan || ContainsIgnoreCase(material.source_language, "HLSL"))
            {
                return false;
            }
        }
        else if (graphics_api == GraphicsAPI::DirectX12)
        {
            if (!material.enable_dx12)
            {
                return false;
            }
        }
        else if (graphics_api == GraphicsAPI::Metal)
        {
            if (!material.enable_metal)
            {
                return false;
            }
        }

        return true;
    }

    bool CanUseRuntimeShaderPass(const std::shared_ptr<RHI>& rhi,
                                 const VulkanPBRMaterial& material,
                                 const VulkanShaderPassData* shader_pass)
    {
        if (shader_pass == nullptr || shader_pass->vertex_shader_file.empty() || shader_pass->fragment_shader_file.empty())
        {
            return false;
        }

        if (rhi == nullptr)
        {
            return false;
        }

        const GraphicsAPI graphics_api = rhi->getGraphicsAPI();
        if (graphics_api == GraphicsAPI::Vulkan)
        {
            if (!material.enable_vulkan || ContainsIgnoreCase(material.source_language, "HLSL"))
            {
                return false;
            }
        }
        else if (graphics_api == GraphicsAPI::DirectX12)
        {
            if (!material.enable_dx12)
            {
                return false;
            }
        }
        else if (graphics_api == GraphicsAPI::Metal)
        {
            if (!material.enable_metal)
            {
                return false;
            }
        }

        return true;
    }

    MainCameraPass::MeshGBufferPipelineKey BuildPipelineKey(const VulkanPBRMaterial& material)
    {
        MainCameraPass::MeshGBufferPipelineKey key;
        key.vertex_shader_file = material.vertex_shader_file;
        key.fragment_shader_file = material.fragment_shader_file;
        key.vertex_entry = material.vertex_entry.empty() ? "main" : material.vertex_entry;
        key.fragment_entry = material.fragment_entry.empty() ? "main" : material.fragment_entry;
        key.include_directory = material.include_directory;
        key.source_language = material.source_language;
        key.render_pipeline = material.render_pipeline;
        key.light_mode = material.light_mode;
        key.cull = material.cull;
        key.ztest = material.ztest;
        key.blend = material.blend;
        key.zwrite = material.zwrite;
        key.shader_macros = material.shader_macros;
        return key;
    }

    MainCameraPass::MeshGBufferPipelineKey BuildPipelineKey(const VulkanPBRMaterial& material,
                                                            const VulkanShaderPassData& shader_pass)
    {
        MainCameraPass::MeshGBufferPipelineKey key;
        key.vertex_shader_file = shader_pass.vertex_shader_file;
        key.fragment_shader_file = shader_pass.fragment_shader_file;
        key.vertex_entry = shader_pass.vertex_entry.empty() ? "main" : shader_pass.vertex_entry;
        key.fragment_entry = shader_pass.fragment_entry.empty() ? "main" : shader_pass.fragment_entry;
        key.include_directory = material.include_directory;
        key.source_language = material.source_language;
        key.render_pipeline = shader_pass.render_pipeline.empty() ? material.render_pipeline : shader_pass.render_pipeline;
        key.light_mode = shader_pass.light_mode;
        key.cull = shader_pass.cull.empty() ? material.cull : shader_pass.cull;
        key.ztest = shader_pass.ztest.empty() ? material.ztest : shader_pass.ztest;
        key.blend = shader_pass.blend.empty() ? material.blend : shader_pass.blend;
        key.zwrite = shader_pass.zwrite;

        if (EqualsIgnoreCase(key.light_mode, "Transparent"))
        {
            if (!IsBlendModeEnabled(key.blend))
            {
                key.blend = "Transparent";
            }
            key.zwrite = false;
        }

        key.shader_macros = material.shader_macros;
        return key;
    }

}  // namespace

bool MainCameraPass::MeshGBufferPipelineKey::operator<(const MeshGBufferPipelineKey& rhs) const
{
    return std::tie(vertex_shader_file,
                    fragment_shader_file,
                    vertex_entry,
                    fragment_entry,
                    include_directory,
                    source_language,
                    render_pipeline,
                    light_mode,
                    cull,
                    ztest,
                    blend,
                    zwrite,
                    shader_macros) <
           std::tie(rhs.vertex_shader_file,
                    rhs.fragment_shader_file,
                    rhs.vertex_entry,
                    rhs.fragment_entry,
                    rhs.include_directory,
                    rhs.source_language,
                    rhs.render_pipeline,
                    rhs.light_mode,
                    rhs.cull,
                    rhs.ztest,
                    rhs.blend,
                    rhs.zwrite,
                    rhs.shader_macros);
}

void MainCameraPass::Initialize(const RenderPassInitInfo* init_info)

{
    RenderPass::Initialize(nullptr);

    const MainCameraPassInitInfo* _init_info = static_cast<const MainCameraPassInitInfo*>(init_info);
    m_EnableFxaa = _init_info->enble_fxaa;

    SetupAttachments();
    SetupRenderPass();
    SetupDescriptorSetLayout();
    SetupPipelines();
    SetupDescriptorSet();
    SetupFramebufferDescriptorSet();
    SetupSwapchainFramebuffers();

    SetupParticlePass();
}

void MainCameraPass::PreparePassData(std::shared_ptr<RenderResourceBase> render_resource)
{
    RenderResource* vulkan_resource = static_cast<RenderResource*>(render_resource.get());
    if (vulkan_resource)
    {
        m_MeshPerframeStorageBufferObjects = vulkan_resource->m_MeshPerframeStorageBufferObjects;
        m_MeshPerframeStorageBufferObject = vulkan_resource->m_MeshPerframeStorageBufferObject;
        m_AxisStorageBufferObject = vulkan_resource->m_AxisStorageBufferObject;
        m_MegaLightsSystem = &vulkan_resource->GetMegaLightsSystem();
    }

    m_ActiveMainCameraVisibleMeshNodes = m_VisiableNodes.p_main_camera_visible_mesh_nodes;
}

bool MainCameraPass::IsViewportValid(ViewportType viewport_type) const
{
    const RHIViewport* viewport = m_Rhi->GetViewport(viewport_type);
    return viewport && viewport->width > 0.0f && viewport->height > 0.0f;
}

void MainCameraPass::SetViewportScissor(ViewportType viewport_type)
{
    auto&& current_command_buffer = m_Rhi->GetCurrentCommandBuffer();
    auto* viewport = m_Rhi->GetViewport(viewport_type);
    RHIRect2D scissor = m_Rhi->GetSwapchainInfo().scissor[static_cast<uint32_t>(viewport_type)];
    m_Rhi->CmdSetViewportPFN(current_command_buffer, 0, 1, viewport);
    m_Rhi->CmdSetScissorPFN(current_command_buffer, 0, 1, &scissor);
}

void MainCameraPass::SetFullscreenViewportScissor()
{
    auto&& current_command_buffer = m_Rhi->GetCurrentCommandBuffer();
    RHIViewport viewport = {0.0f,
                            0.0f,
                            static_cast<float>(m_Rhi->GetSwapchainInfo().extent.width),
                            static_cast<float>(m_Rhi->GetSwapchainInfo().extent.height),
                            0.0f,
                            1.0f};
    RHIRect2D scissor = {0, 0, m_Rhi->GetSwapchainInfo().extent.width, m_Rhi->GetSwapchainInfo().extent.height};
    m_Rhi->CmdSetViewportPFN(current_command_buffer, 0, 1, &viewport);
    m_Rhi->CmdSetScissorPFN(current_command_buffer, 0, 1, &scissor);
}

void MainCameraPass::SetPerViewportData(ViewportType viewport_type)
{
    m_MeshPerframeStorageBufferObject = m_MeshPerframeStorageBufferObjects[static_cast<size_t>(viewport_type)];
    m_MeshPerframeStorageBufferObject.show_skybox =
        m_IsShowSkybox[static_cast<size_t>(viewport_type)] ? 1U : 0U;

    auto render_scene = GET_SYSTEM(RenderSystem)->getRenderScene();
    if (render_scene)
    {
        m_ActiveMainCameraVisibleMeshNodes = &render_scene->GetMainCameraVisibleMeshNodes(viewport_type);
    }
    else
    {
        m_ActiveMainCameraVisibleMeshNodes = m_VisiableNodes.p_main_camera_visible_mesh_nodes;
    }
}

RHIPipeline* MainCameraPass::GetOrCreateMeshGBufferPipeline(const VulkanPBRMaterial& material)
{
    RHIPipeline* const default_pipeline = m_RenderPipelines[_render_pipeline_type_mesh_gbuffer].pipeline;
    if (!CanUseRuntimePrimaryShaderPass(m_Rhi, material))
    {
        return default_pipeline;
    }

    const MainCameraPass::MeshGBufferPipelineKey pipeline_key = BuildPipelineKey(material);

    if (const auto pipeline_it = m_MeshGbufferMaterialPipelines.find(pipeline_key);
        pipeline_it != m_MeshGbufferMaterialPipelines.end())
    {
        return pipeline_it->second != nullptr ? pipeline_it->second : default_pipeline;
    }

    std::vector<std::string> include_paths;
    if (!pipeline_key.include_directory.empty())
    {
        include_paths.push_back(pipeline_key.include_directory);
    }

    std::vector<uint8_t> vertex_binary;
    std::vector<uint8_t> fragment_binary;
    RHIShader* vert_shader_module = m_Rhi->CreateShaderModuleFromFile(
        pipeline_key.vertex_shader_file,
        ShaderStage::Vertex,
        include_paths,
        pipeline_key.shader_macros,
        vertex_binary,
        pipeline_key.vertex_entry);
    RHIShader* frag_shader_module = m_Rhi->CreateShaderModuleFromFile(
        pipeline_key.fragment_shader_file,
        ShaderStage::Fragment,
        include_paths,
        pipeline_key.shader_macros,
        fragment_binary,
        pipeline_key.fragment_entry);

    if (vert_shader_module == nullptr || frag_shader_module == nullptr)
    {
        if (vert_shader_module != nullptr)
        {
            m_Rhi->DestroyShaderModule(vert_shader_module);
        }
        if (frag_shader_module != nullptr)
        {
            m_Rhi->DestroyShaderModule(frag_shader_module);
        }

        LOG_WARNING(ZRender,
                    "fallback to built-in mesh gbuffer pipeline for shader '{}', vert='{}', frag='{}'",
                    material.shader_name,
                    pipeline_key.vertex_shader_file,
                    pipeline_key.fragment_shader_file);
        m_MeshGbufferMaterialPipelines[pipeline_key] = default_pipeline;
        return default_pipeline;
    }

    RHIPipelineShaderStageCreateInfo vert_pipeline_shader_stage_create_info {};
    vert_pipeline_shader_stage_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vert_pipeline_shader_stage_create_info.stage = RHI_SHADER_STAGE_VERTEX_BIT;
    vert_pipeline_shader_stage_create_info.module = vert_shader_module;
    vert_pipeline_shader_stage_create_info.pName = pipeline_key.vertex_entry.c_str();

    RHIPipelineShaderStageCreateInfo frag_pipeline_shader_stage_create_info {};
    frag_pipeline_shader_stage_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    frag_pipeline_shader_stage_create_info.stage = RHI_SHADER_STAGE_FRAGMENT_BIT;
    frag_pipeline_shader_stage_create_info.module = frag_shader_module;
    frag_pipeline_shader_stage_create_info.pName = pipeline_key.fragment_entry.c_str();

    RHIPipelineShaderStageCreateInfo shader_stages[] = {vert_pipeline_shader_stage_create_info,
                                                        frag_pipeline_shader_stage_create_info};

    auto vertex_binding_descriptions = MeshVertex::GetBindingDescriptions();
    auto vertex_attribute_descriptions = MeshVertex::GetAttributeDescriptions();
    RHIPipelineVertexInputStateCreateInfo vertex_input_state_create_info {};
    vertex_input_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input_state_create_info.vertexBindingDescriptionCount = static_cast<uint32_t>(vertex_binding_descriptions.size());
    vertex_input_state_create_info.pVertexBindingDescriptions = vertex_binding_descriptions.data();
    vertex_input_state_create_info.vertexAttributeDescriptionCount =
        static_cast<uint32_t>(vertex_attribute_descriptions.size());
    vertex_input_state_create_info.pVertexAttributeDescriptions = vertex_attribute_descriptions.data();

    RHIPipelineInputAssemblyStateCreateInfo input_assembly_create_info {};
    input_assembly_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly_create_info.topology = RHI_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    input_assembly_create_info.primitiveRestartEnable = RHI_FALSE;

    RHIPipelineViewportStateCreateInfo viewport_state_create_info {};
    viewport_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state_create_info.viewportCount = 1;
    viewport_state_create_info.pViewports = m_Rhi->GetSwapchainInfo().viewport;
    viewport_state_create_info.scissorCount = 1;
    viewport_state_create_info.pScissors = m_Rhi->GetSwapchainInfo().scissor;

    RHIPipelineRasterizationStateCreateInfo rasterization_state_create_info {};
    rasterization_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization_state_create_info.depthClampEnable = RHI_FALSE;
    rasterization_state_create_info.rasterizerDiscardEnable = RHI_FALSE;
    rasterization_state_create_info.polygonMode = RHI_POLYGON_MODE_FILL;
    rasterization_state_create_info.lineWidth = 1.0f;
    rasterization_state_create_info.cullMode = ParseCullMode(pipeline_key.cull);
    rasterization_state_create_info.frontFace = RHI_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterization_state_create_info.depthBiasEnable = RHI_FALSE;
    rasterization_state_create_info.depthBiasConstantFactor = 0.0f;
    rasterization_state_create_info.depthBiasClamp = 0.0f;
    rasterization_state_create_info.depthBiasSlopeFactor = 0.0f;

    RHIPipelineMultisampleStateCreateInfo multisample_state_create_info {};
    multisample_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample_state_create_info.sampleShadingEnable = RHI_FALSE;
    multisample_state_create_info.rasterizationSamples = RHI_SAMPLE_COUNT_1_BIT;

    std::array<RHIPipelineColorBlendAttachmentState, 3> color_blend_attachments {};
    ApplyBlendMode(pipeline_key.blend, color_blend_attachments);

    RHIPipelineColorBlendStateCreateInfo color_blend_state_create_info {};
    color_blend_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    color_blend_state_create_info.logicOpEnable = RHI_FALSE;
    color_blend_state_create_info.logicOp = RHI_LOGIC_OP_COPY;
    color_blend_state_create_info.attachmentCount = static_cast<uint32_t>(color_blend_attachments.size());
    color_blend_state_create_info.pAttachments = color_blend_attachments.data();
    color_blend_state_create_info.blendConstants[0] = 0.0f;
    color_blend_state_create_info.blendConstants[1] = 0.0f;
    color_blend_state_create_info.blendConstants[2] = 0.0f;
    color_blend_state_create_info.blendConstants[3] = 0.0f;

    RHIPipelineDepthStencilStateCreateInfo depth_stencil_create_info {};
    depth_stencil_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_stencil_create_info.depthTestEnable = RHI_TRUE;
    depth_stencil_create_info.depthWriteEnable = pipeline_key.zwrite ? RHI_TRUE : RHI_FALSE;
    depth_stencil_create_info.depthCompareOp = ParseCompareOp(pipeline_key.ztest);
    depth_stencil_create_info.depthBoundsTestEnable = RHI_FALSE;
    depth_stencil_create_info.stencilTestEnable = RHI_FALSE;

    RHIDynamicState dynamic_states[] = {RHI_DYNAMIC_STATE_VIEWPORT, RHI_DYNAMIC_STATE_SCISSOR};
    RHIPipelineDynamicStateCreateInfo dynamic_state_create_info {};
    dynamic_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic_state_create_info.dynamicStateCount = 2;
    dynamic_state_create_info.pDynamicStates = dynamic_states;

    RHIGraphicsPipelineCreateInfo pipeline_info {};
    pipeline_info.sType = RHI_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_info.stageCount = 2;
    pipeline_info.pStages = shader_stages;
    pipeline_info.pVertexInputState = &vertex_input_state_create_info;
    pipeline_info.pInputAssemblyState = &input_assembly_create_info;
    pipeline_info.pViewportState = &viewport_state_create_info;
    pipeline_info.pRasterizationState = &rasterization_state_create_info;
    pipeline_info.pMultisampleState = &multisample_state_create_info;
    pipeline_info.pColorBlendState = &color_blend_state_create_info;
    pipeline_info.pDepthStencilState = &depth_stencil_create_info;
    pipeline_info.layout = m_RenderPipelines[_render_pipeline_type_mesh_gbuffer].layout;
    pipeline_info.renderPass = m_Framebuffer1.render_pass;  // RP1
    pipeline_info.subpass = _main_camera_subpass_basepass;
    pipeline_info.basePipelineHandle = RHI_NULL_HANDLE;
    pipeline_info.pDynamicState = &dynamic_state_create_info;

    RHIPipeline* material_pipeline = RHI_NULL_HANDLE;
    if (RHI_SUCCESS != m_Rhi->CreateGraphicsPipelines(RHI_NULL_HANDLE, 1, &pipeline_info, material_pipeline))
    {
        LOG_WARNING(ZRender,
                    "failed to create runtime mesh gbuffer pipeline for shader '{}', fallback to built-in pipeline",
                    material.shader_name);
        material_pipeline = default_pipeline;
    }

    m_Rhi->DestroyShaderModule(vert_shader_module);
    m_Rhi->DestroyShaderModule(frag_shader_module);
    m_MeshGbufferMaterialPipelines[pipeline_key] = material_pipeline != nullptr ? material_pipeline : default_pipeline;
    return material_pipeline != nullptr ? material_pipeline : default_pipeline;
}

RHIPipeline* MainCameraPass::GetOrCreateMeshTransparentPipeline(const VulkanPBRMaterial& material)
{
    RHIPipeline* const default_pipeline = m_RenderPipelines[_render_pipeline_type_mesh_transparent].pipeline;
    const VulkanShaderPassData* const transparent_pass = FindTransparentShaderPass(material);
    if (!CanUseRuntimeShaderPass(m_Rhi, material, transparent_pass))
    {
        return default_pipeline;
    }

    const MainCameraPass::MeshGBufferPipelineKey pipeline_key = BuildPipelineKey(material, *transparent_pass);
    if (const auto pipeline_it = m_MeshTransparentMaterialPipelines.find(pipeline_key);
        pipeline_it != m_MeshTransparentMaterialPipelines.end())
    {
        return pipeline_it->second != nullptr ? pipeline_it->second : default_pipeline;
    }

    std::vector<std::string> include_paths;
    if (!pipeline_key.include_directory.empty())
    {
        include_paths.push_back(pipeline_key.include_directory);
    }

    std::vector<uint8_t> vertex_binary;
    std::vector<uint8_t> fragment_binary;
    RHIShader* vert_shader_module = m_Rhi->CreateShaderModuleFromFile(
        pipeline_key.vertex_shader_file,
        ShaderStage::Vertex,
        include_paths,
        pipeline_key.shader_macros,
        vertex_binary,
        pipeline_key.vertex_entry);
    RHIShader* frag_shader_module = m_Rhi->CreateShaderModuleFromFile(
        pipeline_key.fragment_shader_file,
        ShaderStage::Fragment,
        include_paths,
        pipeline_key.shader_macros,
        fragment_binary,
        pipeline_key.fragment_entry);
    if (vert_shader_module == nullptr || frag_shader_module == nullptr)
    {
        if (vert_shader_module != nullptr)
        {
            m_Rhi->DestroyShaderModule(vert_shader_module);
        }
        if (frag_shader_module != nullptr)
        {
            m_Rhi->DestroyShaderModule(frag_shader_module);
        }

        LOG_WARNING(ZRender,
                    "fallback to built-in mesh transparent pipeline for shader '{}', vert='{}', frag='{}'",
                    material.shader_name,
                    pipeline_key.vertex_shader_file,
                    pipeline_key.fragment_shader_file);
        m_MeshTransparentMaterialPipelines[pipeline_key] = default_pipeline;
        return default_pipeline;
    }

    RHIPipelineShaderStageCreateInfo vert_pipeline_shader_stage_create_info {};
    vert_pipeline_shader_stage_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vert_pipeline_shader_stage_create_info.stage = RHI_SHADER_STAGE_VERTEX_BIT;
    vert_pipeline_shader_stage_create_info.module = vert_shader_module;
    vert_pipeline_shader_stage_create_info.pName = pipeline_key.vertex_entry.c_str();

    RHIPipelineShaderStageCreateInfo frag_pipeline_shader_stage_create_info {};
    frag_pipeline_shader_stage_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    frag_pipeline_shader_stage_create_info.stage = RHI_SHADER_STAGE_FRAGMENT_BIT;
    frag_pipeline_shader_stage_create_info.module = frag_shader_module;
    frag_pipeline_shader_stage_create_info.pName = pipeline_key.fragment_entry.c_str();

    RHIPipelineShaderStageCreateInfo shader_stages[] = {vert_pipeline_shader_stage_create_info,
                                                        frag_pipeline_shader_stage_create_info};

    auto vertex_binding_descriptions = MeshVertex::GetBindingDescriptions();
    auto vertex_attribute_descriptions = MeshVertex::GetAttributeDescriptions();
    RHIPipelineVertexInputStateCreateInfo vertex_input_state_create_info {};
    vertex_input_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input_state_create_info.vertexBindingDescriptionCount = static_cast<uint32_t>(vertex_binding_descriptions.size());
    vertex_input_state_create_info.pVertexBindingDescriptions = vertex_binding_descriptions.data();
    vertex_input_state_create_info.vertexAttributeDescriptionCount =
        static_cast<uint32_t>(vertex_attribute_descriptions.size());
    vertex_input_state_create_info.pVertexAttributeDescriptions = vertex_attribute_descriptions.data();

    RHIPipelineInputAssemblyStateCreateInfo input_assembly_create_info {};
    input_assembly_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly_create_info.topology = RHI_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    input_assembly_create_info.primitiveRestartEnable = RHI_FALSE;

    RHIPipelineViewportStateCreateInfo viewport_state_create_info {};
    viewport_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state_create_info.viewportCount = 1;
    viewport_state_create_info.pViewports = m_Rhi->GetSwapchainInfo().viewport;
    viewport_state_create_info.scissorCount = 1;
    viewport_state_create_info.pScissors = m_Rhi->GetSwapchainInfo().scissor;

    RHIPipelineRasterizationStateCreateInfo rasterization_state_create_info {};
    rasterization_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization_state_create_info.depthClampEnable = RHI_FALSE;
    rasterization_state_create_info.rasterizerDiscardEnable = RHI_FALSE;
    rasterization_state_create_info.polygonMode = RHI_POLYGON_MODE_FILL;
    rasterization_state_create_info.lineWidth = 1.0f;
    rasterization_state_create_info.cullMode = ParseCullMode(pipeline_key.cull);
    rasterization_state_create_info.frontFace = RHI_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterization_state_create_info.depthBiasEnable = RHI_FALSE;
    rasterization_state_create_info.depthBiasConstantFactor = 0.0f;
    rasterization_state_create_info.depthBiasClamp = 0.0f;
    rasterization_state_create_info.depthBiasSlopeFactor = 0.0f;

    RHIPipelineMultisampleStateCreateInfo multisample_state_create_info {};
    multisample_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample_state_create_info.sampleShadingEnable = RHI_FALSE;
    multisample_state_create_info.rasterizationSamples = RHI_SAMPLE_COUNT_1_BIT;

    std::array<RHIPipelineColorBlendAttachmentState, 1> color_blend_attachments {};
    ApplyBlendMode(pipeline_key.blend, color_blend_attachments);

    RHIPipelineColorBlendStateCreateInfo color_blend_state_create_info {};
    color_blend_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    color_blend_state_create_info.logicOpEnable = RHI_FALSE;
    color_blend_state_create_info.logicOp = RHI_LOGIC_OP_COPY;
    color_blend_state_create_info.attachmentCount = static_cast<uint32_t>(color_blend_attachments.size());
    color_blend_state_create_info.pAttachments = color_blend_attachments.data();
    color_blend_state_create_info.blendConstants[0] = 0.0f;
    color_blend_state_create_info.blendConstants[1] = 0.0f;
    color_blend_state_create_info.blendConstants[2] = 0.0f;
    color_blend_state_create_info.blendConstants[3] = 0.0f;

    RHIPipelineDepthStencilStateCreateInfo depth_stencil_create_info {};
    depth_stencil_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_stencil_create_info.depthTestEnable = RHI_TRUE;
    depth_stencil_create_info.depthWriteEnable = pipeline_key.zwrite ? RHI_TRUE : RHI_FALSE;
    depth_stencil_create_info.depthCompareOp = ParseCompareOp(pipeline_key.ztest);
    depth_stencil_create_info.depthBoundsTestEnable = RHI_FALSE;
    depth_stencil_create_info.stencilTestEnable = RHI_FALSE;

    RHIDynamicState dynamic_states[] = {RHI_DYNAMIC_STATE_VIEWPORT, RHI_DYNAMIC_STATE_SCISSOR};
    RHIPipelineDynamicStateCreateInfo dynamic_state_create_info {};
    dynamic_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic_state_create_info.dynamicStateCount = 2;
    dynamic_state_create_info.pDynamicStates = dynamic_states;

    RHIGraphicsPipelineCreateInfo pipeline_info {};
    pipeline_info.sType = RHI_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_info.stageCount = 2;
    pipeline_info.pStages = shader_stages;
    pipeline_info.pVertexInputState = &vertex_input_state_create_info;
    pipeline_info.pInputAssemblyState = &input_assembly_create_info;
    pipeline_info.pViewportState = &viewport_state_create_info;
    pipeline_info.pRasterizationState = &rasterization_state_create_info;
    pipeline_info.pMultisampleState = &multisample_state_create_info;
    pipeline_info.pColorBlendState = &color_blend_state_create_info;
    pipeline_info.pDepthStencilState = &depth_stencil_create_info;
    pipeline_info.layout = m_RenderPipelines[_render_pipeline_type_mesh_transparent].layout;
    pipeline_info.renderPass = m_Framebuffer1.render_pass;  // RP1
    pipeline_info.subpass = _main_camera_subpass_forward_lighting;
    pipeline_info.basePipelineHandle = RHI_NULL_HANDLE;
    pipeline_info.pDynamicState = &dynamic_state_create_info;

    RHIPipeline* material_pipeline = RHI_NULL_HANDLE;
    if (RHI_SUCCESS != m_Rhi->CreateGraphicsPipelines(RHI_NULL_HANDLE, 1, &pipeline_info, material_pipeline))
    {
        LOG_WARNING(ZRender,
                    "failed to create runtime mesh transparent pipeline for shader '{}', fallback to built-in pipeline",
                    material.shader_name);
        material_pipeline = default_pipeline;
    }

    m_Rhi->DestroyShaderModule(vert_shader_module);
    m_Rhi->DestroyShaderModule(frag_shader_module);
    m_MeshTransparentMaterialPipelines[pipeline_key] =
        material_pipeline != nullptr ? material_pipeline : default_pipeline;
    return material_pipeline != nullptr ? material_pipeline : default_pipeline;
}

void MainCameraPass::SetupAttachments()

{
    m_Framebuffer.attachments.resize(_main_camera_pass_custom_attachment_count +
                                     _main_camera_pass_post_process_attachment_count);

    m_Framebuffer.attachments[_main_camera_pass_gbuffer_a].format = RHI_FORMAT_R8G8B8A8_UNORM;
    m_Framebuffer.attachments[_main_camera_pass_gbuffer_b].format = RHI_FORMAT_R8G8B8A8_UNORM;
    m_Framebuffer.attachments[_main_camera_pass_gbuffer_c].format = RHI_FORMAT_R8G8B8A8_SRGB;
    m_Framebuffer.attachments[_main_camera_pass_backup_buffer_odd].format = RHI_FORMAT_R16G16B16A16_SFLOAT;
    m_Framebuffer.attachments[_main_camera_pass_backup_buffer_even].format = RHI_FORMAT_R16G16B16A16_SFLOAT;

    for (int buffer_index = 0; buffer_index < _main_camera_pass_custom_attachment_count; ++buffer_index)
    {
        if (buffer_index == _main_camera_pass_gbuffer_a)
        {
            m_Rhi->CreateImage(m_Rhi->GetSwapchainInfo().extent.width,
                               m_Rhi->GetSwapchainInfo().extent.height,
                               m_Framebuffer.attachments[_main_camera_pass_gbuffer_a].format,
                               RHI_IMAGE_TILING_OPTIMAL,
                               RHI_IMAGE_USAGE_INPUT_ATTACHMENT_BIT | RHI_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                   RHI_IMAGE_USAGE_TRANSFER_SRC_BIT,
                               RHI_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                               m_Framebuffer.attachments[_main_camera_pass_gbuffer_a].image,
                               m_Framebuffer.attachments[_main_camera_pass_gbuffer_a].mem,
                               0,
                               1,
                               1);
        }
        else
        {
            m_Rhi->CreateImage(m_Rhi->GetSwapchainInfo().extent.width,
                               m_Rhi->GetSwapchainInfo().extent.height,
                               m_Framebuffer.attachments[buffer_index].format,
                               RHI_IMAGE_TILING_OPTIMAL,
                               RHI_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | RHI_IMAGE_USAGE_INPUT_ATTACHMENT_BIT |
                                   RHI_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
                               RHI_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                               m_Framebuffer.attachments[buffer_index].image,
                               m_Framebuffer.attachments[buffer_index].mem,
                               0,
                               1,
                               1);
        }

        m_Rhi->CreateImageView(m_Framebuffer.attachments[buffer_index].image,
                               m_Framebuffer.attachments[buffer_index].format,
                               RHI_IMAGE_ASPECT_COLOR_BIT,
                               RHI_IMAGE_VIEW_TYPE_2D,
                               1,
                               1,
                               m_Framebuffer.attachments[buffer_index].view);
    }

    m_Framebuffer.attachments[_main_camera_pass_post_process_buffer_odd].format = RHI_FORMAT_R16G16B16A16_SFLOAT;
    m_Framebuffer.attachments[_main_camera_pass_post_process_buffer_even].format = RHI_FORMAT_R16G16B16A16_SFLOAT;
    for (int attachment_index = _main_camera_pass_custom_attachment_count;
         attachment_index < _main_camera_pass_custom_attachment_count + _main_camera_pass_post_process_attachment_count;
         ++attachment_index)
    {
        m_Rhi->CreateImage(m_Rhi->GetSwapchainInfo().extent.width,
                           m_Rhi->GetSwapchainInfo().extent.height,
                           m_Framebuffer.attachments[attachment_index].format,
                           RHI_IMAGE_TILING_OPTIMAL,
                           RHI_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | RHI_IMAGE_USAGE_INPUT_ATTACHMENT_BIT |
                               RHI_IMAGE_USAGE_SAMPLED_BIT,
                           RHI_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                           m_Framebuffer.attachments[attachment_index].image,
                           m_Framebuffer.attachments[attachment_index].mem,
                           0,
                           1,
                           1);

        m_Rhi->CreateImageView(m_Framebuffer.attachments[attachment_index].image,
                               m_Framebuffer.attachments[attachment_index].format,
                               RHI_IMAGE_ASPECT_COLOR_BIT,
                               RHI_IMAGE_VIEW_TYPE_2D,
                               1,
                               1,
                               m_Framebuffer.attachments[attachment_index].view);
    }
}

// PR-V4 part 2 dispatcher: builds RP1 (m_Framebuffer1.render_pass) +
// RP2 (m_Framebuffer.render_pass). The two RPs are functionally the
// same single-RP topology that used to live here, just sliced around
// the ex-tone_mapping subpass; BindlessTonemapPass now owns its own RP
// in between.
void MainCameraPass::SetupRenderPass()
{
    SetupRenderPass1();
    SetupRenderPass2();
}

// =====================================================================
// RP1: gbuffer + deferred lighting + forward lighting (3 subpasses).
// Attachments: [gbuffer_a, gbuffer_b, gbuffer_c, backup_odd, depth]
// (5 attachments). The order matches the global enum slice
// [_main_camera_pass_gbuffer_a .. _main_camera_pass_backup_buffer_odd]
// + depth, so the framebuffer attachment view list can be assembled
// directly from m_Framebuffer.attachments[] indices.
// =====================================================================
void MainCameraPass::SetupRenderPass1()
{
    enum
    {
        kRP1_GBufferA = 0,
        kRP1_GBufferB = 1,
        kRP1_GBufferC = 2,
        kRP1_BackupOdd = 3,
        kRP1_Depth = 4,
        kRP1_AttachmentCount = 5,
    };

    RHIAttachmentDescription attachments[kRP1_AttachmentCount] = {};

    RHIAttachmentDescription& gbuffer_normal_attachment_description = attachments[kRP1_GBufferA];
    gbuffer_normal_attachment_description.format = m_Framebuffer.attachments[_main_camera_pass_gbuffer_a].format;
    gbuffer_normal_attachment_description.samples = RHI_SAMPLE_COUNT_1_BIT;
    gbuffer_normal_attachment_description.loadOp = RHI_ATTACHMENT_LOAD_OP_CLEAR;
    gbuffer_normal_attachment_description.storeOp = RHI_ATTACHMENT_STORE_OP_STORE;
    gbuffer_normal_attachment_description.stencilLoadOp = RHI_ATTACHMENT_LOAD_OP_DONT_CARE;
    gbuffer_normal_attachment_description.stencilStoreOp = RHI_ATTACHMENT_STORE_OP_DONT_CARE;
    gbuffer_normal_attachment_description.initialLayout = RHI_IMAGE_LAYOUT_UNDEFINED;
    gbuffer_normal_attachment_description.finalLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    RHIAttachmentDescription& gbuffer_metallic_roughness_shadingmodeid_attachment_description =
        attachments[kRP1_GBufferB];
    gbuffer_metallic_roughness_shadingmodeid_attachment_description.format =
        m_Framebuffer.attachments[_main_camera_pass_gbuffer_b].format;
    gbuffer_metallic_roughness_shadingmodeid_attachment_description.samples = RHI_SAMPLE_COUNT_1_BIT;
    gbuffer_metallic_roughness_shadingmodeid_attachment_description.loadOp = RHI_ATTACHMENT_LOAD_OP_CLEAR;
    gbuffer_metallic_roughness_shadingmodeid_attachment_description.storeOp = RHI_ATTACHMENT_STORE_OP_DONT_CARE;
    gbuffer_metallic_roughness_shadingmodeid_attachment_description.stencilLoadOp = RHI_ATTACHMENT_LOAD_OP_DONT_CARE;
    gbuffer_metallic_roughness_shadingmodeid_attachment_description.stencilStoreOp = RHI_ATTACHMENT_STORE_OP_DONT_CARE;
    gbuffer_metallic_roughness_shadingmodeid_attachment_description.initialLayout = RHI_IMAGE_LAYOUT_UNDEFINED;
    gbuffer_metallic_roughness_shadingmodeid_attachment_description.finalLayout =
        RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    RHIAttachmentDescription& gbuffer_albedo_attachment_description = attachments[kRP1_GBufferC];
    gbuffer_albedo_attachment_description.format = m_Framebuffer.attachments[_main_camera_pass_gbuffer_c].format;
    gbuffer_albedo_attachment_description.samples = RHI_SAMPLE_COUNT_1_BIT;
    gbuffer_albedo_attachment_description.loadOp = RHI_ATTACHMENT_LOAD_OP_CLEAR;
    gbuffer_albedo_attachment_description.storeOp = RHI_ATTACHMENT_STORE_OP_DONT_CARE;
    gbuffer_albedo_attachment_description.stencilLoadOp = RHI_ATTACHMENT_LOAD_OP_DONT_CARE;
    gbuffer_albedo_attachment_description.stencilStoreOp = RHI_ATTACHMENT_STORE_OP_DONT_CARE;
    gbuffer_albedo_attachment_description.initialLayout = RHI_IMAGE_LAYOUT_UNDEFINED;
    gbuffer_albedo_attachment_description.finalLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    // RP1 must STORE backup_odd because BindlessTonemapPass samples it
    // after RP1 ends. finalLayout=SHADER_READ_ONLY hands it off ready
    // for the bindless sampler.
    RHIAttachmentDescription& backup_odd_color_attachment_description = attachments[kRP1_BackupOdd];
    backup_odd_color_attachment_description.format =
        m_Framebuffer.attachments[_main_camera_pass_backup_buffer_odd].format;
    backup_odd_color_attachment_description.samples = RHI_SAMPLE_COUNT_1_BIT;
    backup_odd_color_attachment_description.loadOp = RHI_ATTACHMENT_LOAD_OP_CLEAR;
    backup_odd_color_attachment_description.storeOp = RHI_ATTACHMENT_STORE_OP_STORE;
    backup_odd_color_attachment_description.stencilLoadOp = RHI_ATTACHMENT_LOAD_OP_DONT_CARE;
    backup_odd_color_attachment_description.stencilStoreOp = RHI_ATTACHMENT_STORE_OP_DONT_CARE;
    backup_odd_color_attachment_description.initialLayout = RHI_IMAGE_LAYOUT_UNDEFINED;
    backup_odd_color_attachment_description.finalLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    RHIAttachmentDescription& depth_attachment_description = attachments[kRP1_Depth];
    depth_attachment_description.format = m_Rhi->GetDepthImageInfo().depth_image_format;
    depth_attachment_description.samples = RHI_SAMPLE_COUNT_1_BIT;
    depth_attachment_description.loadOp = RHI_ATTACHMENT_LOAD_OP_CLEAR;
    depth_attachment_description.storeOp = RHI_ATTACHMENT_STORE_OP_STORE;
    depth_attachment_description.stencilLoadOp = RHI_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth_attachment_description.stencilStoreOp = RHI_ATTACHMENT_STORE_OP_DONT_CARE;
    depth_attachment_description.initialLayout = RHI_IMAGE_LAYOUT_UNDEFINED;
    depth_attachment_description.finalLayout = RHI_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    RHISubpassDescription subpasses[_main_camera_rp1_subpass_count] = {};

    RHIAttachmentReference base_pass_color_attachments_reference[3] = {};
    base_pass_color_attachments_reference[0].attachment = &gbuffer_normal_attachment_description - attachments;
    base_pass_color_attachments_reference[0].layout = RHI_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    base_pass_color_attachments_reference[1].attachment =
        &gbuffer_metallic_roughness_shadingmodeid_attachment_description - attachments;
    base_pass_color_attachments_reference[1].layout = RHI_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    base_pass_color_attachments_reference[2].attachment = &gbuffer_albedo_attachment_description - attachments;
    base_pass_color_attachments_reference[2].layout = RHI_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    RHIAttachmentReference base_pass_depth_attachment_reference {};
    base_pass_depth_attachment_reference.attachment = &depth_attachment_description - attachments;
    base_pass_depth_attachment_reference.layout = RHI_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    RHISubpassDescription& base_pass = subpasses[_main_camera_subpass_basepass];
    base_pass.pipelineBindPoint = RHI_PIPELINE_BIND_POINT_GRAPHICS;
    base_pass.colorAttachmentCount =
        sizeof(base_pass_color_attachments_reference) / sizeof(base_pass_color_attachments_reference[0]);
    base_pass.pColorAttachments = &base_pass_color_attachments_reference[0];
    base_pass.pDepthStencilAttachment = &base_pass_depth_attachment_reference;
    base_pass.preserveAttachmentCount = 0;
    base_pass.pPreserveAttachments = NULL;

    RHIAttachmentReference deferred_lighting_pass_input_attachments_reference[4] = {};
    deferred_lighting_pass_input_attachments_reference[0].attachment =
        &gbuffer_normal_attachment_description - attachments;
    deferred_lighting_pass_input_attachments_reference[0].layout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    deferred_lighting_pass_input_attachments_reference[1].attachment =
        &gbuffer_metallic_roughness_shadingmodeid_attachment_description - attachments;
    deferred_lighting_pass_input_attachments_reference[1].layout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    deferred_lighting_pass_input_attachments_reference[2].attachment =
        &gbuffer_albedo_attachment_description - attachments;
    deferred_lighting_pass_input_attachments_reference[2].layout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    deferred_lighting_pass_input_attachments_reference[3].attachment = &depth_attachment_description - attachments;
    deferred_lighting_pass_input_attachments_reference[3].layout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    RHIAttachmentReference deferred_lighting_pass_color_attachment_reference[1] = {};
    deferred_lighting_pass_color_attachment_reference[0].attachment =
        &backup_odd_color_attachment_description - attachments;
    deferred_lighting_pass_color_attachment_reference[0].layout = RHI_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    RHISubpassDescription& deferred_lighting_pass = subpasses[_main_camera_subpass_deferred_lighting];
    deferred_lighting_pass.pipelineBindPoint = RHI_PIPELINE_BIND_POINT_GRAPHICS;
    deferred_lighting_pass.inputAttachmentCount = sizeof(deferred_lighting_pass_input_attachments_reference) /
                                                  sizeof(deferred_lighting_pass_input_attachments_reference[0]);
    deferred_lighting_pass.pInputAttachments = &deferred_lighting_pass_input_attachments_reference[0];
    deferred_lighting_pass.colorAttachmentCount = sizeof(deferred_lighting_pass_color_attachment_reference) /
                                                  sizeof(deferred_lighting_pass_color_attachment_reference[0]);
    deferred_lighting_pass.pColorAttachments = &deferred_lighting_pass_color_attachment_reference[0];
    deferred_lighting_pass.pDepthStencilAttachment = NULL;
    deferred_lighting_pass.preserveAttachmentCount = 0;
    deferred_lighting_pass.pPreserveAttachments = NULL;

    RHIAttachmentReference forward_lighting_pass_color_attachments_reference[1] = {};
    forward_lighting_pass_color_attachments_reference[0].attachment =
        &backup_odd_color_attachment_description - attachments;
    forward_lighting_pass_color_attachments_reference[0].layout = RHI_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    RHIAttachmentReference forward_lighting_pass_depth_attachment_reference {};
    forward_lighting_pass_depth_attachment_reference.attachment = &depth_attachment_description - attachments;
    forward_lighting_pass_depth_attachment_reference.layout = RHI_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    RHISubpassDescription& forward_lighting_pass = subpasses[_main_camera_subpass_forward_lighting];
    forward_lighting_pass.pipelineBindPoint = RHI_PIPELINE_BIND_POINT_GRAPHICS;
    forward_lighting_pass.inputAttachmentCount = 0U;
    forward_lighting_pass.pInputAttachments = NULL;
    forward_lighting_pass.colorAttachmentCount = sizeof(forward_lighting_pass_color_attachments_reference) /
                                                 sizeof(forward_lighting_pass_color_attachments_reference[0]);
    forward_lighting_pass.pColorAttachments = &forward_lighting_pass_color_attachments_reference[0];
    forward_lighting_pass.pDepthStencilAttachment = &forward_lighting_pass_depth_attachment_reference;
    forward_lighting_pass.preserveAttachmentCount = 0;
    forward_lighting_pass.pPreserveAttachments = NULL;

    RHISubpassDependency dependencies[3] = {};

    RHISubpassDependency& deferred_lighting_pass_depend_on_shadow_map_pass = dependencies[0];
    deferred_lighting_pass_depend_on_shadow_map_pass.srcSubpass = RHI_SUBPASS_EXTERNAL;
    deferred_lighting_pass_depend_on_shadow_map_pass.dstSubpass = _main_camera_subpass_deferred_lighting;
    deferred_lighting_pass_depend_on_shadow_map_pass.srcStageMask = RHI_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deferred_lighting_pass_depend_on_shadow_map_pass.dstStageMask = RHI_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deferred_lighting_pass_depend_on_shadow_map_pass.srcAccessMask = RHI_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deferred_lighting_pass_depend_on_shadow_map_pass.dstAccessMask = RHI_ACCESS_SHADER_READ_BIT;
    deferred_lighting_pass_depend_on_shadow_map_pass.dependencyFlags = 0;  // NOT BY REGION

    RHISubpassDependency& deferred_lighting_pass_depend_on_base_pass = dependencies[1];
    deferred_lighting_pass_depend_on_base_pass.srcSubpass = _main_camera_subpass_basepass;
    deferred_lighting_pass_depend_on_base_pass.dstSubpass = _main_camera_subpass_deferred_lighting;
    deferred_lighting_pass_depend_on_base_pass.srcStageMask =
        RHI_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | RHI_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deferred_lighting_pass_depend_on_base_pass.dstStageMask =
        RHI_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | RHI_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deferred_lighting_pass_depend_on_base_pass.srcAccessMask =
        RHI_ACCESS_SHADER_WRITE_BIT | RHI_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deferred_lighting_pass_depend_on_base_pass.dstAccessMask =
        RHI_ACCESS_SHADER_READ_BIT | RHI_ACCESS_COLOR_ATTACHMENT_READ_BIT;
    deferred_lighting_pass_depend_on_base_pass.dependencyFlags = RHI_DEPENDENCY_BY_REGION_BIT;

    RHISubpassDependency& forward_lighting_pass_depend_on_deferred_lighting_pass = dependencies[2];
    forward_lighting_pass_depend_on_deferred_lighting_pass.srcSubpass = _main_camera_subpass_deferred_lighting;
    forward_lighting_pass_depend_on_deferred_lighting_pass.dstSubpass = _main_camera_subpass_forward_lighting;
    forward_lighting_pass_depend_on_deferred_lighting_pass.srcStageMask =
        RHI_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | RHI_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    forward_lighting_pass_depend_on_deferred_lighting_pass.dstStageMask =
        RHI_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | RHI_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    forward_lighting_pass_depend_on_deferred_lighting_pass.srcAccessMask =
        RHI_ACCESS_SHADER_WRITE_BIT | RHI_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    forward_lighting_pass_depend_on_deferred_lighting_pass.dstAccessMask =
        RHI_ACCESS_SHADER_READ_BIT | RHI_ACCESS_COLOR_ATTACHMENT_READ_BIT;
    forward_lighting_pass_depend_on_deferred_lighting_pass.dependencyFlags = RHI_DEPENDENCY_BY_REGION_BIT;

    RHIRenderPassCreateInfo renderpass_create_info {};
    renderpass_create_info.sType = RHI_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderpass_create_info.attachmentCount = (sizeof(attachments) / sizeof(attachments[0]));
    renderpass_create_info.pAttachments = attachments;
    renderpass_create_info.subpassCount = (sizeof(subpasses) / sizeof(subpasses[0]));
    renderpass_create_info.pSubpasses = subpasses;
    renderpass_create_info.dependencyCount = (sizeof(dependencies) / sizeof(dependencies[0]));
    renderpass_create_info.pDependencies = dependencies;

    if (m_Rhi->CreateRenderPass(&renderpass_create_info, m_Framebuffer1.render_pass) != RHI_SUCCESS)
    {
        throw std::runtime_error("failed to create main_camera RP1");
    }
}

// =====================================================================
// RP2: post-tonemap chain (color_grading, fxaa, ui, combine_ui).
// Attachments: [backup_odd, backup_even, post_process_odd, swapchain].
//
// IMPORTANT layout/loadOp choices:
//   * backup_odd / backup_even enter RP2 already in
//     SHADER_READ_ONLY_OPTIMAL (RP1 left backup_odd that way;
//     BindlessTonemapPass left backup_even that way). RP2 uses
//     LOAD_OP_LOAD on both and initialLayout=SHADER_READ_ONLY_OPTIMAL
//     to retain those contents.
//   * The fxaa subpass writes back into backup_odd, and the ui subpass
//     writes into backup_even, so both must allow color-attachment
//     transitions inside RP2.
//   * Swapchain image is the final color target (combine_ui).
// =====================================================================
void MainCameraPass::SetupRenderPass2()
{
    enum
    {
        kRP2_BackupOdd = 0,
        kRP2_BackupEven = 1,
        kRP2_PostOdd = 2,  // post_process_odd intermediate (used when FXAA on)
        kRP2_Swapchain = 3,
        kRP2_AttachmentCount = 4,
    };

    RHIAttachmentDescription attachments[kRP2_AttachmentCount] = {};

    // backup_odd: produced by RP1 (already SHADER_READ_ONLY); RP2's
    // fxaa writes into it again, then combine_ui samples it.
    RHIAttachmentDescription& backup_odd_color_attachment_description = attachments[kRP2_BackupOdd];
    backup_odd_color_attachment_description.format =
        m_Framebuffer.attachments[_main_camera_pass_backup_buffer_odd].format;
    backup_odd_color_attachment_description.samples = RHI_SAMPLE_COUNT_1_BIT;
    backup_odd_color_attachment_description.loadOp = RHI_ATTACHMENT_LOAD_OP_LOAD;
    backup_odd_color_attachment_description.storeOp = RHI_ATTACHMENT_STORE_OP_STORE;
    backup_odd_color_attachment_description.stencilLoadOp = RHI_ATTACHMENT_LOAD_OP_DONT_CARE;
    backup_odd_color_attachment_description.stencilStoreOp = RHI_ATTACHMENT_STORE_OP_DONT_CARE;
    backup_odd_color_attachment_description.initialLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    backup_odd_color_attachment_description.finalLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    // backup_even: produced by BindlessTonemapPass between RP1 and RP2.
    RHIAttachmentDescription& backup_even_color_attachment_description = attachments[kRP2_BackupEven];
    backup_even_color_attachment_description.format =
        m_Framebuffer.attachments[_main_camera_pass_backup_buffer_even].format;
    backup_even_color_attachment_description.samples = RHI_SAMPLE_COUNT_1_BIT;
    backup_even_color_attachment_description.loadOp = RHI_ATTACHMENT_LOAD_OP_LOAD;
    backup_even_color_attachment_description.storeOp = RHI_ATTACHMENT_STORE_OP_STORE;
    backup_even_color_attachment_description.stencilLoadOp = RHI_ATTACHMENT_LOAD_OP_DONT_CARE;
    backup_even_color_attachment_description.stencilStoreOp = RHI_ATTACHMENT_STORE_OP_DONT_CARE;
    backup_even_color_attachment_description.initialLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    backup_even_color_attachment_description.finalLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    // post_process_odd: only meaningfully written when FXAA is enabled
    // (color_grading -> fxaa intermediate). Cleared at start.
    RHIAttachmentDescription& post_process_odd_color_attachment_description = attachments[kRP2_PostOdd];
    post_process_odd_color_attachment_description.format =
        m_Framebuffer.attachments[_main_camera_pass_post_process_buffer_odd].format;
    post_process_odd_color_attachment_description.samples = RHI_SAMPLE_COUNT_1_BIT;
    post_process_odd_color_attachment_description.loadOp = RHI_ATTACHMENT_LOAD_OP_CLEAR;
    post_process_odd_color_attachment_description.storeOp = RHI_ATTACHMENT_STORE_OP_DONT_CARE;
    post_process_odd_color_attachment_description.stencilLoadOp = RHI_ATTACHMENT_LOAD_OP_DONT_CARE;
    post_process_odd_color_attachment_description.stencilStoreOp = RHI_ATTACHMENT_STORE_OP_DONT_CARE;
    post_process_odd_color_attachment_description.initialLayout = RHI_IMAGE_LAYOUT_UNDEFINED;
    post_process_odd_color_attachment_description.finalLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    // Swapchain image: per-FB (set per swapchain image at framebuffer creation).
    RHIAttachmentDescription& swapchain_image_attachment_description = attachments[kRP2_Swapchain];
    swapchain_image_attachment_description.format = m_Rhi->GetSwapchainInfo().image_format;
    swapchain_image_attachment_description.samples = RHI_SAMPLE_COUNT_1_BIT;
    swapchain_image_attachment_description.loadOp = RHI_ATTACHMENT_LOAD_OP_CLEAR;
    swapchain_image_attachment_description.storeOp = RHI_ATTACHMENT_STORE_OP_STORE;
    swapchain_image_attachment_description.stencilLoadOp = RHI_ATTACHMENT_LOAD_OP_DONT_CARE;
    swapchain_image_attachment_description.stencilStoreOp = RHI_ATTACHMENT_STORE_OP_DONT_CARE;
    swapchain_image_attachment_description.initialLayout = RHI_IMAGE_LAYOUT_UNDEFINED;
    swapchain_image_attachment_description.finalLayout = RHI_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    RHISubpassDescription subpasses[_main_camera_rp2_subpass_count] = {};

    // ---- subpass 0: color_grading ----
    // input: backup_even (BindlessTonemap result)
    // output: post_process_odd (when FXAA on) or backup_odd (FXAA off)
    RHIAttachmentReference color_grading_pass_input_attachment_reference {};
    color_grading_pass_input_attachment_reference.attachment = kRP2_BackupEven;
    color_grading_pass_input_attachment_reference.layout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    RHIAttachmentReference color_grading_pass_color_attachment_reference {};
    color_grading_pass_color_attachment_reference.attachment =
        m_EnableFxaa ? static_cast<uint32_t>(kRP2_PostOdd) : static_cast<uint32_t>(kRP2_BackupOdd);
    color_grading_pass_color_attachment_reference.layout = RHI_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    RHISubpassDescription& color_grading_pass = subpasses[_main_camera_subpass_color_grading];
    color_grading_pass.pipelineBindPoint = RHI_PIPELINE_BIND_POINT_GRAPHICS;
    color_grading_pass.inputAttachmentCount = 1;
    color_grading_pass.pInputAttachments = &color_grading_pass_input_attachment_reference;
    color_grading_pass.colorAttachmentCount = 1;
    color_grading_pass.pColorAttachments = &color_grading_pass_color_attachment_reference;
    color_grading_pass.pDepthStencilAttachment = NULL;
    color_grading_pass.preserveAttachmentCount = 0;
    color_grading_pass.pPreserveAttachments = NULL;

    // ---- subpass 1: fxaa ----
    RHIAttachmentReference fxaa_pass_input_attachment_reference {};
    fxaa_pass_input_attachment_reference.attachment =
        m_EnableFxaa ? static_cast<uint32_t>(kRP2_PostOdd) : static_cast<uint32_t>(kRP2_BackupEven);
    fxaa_pass_input_attachment_reference.layout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    RHIAttachmentReference fxaa_pass_color_attachment_reference {};
    fxaa_pass_color_attachment_reference.attachment = kRP2_BackupOdd;
    fxaa_pass_color_attachment_reference.layout = RHI_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    RHISubpassDescription& fxaa_pass = subpasses[_main_camera_subpass_fxaa];
    fxaa_pass.pipelineBindPoint = RHI_PIPELINE_BIND_POINT_GRAPHICS;
    fxaa_pass.inputAttachmentCount = 1;
    fxaa_pass.pInputAttachments = &fxaa_pass_input_attachment_reference;
    fxaa_pass.colorAttachmentCount = 1;
    fxaa_pass.pColorAttachments = &fxaa_pass_color_attachment_reference;
    fxaa_pass.pDepthStencilAttachment = NULL;
    fxaa_pass.preserveAttachmentCount = 0;
    fxaa_pass.pPreserveAttachments = NULL;

    // ---- subpass 2: ui (writes into backup_even, preserves backup_odd) ----
    RHIAttachmentReference ui_pass_color_attachment_reference {};
    ui_pass_color_attachment_reference.attachment = kRP2_BackupEven;
    ui_pass_color_attachment_reference.layout = RHI_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    uint32_t ui_pass_preserve_attachment = kRP2_BackupOdd;

    RHISubpassDescription& ui_pass = subpasses[_main_camera_subpass_ui];
    ui_pass.pipelineBindPoint = RHI_PIPELINE_BIND_POINT_GRAPHICS;
    ui_pass.inputAttachmentCount = 0;
    ui_pass.pInputAttachments = NULL;
    ui_pass.colorAttachmentCount = 1;
    ui_pass.pColorAttachments = &ui_pass_color_attachment_reference;
    ui_pass.pDepthStencilAttachment = NULL;
    ui_pass.preserveAttachmentCount = 1;
    ui_pass.pPreserveAttachments = &ui_pass_preserve_attachment;

    // ---- subpass 3: combine_ui (final to swapchain) ----
    RHIAttachmentReference combine_ui_pass_input_attachments_reference[2] = {};
    combine_ui_pass_input_attachments_reference[0].attachment = kRP2_BackupOdd;
    combine_ui_pass_input_attachments_reference[0].layout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    combine_ui_pass_input_attachments_reference[1].attachment = kRP2_BackupEven;
    combine_ui_pass_input_attachments_reference[1].layout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    RHIAttachmentReference combine_ui_pass_color_attachment_reference {};
    combine_ui_pass_color_attachment_reference.attachment = kRP2_Swapchain;
    combine_ui_pass_color_attachment_reference.layout = RHI_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    RHISubpassDescription& combine_ui_pass = subpasses[_main_camera_subpass_combine_ui];
    combine_ui_pass.pipelineBindPoint = RHI_PIPELINE_BIND_POINT_GRAPHICS;
    combine_ui_pass.inputAttachmentCount =
        sizeof(combine_ui_pass_input_attachments_reference) / sizeof(combine_ui_pass_input_attachments_reference[0]);
    combine_ui_pass.pInputAttachments = combine_ui_pass_input_attachments_reference;
    combine_ui_pass.colorAttachmentCount = 1;
    combine_ui_pass.pColorAttachments = &combine_ui_pass_color_attachment_reference;
    combine_ui_pass.pDepthStencilAttachment = NULL;
    combine_ui_pass.preserveAttachmentCount = 0;
    combine_ui_pass.pPreserveAttachments = NULL;

    // ---- dependencies ----
    RHISubpassDependency dependencies[5] = {};

    // dep[0]: external (BindlessTonemap fragment-shader write into backup_even)
    //         -> color_grading reads it as input attachment.
    RHISubpassDependency& color_grading_pass_depend_on_external = dependencies[0];
    color_grading_pass_depend_on_external.srcSubpass = RHI_SUBPASS_EXTERNAL;
    color_grading_pass_depend_on_external.dstSubpass = _main_camera_subpass_color_grading;
    color_grading_pass_depend_on_external.srcStageMask =
        RHI_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | RHI_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    color_grading_pass_depend_on_external.dstStageMask =
        RHI_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | RHI_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    color_grading_pass_depend_on_external.srcAccessMask =
        RHI_ACCESS_SHADER_WRITE_BIT | RHI_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    color_grading_pass_depend_on_external.dstAccessMask =
        RHI_ACCESS_SHADER_READ_BIT | RHI_ACCESS_COLOR_ATTACHMENT_READ_BIT;
    color_grading_pass_depend_on_external.dependencyFlags = RHI_DEPENDENCY_BY_REGION_BIT;

    RHISubpassDependency& fxaa_pass_depend_on_color_grading_pass = dependencies[1];
    fxaa_pass_depend_on_color_grading_pass.srcSubpass = _main_camera_subpass_color_grading;
    fxaa_pass_depend_on_color_grading_pass.dstSubpass = _main_camera_subpass_fxaa;
    fxaa_pass_depend_on_color_grading_pass.srcStageMask =
        RHI_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | RHI_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    fxaa_pass_depend_on_color_grading_pass.dstStageMask =
        RHI_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | RHI_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    fxaa_pass_depend_on_color_grading_pass.srcAccessMask =
        RHI_ACCESS_SHADER_WRITE_BIT | RHI_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    fxaa_pass_depend_on_color_grading_pass.dstAccessMask =
        RHI_ACCESS_SHADER_READ_BIT | RHI_ACCESS_COLOR_ATTACHMENT_READ_BIT;
    fxaa_pass_depend_on_color_grading_pass.dependencyFlags = RHI_DEPENDENCY_BY_REGION_BIT;

    RHISubpassDependency& ui_pass_depend_on_fxaa_pass = dependencies[2];
    ui_pass_depend_on_fxaa_pass.srcSubpass = _main_camera_subpass_fxaa;
    ui_pass_depend_on_fxaa_pass.dstSubpass = _main_camera_subpass_ui;
    ui_pass_depend_on_fxaa_pass.srcStageMask =
        RHI_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | RHI_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    ui_pass_depend_on_fxaa_pass.dstStageMask =
        RHI_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | RHI_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    ui_pass_depend_on_fxaa_pass.srcAccessMask = RHI_ACCESS_SHADER_WRITE_BIT | RHI_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    ui_pass_depend_on_fxaa_pass.dstAccessMask = RHI_ACCESS_SHADER_READ_BIT | RHI_ACCESS_COLOR_ATTACHMENT_READ_BIT;
    ui_pass_depend_on_fxaa_pass.dependencyFlags = RHI_DEPENDENCY_BY_REGION_BIT;

    RHISubpassDependency& combine_ui_pass_depend_on_ui_pass = dependencies[3];
    combine_ui_pass_depend_on_ui_pass.srcSubpass = _main_camera_subpass_ui;
    combine_ui_pass_depend_on_ui_pass.dstSubpass = _main_camera_subpass_combine_ui;
    combine_ui_pass_depend_on_ui_pass.srcStageMask =
        RHI_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | RHI_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    combine_ui_pass_depend_on_ui_pass.dstStageMask =
        RHI_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | RHI_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    combine_ui_pass_depend_on_ui_pass.srcAccessMask =
        RHI_ACCESS_SHADER_WRITE_BIT | RHI_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    combine_ui_pass_depend_on_ui_pass.dstAccessMask = RHI_ACCESS_SHADER_READ_BIT | RHI_ACCESS_COLOR_ATTACHMENT_READ_BIT;
    combine_ui_pass_depend_on_ui_pass.dependencyFlags = RHI_DEPENDENCY_BY_REGION_BIT;

    // dep[4]: external -> combine_ui covers backup_odd that came from RP1
    // (when FXAA is disabled and color_grading didn't touch it).
    RHISubpassDependency& combine_ui_pass_depend_on_external = dependencies[4];
    combine_ui_pass_depend_on_external.srcSubpass = RHI_SUBPASS_EXTERNAL;
    combine_ui_pass_depend_on_external.dstSubpass = _main_camera_subpass_combine_ui;
    combine_ui_pass_depend_on_external.srcStageMask =
        RHI_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | RHI_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    combine_ui_pass_depend_on_external.dstStageMask = RHI_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    combine_ui_pass_depend_on_external.srcAccessMask =
        RHI_ACCESS_SHADER_WRITE_BIT | RHI_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    combine_ui_pass_depend_on_external.dstAccessMask = RHI_ACCESS_SHADER_READ_BIT;
    combine_ui_pass_depend_on_external.dependencyFlags = RHI_DEPENDENCY_BY_REGION_BIT;

    RHIRenderPassCreateInfo renderpass_create_info {};
    renderpass_create_info.sType = RHI_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderpass_create_info.attachmentCount = (sizeof(attachments) / sizeof(attachments[0]));
    renderpass_create_info.pAttachments = attachments;
    renderpass_create_info.subpassCount = (sizeof(subpasses) / sizeof(subpasses[0]));
    renderpass_create_info.pSubpasses = subpasses;
    renderpass_create_info.dependencyCount = (sizeof(dependencies) / sizeof(dependencies[0]));
    renderpass_create_info.pDependencies = dependencies;

    if (m_Rhi->CreateRenderPass(&renderpass_create_info, m_Framebuffer.render_pass) != RHI_SUCCESS)
    {
        throw std::runtime_error("failed to create main_camera RP2");
    }
}

void MainCameraPass::SetupDescriptorSetLayout()
{
    m_DescriptorInfos.resize(_layout_type_count);

    {
        RHIDescriptorSetLayoutBinding mesh_mesh_layout_bindings[1];

        RHIDescriptorSetLayoutBinding& mesh_mesh_layout_uniform_buffer_binding = mesh_mesh_layout_bindings[0];
        mesh_mesh_layout_uniform_buffer_binding.binding = 0;
        mesh_mesh_layout_uniform_buffer_binding.descriptorType = RHI_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        mesh_mesh_layout_uniform_buffer_binding.descriptorCount = 1;
        mesh_mesh_layout_uniform_buffer_binding.stageFlags = RHI_SHADER_STAGE_VERTEX_BIT;
        mesh_mesh_layout_uniform_buffer_binding.pImmutableSamplers = NULL;

        RHIDescriptorSetLayoutCreateInfo mesh_mesh_layout_create_info {};
        mesh_mesh_layout_create_info.sType = RHI_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        mesh_mesh_layout_create_info.bindingCount = 1;
        mesh_mesh_layout_create_info.pBindings = mesh_mesh_layout_bindings;

        if (m_Rhi->CreateDescriptorSetLayout(&mesh_mesh_layout_create_info, m_DescriptorInfos[_per_mesh].layout) !=
            RHI_SUCCESS)
        {
            throw std::runtime_error("create mesh mesh layout");
        }
    }

    {
        RHIDescriptorSetLayoutBinding mesh_global_layout_bindings[13];

        RHIDescriptorSetLayoutBinding& mesh_global_layout_perframe_storage_buffer_binding =
            mesh_global_layout_bindings[0];
        mesh_global_layout_perframe_storage_buffer_binding.binding = 0;
        mesh_global_layout_perframe_storage_buffer_binding.descriptorType = RHI_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
        mesh_global_layout_perframe_storage_buffer_binding.descriptorCount = 1;
        mesh_global_layout_perframe_storage_buffer_binding.stageFlags =
            RHI_SHADER_STAGE_VERTEX_BIT | RHI_SHADER_STAGE_FRAGMENT_BIT;
        mesh_global_layout_perframe_storage_buffer_binding.pImmutableSamplers = NULL;

        RHIDescriptorSetLayoutBinding& mesh_global_layout_perdrawcall_storage_buffer_binding =
            mesh_global_layout_bindings[1];
        mesh_global_layout_perdrawcall_storage_buffer_binding.binding = 1;
        mesh_global_layout_perdrawcall_storage_buffer_binding.descriptorType =
            RHI_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
        mesh_global_layout_perdrawcall_storage_buffer_binding.descriptorCount = 1;
        mesh_global_layout_perdrawcall_storage_buffer_binding.stageFlags = RHI_SHADER_STAGE_VERTEX_BIT;
        mesh_global_layout_perdrawcall_storage_buffer_binding.pImmutableSamplers = NULL;

        RHIDescriptorSetLayoutBinding& mesh_global_layout_per_drawcall_vertex_blending_storage_buffer_binding =
            mesh_global_layout_bindings[2];
        mesh_global_layout_per_drawcall_vertex_blending_storage_buffer_binding.binding = 2;
        mesh_global_layout_per_drawcall_vertex_blending_storage_buffer_binding.descriptorType =
            RHI_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
        mesh_global_layout_per_drawcall_vertex_blending_storage_buffer_binding.descriptorCount = 1;
        mesh_global_layout_per_drawcall_vertex_blending_storage_buffer_binding.stageFlags = RHI_SHADER_STAGE_VERTEX_BIT;
        mesh_global_layout_per_drawcall_vertex_blending_storage_buffer_binding.pImmutableSamplers = NULL;

        RHIDescriptorSetLayoutBinding& mesh_global_layout_brdfLUT_texture_binding = mesh_global_layout_bindings[3];
        mesh_global_layout_brdfLUT_texture_binding.binding = 3;
        mesh_global_layout_brdfLUT_texture_binding.descriptorType = RHI_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        mesh_global_layout_brdfLUT_texture_binding.descriptorCount = 1;
        mesh_global_layout_brdfLUT_texture_binding.stageFlags = RHI_SHADER_STAGE_FRAGMENT_BIT;
        mesh_global_layout_brdfLUT_texture_binding.pImmutableSamplers = NULL;

        RHIDescriptorSetLayoutBinding& mesh_global_layout_irradiance_texture_binding = mesh_global_layout_bindings[4];
        mesh_global_layout_irradiance_texture_binding = mesh_global_layout_brdfLUT_texture_binding;
        mesh_global_layout_irradiance_texture_binding.binding = 4;

        RHIDescriptorSetLayoutBinding& mesh_global_layout_specular_texture_binding = mesh_global_layout_bindings[5];
        mesh_global_layout_specular_texture_binding = mesh_global_layout_brdfLUT_texture_binding;
        mesh_global_layout_specular_texture_binding.binding = 5;

        RHIDescriptorSetLayoutBinding& mesh_global_layout_point_light_shadow_texture_binding =
            mesh_global_layout_bindings[6];
        mesh_global_layout_point_light_shadow_texture_binding = mesh_global_layout_brdfLUT_texture_binding;
        mesh_global_layout_point_light_shadow_texture_binding.binding = 6;

        RHIDescriptorSetLayoutBinding& mesh_global_layout_directional_light_shadow_texture_binding =
            mesh_global_layout_bindings[7];
        mesh_global_layout_directional_light_shadow_texture_binding = mesh_global_layout_brdfLUT_texture_binding;
        mesh_global_layout_directional_light_shadow_texture_binding.binding = 7;

        RHIDescriptorSetLayoutBinding& mesh_global_layout_megalights_lights_binding = mesh_global_layout_bindings[8];
        mesh_global_layout_megalights_lights_binding.binding = 8;
        mesh_global_layout_megalights_lights_binding.descriptorType = RHI_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        mesh_global_layout_megalights_lights_binding.descriptorCount = 1;
        mesh_global_layout_megalights_lights_binding.stageFlags = RHI_SHADER_STAGE_FRAGMENT_BIT;

        RHIDescriptorSetLayoutBinding& mesh_global_layout_megalights_indices_binding = mesh_global_layout_bindings[9];
        mesh_global_layout_megalights_indices_binding = mesh_global_layout_megalights_lights_binding;
        mesh_global_layout_megalights_indices_binding.binding = 9;

        RHIDescriptorSetLayoutBinding& mesh_global_layout_megalights_ranges_binding = mesh_global_layout_bindings[10];
        mesh_global_layout_megalights_ranges_binding = mesh_global_layout_megalights_lights_binding;
        mesh_global_layout_megalights_ranges_binding.binding = 10;

        RHIDescriptorSetLayoutBinding& mesh_global_layout_megalights_history_read_binding =
            mesh_global_layout_bindings[11];
        mesh_global_layout_megalights_history_read_binding.binding = 11;
        mesh_global_layout_megalights_history_read_binding.descriptorType = RHI_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        mesh_global_layout_megalights_history_read_binding.descriptorCount = 1;
        mesh_global_layout_megalights_history_read_binding.stageFlags = RHI_SHADER_STAGE_FRAGMENT_BIT;
        mesh_global_layout_megalights_history_read_binding.pImmutableSamplers = NULL;

        RHIDescriptorSetLayoutBinding& mesh_global_layout_megalights_history_write_binding =
            mesh_global_layout_bindings[12];
        mesh_global_layout_megalights_history_write_binding.binding = 12;
        mesh_global_layout_megalights_history_write_binding.descriptorType = RHI_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        mesh_global_layout_megalights_history_write_binding.descriptorCount = 1;
        mesh_global_layout_megalights_history_write_binding.stageFlags = RHI_SHADER_STAGE_FRAGMENT_BIT;
        mesh_global_layout_megalights_history_write_binding.pImmutableSamplers = NULL;

        RHIDescriptorSetLayoutCreateInfo mesh_global_layout_create_info;
        mesh_global_layout_create_info.sType = RHI_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        mesh_global_layout_create_info.pNext = NULL;
        mesh_global_layout_create_info.flags = 0;
        mesh_global_layout_create_info.bindingCount =
            (sizeof(mesh_global_layout_bindings) / sizeof(mesh_global_layout_bindings[0]));
        mesh_global_layout_create_info.pBindings = mesh_global_layout_bindings;

        if (RHI_SUCCESS !=
            m_Rhi->CreateDescriptorSetLayout(&mesh_global_layout_create_info, m_DescriptorInfos[_mesh_global].layout))
        {
            throw std::runtime_error("create mesh global layout");
        }
    }

    {
        RHIDescriptorSetLayoutBinding mesh_material_layout_bindings[6];

        // (set = 2, binding = 0 in fragment shader)
        RHIDescriptorSetLayoutBinding& mesh_material_layout_uniform_buffer_binding = mesh_material_layout_bindings[0];
        mesh_material_layout_uniform_buffer_binding.binding = 0;
        mesh_material_layout_uniform_buffer_binding.descriptorType = RHI_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        mesh_material_layout_uniform_buffer_binding.descriptorCount = 1;
        mesh_material_layout_uniform_buffer_binding.stageFlags = RHI_SHADER_STAGE_FRAGMENT_BIT;
        mesh_material_layout_uniform_buffer_binding.pImmutableSamplers = nullptr;

        // (set = 2, binding = 1 in fragment shader)
        RHIDescriptorSetLayoutBinding& mesh_material_layout_base_color_texture_binding =
            mesh_material_layout_bindings[1];
        mesh_material_layout_base_color_texture_binding.binding = 1;
        mesh_material_layout_base_color_texture_binding.descriptorType = RHI_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        mesh_material_layout_base_color_texture_binding.descriptorCount = 1;
        mesh_material_layout_base_color_texture_binding.stageFlags = RHI_SHADER_STAGE_FRAGMENT_BIT;
        mesh_material_layout_base_color_texture_binding.pImmutableSamplers = nullptr;

        // (set = 2, binding = 2 in fragment shader)
        RHIDescriptorSetLayoutBinding& mesh_material_layout_metallic_roughness_texture_binding =
            mesh_material_layout_bindings[2];
        mesh_material_layout_metallic_roughness_texture_binding = mesh_material_layout_base_color_texture_binding;
        mesh_material_layout_metallic_roughness_texture_binding.binding = 2;

        // (set = 2, binding = 3 in fragment shader)
        RHIDescriptorSetLayoutBinding& mesh_material_layout_normal_roughness_texture_binding =
            mesh_material_layout_bindings[3];
        mesh_material_layout_normal_roughness_texture_binding = mesh_material_layout_base_color_texture_binding;
        mesh_material_layout_normal_roughness_texture_binding.binding = 3;

        // (set = 2, binding = 4 in fragment shader)
        RHIDescriptorSetLayoutBinding& mesh_material_layout_occlusion_texture_binding =
            mesh_material_layout_bindings[4];
        mesh_material_layout_occlusion_texture_binding = mesh_material_layout_base_color_texture_binding;
        mesh_material_layout_occlusion_texture_binding.binding = 4;

        // (set = 2, binding = 5 in fragment shader)
        RHIDescriptorSetLayoutBinding& mesh_material_layout_emissive_texture_binding = mesh_material_layout_bindings[5];
        mesh_material_layout_emissive_texture_binding = mesh_material_layout_base_color_texture_binding;
        mesh_material_layout_emissive_texture_binding.binding = 5;

        RHIDescriptorSetLayoutCreateInfo mesh_material_layout_create_info;
        mesh_material_layout_create_info.sType = RHI_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        mesh_material_layout_create_info.pNext = NULL;
        mesh_material_layout_create_info.flags = 0;
        mesh_material_layout_create_info.bindingCount = 6;
        mesh_material_layout_create_info.pBindings = mesh_material_layout_bindings;

        if (m_Rhi->CreateDescriptorSetLayout(&mesh_material_layout_create_info,
                                             m_DescriptorInfos[_mesh_per_material].layout) != RHI_SUCCESS)
        {
            throw std::runtime_error("create mesh material layout");
        }
    }

    {
        RHIDescriptorSetLayoutBinding skybox_layout_bindings[2];

        RHIDescriptorSetLayoutBinding& skybox_layout_perframe_storage_buffer_binding = skybox_layout_bindings[0];
        skybox_layout_perframe_storage_buffer_binding.binding = 0;
        skybox_layout_perframe_storage_buffer_binding.descriptorType = RHI_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
        skybox_layout_perframe_storage_buffer_binding.descriptorCount = 1;
        skybox_layout_perframe_storage_buffer_binding.stageFlags = RHI_SHADER_STAGE_VERTEX_BIT;
        skybox_layout_perframe_storage_buffer_binding.pImmutableSamplers = NULL;

        RHIDescriptorSetLayoutBinding& skybox_layout_specular_texture_binding = skybox_layout_bindings[1];
        skybox_layout_specular_texture_binding.binding = 1;
        skybox_layout_specular_texture_binding.descriptorType = RHI_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        skybox_layout_specular_texture_binding.descriptorCount = 1;
        skybox_layout_specular_texture_binding.stageFlags = RHI_SHADER_STAGE_FRAGMENT_BIT;
        skybox_layout_specular_texture_binding.pImmutableSamplers = NULL;

        RHIDescriptorSetLayoutCreateInfo skybox_layout_create_info {};
        skybox_layout_create_info.sType = RHI_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        skybox_layout_create_info.bindingCount = 2;
        skybox_layout_create_info.pBindings = skybox_layout_bindings;

        if (RHI_SUCCESS !=
            m_Rhi->CreateDescriptorSetLayout(&skybox_layout_create_info, m_DescriptorInfos[_skybox].layout))
        {
            throw std::runtime_error("create skybox layout");
        }
    }

    {
        RHIDescriptorSetLayoutBinding axis_layout_bindings[2];

        RHIDescriptorSetLayoutBinding& axis_layout_perframe_storage_buffer_binding = axis_layout_bindings[0];
        axis_layout_perframe_storage_buffer_binding.binding = 0;
        axis_layout_perframe_storage_buffer_binding.descriptorType = RHI_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
        axis_layout_perframe_storage_buffer_binding.descriptorCount = 1;
        axis_layout_perframe_storage_buffer_binding.stageFlags = RHI_SHADER_STAGE_VERTEX_BIT;
        axis_layout_perframe_storage_buffer_binding.pImmutableSamplers = NULL;

        RHIDescriptorSetLayoutBinding& axis_layout_storage_buffer_binding = axis_layout_bindings[1];
        axis_layout_storage_buffer_binding.binding = 1;
        axis_layout_storage_buffer_binding.descriptorType = RHI_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        axis_layout_storage_buffer_binding.descriptorCount = 1;
        axis_layout_storage_buffer_binding.stageFlags = RHI_SHADER_STAGE_VERTEX_BIT;
        axis_layout_storage_buffer_binding.pImmutableSamplers = NULL;

        RHIDescriptorSetLayoutCreateInfo axis_layout_create_info {};
        axis_layout_create_info.sType = RHI_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        axis_layout_create_info.bindingCount = 2;
        axis_layout_create_info.pBindings = axis_layout_bindings;

        if (RHI_SUCCESS != m_Rhi->CreateDescriptorSetLayout(&axis_layout_create_info, m_DescriptorInfos[_axis].layout))
        {
            throw std::runtime_error("create axis layout");
        }
    }

    {
        RHIDescriptorSetLayoutBinding gbuffer_lighting_global_layout_bindings[4];

        RHIDescriptorSetLayoutBinding& gbuffer_normal_global_layout_input_attachment_binding =
            gbuffer_lighting_global_layout_bindings[0];
        gbuffer_normal_global_layout_input_attachment_binding.binding = 0;
        gbuffer_normal_global_layout_input_attachment_binding.descriptorType = RHI_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
        gbuffer_normal_global_layout_input_attachment_binding.descriptorCount = 1;
        gbuffer_normal_global_layout_input_attachment_binding.stageFlags = RHI_SHADER_STAGE_FRAGMENT_BIT;

        RHIDescriptorSetLayoutBinding& gbuffer_metallic_roughness_shadingmodeid_global_layout_input_attachment_binding =
            gbuffer_lighting_global_layout_bindings[1];
        gbuffer_metallic_roughness_shadingmodeid_global_layout_input_attachment_binding.binding = 1;
        gbuffer_metallic_roughness_shadingmodeid_global_layout_input_attachment_binding.descriptorType =
            RHI_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
        gbuffer_metallic_roughness_shadingmodeid_global_layout_input_attachment_binding.descriptorCount = 1;
        gbuffer_metallic_roughness_shadingmodeid_global_layout_input_attachment_binding.stageFlags =
            RHI_SHADER_STAGE_FRAGMENT_BIT;

        RHIDescriptorSetLayoutBinding& gbuffer_albedo_global_layout_input_attachment_binding =
            gbuffer_lighting_global_layout_bindings[2];
        gbuffer_albedo_global_layout_input_attachment_binding.binding = 2;
        gbuffer_albedo_global_layout_input_attachment_binding.descriptorType = RHI_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
        gbuffer_albedo_global_layout_input_attachment_binding.descriptorCount = 1;
        gbuffer_albedo_global_layout_input_attachment_binding.stageFlags = RHI_SHADER_STAGE_FRAGMENT_BIT;

        RHIDescriptorSetLayoutBinding& gbuffer_depth_global_layout_input_attachment_binding =
            gbuffer_lighting_global_layout_bindings[3];
        gbuffer_depth_global_layout_input_attachment_binding.binding = 3;
        gbuffer_depth_global_layout_input_attachment_binding.descriptorType = RHI_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
        gbuffer_depth_global_layout_input_attachment_binding.descriptorCount = 1;
        gbuffer_depth_global_layout_input_attachment_binding.stageFlags = RHI_SHADER_STAGE_FRAGMENT_BIT;

        RHIDescriptorSetLayoutCreateInfo gbuffer_lighting_global_layout_create_info;
        gbuffer_lighting_global_layout_create_info.sType = RHI_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        gbuffer_lighting_global_layout_create_info.pNext = NULL;
        gbuffer_lighting_global_layout_create_info.flags = 0;
        gbuffer_lighting_global_layout_create_info.bindingCount =
            sizeof(gbuffer_lighting_global_layout_bindings) / sizeof(gbuffer_lighting_global_layout_bindings[0]);
        gbuffer_lighting_global_layout_create_info.pBindings = gbuffer_lighting_global_layout_bindings;

        if (RHI_SUCCESS != m_Rhi->CreateDescriptorSetLayout(&gbuffer_lighting_global_layout_create_info,
                                                            m_DescriptorInfos[_deferred_lighting].layout))
        {
            throw std::runtime_error("create deferred lighting global layout");
        }
    }

    {
        RHIDescriptorSetLayoutBinding spatial_surface_bindings[4] {};

        for (uint32_t binding_index = 0; binding_index < 4; ++binding_index)
        {
            spatial_surface_bindings[binding_index].binding = binding_index;
            spatial_surface_bindings[binding_index].descriptorType = RHI_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            spatial_surface_bindings[binding_index].descriptorCount = 1;
            spatial_surface_bindings[binding_index].stageFlags = RHI_SHADER_STAGE_FRAGMENT_BIT;
            spatial_surface_bindings[binding_index].pImmutableSamplers = NULL;
        }

        RHIDescriptorSetLayoutCreateInfo spatial_layout_create_info {};
        spatial_layout_create_info.sType = RHI_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        spatial_layout_create_info.bindingCount = 4;
        spatial_layout_create_info.pBindings = spatial_surface_bindings;

        if (RHI_SUCCESS != m_Rhi->CreateDescriptorSetLayout(&spatial_layout_create_info,
                                                            m_DescriptorInfos[_megalights_spatial_surfaces].layout))
        {
            throw std::runtime_error("create megalights spatial surfaces layout");
        }
    }
}

void MainCameraPass::SetupPipelines()
{
    m_RenderPipelines.resize(_render_pipeline_type_count);

    // mesh gbuffer
    {
        RHIDescriptorSetLayout* descriptorset_layouts[3] = {m_DescriptorInfos[_mesh_global].layout,
                                                            m_DescriptorInfos[_per_mesh].layout,
                                                            m_DescriptorInfos[_mesh_per_material].layout};
        RHIPipelineLayoutCreateInfo pipeline_layout_create_info {};
        pipeline_layout_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipeline_layout_create_info.setLayoutCount = 3;
        pipeline_layout_create_info.pSetLayouts = descriptorset_layouts;

        if (m_Rhi->CreatePipelineLayout(&pipeline_layout_create_info,
                                        m_RenderPipelines[_render_pipeline_type_mesh_gbuffer].layout) != RHI_SUCCESS)
        {
            throw std::runtime_error("create mesh gbuffer pipeline layout");
        }

        RHIShader* vert_shader_module = m_Rhi->CreateShaderModule(MESH_VERT);
        RHIShader* frag_shader_module = m_Rhi->CreateShaderModule(MESH_GBUFFER_FRAG);

        RHIPipelineShaderStageCreateInfo vert_pipeline_shader_stage_create_info {};
        vert_pipeline_shader_stage_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        vert_pipeline_shader_stage_create_info.stage = RHI_SHADER_STAGE_VERTEX_BIT;
        vert_pipeline_shader_stage_create_info.module = vert_shader_module;
        vert_pipeline_shader_stage_create_info.pName = "main";

        RHIPipelineShaderStageCreateInfo frag_pipeline_shader_stage_create_info {};
        frag_pipeline_shader_stage_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        frag_pipeline_shader_stage_create_info.stage = RHI_SHADER_STAGE_FRAGMENT_BIT;
        frag_pipeline_shader_stage_create_info.module = frag_shader_module;
        frag_pipeline_shader_stage_create_info.pName = "main";

        RHIPipelineShaderStageCreateInfo shader_stages[] = {vert_pipeline_shader_stage_create_info,
                                                            frag_pipeline_shader_stage_create_info};

        auto vertex_binding_descriptions = MeshVertex::GetBindingDescriptions();
        auto vertex_attribute_descriptions = MeshVertex::GetAttributeDescriptions();
        RHIPipelineVertexInputStateCreateInfo vertex_input_state_create_info {};
        vertex_input_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertex_input_state_create_info.vertexBindingDescriptionCount = vertex_binding_descriptions.size();
        vertex_input_state_create_info.pVertexBindingDescriptions = &vertex_binding_descriptions[0];
        vertex_input_state_create_info.vertexAttributeDescriptionCount = vertex_attribute_descriptions.size();
        vertex_input_state_create_info.pVertexAttributeDescriptions = &vertex_attribute_descriptions[0];

        RHIPipelineInputAssemblyStateCreateInfo input_assembly_create_info {};
        input_assembly_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        input_assembly_create_info.topology = RHI_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        input_assembly_create_info.primitiveRestartEnable = RHI_FALSE;

        RHIPipelineViewportStateCreateInfo viewport_state_create_info {};
        viewport_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewport_state_create_info.viewportCount = 1;
        viewport_state_create_info.pViewports = m_Rhi->GetSwapchainInfo().viewport;
        viewport_state_create_info.scissorCount = 1;
        viewport_state_create_info.pScissors = m_Rhi->GetSwapchainInfo().scissor;

        RHIPipelineRasterizationStateCreateInfo rasterization_state_create_info {};
        rasterization_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterization_state_create_info.depthClampEnable = RHI_FALSE;
        rasterization_state_create_info.rasterizerDiscardEnable = RHI_FALSE;
        rasterization_state_create_info.polygonMode = RHI_POLYGON_MODE_FILL;
        rasterization_state_create_info.lineWidth = 1.0f;
        rasterization_state_create_info.cullMode = RHI_CULL_MODE_BACK_BIT;
        rasterization_state_create_info.frontFace = RHI_FRONT_FACE_COUNTER_CLOCKWISE;
        rasterization_state_create_info.depthBiasEnable = RHI_FALSE;
        rasterization_state_create_info.depthBiasConstantFactor = 0.0f;
        rasterization_state_create_info.depthBiasClamp = 0.0f;
        rasterization_state_create_info.depthBiasSlopeFactor = 0.0f;

        RHIPipelineMultisampleStateCreateInfo multisample_state_create_info {};
        multisample_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample_state_create_info.sampleShadingEnable = RHI_FALSE;
        multisample_state_create_info.rasterizationSamples = RHI_SAMPLE_COUNT_1_BIT;

        RHIPipelineColorBlendAttachmentState color_blend_attachments[3] = {};
        color_blend_attachments[0].colorWriteMask = RHI_COLOR_COMPONENT_R_BIT | RHI_COLOR_COMPONENT_G_BIT |
                                                    RHI_COLOR_COMPONENT_B_BIT | RHI_COLOR_COMPONENT_A_BIT;
        color_blend_attachments[0].blendEnable = RHI_FALSE;
        color_blend_attachments[0].srcColorBlendFactor = RHI_BLEND_FACTOR_ONE;
        color_blend_attachments[0].dstColorBlendFactor = RHI_BLEND_FACTOR_ZERO;
        color_blend_attachments[0].colorBlendOp = RHI_BLEND_OP_ADD;
        color_blend_attachments[0].srcAlphaBlendFactor = RHI_BLEND_FACTOR_ONE;
        color_blend_attachments[0].dstAlphaBlendFactor = RHI_BLEND_FACTOR_ZERO;
        color_blend_attachments[0].alphaBlendOp = RHI_BLEND_OP_ADD;
        color_blend_attachments[1].colorWriteMask = RHI_COLOR_COMPONENT_R_BIT | RHI_COLOR_COMPONENT_G_BIT |
                                                    RHI_COLOR_COMPONENT_B_BIT | RHI_COLOR_COMPONENT_A_BIT;
        color_blend_attachments[1].blendEnable = RHI_FALSE;
        color_blend_attachments[1].srcColorBlendFactor = RHI_BLEND_FACTOR_ONE;
        color_blend_attachments[1].dstColorBlendFactor = RHI_BLEND_FACTOR_ZERO;
        color_blend_attachments[1].colorBlendOp = RHI_BLEND_OP_ADD;
        color_blend_attachments[1].srcAlphaBlendFactor = RHI_BLEND_FACTOR_ONE;
        color_blend_attachments[1].dstAlphaBlendFactor = RHI_BLEND_FACTOR_ZERO;
        color_blend_attachments[1].alphaBlendOp = RHI_BLEND_OP_ADD;
        color_blend_attachments[2].colorWriteMask = RHI_COLOR_COMPONENT_R_BIT | RHI_COLOR_COMPONENT_G_BIT |
                                                    RHI_COLOR_COMPONENT_B_BIT | RHI_COLOR_COMPONENT_A_BIT;
        color_blend_attachments[2].blendEnable = RHI_FALSE;
        color_blend_attachments[2].srcColorBlendFactor = RHI_BLEND_FACTOR_ONE;
        color_blend_attachments[2].dstColorBlendFactor = RHI_BLEND_FACTOR_ZERO;
        color_blend_attachments[2].colorBlendOp = RHI_BLEND_OP_ADD;
        color_blend_attachments[2].srcAlphaBlendFactor = RHI_BLEND_FACTOR_ONE;
        color_blend_attachments[2].dstAlphaBlendFactor = RHI_BLEND_FACTOR_ZERO;
        color_blend_attachments[2].alphaBlendOp = RHI_BLEND_OP_ADD;

        RHIPipelineColorBlendStateCreateInfo color_blend_state_create_info = {};
        color_blend_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        color_blend_state_create_info.logicOpEnable = RHI_FALSE;
        color_blend_state_create_info.logicOp = RHI_LOGIC_OP_COPY;
        color_blend_state_create_info.attachmentCount =
            sizeof(color_blend_attachments) / sizeof(color_blend_attachments[0]);
        color_blend_state_create_info.pAttachments = &color_blend_attachments[0];
        color_blend_state_create_info.blendConstants[0] = 0.0f;
        color_blend_state_create_info.blendConstants[1] = 0.0f;
        color_blend_state_create_info.blendConstants[2] = 0.0f;
        color_blend_state_create_info.blendConstants[3] = 0.0f;

        RHIPipelineDepthStencilStateCreateInfo depth_stencil_create_info {};
        depth_stencil_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depth_stencil_create_info.depthTestEnable = RHI_TRUE;
        depth_stencil_create_info.depthWriteEnable = RHI_TRUE;
        depth_stencil_create_info.depthCompareOp = RHI_COMPARE_OP_LESS;
        depth_stencil_create_info.depthBoundsTestEnable = RHI_FALSE;
        depth_stencil_create_info.stencilTestEnable = RHI_FALSE;

        RHIDynamicState dynamic_states[] = {RHI_DYNAMIC_STATE_VIEWPORT, RHI_DYNAMIC_STATE_SCISSOR};
        RHIPipelineDynamicStateCreateInfo dynamic_state_create_info {};
        dynamic_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic_state_create_info.dynamicStateCount = 2;
        dynamic_state_create_info.pDynamicStates = dynamic_states;

        RHIGraphicsPipelineCreateInfo pipelineInfo {};
        pipelineInfo.sType = RHI_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipelineInfo.stageCount = 2;
        pipelineInfo.pStages = shader_stages;
        pipelineInfo.pVertexInputState = &vertex_input_state_create_info;
        pipelineInfo.pInputAssemblyState = &input_assembly_create_info;
        pipelineInfo.pViewportState = &viewport_state_create_info;
        pipelineInfo.pRasterizationState = &rasterization_state_create_info;
        pipelineInfo.pMultisampleState = &multisample_state_create_info;
        pipelineInfo.pColorBlendState = &color_blend_state_create_info;
        pipelineInfo.pDepthStencilState = &depth_stencil_create_info;
        pipelineInfo.layout = m_RenderPipelines[_render_pipeline_type_mesh_gbuffer].layout;
        pipelineInfo.renderPass = m_Framebuffer1.render_pass;  // RP1
        pipelineInfo.subpass = _main_camera_subpass_basepass;
        pipelineInfo.basePipelineHandle = RHI_NULL_HANDLE;
        pipelineInfo.pDynamicState = &dynamic_state_create_info;

        if (RHI_SUCCESS !=
            m_Rhi->CreateGraphicsPipelines(
                RHI_NULL_HANDLE, 1, &pipelineInfo, m_RenderPipelines[_render_pipeline_type_mesh_gbuffer].pipeline))
        {
            throw std::runtime_error("create mesh gbuffer graphics pipeline");
        }

        m_Rhi->DestroyShaderModule(vert_shader_module);
        m_Rhi->DestroyShaderModule(frag_shader_module);
    }

    // mesh transparent
    {
        RHIDescriptorSetLayout* descriptorset_layouts[3] = {m_DescriptorInfos[_mesh_global].layout,
                                                            m_DescriptorInfos[_per_mesh].layout,
                                                            m_DescriptorInfos[_mesh_per_material].layout};
        RHIPipelineLayoutCreateInfo pipeline_layout_create_info {};
        pipeline_layout_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipeline_layout_create_info.setLayoutCount = 3;
        pipeline_layout_create_info.pSetLayouts = descriptorset_layouts;

        if (m_Rhi->CreatePipelineLayout(&pipeline_layout_create_info,
                                        m_RenderPipelines[_render_pipeline_type_mesh_transparent].layout) != RHI_SUCCESS)
        {
            throw std::runtime_error("create mesh transparent pipeline layout");
        }

        RHIShader* vert_shader_module = m_Rhi->CreateShaderModule(MESH_VERT);
        RHIShader* frag_shader_module = m_Rhi->CreateShaderModule(MESH_FRAG);

        RHIPipelineShaderStageCreateInfo vert_pipeline_shader_stage_create_info {};
        vert_pipeline_shader_stage_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        vert_pipeline_shader_stage_create_info.stage = RHI_SHADER_STAGE_VERTEX_BIT;
        vert_pipeline_shader_stage_create_info.module = vert_shader_module;
        vert_pipeline_shader_stage_create_info.pName = "main";

        RHIPipelineShaderStageCreateInfo frag_pipeline_shader_stage_create_info {};
        frag_pipeline_shader_stage_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        frag_pipeline_shader_stage_create_info.stage = RHI_SHADER_STAGE_FRAGMENT_BIT;
        frag_pipeline_shader_stage_create_info.module = frag_shader_module;
        frag_pipeline_shader_stage_create_info.pName = "main";

        RHIPipelineShaderStageCreateInfo shader_stages[] = {vert_pipeline_shader_stage_create_info,
                                                            frag_pipeline_shader_stage_create_info};

        auto vertex_binding_descriptions = MeshVertex::GetBindingDescriptions();
        auto vertex_attribute_descriptions = MeshVertex::GetAttributeDescriptions();
        RHIPipelineVertexInputStateCreateInfo vertex_input_state_create_info {};
        vertex_input_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertex_input_state_create_info.vertexBindingDescriptionCount = static_cast<uint32_t>(vertex_binding_descriptions.size());
        vertex_input_state_create_info.pVertexBindingDescriptions = vertex_binding_descriptions.data();
        vertex_input_state_create_info.vertexAttributeDescriptionCount = static_cast<uint32_t>(vertex_attribute_descriptions.size());
        vertex_input_state_create_info.pVertexAttributeDescriptions = vertex_attribute_descriptions.data();

        RHIPipelineInputAssemblyStateCreateInfo input_assembly_create_info {};
        input_assembly_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        input_assembly_create_info.topology = RHI_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        input_assembly_create_info.primitiveRestartEnable = RHI_FALSE;

        RHIPipelineViewportStateCreateInfo viewport_state_create_info {};
        viewport_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewport_state_create_info.viewportCount = 1;
        viewport_state_create_info.pViewports = m_Rhi->GetSwapchainInfo().viewport;
        viewport_state_create_info.scissorCount = 1;
        viewport_state_create_info.pScissors = m_Rhi->GetSwapchainInfo().scissor;

        RHIPipelineRasterizationStateCreateInfo rasterization_state_create_info {};
        rasterization_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterization_state_create_info.depthClampEnable = RHI_FALSE;
        rasterization_state_create_info.rasterizerDiscardEnable = RHI_FALSE;
        rasterization_state_create_info.polygonMode = RHI_POLYGON_MODE_FILL;
        rasterization_state_create_info.lineWidth = 1.0f;
        rasterization_state_create_info.cullMode = RHI_CULL_MODE_BACK_BIT;
        rasterization_state_create_info.frontFace = RHI_FRONT_FACE_COUNTER_CLOCKWISE;
        rasterization_state_create_info.depthBiasEnable = RHI_FALSE;
        rasterization_state_create_info.depthBiasConstantFactor = 0.0f;
        rasterization_state_create_info.depthBiasClamp = 0.0f;
        rasterization_state_create_info.depthBiasSlopeFactor = 0.0f;

        RHIPipelineMultisampleStateCreateInfo multisample_state_create_info {};
        multisample_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample_state_create_info.sampleShadingEnable = RHI_FALSE;
        multisample_state_create_info.rasterizationSamples = RHI_SAMPLE_COUNT_1_BIT;

        std::array<RHIPipelineColorBlendAttachmentState, 1> color_blend_attachments {};
        ApplyBlendMode("Transparent", color_blend_attachments);

        RHIPipelineColorBlendStateCreateInfo color_blend_state_create_info {};
        color_blend_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        color_blend_state_create_info.logicOpEnable = RHI_FALSE;
        color_blend_state_create_info.logicOp = RHI_LOGIC_OP_COPY;
        color_blend_state_create_info.attachmentCount = static_cast<uint32_t>(color_blend_attachments.size());
        color_blend_state_create_info.pAttachments = color_blend_attachments.data();
        color_blend_state_create_info.blendConstants[0] = 0.0f;
        color_blend_state_create_info.blendConstants[1] = 0.0f;
        color_blend_state_create_info.blendConstants[2] = 0.0f;
        color_blend_state_create_info.blendConstants[3] = 0.0f;

        RHIPipelineDepthStencilStateCreateInfo depth_stencil_create_info {};
        depth_stencil_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depth_stencil_create_info.depthTestEnable = RHI_TRUE;
        depth_stencil_create_info.depthWriteEnable = RHI_FALSE;
        depth_stencil_create_info.depthCompareOp = RHI_COMPARE_OP_LESS_OR_EQUAL;
        depth_stencil_create_info.depthBoundsTestEnable = RHI_FALSE;
        depth_stencil_create_info.stencilTestEnable = RHI_FALSE;

        RHIDynamicState dynamic_states[] = {RHI_DYNAMIC_STATE_VIEWPORT, RHI_DYNAMIC_STATE_SCISSOR};
        RHIPipelineDynamicStateCreateInfo dynamic_state_create_info {};
        dynamic_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic_state_create_info.dynamicStateCount = 2;
        dynamic_state_create_info.pDynamicStates = dynamic_states;

        RHIGraphicsPipelineCreateInfo pipelineInfo {};
        pipelineInfo.sType = RHI_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipelineInfo.stageCount = 2;
        pipelineInfo.pStages = shader_stages;
        pipelineInfo.pVertexInputState = &vertex_input_state_create_info;
        pipelineInfo.pInputAssemblyState = &input_assembly_create_info;
        pipelineInfo.pViewportState = &viewport_state_create_info;
        pipelineInfo.pRasterizationState = &rasterization_state_create_info;
        pipelineInfo.pMultisampleState = &multisample_state_create_info;
        pipelineInfo.pColorBlendState = &color_blend_state_create_info;
        pipelineInfo.pDepthStencilState = &depth_stencil_create_info;
        pipelineInfo.layout = m_RenderPipelines[_render_pipeline_type_mesh_transparent].layout;
        pipelineInfo.renderPass = m_Framebuffer1.render_pass;  // RP1
        pipelineInfo.subpass = _main_camera_subpass_forward_lighting;
        pipelineInfo.basePipelineHandle = RHI_NULL_HANDLE;
        pipelineInfo.pDynamicState = &dynamic_state_create_info;

        if (RHI_SUCCESS !=
            m_Rhi->CreateGraphicsPipelines(RHI_NULL_HANDLE,
                                           1,
                                           &pipelineInfo,
                                           m_RenderPipelines[_render_pipeline_type_mesh_transparent].pipeline))
        {
            throw std::runtime_error("create mesh transparent graphics pipeline");
        }

        m_Rhi->DestroyShaderModule(vert_shader_module);
        m_Rhi->DestroyShaderModule(frag_shader_module);
    }

    // deferred lighting
    {
        RHIDescriptorSetLayout* descriptorset_layouts[3] = {m_DescriptorInfos[_mesh_global].layout,
                                                            m_DescriptorInfos[_deferred_lighting].layout,
                                                            m_DescriptorInfos[_skybox].layout};
        RHIPipelineLayoutCreateInfo pipeline_layout_create_info {};
        pipeline_layout_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipeline_layout_create_info.setLayoutCount = sizeof(descriptorset_layouts) / sizeof(descriptorset_layouts[0]);
        pipeline_layout_create_info.pSetLayouts = descriptorset_layouts;

        if (RHI_SUCCESS !=
            m_Rhi->CreatePipelineLayout(&pipeline_layout_create_info,
                                        m_RenderPipelines[_render_pipeline_type_deferred_lighting].layout))
        {
            throw std::runtime_error("create deferred lighting pipeline layout");
        }

        RHIShader* vert_shader_module = m_Rhi->CreateShaderModule(DEFERRED_LIGHTING_VERT);
        RHIShader* frag_shader_module = m_Rhi->CreateShaderModule(DEFERRED_LIGHTING_FRAG);

        RHIPipelineShaderStageCreateInfo vert_pipeline_shader_stage_create_info {};
        vert_pipeline_shader_stage_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        vert_pipeline_shader_stage_create_info.stage = RHI_SHADER_STAGE_VERTEX_BIT;
        vert_pipeline_shader_stage_create_info.module = vert_shader_module;
        vert_pipeline_shader_stage_create_info.pName = "main";
        // vert_pipeline_shader_stage_create_info.pSpecializationInfo

        RHIPipelineShaderStageCreateInfo frag_pipeline_shader_stage_create_info {};
        frag_pipeline_shader_stage_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        frag_pipeline_shader_stage_create_info.stage = RHI_SHADER_STAGE_FRAGMENT_BIT;
        frag_pipeline_shader_stage_create_info.module = frag_shader_module;
        frag_pipeline_shader_stage_create_info.pName = "main";

        RHIPipelineShaderStageCreateInfo shader_stages[] = {vert_pipeline_shader_stage_create_info,
                                                            frag_pipeline_shader_stage_create_info};

        auto vertex_binding_descriptions = MeshVertex::GetBindingDescriptions();
        auto vertex_attribute_descriptions = MeshVertex::GetAttributeDescriptions();
        RHIPipelineVertexInputStateCreateInfo vertex_input_state_create_info {};
        vertex_input_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertex_input_state_create_info.vertexBindingDescriptionCount = 0;
        vertex_input_state_create_info.pVertexBindingDescriptions = NULL;
        vertex_input_state_create_info.vertexBindingDescriptionCount = 0;
        vertex_input_state_create_info.pVertexAttributeDescriptions = NULL;

        RHIPipelineInputAssemblyStateCreateInfo input_assembly_create_info {};
        input_assembly_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        input_assembly_create_info.topology = RHI_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        input_assembly_create_info.primitiveRestartEnable = RHI_FALSE;

        RHIPipelineViewportStateCreateInfo viewport_state_create_info {};
        viewport_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewport_state_create_info.viewportCount = 1;
        viewport_state_create_info.pViewports = m_Rhi->GetSwapchainInfo().viewport;
        viewport_state_create_info.scissorCount = 1;
        viewport_state_create_info.pScissors = m_Rhi->GetSwapchainInfo().scissor;

        RHIPipelineRasterizationStateCreateInfo rasterization_state_create_info {};
        rasterization_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterization_state_create_info.depthClampEnable = RHI_FALSE;
        rasterization_state_create_info.rasterizerDiscardEnable = RHI_FALSE;
        rasterization_state_create_info.polygonMode = RHI_POLYGON_MODE_FILL;
        rasterization_state_create_info.lineWidth = 1.0f;
        rasterization_state_create_info.cullMode = RHI_CULL_MODE_BACK_BIT;
        rasterization_state_create_info.frontFace = RHI_FRONT_FACE_CLOCKWISE;
        rasterization_state_create_info.depthBiasEnable = RHI_FALSE;
        rasterization_state_create_info.depthBiasConstantFactor = 0.0f;
        rasterization_state_create_info.depthBiasClamp = 0.0f;
        rasterization_state_create_info.depthBiasSlopeFactor = 0.0f;

        RHIPipelineMultisampleStateCreateInfo multisample_state_create_info {};
        multisample_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample_state_create_info.sampleShadingEnable = RHI_FALSE;
        multisample_state_create_info.rasterizationSamples = RHI_SAMPLE_COUNT_1_BIT;

        RHIPipelineColorBlendAttachmentState color_blend_attachments[1] = {};
        color_blend_attachments[0].colorWriteMask = RHI_COLOR_COMPONENT_R_BIT | RHI_COLOR_COMPONENT_G_BIT |
                                                    RHI_COLOR_COMPONENT_B_BIT | RHI_COLOR_COMPONENT_A_BIT;
        color_blend_attachments[0].blendEnable = RHI_FALSE;
        color_blend_attachments[0].srcColorBlendFactor = RHI_BLEND_FACTOR_ONE;
        color_blend_attachments[0].dstColorBlendFactor = RHI_BLEND_FACTOR_ONE;
        color_blend_attachments[0].colorBlendOp = RHI_BLEND_OP_ADD;
        color_blend_attachments[0].srcAlphaBlendFactor = RHI_BLEND_FACTOR_ONE;
        color_blend_attachments[0].dstAlphaBlendFactor = RHI_BLEND_FACTOR_ONE;
        color_blend_attachments[0].alphaBlendOp = RHI_BLEND_OP_ADD;

        RHIPipelineColorBlendStateCreateInfo color_blend_state_create_info = {};
        color_blend_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        color_blend_state_create_info.logicOpEnable = RHI_FALSE;
        color_blend_state_create_info.logicOp = RHI_LOGIC_OP_COPY;
        color_blend_state_create_info.attachmentCount =
            sizeof(color_blend_attachments) / sizeof(color_blend_attachments[0]);
        color_blend_state_create_info.pAttachments = &color_blend_attachments[0];
        color_blend_state_create_info.blendConstants[0] = 0.0f;
        color_blend_state_create_info.blendConstants[1] = 0.0f;
        color_blend_state_create_info.blendConstants[2] = 0.0f;
        color_blend_state_create_info.blendConstants[3] = 0.0f;

        RHIPipelineDepthStencilStateCreateInfo depth_stencil_create_info {};
        depth_stencil_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depth_stencil_create_info.depthTestEnable = RHI_FALSE;
        depth_stencil_create_info.depthWriteEnable = RHI_FALSE;
        depth_stencil_create_info.depthCompareOp = RHI_COMPARE_OP_ALWAYS;
        depth_stencil_create_info.depthBoundsTestEnable = RHI_FALSE;
        depth_stencil_create_info.stencilTestEnable = RHI_FALSE;

        RHIDynamicState dynamic_states[] = {RHI_DYNAMIC_STATE_VIEWPORT, RHI_DYNAMIC_STATE_SCISSOR};
        RHIPipelineDynamicStateCreateInfo dynamic_state_create_info {};
        dynamic_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic_state_create_info.dynamicStateCount = 2;
        dynamic_state_create_info.pDynamicStates = dynamic_states;

        RHIGraphicsPipelineCreateInfo pipelineInfo {};
        pipelineInfo.sType = RHI_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipelineInfo.stageCount = 2;
        pipelineInfo.pStages = shader_stages;
        pipelineInfo.pVertexInputState = &vertex_input_state_create_info;
        pipelineInfo.pInputAssemblyState = &input_assembly_create_info;
        pipelineInfo.pViewportState = &viewport_state_create_info;
        pipelineInfo.pRasterizationState = &rasterization_state_create_info;
        pipelineInfo.pMultisampleState = &multisample_state_create_info;
        pipelineInfo.pColorBlendState = &color_blend_state_create_info;
        pipelineInfo.pDepthStencilState = &depth_stencil_create_info;
        pipelineInfo.layout = m_RenderPipelines[_render_pipeline_type_deferred_lighting].layout;
        pipelineInfo.renderPass = m_Framebuffer1.render_pass;  // RP1
        pipelineInfo.subpass = _main_camera_subpass_deferred_lighting;
        pipelineInfo.basePipelineHandle = RHI_NULL_HANDLE;
        pipelineInfo.pDynamicState = &dynamic_state_create_info;

        if (RHI_SUCCESS !=
            m_Rhi->CreateGraphicsPipelines(RHI_NULL_HANDLE,
                                           1,
                                           &pipelineInfo,
                                           m_RenderPipelines[_render_pipeline_type_deferred_lighting].pipeline))
        {
            throw std::runtime_error("create deferred lighting graphics pipeline");
        }

        m_Rhi->DestroyShaderModule(vert_shader_module);
        m_Rhi->DestroyShaderModule(frag_shader_module);
    }

    // megalights deferred (fullscreen, same layout as deferred lighting)
    {
        RHIDescriptorSetLayout* descriptorset_layouts[3] = {m_DescriptorInfos[_mesh_global].layout,
                                                            m_DescriptorInfos[_deferred_lighting].layout,
                                                            m_DescriptorInfos[_skybox].layout};
        RHIPipelineLayoutCreateInfo pipeline_layout_create_info {};
        pipeline_layout_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipeline_layout_create_info.setLayoutCount = sizeof(descriptorset_layouts) / sizeof(descriptorset_layouts[0]);
        pipeline_layout_create_info.pSetLayouts = descriptorset_layouts;

        if (RHI_SUCCESS !=
            m_Rhi->CreatePipelineLayout(&pipeline_layout_create_info,
                                        m_RenderPipelines[_render_pipeline_type_megalights_deferred].layout))
        {
            throw std::runtime_error("create megalights deferred pipeline layout");
        }

        RHIShader* vert_shader_module = m_Rhi->CreateShaderModule(MEGALIGHTS_DEFERRED_VERT);
        RHIShader* frag_shader_module = m_Rhi->CreateShaderModule(MEGALIGHTS_DEFERRED_FRAG);

        RHIPipelineShaderStageCreateInfo vert_pipeline_shader_stage_create_info {};
        vert_pipeline_shader_stage_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        vert_pipeline_shader_stage_create_info.stage = RHI_SHADER_STAGE_VERTEX_BIT;
        vert_pipeline_shader_stage_create_info.module = vert_shader_module;
        vert_pipeline_shader_stage_create_info.pName = "main";

        RHIPipelineShaderStageCreateInfo frag_pipeline_shader_stage_create_info {};
        frag_pipeline_shader_stage_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        frag_pipeline_shader_stage_create_info.stage = RHI_SHADER_STAGE_FRAGMENT_BIT;
        frag_pipeline_shader_stage_create_info.module = frag_shader_module;
        frag_pipeline_shader_stage_create_info.pName = "main";

        RHIPipelineShaderStageCreateInfo shader_stages[] = {vert_pipeline_shader_stage_create_info,
                                                            frag_pipeline_shader_stage_create_info};

        RHIPipelineVertexInputStateCreateInfo vertex_input_state_create_info {};
        vertex_input_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

        RHIPipelineInputAssemblyStateCreateInfo input_assembly_create_info {};
        input_assembly_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        input_assembly_create_info.topology = RHI_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        input_assembly_create_info.primitiveRestartEnable = RHI_FALSE;

        RHIPipelineViewportStateCreateInfo viewport_state_create_info {};
        viewport_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewport_state_create_info.viewportCount = 1;
        viewport_state_create_info.pViewports = m_Rhi->GetSwapchainInfo().viewport;
        viewport_state_create_info.scissorCount = 1;
        viewport_state_create_info.pScissors = m_Rhi->GetSwapchainInfo().scissor;

        RHIPipelineRasterizationStateCreateInfo rasterization_state_create_info {};
        rasterization_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterization_state_create_info.depthClampEnable = RHI_FALSE;
        rasterization_state_create_info.rasterizerDiscardEnable = RHI_FALSE;
        rasterization_state_create_info.polygonMode = RHI_POLYGON_MODE_FILL;
        rasterization_state_create_info.cullMode = RHI_CULL_MODE_NONE;
        rasterization_state_create_info.frontFace = RHI_FRONT_FACE_CLOCKWISE;
        rasterization_state_create_info.depthBiasEnable = RHI_FALSE;

        RHIPipelineMultisampleStateCreateInfo multisample_state_create_info {};
        multisample_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample_state_create_info.rasterizationSamples = RHI_SAMPLE_COUNT_1_BIT;

        RHIPipelineColorBlendAttachmentState color_blend_attachment {};
        color_blend_attachment.blendEnable = RHI_FALSE;
        color_blend_attachment.colorWriteMask = RHI_COLOR_COMPONENT_R_BIT | RHI_COLOR_COMPONENT_G_BIT |
                                                RHI_COLOR_COMPONENT_B_BIT | RHI_COLOR_COMPONENT_A_BIT;

        RHIPipelineColorBlendStateCreateInfo color_blend_state_create_info = {};
        color_blend_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        color_blend_state_create_info.logicOpEnable = RHI_FALSE;
        color_blend_state_create_info.attachmentCount = 1;
        color_blend_state_create_info.pAttachments = &color_blend_attachment;

        RHIPipelineDepthStencilStateCreateInfo depth_stencil_create_info {};
        depth_stencil_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depth_stencil_create_info.depthTestEnable = RHI_FALSE;
        depth_stencil_create_info.depthWriteEnable = RHI_FALSE;

        RHIDynamicState dynamic_states[] = {RHI_DYNAMIC_STATE_VIEWPORT, RHI_DYNAMIC_STATE_SCISSOR};
        RHIPipelineDynamicStateCreateInfo dynamic_state_create_info {};
        dynamic_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic_state_create_info.dynamicStateCount = 2;
        dynamic_state_create_info.pDynamicStates = dynamic_states;

        RHIGraphicsPipelineCreateInfo pipelineInfo {};
        pipelineInfo.sType = RHI_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipelineInfo.stageCount = 2;
        pipelineInfo.pStages = shader_stages;
        pipelineInfo.pVertexInputState = &vertex_input_state_create_info;
        pipelineInfo.pInputAssemblyState = &input_assembly_create_info;
        pipelineInfo.pViewportState = &viewport_state_create_info;
        pipelineInfo.pRasterizationState = &rasterization_state_create_info;
        pipelineInfo.pMultisampleState = &multisample_state_create_info;
        pipelineInfo.pColorBlendState = &color_blend_state_create_info;
        pipelineInfo.pDepthStencilState = &depth_stencil_create_info;
        pipelineInfo.layout = m_RenderPipelines[_render_pipeline_type_megalights_deferred].layout;
        pipelineInfo.renderPass = m_Framebuffer1.render_pass;
        pipelineInfo.subpass = _main_camera_subpass_deferred_lighting;
        pipelineInfo.pDynamicState = &dynamic_state_create_info;

        if (RHI_SUCCESS !=
            m_Rhi->CreateGraphicsPipelines(RHI_NULL_HANDLE,
                                           1,
                                           &pipelineInfo,
                                           m_RenderPipelines[_render_pipeline_type_megalights_deferred].pipeline))
        {
            throw std::runtime_error("create megalights deferred graphics pipeline");
        }

        m_Rhi->DestroyShaderModule(vert_shader_module);
        m_Rhi->DestroyShaderModule(frag_shader_module);
    }

    // megalights spatial denoise (sampled gbuffer set + mesh global)
    {
        RHIDescriptorSetLayout* descriptorset_layouts[2] = {m_DescriptorInfos[_mesh_global].layout,
                                                            m_DescriptorInfos[_megalights_spatial_surfaces].layout};
        RHIPipelineLayoutCreateInfo pipeline_layout_create_info {};
        pipeline_layout_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipeline_layout_create_info.setLayoutCount = 2;
        pipeline_layout_create_info.pSetLayouts = descriptorset_layouts;

        if (RHI_SUCCESS !=
            m_Rhi->CreatePipelineLayout(&pipeline_layout_create_info,
                                        m_RenderPipelines[_render_pipeline_type_megalights_spatial].layout))
        {
            throw std::runtime_error("create megalights spatial pipeline layout");
        }

        RHIShader* vert_shader_module = m_Rhi->CreateShaderModule(MEGALIGHTS_DEFERRED_VERT);
        RHIShader* frag_shader_module = m_Rhi->CreateShaderModule(MEGALIGHTS_SPATIAL_FRAG);

        RHIPipelineShaderStageCreateInfo vert_pipeline_shader_stage_create_info {};
        vert_pipeline_shader_stage_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        vert_pipeline_shader_stage_create_info.stage = RHI_SHADER_STAGE_VERTEX_BIT;
        vert_pipeline_shader_stage_create_info.module = vert_shader_module;
        vert_pipeline_shader_stage_create_info.pName = "main";

        RHIPipelineShaderStageCreateInfo frag_pipeline_shader_stage_create_info {};
        frag_pipeline_shader_stage_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        frag_pipeline_shader_stage_create_info.stage = RHI_SHADER_STAGE_FRAGMENT_BIT;
        frag_pipeline_shader_stage_create_info.module = frag_shader_module;
        frag_pipeline_shader_stage_create_info.pName = "main";

        RHIPipelineShaderStageCreateInfo shader_stages[] = {vert_pipeline_shader_stage_create_info,
                                                            frag_pipeline_shader_stage_create_info};

        RHIPipelineVertexInputStateCreateInfo vertex_input_state_create_info {};
        vertex_input_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

        RHIPipelineInputAssemblyStateCreateInfo input_assembly_create_info {};
        input_assembly_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        input_assembly_create_info.topology = RHI_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        input_assembly_create_info.primitiveRestartEnable = RHI_FALSE;

        RHIPipelineViewportStateCreateInfo viewport_state_create_info {};
        viewport_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewport_state_create_info.viewportCount = 1;
        viewport_state_create_info.pViewports = m_Rhi->GetSwapchainInfo().viewport;
        viewport_state_create_info.scissorCount = 1;
        viewport_state_create_info.pScissors = m_Rhi->GetSwapchainInfo().scissor;

        RHIPipelineRasterizationStateCreateInfo rasterization_state_create_info {};
        rasterization_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterization_state_create_info.depthClampEnable = RHI_FALSE;
        rasterization_state_create_info.rasterizerDiscardEnable = RHI_FALSE;
        rasterization_state_create_info.polygonMode = RHI_POLYGON_MODE_FILL;
        rasterization_state_create_info.cullMode = RHI_CULL_MODE_NONE;
        rasterization_state_create_info.frontFace = RHI_FRONT_FACE_CLOCKWISE;
        rasterization_state_create_info.depthBiasEnable = RHI_FALSE;

        RHIPipelineMultisampleStateCreateInfo multisample_state_create_info {};
        multisample_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample_state_create_info.rasterizationSamples = RHI_SAMPLE_COUNT_1_BIT;

        RHIPipelineColorBlendAttachmentState color_blend_attachment {};
        color_blend_attachment.blendEnable = RHI_FALSE;
        color_blend_attachment.colorWriteMask = RHI_COLOR_COMPONENT_R_BIT | RHI_COLOR_COMPONENT_G_BIT |
                                                RHI_COLOR_COMPONENT_B_BIT | RHI_COLOR_COMPONENT_A_BIT;

        RHIPipelineColorBlendStateCreateInfo color_blend_state_create_info = {};
        color_blend_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        color_blend_state_create_info.logicOpEnable = RHI_FALSE;
        color_blend_state_create_info.attachmentCount = 1;
        color_blend_state_create_info.pAttachments = &color_blend_attachment;

        RHIPipelineDepthStencilStateCreateInfo depth_stencil_create_info {};
        depth_stencil_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depth_stencil_create_info.depthTestEnable = RHI_FALSE;
        depth_stencil_create_info.depthWriteEnable = RHI_FALSE;

        RHIDynamicState dynamic_states[] = {RHI_DYNAMIC_STATE_VIEWPORT, RHI_DYNAMIC_STATE_SCISSOR};
        RHIPipelineDynamicStateCreateInfo dynamic_state_create_info {};
        dynamic_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic_state_create_info.dynamicStateCount = 2;
        dynamic_state_create_info.pDynamicStates = dynamic_states;

        RHIGraphicsPipelineCreateInfo pipelineInfo {};
        pipelineInfo.sType = RHI_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipelineInfo.stageCount = 2;
        pipelineInfo.pStages = shader_stages;
        pipelineInfo.pVertexInputState = &vertex_input_state_create_info;
        pipelineInfo.pInputAssemblyState = &input_assembly_create_info;
        pipelineInfo.pViewportState = &viewport_state_create_info;
        pipelineInfo.pRasterizationState = &rasterization_state_create_info;
        pipelineInfo.pMultisampleState = &multisample_state_create_info;
        pipelineInfo.pColorBlendState = &color_blend_state_create_info;
        pipelineInfo.pDepthStencilState = &depth_stencil_create_info;
        pipelineInfo.layout = m_RenderPipelines[_render_pipeline_type_megalights_spatial].layout;
        pipelineInfo.renderPass = m_Framebuffer1.render_pass;
        pipelineInfo.subpass = _main_camera_subpass_deferred_lighting;
        pipelineInfo.pDynamicState = &dynamic_state_create_info;

        if (RHI_SUCCESS !=
            m_Rhi->CreateGraphicsPipelines(RHI_NULL_HANDLE,
                                           1,
                                           &pipelineInfo,
                                           m_RenderPipelines[_render_pipeline_type_megalights_spatial].pipeline))
        {
            throw std::runtime_error("create megalights spatial graphics pipeline");
        }

        m_Rhi->DestroyShaderModule(vert_shader_module);
        m_Rhi->DestroyShaderModule(frag_shader_module);
    }

    // skybox
    {
        RHIDescriptorSetLayout* descriptorset_layouts[1] = {m_DescriptorInfos[_skybox].layout};
        RHIPipelineLayoutCreateInfo pipeline_layout_create_info {};
        pipeline_layout_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipeline_layout_create_info.setLayoutCount = 1;
        pipeline_layout_create_info.pSetLayouts = descriptorset_layouts;

        if (m_Rhi->CreatePipelineLayout(&pipeline_layout_create_info,
                                        m_RenderPipelines[_render_pipeline_type_skybox].layout) != RHI_SUCCESS)
        {
            throw std::runtime_error("create skybox pipeline layout");
        }

        RHIShader* vert_shader_module = m_Rhi->CreateShaderModule(SKYBOX_VERT);
        RHIShader* frag_shader_module = m_Rhi->CreateShaderModule(SKYBOX_FRAG);

        RHIPipelineShaderStageCreateInfo vert_pipeline_shader_stage_create_info {};
        vert_pipeline_shader_stage_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        vert_pipeline_shader_stage_create_info.stage = RHI_SHADER_STAGE_VERTEX_BIT;
        vert_pipeline_shader_stage_create_info.module = vert_shader_module;
        vert_pipeline_shader_stage_create_info.pName = "main";
        // vert_pipeline_shader_stage_create_info.pSpecializationInfo

        RHIPipelineShaderStageCreateInfo frag_pipeline_shader_stage_create_info {};
        frag_pipeline_shader_stage_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        frag_pipeline_shader_stage_create_info.stage = RHI_SHADER_STAGE_FRAGMENT_BIT;
        frag_pipeline_shader_stage_create_info.module = frag_shader_module;
        frag_pipeline_shader_stage_create_info.pName = "main";

        RHIPipelineShaderStageCreateInfo shader_stages[] = {vert_pipeline_shader_stage_create_info,
                                                            frag_pipeline_shader_stage_create_info};

        auto vertex_binding_descriptions = MeshVertex::GetBindingDescriptions();
        auto vertex_attribute_descriptions = MeshVertex::GetAttributeDescriptions();
        RHIPipelineVertexInputStateCreateInfo vertex_input_state_create_info {};
        vertex_input_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertex_input_state_create_info.vertexBindingDescriptionCount = 0;
        vertex_input_state_create_info.pVertexBindingDescriptions = NULL;
        vertex_input_state_create_info.vertexAttributeDescriptionCount = 0;
        vertex_input_state_create_info.pVertexAttributeDescriptions = NULL;

        RHIPipelineInputAssemblyStateCreateInfo input_assembly_create_info {};
        input_assembly_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        input_assembly_create_info.topology = RHI_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        input_assembly_create_info.primitiveRestartEnable = RHI_FALSE;

        RHIPipelineViewportStateCreateInfo viewport_state_create_info {};
        viewport_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewport_state_create_info.viewportCount = 1;
        viewport_state_create_info.pViewports = m_Rhi->GetSwapchainInfo().viewport;
        viewport_state_create_info.scissorCount = 1;
        viewport_state_create_info.pScissors = m_Rhi->GetSwapchainInfo().scissor;

        RHIPipelineRasterizationStateCreateInfo rasterization_state_create_info {};
        rasterization_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterization_state_create_info.depthClampEnable = RHI_FALSE;
        rasterization_state_create_info.rasterizerDiscardEnable = RHI_FALSE;
        rasterization_state_create_info.polygonMode = RHI_POLYGON_MODE_FILL;
        rasterization_state_create_info.lineWidth = 1.0f;
        rasterization_state_create_info.cullMode = RHI_CULL_MODE_BACK_BIT;
        rasterization_state_create_info.frontFace = RHI_FRONT_FACE_COUNTER_CLOCKWISE;
        rasterization_state_create_info.depthBiasEnable = RHI_FALSE;
        rasterization_state_create_info.depthBiasConstantFactor = 0.0f;
        rasterization_state_create_info.depthBiasClamp = 0.0f;
        rasterization_state_create_info.depthBiasSlopeFactor = 0.0f;

        RHIPipelineMultisampleStateCreateInfo multisample_state_create_info {};
        multisample_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample_state_create_info.sampleShadingEnable = RHI_FALSE;
        multisample_state_create_info.rasterizationSamples = RHI_SAMPLE_COUNT_1_BIT;

        RHIPipelineColorBlendAttachmentState color_blend_attachments[1] = {};
        color_blend_attachments[0].colorWriteMask = RHI_COLOR_COMPONENT_R_BIT | RHI_COLOR_COMPONENT_G_BIT |
                                                    RHI_COLOR_COMPONENT_B_BIT | RHI_COLOR_COMPONENT_A_BIT;
        color_blend_attachments[0].blendEnable = RHI_FALSE;
        color_blend_attachments[0].srcColorBlendFactor = RHI_BLEND_FACTOR_ONE;
        color_blend_attachments[0].dstColorBlendFactor = RHI_BLEND_FACTOR_ZERO;
        color_blend_attachments[0].colorBlendOp = RHI_BLEND_OP_ADD;
        color_blend_attachments[0].srcAlphaBlendFactor = RHI_BLEND_FACTOR_ONE;
        color_blend_attachments[0].dstAlphaBlendFactor = RHI_BLEND_FACTOR_ZERO;
        color_blend_attachments[0].alphaBlendOp = RHI_BLEND_OP_ADD;

        RHIPipelineColorBlendStateCreateInfo color_blend_state_create_info = {};
        color_blend_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        color_blend_state_create_info.logicOpEnable = RHI_FALSE;
        color_blend_state_create_info.logicOp = RHI_LOGIC_OP_COPY;
        color_blend_state_create_info.attachmentCount =
            sizeof(color_blend_attachments) / sizeof(color_blend_attachments[0]);
        color_blend_state_create_info.pAttachments = &color_blend_attachments[0];
        color_blend_state_create_info.blendConstants[0] = 0.0f;
        color_blend_state_create_info.blendConstants[1] = 0.0f;
        color_blend_state_create_info.blendConstants[2] = 0.0f;
        color_blend_state_create_info.blendConstants[3] = 0.0f;

        RHIPipelineDepthStencilStateCreateInfo depth_stencil_create_info {};
        depth_stencil_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depth_stencil_create_info.depthTestEnable = RHI_TRUE;
        depth_stencil_create_info.depthWriteEnable = RHI_TRUE;
        depth_stencil_create_info.depthCompareOp = RHI_COMPARE_OP_LESS;
        depth_stencil_create_info.depthBoundsTestEnable = RHI_FALSE;
        depth_stencil_create_info.stencilTestEnable = RHI_FALSE;

        RHIDynamicState dynamic_states[] = {RHI_DYNAMIC_STATE_VIEWPORT, RHI_DYNAMIC_STATE_SCISSOR};
        RHIPipelineDynamicStateCreateInfo dynamic_state_create_info {};
        dynamic_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic_state_create_info.dynamicStateCount = 2;
        dynamic_state_create_info.pDynamicStates = dynamic_states;

        RHIGraphicsPipelineCreateInfo pipelineInfo {};
        pipelineInfo.sType = RHI_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipelineInfo.stageCount = 2;
        pipelineInfo.pStages = shader_stages;
        pipelineInfo.pVertexInputState = &vertex_input_state_create_info;
        pipelineInfo.pInputAssemblyState = &input_assembly_create_info;
        pipelineInfo.pViewportState = &viewport_state_create_info;
        pipelineInfo.pRasterizationState = &rasterization_state_create_info;
        pipelineInfo.pMultisampleState = &multisample_state_create_info;
        pipelineInfo.pColorBlendState = &color_blend_state_create_info;
        pipelineInfo.pDepthStencilState = &depth_stencil_create_info;
        pipelineInfo.layout = m_RenderPipelines[_render_pipeline_type_skybox].layout;
        pipelineInfo.renderPass = m_Framebuffer1.render_pass;  // RP1
        pipelineInfo.subpass = _main_camera_subpass_forward_lighting;
        pipelineInfo.basePipelineHandle = RHI_NULL_HANDLE;
        pipelineInfo.pDynamicState = &dynamic_state_create_info;

        if (RHI_SUCCESS !=
            m_Rhi->CreateGraphicsPipelines(
                RHI_NULL_HANDLE, 1, &pipelineInfo, m_RenderPipelines[_render_pipeline_type_skybox].pipeline))
        {
            throw std::runtime_error("create skybox graphics pipeline");
        }

        m_Rhi->DestroyShaderModule(vert_shader_module);
        m_Rhi->DestroyShaderModule(frag_shader_module);
    }

    // draw axis
    {
        RHIDescriptorSetLayout* descriptorset_layouts[1] = {m_DescriptorInfos[_axis].layout};
        RHIPipelineLayoutCreateInfo pipeline_layout_create_info {};
        pipeline_layout_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipeline_layout_create_info.setLayoutCount = 1;
        pipeline_layout_create_info.pSetLayouts = descriptorset_layouts;

        if (m_Rhi->CreatePipelineLayout(&pipeline_layout_create_info,
                                        m_RenderPipelines[_render_pipeline_type_axis].layout) != RHI_SUCCESS)
        {
            throw std::runtime_error("create axis pipeline layout");
        }

        RHIShader* vert_shader_module = m_Rhi->CreateShaderModule(AXIS_VERT);
        RHIShader* frag_shader_module = m_Rhi->CreateShaderModule(AXIS_FRAG);

        RHIPipelineShaderStageCreateInfo vert_pipeline_shader_stage_create_info {};
        vert_pipeline_shader_stage_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        vert_pipeline_shader_stage_create_info.stage = RHI_SHADER_STAGE_VERTEX_BIT;
        vert_pipeline_shader_stage_create_info.module = vert_shader_module;
        vert_pipeline_shader_stage_create_info.pName = "main";
        // vert_pipeline_shader_stage_create_info.pSpecializationInfo

        RHIPipelineShaderStageCreateInfo frag_pipeline_shader_stage_create_info {};
        frag_pipeline_shader_stage_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        frag_pipeline_shader_stage_create_info.stage = RHI_SHADER_STAGE_FRAGMENT_BIT;
        frag_pipeline_shader_stage_create_info.module = frag_shader_module;
        frag_pipeline_shader_stage_create_info.pName = "main";

        RHIPipelineShaderStageCreateInfo shader_stages[] = {vert_pipeline_shader_stage_create_info,
                                                            frag_pipeline_shader_stage_create_info};

        auto vertex_binding_descriptions = MeshVertex::GetBindingDescriptions();
        auto vertex_attribute_descriptions = MeshVertex::GetAttributeDescriptions();
        RHIPipelineVertexInputStateCreateInfo vertex_input_state_create_info {};
        vertex_input_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertex_input_state_create_info.vertexBindingDescriptionCount = vertex_binding_descriptions.size();
        vertex_input_state_create_info.pVertexBindingDescriptions = &vertex_binding_descriptions[0];
        vertex_input_state_create_info.vertexAttributeDescriptionCount = vertex_attribute_descriptions.size();
        vertex_input_state_create_info.pVertexAttributeDescriptions = &vertex_attribute_descriptions[0];

        RHIPipelineInputAssemblyStateCreateInfo input_assembly_create_info {};
        input_assembly_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        input_assembly_create_info.topology = RHI_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        input_assembly_create_info.primitiveRestartEnable = RHI_FALSE;

        RHIPipelineViewportStateCreateInfo viewport_state_create_info {};
        viewport_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewport_state_create_info.viewportCount = 1;
        viewport_state_create_info.pViewports = m_Rhi->GetSwapchainInfo().viewport;
        viewport_state_create_info.scissorCount = 1;
        viewport_state_create_info.pScissors = m_Rhi->GetSwapchainInfo().scissor;

        RHIPipelineRasterizationStateCreateInfo rasterization_state_create_info {};
        rasterization_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterization_state_create_info.depthClampEnable = RHI_FALSE;
        rasterization_state_create_info.rasterizerDiscardEnable = RHI_FALSE;
        rasterization_state_create_info.polygonMode = RHI_POLYGON_MODE_FILL;
        rasterization_state_create_info.lineWidth = 1.0f;
        rasterization_state_create_info.cullMode = RHI_CULL_MODE_NONE;
        rasterization_state_create_info.frontFace = RHI_FRONT_FACE_COUNTER_CLOCKWISE;
        rasterization_state_create_info.depthBiasEnable = RHI_FALSE;
        rasterization_state_create_info.depthBiasConstantFactor = 0.0f;
        rasterization_state_create_info.depthBiasClamp = 0.0f;
        rasterization_state_create_info.depthBiasSlopeFactor = 0.0f;

        RHIPipelineMultisampleStateCreateInfo multisample_state_create_info {};
        multisample_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample_state_create_info.sampleShadingEnable = RHI_FALSE;
        multisample_state_create_info.rasterizationSamples = RHI_SAMPLE_COUNT_1_BIT;

        RHIPipelineColorBlendAttachmentState color_blend_attachment_state {};
        color_blend_attachment_state.colorWriteMask = RHI_COLOR_COMPONENT_R_BIT | RHI_COLOR_COMPONENT_G_BIT |
                                                      RHI_COLOR_COMPONENT_B_BIT | RHI_COLOR_COMPONENT_A_BIT;
        color_blend_attachment_state.blendEnable = RHI_FALSE;
        color_blend_attachment_state.srcColorBlendFactor = RHI_BLEND_FACTOR_ONE;
        color_blend_attachment_state.dstColorBlendFactor = RHI_BLEND_FACTOR_ZERO;
        color_blend_attachment_state.colorBlendOp = RHI_BLEND_OP_ADD;
        color_blend_attachment_state.srcAlphaBlendFactor = RHI_BLEND_FACTOR_ONE;
        color_blend_attachment_state.dstAlphaBlendFactor = RHI_BLEND_FACTOR_ZERO;
        color_blend_attachment_state.alphaBlendOp = RHI_BLEND_OP_ADD;

        RHIPipelineColorBlendStateCreateInfo color_blend_state_create_info {};
        color_blend_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        color_blend_state_create_info.logicOpEnable = RHI_FALSE;
        color_blend_state_create_info.logicOp = RHI_LOGIC_OP_COPY;
        color_blend_state_create_info.attachmentCount = 1;
        color_blend_state_create_info.pAttachments = &color_blend_attachment_state;
        color_blend_state_create_info.blendConstants[0] = 0.0f;
        color_blend_state_create_info.blendConstants[1] = 0.0f;
        color_blend_state_create_info.blendConstants[2] = 0.0f;
        color_blend_state_create_info.blendConstants[3] = 0.0f;

        RHIPipelineDepthStencilStateCreateInfo depth_stencil_create_info {};
        depth_stencil_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depth_stencil_create_info.depthTestEnable = RHI_FALSE;
        depth_stencil_create_info.depthWriteEnable = RHI_FALSE;
        depth_stencil_create_info.depthCompareOp = RHI_COMPARE_OP_LESS;
        depth_stencil_create_info.depthBoundsTestEnable = RHI_FALSE;
        depth_stencil_create_info.stencilTestEnable = RHI_FALSE;

        RHIDynamicState dynamic_states[] = {RHI_DYNAMIC_STATE_VIEWPORT, RHI_DYNAMIC_STATE_SCISSOR};
        RHIPipelineDynamicStateCreateInfo dynamic_state_create_info {};
        dynamic_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic_state_create_info.dynamicStateCount = 2;
        dynamic_state_create_info.pDynamicStates = dynamic_states;

        RHIGraphicsPipelineCreateInfo pipelineInfo {};
        pipelineInfo.sType = RHI_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipelineInfo.stageCount = 2;
        pipelineInfo.pStages = shader_stages;
        pipelineInfo.pVertexInputState = &vertex_input_state_create_info;
        pipelineInfo.pInputAssemblyState = &input_assembly_create_info;
        pipelineInfo.pViewportState = &viewport_state_create_info;
        pipelineInfo.pRasterizationState = &rasterization_state_create_info;
        pipelineInfo.pMultisampleState = &multisample_state_create_info;
        pipelineInfo.pColorBlendState = &color_blend_state_create_info;
        pipelineInfo.pDepthStencilState = &depth_stencil_create_info;
        pipelineInfo.layout = m_RenderPipelines[_render_pipeline_type_axis].layout;
        pipelineInfo.renderPass = m_Framebuffer.render_pass;
        pipelineInfo.subpass = _main_camera_subpass_ui;
        pipelineInfo.basePipelineHandle = RHI_NULL_HANDLE;
        pipelineInfo.pDynamicState = &dynamic_state_create_info;

        if (RHI_SUCCESS !=
            m_Rhi->CreateGraphicsPipelines(
                RHI_NULL_HANDLE, 1, &pipelineInfo, m_RenderPipelines[_render_pipeline_type_axis].pipeline))
        {
            throw std::runtime_error("create axis graphics pipeline");
        }

        m_Rhi->DestroyShaderModule(vert_shader_module);
        m_Rhi->DestroyShaderModule(frag_shader_module);
    }
}

void MainCameraPass::SetupDescriptorSet()
{
    SetupModelGlobalDescriptorSet();
    SetupSkyboxDescriptorSet();
    SetupAxisDescriptorSet();
    SetupGbufferLightingDescriptorSet();
}

void MainCameraPass::SetupModelGlobalDescriptorSet()
{
    // update common model's global descriptor set
    RHIDescriptorSetAllocateInfo mesh_global_descriptor_set_alloc_info;
    mesh_global_descriptor_set_alloc_info.sType = RHI_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    mesh_global_descriptor_set_alloc_info.pNext = NULL;
    mesh_global_descriptor_set_alloc_info.descriptorPool = m_Rhi->GetDescriptorPoor();
    mesh_global_descriptor_set_alloc_info.descriptorSetCount = 1;
    mesh_global_descriptor_set_alloc_info.pSetLayouts = &m_DescriptorInfos[_mesh_global].layout;

    if (RHI_SUCCESS != m_Rhi->AllocateDescriptorSets(&mesh_global_descriptor_set_alloc_info,
                                                     m_DescriptorInfos[_mesh_global].descriptor_set))
    {
        throw std::runtime_error("allocate mesh global descriptor set");
    }

    RHIDescriptorBufferInfo mesh_perframe_storage_buffer_info = {};
    // this offset plus dynamic_offset should not be greater than the size of the buffer
    mesh_perframe_storage_buffer_info.offset = 0;
    // the range means the size actually used by the shader per draw call
    mesh_perframe_storage_buffer_info.range = sizeof(MeshPerframeStorageBufferObject);
    mesh_perframe_storage_buffer_info.buffer = m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbuffer;
    assert(mesh_perframe_storage_buffer_info.range <
           m_GlobalRenderResource->m_StorageBuffer.m_MaxStorageBufferRange);

    RHIDescriptorBufferInfo mesh_perdrawcall_storage_buffer_info = {};
    mesh_perdrawcall_storage_buffer_info.offset = 0;
    mesh_perdrawcall_storage_buffer_info.range = sizeof(MeshPerdrawcallStorageBufferObject);
    mesh_perdrawcall_storage_buffer_info.buffer = m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbuffer;
    assert(mesh_perdrawcall_storage_buffer_info.range <
           m_GlobalRenderResource->m_StorageBuffer.m_MaxStorageBufferRange);

    RHIDescriptorBufferInfo mesh_per_drawcall_vertex_blending_storage_buffer_info = {};
    mesh_per_drawcall_vertex_blending_storage_buffer_info.offset = 0;
    mesh_per_drawcall_vertex_blending_storage_buffer_info.range =
        sizeof(MeshPerdrawcallVertexBlendingStorageBufferObject);
    mesh_per_drawcall_vertex_blending_storage_buffer_info.buffer =
        m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbuffer;
    assert(mesh_per_drawcall_vertex_blending_storage_buffer_info.range <
           m_GlobalRenderResource->m_StorageBuffer.m_MaxStorageBufferRange);

    RHIDescriptorImageInfo brdf_texture_image_info = {};
    brdf_texture_image_info.sampler = m_GlobalRenderResource->m_IblResource.m_BrdflutTextureSampler;
    brdf_texture_image_info.imageView = m_GlobalRenderResource->m_IblResource.m_BrdflutTextureImageView;
    brdf_texture_image_info.imageLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    RHIDescriptorImageInfo irradiance_texture_image_info = {};
    irradiance_texture_image_info.sampler = m_GlobalRenderResource->m_IblResource.m_IrradianceTextureSampler;
    irradiance_texture_image_info.imageView = m_GlobalRenderResource->m_IblResource.m_IrradianceTextureImageView;
    irradiance_texture_image_info.imageLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    RHIDescriptorImageInfo specular_texture_image_info {};
    specular_texture_image_info.sampler = m_GlobalRenderResource->m_IblResource.m_SpecularTextureSampler;
    specular_texture_image_info.imageView = m_GlobalRenderResource->m_IblResource.m_SpecularTextureImageView;
    specular_texture_image_info.imageLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    RHIDescriptorImageInfo point_light_shadow_texture_image_info {};
    point_light_shadow_texture_image_info.sampler = m_Rhi->GetOrCreateDefaultSampler(Default_Sampler_Nearest);
    point_light_shadow_texture_image_info.imageView = m_PointLightShadowColorImageView;
    point_light_shadow_texture_image_info.imageLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    RHIDescriptorImageInfo directional_light_shadow_texture_image_info {};
    directional_light_shadow_texture_image_info.sampler = m_Rhi->GetOrCreateDefaultSampler(Default_Sampler_Nearest);
    directional_light_shadow_texture_image_info.imageView = m_DirectionalLightShadowColorImageView;
    directional_light_shadow_texture_image_info.imageLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    RHIWriteDescriptorSet mesh_descriptor_writes_info[13];

    mesh_descriptor_writes_info[0].sType = RHI_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    mesh_descriptor_writes_info[0].pNext = NULL;
    mesh_descriptor_writes_info[0].dstSet = m_DescriptorInfos[_mesh_global].descriptor_set;
    mesh_descriptor_writes_info[0].dstBinding = 0;
    mesh_descriptor_writes_info[0].dstArrayElement = 0;
    mesh_descriptor_writes_info[0].descriptorType = RHI_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
    mesh_descriptor_writes_info[0].descriptorCount = 1;
    mesh_descriptor_writes_info[0].pBufferInfo = &mesh_perframe_storage_buffer_info;

    mesh_descriptor_writes_info[1].sType = RHI_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    mesh_descriptor_writes_info[1].pNext = NULL;
    mesh_descriptor_writes_info[1].dstSet = m_DescriptorInfos[_mesh_global].descriptor_set;
    mesh_descriptor_writes_info[1].dstBinding = 1;
    mesh_descriptor_writes_info[1].dstArrayElement = 0;
    mesh_descriptor_writes_info[1].descriptorType = RHI_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
    mesh_descriptor_writes_info[1].descriptorCount = 1;
    mesh_descriptor_writes_info[1].pBufferInfo = &mesh_perdrawcall_storage_buffer_info;

    mesh_descriptor_writes_info[2].sType = RHI_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    mesh_descriptor_writes_info[2].pNext = NULL;
    mesh_descriptor_writes_info[2].dstSet = m_DescriptorInfos[_mesh_global].descriptor_set;
    mesh_descriptor_writes_info[2].dstBinding = 2;
    mesh_descriptor_writes_info[2].dstArrayElement = 0;
    mesh_descriptor_writes_info[2].descriptorType = RHI_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
    mesh_descriptor_writes_info[2].descriptorCount = 1;
    mesh_descriptor_writes_info[2].pBufferInfo = &mesh_per_drawcall_vertex_blending_storage_buffer_info;

    mesh_descriptor_writes_info[3].sType = RHI_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    mesh_descriptor_writes_info[3].pNext = NULL;
    mesh_descriptor_writes_info[3].dstSet = m_DescriptorInfos[_mesh_global].descriptor_set;
    mesh_descriptor_writes_info[3].dstBinding = 3;
    mesh_descriptor_writes_info[3].dstArrayElement = 0;
    mesh_descriptor_writes_info[3].descriptorType = RHI_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    mesh_descriptor_writes_info[3].descriptorCount = 1;
    mesh_descriptor_writes_info[3].pImageInfo = &brdf_texture_image_info;

    mesh_descriptor_writes_info[4] = mesh_descriptor_writes_info[3];
    mesh_descriptor_writes_info[4].dstBinding = 4;
    mesh_descriptor_writes_info[4].pImageInfo = &irradiance_texture_image_info;

    mesh_descriptor_writes_info[5] = mesh_descriptor_writes_info[3];
    mesh_descriptor_writes_info[5].dstBinding = 5;
    mesh_descriptor_writes_info[5].pImageInfo = &specular_texture_image_info;

    mesh_descriptor_writes_info[6] = mesh_descriptor_writes_info[3];
    mesh_descriptor_writes_info[6].dstBinding = 6;
    mesh_descriptor_writes_info[6].pImageInfo = &point_light_shadow_texture_image_info;

    mesh_descriptor_writes_info[7] = mesh_descriptor_writes_info[3];
    mesh_descriptor_writes_info[7].dstBinding = 7;
    mesh_descriptor_writes_info[7].pImageInfo = &directional_light_shadow_texture_image_info;

    RHIDescriptorBufferInfo megalights_lights_buffer_info = {};
    RHIDescriptorBufferInfo megalights_indices_buffer_info = {};
    RHIDescriptorBufferInfo megalights_ranges_buffer_info = {};
    if (m_MegaLightsSystem != nullptr)
    {
        megalights_lights_buffer_info  = m_MegaLightsSystem->GetLightsBufferInfo();
        megalights_indices_buffer_info = m_MegaLightsSystem->GetTileIndicesBufferInfo();
        megalights_ranges_buffer_info  = m_MegaLightsSystem->GetTileRangesBufferInfo();
    }

    mesh_descriptor_writes_info[8].sType = RHI_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    mesh_descriptor_writes_info[8].dstSet = m_DescriptorInfos[_mesh_global].descriptor_set;
    mesh_descriptor_writes_info[8].dstBinding = 8;
    mesh_descriptor_writes_info[8].descriptorType = RHI_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    mesh_descriptor_writes_info[8].descriptorCount = 1;
    mesh_descriptor_writes_info[8].pBufferInfo = &megalights_lights_buffer_info;

    mesh_descriptor_writes_info[9] = mesh_descriptor_writes_info[8];
    mesh_descriptor_writes_info[9].dstBinding = 9;
    mesh_descriptor_writes_info[9].pBufferInfo = &megalights_indices_buffer_info;

    mesh_descriptor_writes_info[10] = mesh_descriptor_writes_info[8];
    mesh_descriptor_writes_info[10].dstBinding = 10;
    mesh_descriptor_writes_info[10].pBufferInfo = &megalights_ranges_buffer_info;

    RHIDescriptorImageInfo megalights_history_read_info = {};
    RHIDescriptorImageInfo megalights_history_write_info = {};
    if (m_MegaLightsSystem != nullptr)
    {
        megalights_history_read_info = m_MegaLightsSystem->GetHistoryReadImageInfo(ViewportType::game);
        megalights_history_write_info = m_MegaLightsSystem->GetHistoryWriteImageInfo(ViewportType::game);
    }

    mesh_descriptor_writes_info[11].sType = RHI_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    mesh_descriptor_writes_info[11].dstSet = m_DescriptorInfos[_mesh_global].descriptor_set;
    mesh_descriptor_writes_info[11].dstBinding = 11;
    mesh_descriptor_writes_info[11].descriptorType = RHI_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    mesh_descriptor_writes_info[11].descriptorCount = 1;
    mesh_descriptor_writes_info[11].pImageInfo = &megalights_history_read_info;

    mesh_descriptor_writes_info[12].sType = RHI_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    mesh_descriptor_writes_info[12].dstSet = m_DescriptorInfos[_mesh_global].descriptor_set;
    mesh_descriptor_writes_info[12].dstBinding = 12;
    mesh_descriptor_writes_info[12].descriptorType = RHI_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    mesh_descriptor_writes_info[12].descriptorCount = 1;
    mesh_descriptor_writes_info[12].pImageInfo = &megalights_history_write_info;

    m_Rhi->UpdateDescriptorSets(sizeof(mesh_descriptor_writes_info) / sizeof(mesh_descriptor_writes_info[0]),
                                mesh_descriptor_writes_info,
                                0,
                                NULL);
}

void MainCameraPass::UpdateMegaLightsDescriptorSets(ViewportType viewport_type)
{
    if (m_MegaLightsSystem == nullptr)
    {
        return;
    }

    RHIDescriptorBufferInfo megalights_lights_buffer_info  = m_MegaLightsSystem->GetLightsBufferInfo();
    RHIDescriptorBufferInfo megalights_indices_buffer_info = m_MegaLightsSystem->GetTileIndicesBufferInfo();
    RHIDescriptorBufferInfo megalights_ranges_buffer_info  = m_MegaLightsSystem->GetTileRangesBufferInfo();
    RHIDescriptorImageInfo megalights_history_read_info = m_MegaLightsSystem->GetHistoryReadImageInfo(viewport_type);
    RHIDescriptorImageInfo megalights_history_write_info = m_MegaLightsSystem->GetHistoryWriteImageInfo(viewport_type);

    RHIWriteDescriptorSet writes[5] {};
    for (uint32_t i = 0; i < 3; ++i)
    {
        writes[i].sType          = RHI_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet           = m_DescriptorInfos[_mesh_global].descriptor_set;
        writes[i].dstBinding       = 8 + i;
        writes[i].descriptorType = RHI_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].descriptorCount = 1;
    }
    writes[0].pBufferInfo = &megalights_lights_buffer_info;
    writes[1].pBufferInfo = &megalights_indices_buffer_info;
    writes[2].pBufferInfo = &megalights_ranges_buffer_info;

    writes[3].sType = RHI_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[3].dstSet = m_DescriptorInfos[_mesh_global].descriptor_set;
    writes[3].dstBinding = 11;
    writes[3].descriptorType = RHI_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[3].descriptorCount = 1;
    writes[3].pImageInfo = &megalights_history_read_info;

    writes[4].sType = RHI_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[4].dstSet = m_DescriptorInfos[_mesh_global].descriptor_set;
    writes[4].dstBinding = 12;
    writes[4].descriptorType = RHI_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[4].descriptorCount = 1;
    writes[4].pImageInfo = &megalights_history_write_info;

    m_Rhi->UpdateDescriptorSets(5, writes, 0, nullptr);
}

void MainCameraPass::UpdateMegaLightsSpatialDescriptorSets(ViewportType viewport_type)
{
    if (m_MegaLightsSystem == nullptr)
    {
        return;
    }

    RHIDescriptorBufferInfo megalights_lights_buffer_info  = m_MegaLightsSystem->GetLightsBufferInfo();
    RHIDescriptorBufferInfo megalights_indices_buffer_info = m_MegaLightsSystem->GetTileIndicesBufferInfo();
    RHIDescriptorBufferInfo megalights_ranges_buffer_info  = m_MegaLightsSystem->GetTileRangesBufferInfo();
    RHIDescriptorImageInfo megalights_direct_sample_info =
        m_MegaLightsSystem->GetSpatialDirectSampleImageInfo(viewport_type);
    RHIDescriptorImageInfo megalights_history_write_info = m_MegaLightsSystem->GetHistoryWriteImageInfo(viewport_type);

    RHIWriteDescriptorSet writes[5] {};
    for (uint32_t i = 0; i < 3; ++i)
    {
        writes[i].sType           = RHI_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet          = m_DescriptorInfos[_mesh_global].descriptor_set;
        writes[i].dstBinding      = 8 + i;
        writes[i].descriptorType  = RHI_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].descriptorCount = 1;
    }
    writes[0].pBufferInfo = &megalights_lights_buffer_info;
    writes[1].pBufferInfo = &megalights_indices_buffer_info;
    writes[2].pBufferInfo = &megalights_ranges_buffer_info;

    writes[3].sType = RHI_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[3].dstSet = m_DescriptorInfos[_mesh_global].descriptor_set;
    writes[3].dstBinding = 11;
    writes[3].descriptorType = RHI_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[3].descriptorCount = 1;
    writes[3].pImageInfo = &megalights_direct_sample_info;

    writes[4].sType = RHI_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[4].dstSet = m_DescriptorInfos[_mesh_global].descriptor_set;
    writes[4].dstBinding = 12;
    writes[4].descriptorType = RHI_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[4].descriptorCount = 1;
    writes[4].pImageInfo = &megalights_history_write_info;

    m_Rhi->UpdateDescriptorSets(5, writes, 0, nullptr);
}

void MainCameraPass::SetupSkyboxDescriptorSet()
{
    RHIDescriptorSetAllocateInfo skybox_descriptor_set_alloc_info;
    skybox_descriptor_set_alloc_info.sType = RHI_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    skybox_descriptor_set_alloc_info.pNext = NULL;
    skybox_descriptor_set_alloc_info.descriptorPool = m_Rhi->GetDescriptorPoor();
    skybox_descriptor_set_alloc_info.descriptorSetCount = 1;
    skybox_descriptor_set_alloc_info.pSetLayouts = &m_DescriptorInfos[_skybox].layout;

    if (RHI_SUCCESS !=
        m_Rhi->AllocateDescriptorSets(&skybox_descriptor_set_alloc_info, m_DescriptorInfos[_skybox].descriptor_set))
    {
        throw std::runtime_error("allocate skybox descriptor set");
    }

    RHIDescriptorBufferInfo mesh_perframe_storage_buffer_info = {};
    mesh_perframe_storage_buffer_info.offset = 0;
    mesh_perframe_storage_buffer_info.range = sizeof(MeshPerframeStorageBufferObject);
    mesh_perframe_storage_buffer_info.buffer = m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbuffer;
    assert(mesh_perframe_storage_buffer_info.range <
           m_GlobalRenderResource->m_StorageBuffer.m_MaxStorageBufferRange);

    RHIDescriptorImageInfo specular_texture_image_info = {};
    specular_texture_image_info.sampler = m_GlobalRenderResource->m_IblResource.m_SpecularTextureSampler;
    specular_texture_image_info.imageView = m_GlobalRenderResource->m_IblResource.m_SpecularTextureImageView;
    specular_texture_image_info.imageLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    RHIWriteDescriptorSet skybox_descriptor_writes_info[2];

    skybox_descriptor_writes_info[0].sType = RHI_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    skybox_descriptor_writes_info[0].pNext = NULL;
    skybox_descriptor_writes_info[0].dstSet = m_DescriptorInfos[_skybox].descriptor_set;
    skybox_descriptor_writes_info[0].dstBinding = 0;
    skybox_descriptor_writes_info[0].dstArrayElement = 0;
    skybox_descriptor_writes_info[0].descriptorType = RHI_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
    skybox_descriptor_writes_info[0].descriptorCount = 1;
    skybox_descriptor_writes_info[0].pBufferInfo = &mesh_perframe_storage_buffer_info;

    skybox_descriptor_writes_info[1].sType = RHI_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    skybox_descriptor_writes_info[1].pNext = NULL;
    skybox_descriptor_writes_info[1].dstSet = m_DescriptorInfos[_skybox].descriptor_set;
    skybox_descriptor_writes_info[1].dstBinding = 1;
    skybox_descriptor_writes_info[1].dstArrayElement = 0;
    skybox_descriptor_writes_info[1].descriptorType = RHI_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    skybox_descriptor_writes_info[1].descriptorCount = 1;
    skybox_descriptor_writes_info[1].pImageInfo = &specular_texture_image_info;

    m_Rhi->UpdateDescriptorSets(2, skybox_descriptor_writes_info, 0, NULL);
}

void MainCameraPass::SetupAxisDescriptorSet()
{
    RHIDescriptorSetAllocateInfo axis_descriptor_set_alloc_info;
    axis_descriptor_set_alloc_info.sType = RHI_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    axis_descriptor_set_alloc_info.pNext = NULL;
    axis_descriptor_set_alloc_info.descriptorPool = m_Rhi->GetDescriptorPoor();
    axis_descriptor_set_alloc_info.descriptorSetCount = 1;
    axis_descriptor_set_alloc_info.pSetLayouts = &m_DescriptorInfos[_axis].layout;

    if (RHI_SUCCESS !=
        m_Rhi->AllocateDescriptorSets(&axis_descriptor_set_alloc_info, m_DescriptorInfos[_axis].descriptor_set))
    {
        throw std::runtime_error("allocate axis descriptor set");
    }

    RHIDescriptorBufferInfo mesh_perframe_storage_buffer_info = {};
    mesh_perframe_storage_buffer_info.offset = 0;
    mesh_perframe_storage_buffer_info.range = sizeof(MeshPerframeStorageBufferObject);
    mesh_perframe_storage_buffer_info.buffer = m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbuffer;
    assert(mesh_perframe_storage_buffer_info.range <
           m_GlobalRenderResource->m_StorageBuffer.m_MaxStorageBufferRange);

    RHIDescriptorBufferInfo axis_storage_buffer_info = {};
    axis_storage_buffer_info.offset = 0;
    axis_storage_buffer_info.range = sizeof(AxisStorageBufferObject);
    axis_storage_buffer_info.buffer = m_GlobalRenderResource->m_StorageBuffer.m_AxisInefficientStorageBuffer;

    RHIWriteDescriptorSet axis_descriptor_writes_info[2];

    axis_descriptor_writes_info[0].sType = RHI_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    axis_descriptor_writes_info[0].pNext = NULL;
    axis_descriptor_writes_info[0].dstSet = m_DescriptorInfos[_axis].descriptor_set;
    axis_descriptor_writes_info[0].dstBinding = 0;
    axis_descriptor_writes_info[0].dstArrayElement = 0;
    axis_descriptor_writes_info[0].descriptorType = RHI_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
    axis_descriptor_writes_info[0].descriptorCount = 1;
    axis_descriptor_writes_info[0].pBufferInfo = &mesh_perframe_storage_buffer_info;

    axis_descriptor_writes_info[1].sType = RHI_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    axis_descriptor_writes_info[1].pNext = NULL;
    axis_descriptor_writes_info[1].dstSet = m_DescriptorInfos[_axis].descriptor_set;
    axis_descriptor_writes_info[1].dstBinding = 1;
    axis_descriptor_writes_info[1].dstArrayElement = 0;
    axis_descriptor_writes_info[1].descriptorType = RHI_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    axis_descriptor_writes_info[1].descriptorCount = 1;
    axis_descriptor_writes_info[1].pBufferInfo = &axis_storage_buffer_info;

    m_Rhi->UpdateDescriptorSets(
        (uint32_t)(sizeof(axis_descriptor_writes_info) / sizeof(axis_descriptor_writes_info[0])),
        axis_descriptor_writes_info,
        0,
        NULL);
}

void MainCameraPass::SetupGbufferLightingDescriptorSet()
{
    RHIDescriptorSetAllocateInfo gbuffer_light_global_descriptor_set_alloc_info;
    gbuffer_light_global_descriptor_set_alloc_info.sType = RHI_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    gbuffer_light_global_descriptor_set_alloc_info.pNext = NULL;
    gbuffer_light_global_descriptor_set_alloc_info.descriptorPool = m_Rhi->GetDescriptorPoor();
    gbuffer_light_global_descriptor_set_alloc_info.descriptorSetCount = 1;
    gbuffer_light_global_descriptor_set_alloc_info.pSetLayouts = &m_DescriptorInfos[_deferred_lighting].layout;

    if (RHI_SUCCESS != m_Rhi->AllocateDescriptorSets(&gbuffer_light_global_descriptor_set_alloc_info,
                                                     m_DescriptorInfos[_deferred_lighting].descriptor_set))
    {
        throw std::runtime_error("allocate gbuffer light global descriptor set");
    }
}

void MainCameraPass::SetupFramebufferDescriptorSet()
{
    RHIDescriptorImageInfo gbuffer_normal_input_attachment_info = {};
    gbuffer_normal_input_attachment_info.sampler = m_Rhi->GetOrCreateDefaultSampler(Default_Sampler_Nearest);
    gbuffer_normal_input_attachment_info.imageView = m_Framebuffer.attachments[_main_camera_pass_gbuffer_a].view;
    gbuffer_normal_input_attachment_info.imageLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    RHIDescriptorImageInfo gbuffer_metallic_roughness_shadingmodeid_input_attachment_info = {};
    gbuffer_metallic_roughness_shadingmodeid_input_attachment_info.sampler =
        m_Rhi->GetOrCreateDefaultSampler(Default_Sampler_Nearest);
    gbuffer_metallic_roughness_shadingmodeid_input_attachment_info.imageView =
        m_Framebuffer.attachments[_main_camera_pass_gbuffer_b].view;
    gbuffer_metallic_roughness_shadingmodeid_input_attachment_info.imageLayout =
        RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    RHIDescriptorImageInfo gbuffer_albedo_input_attachment_info = {};
    gbuffer_albedo_input_attachment_info.sampler = m_Rhi->GetOrCreateDefaultSampler(Default_Sampler_Nearest);
    gbuffer_albedo_input_attachment_info.imageView = m_Framebuffer.attachments[_main_camera_pass_gbuffer_c].view;
    gbuffer_albedo_input_attachment_info.imageLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    RHIDescriptorImageInfo depth_input_attachment_info = {};
    depth_input_attachment_info.sampler = m_Rhi->GetOrCreateDefaultSampler(Default_Sampler_Nearest);
    depth_input_attachment_info.imageView = m_Rhi->GetDepthImageInfo().depth_image_view;
    depth_input_attachment_info.imageLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    RHIWriteDescriptorSet deferred_lighting_descriptor_writes_info[4];

    RHIWriteDescriptorSet& gbuffer_normal_descriptor_input_attachment_write_info =
        deferred_lighting_descriptor_writes_info[0];
    gbuffer_normal_descriptor_input_attachment_write_info.sType = RHI_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    gbuffer_normal_descriptor_input_attachment_write_info.pNext = NULL;
    gbuffer_normal_descriptor_input_attachment_write_info.dstSet =
        m_DescriptorInfos[_deferred_lighting].descriptor_set;
    gbuffer_normal_descriptor_input_attachment_write_info.dstBinding = 0;
    gbuffer_normal_descriptor_input_attachment_write_info.dstArrayElement = 0;
    gbuffer_normal_descriptor_input_attachment_write_info.descriptorType = RHI_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
    gbuffer_normal_descriptor_input_attachment_write_info.descriptorCount = 1;
    gbuffer_normal_descriptor_input_attachment_write_info.pImageInfo = &gbuffer_normal_input_attachment_info;

    RHIWriteDescriptorSet& gbuffer_metallic_roughness_shadingmodeid_descriptor_input_attachment_write_info =
        deferred_lighting_descriptor_writes_info[1];
    gbuffer_metallic_roughness_shadingmodeid_descriptor_input_attachment_write_info.sType =
        RHI_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    gbuffer_metallic_roughness_shadingmodeid_descriptor_input_attachment_write_info.pNext = NULL;
    gbuffer_metallic_roughness_shadingmodeid_descriptor_input_attachment_write_info.dstSet =
        m_DescriptorInfos[_deferred_lighting].descriptor_set;
    gbuffer_metallic_roughness_shadingmodeid_descriptor_input_attachment_write_info.dstBinding = 1;
    gbuffer_metallic_roughness_shadingmodeid_descriptor_input_attachment_write_info.dstArrayElement = 0;
    gbuffer_metallic_roughness_shadingmodeid_descriptor_input_attachment_write_info.descriptorType =
        RHI_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
    gbuffer_metallic_roughness_shadingmodeid_descriptor_input_attachment_write_info.descriptorCount = 1;
    gbuffer_metallic_roughness_shadingmodeid_descriptor_input_attachment_write_info.pImageInfo =
        &gbuffer_metallic_roughness_shadingmodeid_input_attachment_info;

    RHIWriteDescriptorSet& gbuffer_albedo_descriptor_input_attachment_write_info =
        deferred_lighting_descriptor_writes_info[2];
    gbuffer_albedo_descriptor_input_attachment_write_info.sType = RHI_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    gbuffer_albedo_descriptor_input_attachment_write_info.pNext = NULL;
    gbuffer_albedo_descriptor_input_attachment_write_info.dstSet =
        m_DescriptorInfos[_deferred_lighting].descriptor_set;
    gbuffer_albedo_descriptor_input_attachment_write_info.dstBinding = 2;
    gbuffer_albedo_descriptor_input_attachment_write_info.dstArrayElement = 0;
    gbuffer_albedo_descriptor_input_attachment_write_info.descriptorType = RHI_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
    gbuffer_albedo_descriptor_input_attachment_write_info.descriptorCount = 1;
    gbuffer_albedo_descriptor_input_attachment_write_info.pImageInfo = &gbuffer_albedo_input_attachment_info;

    RHIWriteDescriptorSet& depth_descriptor_input_attachment_write_info = deferred_lighting_descriptor_writes_info[3];
    depth_descriptor_input_attachment_write_info.sType = RHI_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    depth_descriptor_input_attachment_write_info.pNext = NULL;
    depth_descriptor_input_attachment_write_info.dstSet = m_DescriptorInfos[_deferred_lighting].descriptor_set;
    depth_descriptor_input_attachment_write_info.dstBinding = 3;
    depth_descriptor_input_attachment_write_info.dstArrayElement = 0;
    depth_descriptor_input_attachment_write_info.descriptorType = RHI_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
    depth_descriptor_input_attachment_write_info.descriptorCount = 1;
    depth_descriptor_input_attachment_write_info.pImageInfo = &depth_input_attachment_info;

    m_Rhi->UpdateDescriptorSets(sizeof(deferred_lighting_descriptor_writes_info) /
                                    sizeof(deferred_lighting_descriptor_writes_info[0]),
                                deferred_lighting_descriptor_writes_info,
                                0,
                                NULL);

    SetupMegaLightsSpatialSurfacesDescriptorSet();
}

void MainCameraPass::SetupMegaLightsSpatialSurfacesDescriptorSet()
{
    if (m_DescriptorInfos[_megalights_spatial_surfaces].descriptor_set == nullptr)
    {
        RHIDescriptorSetAllocateInfo alloc_info {};
        alloc_info.sType = RHI_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc_info.descriptorPool = m_Rhi->GetDescriptorPoor();
        alloc_info.descriptorSetCount = 1;
        alloc_info.pSetLayouts = &m_DescriptorInfos[_megalights_spatial_surfaces].layout;

        if (RHI_SUCCESS != m_Rhi->AllocateDescriptorSets(&alloc_info,
                                                         m_DescriptorInfos[_megalights_spatial_surfaces].descriptor_set))
        {
            throw std::runtime_error("allocate megalights spatial surfaces descriptor set");
        }
    }

    RHIDescriptorImageInfo gbuffer_a_info {};
    gbuffer_a_info.sampler = m_Rhi->GetOrCreateDefaultSampler(Default_Sampler_Linear);
    gbuffer_a_info.imageView = m_Framebuffer.attachments[_main_camera_pass_gbuffer_a].view;
    gbuffer_a_info.imageLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    RHIDescriptorImageInfo gbuffer_b_info = gbuffer_a_info;
    gbuffer_b_info.imageView = m_Framebuffer.attachments[_main_camera_pass_gbuffer_b].view;

    RHIDescriptorImageInfo gbuffer_c_info = gbuffer_a_info;
    gbuffer_c_info.imageView = m_Framebuffer.attachments[_main_camera_pass_gbuffer_c].view;

    RHIDescriptorImageInfo depth_info = gbuffer_a_info;
    depth_info.sampler = m_Rhi->GetOrCreateDefaultSampler(Default_Sampler_Nearest);
    depth_info.imageView = m_Rhi->GetDepthImageInfo().depth_image_view;

    RHIWriteDescriptorSet writes[4] {};
    for (uint32_t binding_index = 0; binding_index < 4; ++binding_index)
    {
        writes[binding_index].sType = RHI_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[binding_index].dstSet = m_DescriptorInfos[_megalights_spatial_surfaces].descriptor_set;
        writes[binding_index].dstBinding = binding_index;
        writes[binding_index].descriptorType = RHI_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[binding_index].descriptorCount = 1;
    }
    writes[0].pImageInfo = &gbuffer_a_info;
    writes[1].pImageInfo = &gbuffer_b_info;
    writes[2].pImageInfo = &gbuffer_c_info;
    writes[3].pImageInfo = &depth_info;

    m_Rhi->UpdateDescriptorSets(4, writes, 0, nullptr);
}

// Two framebuffers per swapchain image now:
//   * m_Rp1Framebuffer: ONE framebuffer (no swapchain attachment), 5 views
//     (gbuffer_a, gbuffer_b, gbuffer_c, backup_odd, depth) -- matches RP1.
//   * m_SwapchainFramebuffers[i]: per swapchain image, 4 views
//     (backup_odd, backup_even, post_process_odd, swapchain[i]) -- matches RP2.
// BindlessTonemapPass owns its own FB internally; nothing to wire here.
void MainCameraPass::SetupSwapchainFramebuffers()
{
    // ---- RP1 framebuffer (single, swapchain-independent) ----
    {
        RHIImageView* rp1_attachments[5] = {
            m_Framebuffer.attachments[_main_camera_pass_gbuffer_a].view,
            m_Framebuffer.attachments[_main_camera_pass_gbuffer_b].view,
            m_Framebuffer.attachments[_main_camera_pass_gbuffer_c].view,
            m_Framebuffer.attachments[_main_camera_pass_backup_buffer_odd].view,
            m_Rhi->GetDepthImageInfo().depth_image_view,
        };

        RHIFramebufferCreateInfo fb_ci {};
        fb_ci.sType = RHI_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb_ci.flags = 0U;
        fb_ci.renderPass = m_Framebuffer1.render_pass;
        fb_ci.attachmentCount = sizeof(rp1_attachments) / sizeof(rp1_attachments[0]);
        fb_ci.pAttachments = rp1_attachments;
        fb_ci.width = m_Rhi->GetSwapchainInfo().extent.width;
        fb_ci.height = m_Rhi->GetSwapchainInfo().extent.height;
        fb_ci.layers = 1;

        m_Rp1Framebuffer = new VulkanFramebuffer();
        if (RHI_SUCCESS != m_Rhi->CreateFramebuffer(&fb_ci, m_Rp1Framebuffer))
        {
            throw std::runtime_error("create main_camera RP1 framebuffer");
        }
    }

    // ---- RP2 framebuffers (per swapchain image) ----
    m_SwapchainFramebuffers.resize(m_Rhi->GetSwapchainInfo().imageViews.size());
    for (size_t i = 0; i < m_Rhi->GetSwapchainInfo().imageViews.size(); i++)
    {
        // Order MUST match setupRenderPass2's kRP2_* enum.
        RHIImageView* rp2_attachments[4] = {
            m_Framebuffer.attachments[_main_camera_pass_backup_buffer_odd].view,        // kRP2_BackupOdd
            m_Framebuffer.attachments[_main_camera_pass_backup_buffer_even].view,       // kRP2_BackupEven
            m_Framebuffer.attachments[_main_camera_pass_post_process_buffer_odd].view,  // kRP2_PostOdd
            m_Rhi->GetSwapchainInfo().imageViews[i],                                    // kRP2_Swapchain
        };

        RHIFramebufferCreateInfo fb_ci {};
        fb_ci.sType = RHI_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb_ci.flags = 0U;
        fb_ci.renderPass = m_Framebuffer.render_pass;
        fb_ci.attachmentCount = sizeof(rp2_attachments) / sizeof(rp2_attachments[0]);
        fb_ci.pAttachments = rp2_attachments;
        fb_ci.width = m_Rhi->GetSwapchainInfo().extent.width;
        fb_ci.height = m_Rhi->GetSwapchainInfo().extent.height;
        fb_ci.layers = 1;

        m_SwapchainFramebuffers[i] = new VulkanFramebuffer();
        if (RHI_SUCCESS != m_Rhi->CreateFramebuffer(&fb_ci, m_SwapchainFramebuffers[i]))
        {
            throw std::runtime_error("create main_camera RP2 framebuffer");
        }
    }
}

void MainCameraPass::UpdateAfterFramebufferRecreate()
{
    for (size_t i = 0; i < m_Framebuffer.attachments.size(); i++)
    {
        m_Rhi->DestroyImage(m_Framebuffer.attachments[i].image);
        m_Rhi->DestroyImageView(m_Framebuffer.attachments[i].view);
        m_Rhi->FreeMemory(m_Framebuffer.attachments[i].mem);
    }

    if (m_Rp1Framebuffer)
    {
        m_Rhi->DestroyFramebuffer(m_Rp1Framebuffer);
        m_Rp1Framebuffer = nullptr;
    }

    for (auto framebuffer : m_SwapchainFramebuffers)
    {
        m_Rhi->DestroyFramebuffer(framebuffer);
    }

    SetupAttachments();

    SetupFramebufferDescriptorSet();

    SetupSwapchainFramebuffers();

    SetupParticlePass();
}

void MainCameraPass::Draw(ColorGradingPass& color_grading_pass,
                          FXAAPass& fxaa_pass,
                          BindlessTonemapPass& tone_mapping_pass,
                          RenderPass& ui_pass,
                          CombineUIPass& combine_ui_pass,
                          ParticlePass& particle_pass,
                          uint32_t current_swapchain_image_index,
                          const std::vector<RenderCallback>& post_ui_callbacks)
{
    constexpr float k_scene_clear_r = 0.290f;
    constexpr float k_scene_clear_g = 0.345f;
    constexpr float k_scene_clear_b = 0.435f;

    // ====================================================================
    // RP1: gbuffer -> deferred lighting -> forward lighting
    // FB: m_Rp1Framebuffer (single, swapchain-independent)
    // 5 attachments: gbuffer_a, gbuffer_b, gbuffer_c, backup_odd, depth
    // ====================================================================
    {
        RHIRenderPassBeginInfo renderpass_begin_info {};
        renderpass_begin_info.sType = RHI_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        renderpass_begin_info.renderPass = m_Framebuffer1.render_pass;
        renderpass_begin_info.framebuffer = m_Rp1Framebuffer;
        renderpass_begin_info.renderArea.offset = {0, 0};
        renderpass_begin_info.renderArea.extent = m_Rhi->GetSwapchainInfo().extent;

        // Clear values must match attachment order in setupRenderPass1.
        RHIClearValue clear_values[5];
        clear_values[0].color = {{0.0f, 0.0f, 0.0f, 0.0f}};                                   // gbuffer_a
        clear_values[1].color = {{0.0f, 0.0f, 0.0f, 0.0f}};                                   // gbuffer_b
        clear_values[2].color = {{0.0f, 0.0f, 0.0f, 0.0f}};                                   // gbuffer_c
        clear_values[3].color = {{k_scene_clear_r, k_scene_clear_g, k_scene_clear_b, 1.0f}};  // backup_odd
        clear_values[4].depthStencil = {1.0f, 0};                                             // depth
        renderpass_begin_info.clearValueCount = sizeof(clear_values) / sizeof(clear_values[0]);
        renderpass_begin_info.pClearValues = clear_values;

        m_Rhi->CmdBeginRenderPassPFN(
            m_Rhi->GetCurrentCommandBuffer(), &renderpass_begin_info, RHI_SUBPASS_CONTENTS_INLINE);
    }

    constexpr ViewportType k_viewports[] = {ViewportType::game, ViewportType::scene};
    float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};

    m_Rhi->PushEvent(m_Rhi->GetCurrentCommandBuffer(), "BasePass", color);
    for (ViewportType viewport_type : k_viewports)
    {
        if (!IsViewportValid(viewport_type))
            continue;
        SetPerViewportData(viewport_type);
        DrawMeshGbuffer(viewport_type);
    }
    m_Rhi->PopEvent(m_Rhi->GetCurrentCommandBuffer());

    m_Rhi->CmdNextSubpassPFN(m_Rhi->GetCurrentCommandBuffer(), RHI_SUBPASS_CONTENTS_INLINE);

    m_Rhi->PushEvent(m_Rhi->GetCurrentCommandBuffer(), "Deferred Lighting", color);
    for (ViewportType viewport_type : k_viewports)
    {
        if (!IsViewportValid(viewport_type))
            continue;
        SetPerViewportData(viewport_type);
        DrawDeferredLighting(viewport_type);
    }
    m_Rhi->PopEvent(m_Rhi->GetCurrentCommandBuffer());

    m_Rhi->CmdNextSubpassPFN(m_Rhi->GetCurrentCommandBuffer(), RHI_SUBPASS_CONTENTS_INLINE);

    m_Rhi->PushEvent(m_Rhi->GetCurrentCommandBuffer(), "Forward Lighting", color);
    for (ViewportType viewport_type : k_viewports)
    {
        if (!IsViewportValid(viewport_type))
            continue;
        SetPerViewportData(viewport_type);
        DrawSkybox(viewport_type);
        DrawMeshTransparent(viewport_type);
    }
    particle_pass.Draw();
    m_Rhi->PopEvent(m_Rhi->GetCurrentCommandBuffer());

    m_Rhi->CmdEndRenderPassPFN(m_Rhi->GetCurrentCommandBuffer());

    // ====================================================================
    // Bindless tonemap (between RP1 and RP2): samples backup_odd, writes
    // backup_even. Owns its own RP+FB. Both attachments end in
    // SHADER_READ_ONLY_OPTIMAL after this call.
    // ====================================================================
    m_Rhi->PushEvent(m_Rhi->GetCurrentCommandBuffer(), "BindlessTonemap", color);
    tone_mapping_pass.Draw();
    m_Rhi->PopEvent(m_Rhi->GetCurrentCommandBuffer());

    // ====================================================================
    // RP2: color_grading -> fxaa -> ui -> combine_ui
    // FB: m_SwapchainFramebuffers[i] (per-swapchain)
    // 4 attachments: backup_odd, backup_even, post_process_odd, swapchain
    // ====================================================================
    {
        RHIRenderPassBeginInfo renderpass_begin_info {};
        renderpass_begin_info.sType = RHI_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        renderpass_begin_info.renderPass = m_Framebuffer.render_pass;
        renderpass_begin_info.framebuffer = m_SwapchainFramebuffers[current_swapchain_image_index];
        renderpass_begin_info.renderArea.offset = {0, 0};
        renderpass_begin_info.renderArea.extent = m_Rhi->GetSwapchainInfo().extent;

        // Clear values must match RP2 attachment order:
        //  [0] backup_odd  - LOAD_OP_LOAD (clear value ignored, but slot needed)
        //  [1] backup_even - LOAD_OP_LOAD (ditto)
        //  [2] post_process_odd - LOAD_OP_CLEAR
        //  [3] swapchain        - LOAD_OP_CLEAR
        RHIClearValue clear_values[4];
        clear_values[0].color = {{k_scene_clear_r, k_scene_clear_g, k_scene_clear_b, 1.0f}};
        clear_values[1].color = {{k_scene_clear_r, k_scene_clear_g, k_scene_clear_b, 1.0f}};
        clear_values[2].color = {{k_scene_clear_r, k_scene_clear_g, k_scene_clear_b, 1.0f}};
        clear_values[3].color = {{k_scene_clear_r, k_scene_clear_g, k_scene_clear_b, 1.0f}};
        renderpass_begin_info.clearValueCount = sizeof(clear_values) / sizeof(clear_values[0]);
        renderpass_begin_info.pClearValues = clear_values;

        m_Rhi->CmdBeginRenderPassPFN(
            m_Rhi->GetCurrentCommandBuffer(), &renderpass_begin_info, RHI_SUBPASS_CONTENTS_INLINE);
    }

    color_grading_pass.Draw();

    m_Rhi->CmdNextSubpassPFN(m_Rhi->GetCurrentCommandBuffer(), RHI_SUBPASS_CONTENTS_INLINE);

    if (m_EnableFxaa)
        fxaa_pass.Draw();

    m_Rhi->CmdNextSubpassPFN(m_Rhi->GetCurrentCommandBuffer(), RHI_SUBPASS_CONTENTS_INLINE);

    // UI subpass: clear backup_even color, then draw axis + ui.
    RHIClearAttachment clear_attachments[1];
    clear_attachments[0].aspectMask = RHI_IMAGE_ASPECT_COLOR_BIT;
    clear_attachments[0].colorAttachment = 0;
    clear_attachments[0].clearValue.color.float32[0] = 0.0;
    clear_attachments[0].clearValue.color.float32[1] = 0.0;
    clear_attachments[0].clearValue.color.float32[2] = 0.0;
    clear_attachments[0].clearValue.color.float32[3] = 0.0;
    RHIClearRect clear_rects[1];
    clear_rects[0].baseArrayLayer = 0;
    clear_rects[0].layerCount = 1;
    clear_rects[0].rect.offset.x = 0;
    clear_rects[0].rect.offset.y = 0;
    clear_rects[0].rect.extent.width = m_Rhi->GetSwapchainInfo().extent.width;
    clear_rects[0].rect.extent.height = m_Rhi->GetSwapchainInfo().extent.height;
    m_Rhi->CmdClearAttachmentsPFN(m_Rhi->GetCurrentCommandBuffer(),
                                  sizeof(clear_attachments) / sizeof(clear_attachments[0]),
                                  clear_attachments,
                                  sizeof(clear_rects) / sizeof(clear_rects[0]),
                                  clear_rects);

    DrawAxis();

    ui_pass.Draw();

    // 执行 UI 渲染后的回调（Editor 的 ImGui 在这里渲染）
    for (const auto& callback : post_ui_callbacks)
    {
        callback();
    }

    m_Rhi->CmdNextSubpassPFN(m_Rhi->GetCurrentCommandBuffer(), RHI_SUBPASS_CONTENTS_INLINE);

    combine_ui_pass.Draw();

    m_Rhi->CmdEndRenderPassPFN(m_Rhi->GetCurrentCommandBuffer());
}

void MainCameraPass::DrawMeshGbuffer(ViewportType viewport_type)
{
    struct MeshNode
    {
        const Matrix4x4* model_matrix {nullptr};
        const Matrix4x4* joint_matrices {nullptr};
        uint32_t joint_count {0};
    };

    const RenderScene* render_scene = GET_SYSTEM(RenderSystem)->getRenderScene().get();
    const auto& main_camera_visible_mesh_nodes =
        render_scene ? render_scene->GetMainCameraOpaqueMeshNodes(viewport_type) : (m_ActiveMainCameraVisibleMeshNodes ? *m_ActiveMainCameraVisibleMeshNodes : *(m_VisiableNodes.p_main_camera_visible_mesh_nodes));
    std::map<VulkanPBRMaterial*, std::map<VulkanMesh*, std::vector<MeshNode>>> main_camera_mesh_drawcall_batch;

    // reorganize mesh
    for (const RenderMeshNode& node : main_camera_visible_mesh_nodes)

    {
        auto& mesh_instanced = main_camera_mesh_drawcall_batch[AsVulkanMaterialResource(node.ref_material)];
        auto& mesh_nodes = mesh_instanced[AsVulkanMeshResource(node.ref_mesh)];

        MeshNode temp;
        temp.model_matrix = node.model_matrix;
        if (node.enable_vertex_blending)
        {
            temp.joint_matrices = node.joint_matrices;
            temp.joint_count = node.joint_count;
        }

        mesh_nodes.push_back(temp);
    }

    float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    m_Rhi->PushEvent(m_Rhi->GetCurrentCommandBuffer(), "Mesh GBuffer", color);
    SetViewportScissor(viewport_type);

    RHIPipeline* bound_pipeline = nullptr;
    RHIPipelineLayout* pipeline_layout = m_RenderPipelines[_render_pipeline_type_mesh_gbuffer].layout;

    // perframe storage buffer

    uint32_t perframe_dynamic_offset =
        RoundUp(m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbuffersEnd[m_Rhi->GetCurrentFrameIndex()],
                m_GlobalRenderResource->m_StorageBuffer.m_MinStorageBufferOffsetAlignment);

    m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbuffersEnd[m_Rhi->GetCurrentFrameIndex()] =
        perframe_dynamic_offset + sizeof(MeshPerframeStorageBufferObject);
    assert(m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbuffersEnd[m_Rhi->GetCurrentFrameIndex()] <=
           (m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbuffersBegin[m_Rhi->GetCurrentFrameIndex()] +
            m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbuffersSize[m_Rhi->GetCurrentFrameIndex()]));

    (*reinterpret_cast<MeshPerframeStorageBufferObject*>(
        reinterpret_cast<uintptr_t>(
            m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbufferMemoryPointer) +
        perframe_dynamic_offset)) = m_MeshPerframeStorageBufferObject;

    for (auto& pair1 : main_camera_mesh_drawcall_batch)
    {
        VulkanPBRMaterial& material = (*pair1.first);
        auto& mesh_instanced = pair1.second;

        RHIPipeline* const material_pipeline = GetOrCreateMeshGBufferPipeline(material);
        if (material_pipeline != bound_pipeline)
        {
            m_Rhi->CmdBindPipelinePFN(
                m_Rhi->GetCurrentCommandBuffer(), RHI_PIPELINE_BIND_POINT_GRAPHICS, material_pipeline);
            bound_pipeline = material_pipeline;
        }

        // bind per material
        m_Rhi->CmdBindDescriptorSetsPFN(m_Rhi->GetCurrentCommandBuffer(),
                                        RHI_PIPELINE_BIND_POINT_GRAPHICS,
                                        pipeline_layout,
                                        2,
                                        1,
                                        &material.material_descriptor_set,
                                        0,
                                        NULL);

        // TODO: render from near to far

        for (auto& pair2 : mesh_instanced)
        {
            VulkanMesh& mesh = (*pair2.first);
            auto& mesh_nodes = pair2.second;

            uint32_t total_instance_count = static_cast<uint32_t>(mesh_nodes.size());
            if (total_instance_count > 0)
            {
                // bind per mesh
                m_Rhi->CmdBindDescriptorSetsPFN(m_Rhi->GetCurrentCommandBuffer(),
                                                RHI_PIPELINE_BIND_POINT_GRAPHICS,
                                                pipeline_layout,
                                                1,
                                                1,
                                                &mesh.mesh_vertex_blending_descriptor_set,
                                                0,
                                                NULL);

                RHIBuffer* vertex_buffers[] = {mesh.mesh_vertex_position_buffer,
                                               mesh.mesh_vertex_varying_enable_blending_buffer,
                                               mesh.mesh_vertex_varying_buffer};
                RHIDeviceSize offsets[] = {0, 0, 0};
                m_Rhi->CmdBindVertexBuffersPFN(m_Rhi->GetCurrentCommandBuffer(),
                                               0,
                                               (sizeof(vertex_buffers) / sizeof(vertex_buffers[0])),
                                               vertex_buffers,
                                               offsets);
                m_Rhi->CmdBindIndexBufferPFN(
                    m_Rhi->GetCurrentCommandBuffer(), mesh.mesh_index_buffer, 0, RHI_INDEX_TYPE_UINT16);

                uint32_t drawcall_max_instance_count = (sizeof(MeshPerdrawcallStorageBufferObject::mesh_instances) /
                                                        sizeof(MeshPerdrawcallStorageBufferObject::mesh_instances[0]));
                uint32_t drawcall_count =
                    RoundUp(total_instance_count, drawcall_max_instance_count) / drawcall_max_instance_count;

                for (uint32_t drawcall_index = 0; drawcall_index < drawcall_count; ++drawcall_index)
                {
                    uint32_t current_instance_count =
                        ((total_instance_count - drawcall_max_instance_count * drawcall_index) <
                         drawcall_max_instance_count)
                            ? (total_instance_count - drawcall_max_instance_count * drawcall_index)
                            : drawcall_max_instance_count;

                    // per drawcall storage buffer
                    uint32_t perdrawcall_dynamic_offset =
                        RoundUp(m_GlobalRenderResource->m_StorageBuffer
                                    .m_GlobalUploadRingbuffersEnd[m_Rhi->GetCurrentFrameIndex()],
                                m_GlobalRenderResource->m_StorageBuffer.m_MinStorageBufferOffsetAlignment);
                    m_GlobalRenderResource->m_StorageBuffer
                        .m_GlobalUploadRingbuffersEnd[m_Rhi->GetCurrentFrameIndex()] =
                        perdrawcall_dynamic_offset + sizeof(MeshPerdrawcallStorageBufferObject);
                    assert(m_GlobalRenderResource->m_StorageBuffer
                               .m_GlobalUploadRingbuffersEnd[m_Rhi->GetCurrentFrameIndex()] <=
                           (m_GlobalRenderResource->m_StorageBuffer
                                .m_GlobalUploadRingbuffersBegin[m_Rhi->GetCurrentFrameIndex()] +
                            m_GlobalRenderResource->m_StorageBuffer
                                .m_GlobalUploadRingbuffersSize[m_Rhi->GetCurrentFrameIndex()]));

                    MeshPerdrawcallStorageBufferObject& perdrawcall_storage_buffer_object =
                        (*reinterpret_cast<MeshPerdrawcallStorageBufferObject*>(
                            reinterpret_cast<uintptr_t>(
                                m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbufferMemoryPointer) +
                            perdrawcall_dynamic_offset));
                    for (uint32_t i = 0; i < current_instance_count; ++i)
                    {
                        perdrawcall_storage_buffer_object.mesh_instances[i].model_matrix =
                            *mesh_nodes[drawcall_max_instance_count * drawcall_index + i].model_matrix;
                        perdrawcall_storage_buffer_object.mesh_instances[i].enable_vertex_blending =
                            mesh_nodes[drawcall_max_instance_count * drawcall_index + i].joint_matrices ? 1.0 : -1.0;
                    }

                    // per drawcall vertex blending storage buffer
                    uint32_t per_drawcall_vertex_blending_dynamic_offset;
                    bool least_one_enable_vertex_blending = true;
                    for (uint32_t i = 0; i < current_instance_count; ++i)
                    {
                        if (!mesh_nodes[drawcall_max_instance_count * drawcall_index + i].joint_matrices)
                        {
                            least_one_enable_vertex_blending = false;
                            break;
                        }
                    }
                    if (least_one_enable_vertex_blending)
                    {
                        per_drawcall_vertex_blending_dynamic_offset =
                            RoundUp(m_GlobalRenderResource->m_StorageBuffer
                                        .m_GlobalUploadRingbuffersEnd[m_Rhi->GetCurrentFrameIndex()],
                                    m_GlobalRenderResource->m_StorageBuffer.m_MinStorageBufferOffsetAlignment);
                        m_GlobalRenderResource->m_StorageBuffer
                            .m_GlobalUploadRingbuffersEnd[m_Rhi->GetCurrentFrameIndex()] =
                            per_drawcall_vertex_blending_dynamic_offset +
                            sizeof(MeshPerdrawcallVertexBlendingStorageBufferObject);
                        assert(m_GlobalRenderResource->m_StorageBuffer
                                   .m_GlobalUploadRingbuffersEnd[m_Rhi->GetCurrentFrameIndex()] <=
                               (m_GlobalRenderResource->m_StorageBuffer
                                    .m_GlobalUploadRingbuffersBegin[m_Rhi->GetCurrentFrameIndex()] +
                                m_GlobalRenderResource->m_StorageBuffer
                                    .m_GlobalUploadRingbuffersSize[m_Rhi->GetCurrentFrameIndex()]));

                        MeshPerdrawcallVertexBlendingStorageBufferObject&
                            per_drawcall_vertex_blending_storage_buffer_object =
                                (*reinterpret_cast<MeshPerdrawcallVertexBlendingStorageBufferObject*>(
                                    reinterpret_cast<uintptr_t>(m_GlobalRenderResource->m_StorageBuffer
                                                                    .m_GlobalUploadRingbufferMemoryPointer) +
                                    per_drawcall_vertex_blending_dynamic_offset));
                        for (uint32_t i = 0; i < current_instance_count; ++i)
                        {
                            if (mesh_nodes[drawcall_max_instance_count * drawcall_index + i].joint_matrices)
                            {
                                for (uint32_t j = 0;
                                     j < mesh_nodes[drawcall_max_instance_count * drawcall_index + i].joint_count;
                                     ++j)
                                {
                                    per_drawcall_vertex_blending_storage_buffer_object
                                        .joint_matrices[s_MeshVertexBlendingMaxJointCount * i + j] =
                                        mesh_nodes[drawcall_max_instance_count * drawcall_index + i].joint_matrices[j];
                                }
                            }
                        }
                    }
                    else
                    {
                        per_drawcall_vertex_blending_dynamic_offset = 0;
                    }

                    // bind perdrawcall
                    uint32_t dynamic_offsets[3] = {perframe_dynamic_offset,
                                                   perdrawcall_dynamic_offset,
                                                   per_drawcall_vertex_blending_dynamic_offset};
                    m_Rhi->CmdBindDescriptorSetsPFN(m_Rhi->GetCurrentCommandBuffer(),
                                                    RHI_PIPELINE_BIND_POINT_GRAPHICS,
                                                    pipeline_layout,
                                                    0,
                                                    1,
                                                    &m_DescriptorInfos[_mesh_global].descriptor_set,
                                                    3,
                                                    dynamic_offsets);

                    m_Rhi->CmdDrawIndexedPFN(
                        m_Rhi->GetCurrentCommandBuffer(), mesh.mesh_index_count, current_instance_count, 0, 0, 0);
                }
            }
        }
    }

    m_Rhi->PopEvent(m_Rhi->GetCurrentCommandBuffer());
}

void MainCameraPass::DrawMeshTransparent(ViewportType viewport_type)
{
    struct MeshNode
    {
        const Matrix4x4* model_matrix {nullptr};
        const Matrix4x4* joint_matrices {nullptr};
        uint32_t joint_count {0};
    };

    const RenderScene* render_scene = GET_SYSTEM(RenderSystem)->getRenderScene().get();
    const auto& main_camera_transparent_mesh_nodes =
        render_scene ? render_scene->GetMainCameraTransparentMeshNodes(viewport_type) : (m_ActiveMainCameraVisibleMeshNodes ? *m_ActiveMainCameraVisibleMeshNodes : *(m_VisiableNodes.p_main_camera_visible_mesh_nodes));
    std::map<VulkanPBRMaterial*, std::map<VulkanMesh*, std::vector<MeshNode>>> main_camera_mesh_drawcall_batch;

    // reorganize mesh
    for (const RenderMeshNode& node : main_camera_transparent_mesh_nodes)
    {
        auto& mesh_instanced = main_camera_mesh_drawcall_batch[AsVulkanMaterialResource(node.ref_material)];
        auto& mesh_nodes = mesh_instanced[AsVulkanMeshResource(node.ref_mesh)];

        MeshNode temp;
        temp.model_matrix = node.model_matrix;
        if (node.enable_vertex_blending)
        {
            temp.joint_matrices = node.joint_matrices;
            temp.joint_count = node.joint_count;
        }

        mesh_nodes.push_back(temp);
    }

    float color[4] = {0.0f, 1.0f, 1.0f, 1.0f};
    m_Rhi->PushEvent(m_Rhi->GetCurrentCommandBuffer(), "Mesh Transparent", color);
    SetViewportScissor(viewport_type);

    RHIPipeline* bound_pipeline = nullptr;
    RHIPipelineLayout* pipeline_layout = m_RenderPipelines[_render_pipeline_type_mesh_transparent].layout;

    // perframe storage buffer
    uint32_t perframe_dynamic_offset =
        RoundUp(m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbuffersEnd[m_Rhi->GetCurrentFrameIndex()],
                m_GlobalRenderResource->m_StorageBuffer.m_MinStorageBufferOffsetAlignment);

    m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbuffersEnd[m_Rhi->GetCurrentFrameIndex()] =
        perframe_dynamic_offset + sizeof(MeshPerframeStorageBufferObject);

    (*reinterpret_cast<MeshPerframeStorageBufferObject*>(
        reinterpret_cast<uintptr_t>(
            m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbufferMemoryPointer) +
        perframe_dynamic_offset)) = m_MeshPerframeStorageBufferObject;

    for (auto& pair1 : main_camera_mesh_drawcall_batch)
    {
        VulkanPBRMaterial& material = (*pair1.first);
        auto& mesh_instanced = pair1.second;

        RHIPipeline* const material_pipeline = GetOrCreateMeshTransparentPipeline(material);
        if (material_pipeline != bound_pipeline)
        {
            m_Rhi->CmdBindPipelinePFN(
                m_Rhi->GetCurrentCommandBuffer(), RHI_PIPELINE_BIND_POINT_GRAPHICS, material_pipeline);
            bound_pipeline = material_pipeline;
        }

        // bind per material
        m_Rhi->CmdBindDescriptorSetsPFN(m_Rhi->GetCurrentCommandBuffer(),
                                        RHI_PIPELINE_BIND_POINT_GRAPHICS,
                                        pipeline_layout,
                                        2,
                                        1,
                                        &material.material_descriptor_set,
                                        0,
                                        NULL);

        for (auto& pair2 : mesh_instanced)
        {
            VulkanMesh& mesh = (*pair2.first);
            auto& mesh_nodes = pair2.second;

            uint32_t total_instance_count = static_cast<uint32_t>(mesh_nodes.size());
            if (total_instance_count > 0)
            {
                // bind per mesh
                m_Rhi->CmdBindDescriptorSetsPFN(m_Rhi->GetCurrentCommandBuffer(),
                                                RHI_PIPELINE_BIND_POINT_GRAPHICS,
                                                pipeline_layout,
                                                1,
                                                1,
                                                &mesh.mesh_vertex_blending_descriptor_set,
                                                0,
                                                NULL);

                RHIBuffer* vertex_buffers[] = {mesh.mesh_vertex_position_buffer,
                                               mesh.mesh_vertex_varying_enable_blending_buffer,
                                               mesh.mesh_vertex_varying_buffer};
                RHIDeviceSize offsets[] = {0, 0, 0};
                m_Rhi->CmdBindVertexBuffersPFN(m_Rhi->GetCurrentCommandBuffer(),
                                               0,
                                               (sizeof(vertex_buffers) / sizeof(vertex_buffers[0])),
                                               vertex_buffers,
                                               offsets);
                m_Rhi->CmdBindIndexBufferPFN(
                    m_Rhi->GetCurrentCommandBuffer(), mesh.mesh_index_buffer, 0, RHI_INDEX_TYPE_UINT16);

                uint32_t drawcall_max_instance_count = (sizeof(MeshPerdrawcallStorageBufferObject::mesh_instances) /
                                                        sizeof(MeshPerdrawcallStorageBufferObject::mesh_instances[0]));
                uint32_t drawcall_count =
                    RoundUp(total_instance_count, drawcall_max_instance_count) / drawcall_max_instance_count;

                for (uint32_t drawcall_index = 0; drawcall_index < drawcall_count; ++drawcall_index)
                {
                    uint32_t current_instance_count =
                        ((total_instance_count - drawcall_max_instance_count * drawcall_index) <
                         drawcall_max_instance_count)
                            ? (total_instance_count - drawcall_max_instance_count * drawcall_index)
                            : drawcall_max_instance_count;

                    // per drawcall storage buffer
                    uint32_t perdrawcall_dynamic_offset =
                        RoundUp(m_GlobalRenderResource->m_StorageBuffer
                                    .m_GlobalUploadRingbuffersEnd[m_Rhi->GetCurrentFrameIndex()],
                                m_GlobalRenderResource->m_StorageBuffer.m_MinStorageBufferOffsetAlignment);
                    m_GlobalRenderResource->m_StorageBuffer
                        .m_GlobalUploadRingbuffersEnd[m_Rhi->GetCurrentFrameIndex()] =
                        perdrawcall_dynamic_offset + sizeof(MeshPerdrawcallStorageBufferObject);

                    MeshPerdrawcallStorageBufferObject& perdrawcall_storage_buffer_object =
                        (*reinterpret_cast<MeshPerdrawcallStorageBufferObject*>(
                            reinterpret_cast<uintptr_t>(
                                m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbufferMemoryPointer) +
                            perdrawcall_dynamic_offset));
                    for (uint32_t i = 0; i < current_instance_count; ++i)
                    {
                        perdrawcall_storage_buffer_object.mesh_instances[i].model_matrix =
                            *mesh_nodes[drawcall_max_instance_count * drawcall_index + i].model_matrix;
                        perdrawcall_storage_buffer_object.mesh_instances[i].enable_vertex_blending =
                            mesh_nodes[drawcall_max_instance_count * drawcall_index + i].joint_matrices ? 1.0 : -1.0;
                    }

                    // per drawcall vertex blending storage buffer
                    uint32_t per_drawcall_vertex_blending_dynamic_offset;
                    bool least_one_enable_vertex_blending = true;
                    for (uint32_t i = 0; i < current_instance_count; ++i)
                    {
                        if (!mesh_nodes[drawcall_max_instance_count * drawcall_index + i].joint_matrices)
                        {
                            least_one_enable_vertex_blending = false;
                            break;
                        }
                    }
                    if (least_one_enable_vertex_blending)
                    {
                        per_drawcall_vertex_blending_dynamic_offset =
                            RoundUp(m_GlobalRenderResource->m_StorageBuffer
                                        .m_GlobalUploadRingbuffersEnd[m_Rhi->GetCurrentFrameIndex()],
                                    m_GlobalRenderResource->m_StorageBuffer.m_MinStorageBufferOffsetAlignment);
                        m_GlobalRenderResource->m_StorageBuffer
                            .m_GlobalUploadRingbuffersEnd[m_Rhi->GetCurrentFrameIndex()] =
                            per_drawcall_vertex_blending_dynamic_offset +
                            sizeof(MeshPerdrawcallVertexBlendingStorageBufferObject);

                        MeshPerdrawcallVertexBlendingStorageBufferObject&
                            per_drawcall_vertex_blending_storage_buffer_object =
                                (*reinterpret_cast<MeshPerdrawcallVertexBlendingStorageBufferObject*>(
                                    reinterpret_cast<uintptr_t>(m_GlobalRenderResource->m_StorageBuffer
                                                                    .m_GlobalUploadRingbufferMemoryPointer) +
                                    per_drawcall_vertex_blending_dynamic_offset));
                        for (uint32_t i = 0; i < current_instance_count; ++i)
                        {
                            if (mesh_nodes[drawcall_max_instance_count * drawcall_index + i].joint_matrices)
                            {
                                for (uint32_t j = 0;
                                     j < mesh_nodes[drawcall_max_instance_count * drawcall_index + i].joint_count;
                                     ++j)
                                {
                                    per_drawcall_vertex_blending_storage_buffer_object
                                        .joint_matrices[s_MeshVertexBlendingMaxJointCount * i + j] =
                                        mesh_nodes[drawcall_max_instance_count * drawcall_index + i].joint_matrices[j];
                                }
                            }
                        }
                    }
                    else
                    {
                        per_drawcall_vertex_blending_dynamic_offset = 0;
                    }

                    // bind perdrawcall
                    uint32_t dynamic_offsets[3] = {perframe_dynamic_offset,
                                                   perdrawcall_dynamic_offset,
                                                   per_drawcall_vertex_blending_dynamic_offset};
                    m_Rhi->CmdBindDescriptorSetsPFN(m_Rhi->GetCurrentCommandBuffer(),
                                                    RHI_PIPELINE_BIND_POINT_GRAPHICS,
                                                    pipeline_layout,
                                                    0,
                                                    1,
                                                    &m_DescriptorInfos[_mesh_global].descriptor_set,
                                                    3,
                                                    dynamic_offsets);

                    m_Rhi->CmdDrawIndexedPFN(
                        m_Rhi->GetCurrentCommandBuffer(), mesh.mesh_index_count, current_instance_count, 0, 0, 0);
                }
            }
        }
    }

    m_Rhi->PopEvent(m_Rhi->GetCurrentCommandBuffer());
}

void MainCameraPass::DrawDeferredLighting(ViewportType viewport_type)
{
    const bool use_megalights =
        MegaLights::IsEnabled() && m_MegaLightsSystem != nullptr && m_MegaLightsSystem->HasGpuData();
    const uint8_t pipeline_index = use_megalights ? _render_pipeline_type_megalights_deferred
                                                  : _render_pipeline_type_deferred_lighting;

    if (use_megalights)
    {
        m_MegaLightsSystem->PrepareDeferredHistory(viewport_type, m_Rhi->GetCurrentCommandBuffer());
        m_MegaLightsSystem->RefreshTemporalHeader(viewport_type);
        UpdateMegaLightsDescriptorSets(viewport_type);
    }

    m_Rhi->CmdBindPipelinePFN(m_Rhi->GetCurrentCommandBuffer(),
                              RHI_PIPELINE_BIND_POINT_GRAPHICS,
                              m_RenderPipelines[pipeline_index].pipeline);
    SetViewportScissor(viewport_type);

    uint32_t perframe_dynamic_offset =
        RoundUp(m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbuffersEnd[m_Rhi->GetCurrentFrameIndex()],
                m_GlobalRenderResource->m_StorageBuffer.m_MinStorageBufferOffsetAlignment);

    m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbuffersEnd[m_Rhi->GetCurrentFrameIndex()] =
        perframe_dynamic_offset + sizeof(MeshPerframeStorageBufferObject);
    assert(m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbuffersEnd[m_Rhi->GetCurrentFrameIndex()] <=
           (m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbuffersBegin[m_Rhi->GetCurrentFrameIndex()] +
            m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbuffersSize[m_Rhi->GetCurrentFrameIndex()]));

    (*reinterpret_cast<MeshPerframeStorageBufferObject*>(
        reinterpret_cast<uintptr_t>(
            m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbufferMemoryPointer) +
        perframe_dynamic_offset)) = m_MeshPerframeStorageBufferObject;

    RHIDescriptorSet* descriptor_sets[3] = {m_DescriptorInfos[_mesh_global].descriptor_set,
                                            m_DescriptorInfos[_deferred_lighting].descriptor_set,
                                            m_DescriptorInfos[_skybox].descriptor_set};
    uint32_t dynamic_offsets[4] = {perframe_dynamic_offset, perframe_dynamic_offset, 0, 0};
    m_Rhi->CmdBindDescriptorSetsPFN(m_Rhi->GetCurrentCommandBuffer(),
                                    RHI_PIPELINE_BIND_POINT_GRAPHICS,
                                    m_RenderPipelines[pipeline_index].layout,
                                    0,
                                    3,
                                    descriptor_sets,
                                    4,
                                    dynamic_offsets);

    m_Rhi->CmdDraw(m_Rhi->GetCurrentCommandBuffer(), 3, 1, 0, 0);

    if (use_megalights && MegaLights::IsSpatialDenoiseEnabled())
    {
        m_MegaLightsSystem->PrepareSpatialReadBarrier(viewport_type, m_Rhi->GetCurrentCommandBuffer());
        m_MegaLightsSystem->RefreshTemporalHeader(viewport_type);
        UpdateMegaLightsSpatialDescriptorSets(viewport_type);
        DrawMegaLightsSpatialDenoise(viewport_type);
    }

    if (use_megalights)
    {
        m_MegaLightsSystem->EndDeferredPass(viewport_type, m_MeshPerframeStorageBufferObject.proj_view_matrix);
    }
}

void MainCameraPass::DrawMegaLightsSpatialDenoise(ViewportType viewport_type)
{
    m_Rhi->CmdBindPipelinePFN(m_Rhi->GetCurrentCommandBuffer(),
                              RHI_PIPELINE_BIND_POINT_GRAPHICS,
                              m_RenderPipelines[_render_pipeline_type_megalights_spatial].pipeline);
    SetViewportScissor(viewport_type);

    uint32_t perframe_dynamic_offset =
        RoundUp(m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbuffersEnd[m_Rhi->GetCurrentFrameIndex()],
                m_GlobalRenderResource->m_StorageBuffer.m_MinStorageBufferOffsetAlignment);

    m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbuffersEnd[m_Rhi->GetCurrentFrameIndex()] =
        perframe_dynamic_offset + sizeof(MeshPerframeStorageBufferObject);
    assert(m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbuffersEnd[m_Rhi->GetCurrentFrameIndex()] <=
           (m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbuffersBegin[m_Rhi->GetCurrentFrameIndex()] +
            m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbuffersSize[m_Rhi->GetCurrentFrameIndex()]));

    (*reinterpret_cast<MeshPerframeStorageBufferObject*>(
        reinterpret_cast<uintptr_t>(
            m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbufferMemoryPointer) +
        perframe_dynamic_offset)) = m_MeshPerframeStorageBufferObject;

    RHIDescriptorSet* descriptor_sets[2] = {m_DescriptorInfos[_mesh_global].descriptor_set,
                                            m_DescriptorInfos[_megalights_spatial_surfaces].descriptor_set};
    uint32_t dynamic_offsets[2] = {perframe_dynamic_offset, 0};
    m_Rhi->CmdBindDescriptorSetsPFN(m_Rhi->GetCurrentCommandBuffer(),
                                    RHI_PIPELINE_BIND_POINT_GRAPHICS,
                                    m_RenderPipelines[_render_pipeline_type_megalights_spatial].layout,
                                    0,
                                    2,
                                    descriptor_sets,
                                    2,
                                    dynamic_offsets);

    m_Rhi->CmdDraw(m_Rhi->GetCurrentCommandBuffer(), 3, 1, 0, 0);
}

void MainCameraPass::DrawSkybox(ViewportType viewport_type)
{
    if (!m_IsShowSkybox[static_cast<size_t>(viewport_type)])
        return;

    uint32_t perframe_dynamic_offset =

        RoundUp(m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbuffersEnd[m_Rhi->GetCurrentFrameIndex()],
                m_GlobalRenderResource->m_StorageBuffer.m_MinStorageBufferOffsetAlignment);

    m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbuffersEnd[m_Rhi->GetCurrentFrameIndex()] =
        perframe_dynamic_offset + sizeof(MeshPerframeStorageBufferObject);
    assert(m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbuffersEnd[m_Rhi->GetCurrentFrameIndex()] <=
           (m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbuffersBegin[m_Rhi->GetCurrentFrameIndex()] +
            m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbuffersSize[m_Rhi->GetCurrentFrameIndex()]));

    (*reinterpret_cast<MeshPerframeStorageBufferObject*>(
        reinterpret_cast<uintptr_t>(
            m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbufferMemoryPointer) +
        perframe_dynamic_offset)) = m_MeshPerframeStorageBufferObject;

    float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    m_Rhi->PushEvent(m_Rhi->GetCurrentCommandBuffer(), "Skybox", color);

    m_Rhi->CmdBindPipelinePFN(m_Rhi->GetCurrentCommandBuffer(),
                              RHI_PIPELINE_BIND_POINT_GRAPHICS,
                              m_RenderPipelines[_render_pipeline_type_skybox].pipeline);
    SetViewportScissor(viewport_type);
    m_Rhi->CmdBindDescriptorSetsPFN(m_Rhi->GetCurrentCommandBuffer(),

                                    RHI_PIPELINE_BIND_POINT_GRAPHICS,
                                    m_RenderPipelines[_render_pipeline_type_skybox].layout,
                                    0,
                                    1,
                                    &m_DescriptorInfos[_skybox].descriptor_set,
                                    1,
                                    &perframe_dynamic_offset);
    m_Rhi->CmdDraw(m_Rhi->GetCurrentCommandBuffer(), 36, 1, 0, 0);  // 2 triangles(6 vertex) each face, 6 faces

    m_Rhi->PopEvent(m_Rhi->GetCurrentCommandBuffer());
}

void MainCameraPass::DrawAxis()
{
    if (!m_IsShowAxis)
        return;

    SetPerViewportData(ViewportType::scene);

    uint32_t perframe_dynamic_offset =

        RoundUp(m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbuffersEnd[m_Rhi->GetCurrentFrameIndex()],
                m_GlobalRenderResource->m_StorageBuffer.m_MinStorageBufferOffsetAlignment);

    m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbuffersEnd[m_Rhi->GetCurrentFrameIndex()] =
        perframe_dynamic_offset + sizeof(MeshPerframeStorageBufferObject);
    assert(m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbuffersEnd[m_Rhi->GetCurrentFrameIndex()] <=
           (m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbuffersBegin[m_Rhi->GetCurrentFrameIndex()] +
            m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbuffersSize[m_Rhi->GetCurrentFrameIndex()]));

    (*reinterpret_cast<MeshPerframeStorageBufferObject*>(
        reinterpret_cast<uintptr_t>(
            m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbufferMemoryPointer) +
        perframe_dynamic_offset)) = m_MeshPerframeStorageBufferObject;

    float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    auto&& currentCommandBuffer = m_Rhi->GetCurrentCommandBuffer();
    m_Rhi->PushEvent(currentCommandBuffer, "Axis", color);

    m_Rhi->CmdBindPipelinePFN(currentCommandBuffer,
                              RHI_PIPELINE_BIND_POINT_GRAPHICS,
                              m_RenderPipelines[_render_pipeline_type_axis].pipeline);
    SetViewportScissor(ViewportType::scene);
    m_Rhi->CmdBindDescriptorSetsPFN(currentCommandBuffer,

                                    RHI_PIPELINE_BIND_POINT_GRAPHICS,
                                    m_RenderPipelines[_render_pipeline_type_axis].layout,
                                    0,
                                    1,
                                    &m_DescriptorInfos[_axis].descriptor_set,
                                    1,
                                    &perframe_dynamic_offset);

    m_AxisStorageBufferObject.selected_axis = m_SelectedAxis;
    m_AxisStorageBufferObject.model_matrix = m_VisiableNodes.p_axis_node->model_matrix;

    VulkanMesh* axis_mesh = AsVulkanMeshResource(m_VisiableNodes.p_axis_node->ref_mesh);

    RHIBuffer* vertex_buffers[3] = {axis_mesh->mesh_vertex_position_buffer,
                                    axis_mesh->mesh_vertex_varying_enable_blending_buffer,
                                    axis_mesh->mesh_vertex_varying_buffer};
    RHIDeviceSize offsets[3] = {0, 0, 0};
    m_Rhi->CmdBindVertexBuffersPFN(m_Rhi->GetCurrentCommandBuffer(),
                                   0,
                                   (sizeof(vertex_buffers) / sizeof(vertex_buffers[0])),
                                   vertex_buffers,
                                   offsets);
    m_Rhi->CmdBindIndexBufferPFN(
        m_Rhi->GetCurrentCommandBuffer(), axis_mesh->mesh_index_buffer, 0, RHI_INDEX_TYPE_UINT16);
    (*reinterpret_cast<AxisStorageBufferObject*>(reinterpret_cast<uintptr_t>(
        m_GlobalRenderResource->m_StorageBuffer.m_AxisInefficientStorageBufferMemoryPointer))) =
        m_AxisStorageBufferObject;

    m_Rhi->CmdDrawIndexedPFN(m_Rhi->GetCurrentCommandBuffer(), axis_mesh->mesh_index_count, 1, 0, 0, 0);

    m_Rhi->PopEvent(m_Rhi->GetCurrentCommandBuffer());
}

RHICommandBuffer* MainCameraPass::GetRenderCommandBuffer()
{
    return m_Rhi->GetCurrentCommandBuffer();
}

void MainCameraPass::SetupParticlePass()
{
    m_ParticlePass->SetDepthAndNormalImage(m_Rhi->GetDepthImageInfo().depth_image,
                                           m_Framebuffer.attachments[_main_camera_pass_gbuffer_a].image);

    m_ParticlePass->SetRenderPassHandle(m_Framebuffer1.render_pass);  // RP1: forward_lighting subpass
}

void MainCameraPass::SetParticlePass(std::shared_ptr<ParticlePass> pass)
{
    m_ParticlePass = pass;
}