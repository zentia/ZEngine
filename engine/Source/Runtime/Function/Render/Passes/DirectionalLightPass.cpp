#include "Runtime/Function/Render/Passes/DirectionalLightPass.h"

#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Function/Render/Interface/Vulkan/VulkanRenderResource.h"
#include "Runtime/Function/Render/Passes/ShadowPassDx12Shaders.h"
#include "Runtime/Function/Render/RenderHelper.h"
#include "Runtime/Function/Render/RenderMesh.h"
#include "Runtime/Function/Render/RenderResource.h"
#include "Runtime/Function/Render/RenderType.h"
#include "Runtime/Utility/Utility.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <tuple>
#include <vector>

#if defined(Z_HAS_VULKAN)
    #include <mesh_directional_light_shadow_frag.h>
    #include <mesh_directional_light_shadow_vert.h>
#endif

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
        return RHI_COMPARE_OP_LESS;
    }

    const VulkanShaderPassData* FindShaderPassByLightMode(const VulkanPBRMaterial& material, const std::string& light_mode)
    {
        for (const VulkanShaderPassData& shader_pass : material.shader_passes)
        {
            if (EqualsIgnoreCase(shader_pass.light_mode, light_mode))
            {
                return &shader_pass;
            }
        }
        return nullptr;
    }

    bool CanUseRuntimeShadowPass(const std::shared_ptr<RHI>& rhi,
                                 const VulkanPBRMaterial& material,
                                 const VulkanShaderPassData* shader_pass)
    {
        if (shader_pass == nullptr || shader_pass->vertex_shader_file.empty() || shader_pass->fragment_shader_file.empty())
        {
            return false;
        }

        if (!EqualsIgnoreCase(shader_pass->light_mode, "SHADOWCASTER"))
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

    DirectionalLightShadowPass::ShadowPipelineKey BuildShadowPipelineKey(const VulkanPBRMaterial& material,
                                                                         const VulkanShaderPassData& shader_pass)
    {
        DirectionalLightShadowPass::ShadowPipelineKey key;
        key.vertex_shader_file = shader_pass.vertex_shader_file;
        key.fragment_shader_file = shader_pass.fragment_shader_file;
        key.vertex_entry = shader_pass.vertex_entry.empty() ? "main" : shader_pass.vertex_entry;
        key.fragment_entry = shader_pass.fragment_entry.empty() ? "main" : shader_pass.fragment_entry;
        key.include_directory = material.include_directory;
        key.source_language = material.source_language;
        key.cull = shader_pass.cull.empty() ? "Back" : shader_pass.cull;
        key.ztest = shader_pass.ztest.empty() ? "Less" : shader_pass.ztest;
        key.zwrite = shader_pass.zwrite;
        key.shader_macros = material.shader_macros;
        return key;
    }
}  // namespace

bool DirectionalLightShadowPass::ShadowPipelineKey::operator<(const ShadowPipelineKey& rhs) const
{
    return std::tie(vertex_shader_file,
                    fragment_shader_file,
                    vertex_entry,
                    fragment_entry,
                    include_directory,
                    source_language,
                    cull,
                    ztest,
                    zwrite,
                    shader_macros) <
           std::tie(rhs.vertex_shader_file,
                    rhs.fragment_shader_file,
                    rhs.vertex_entry,
                    rhs.fragment_entry,
                    rhs.include_directory,
                    rhs.source_language,
                    rhs.cull,
                    rhs.ztest,
                    rhs.zwrite,
                    rhs.shader_macros);
}

RHIPipeline* DirectionalLightShadowPass::GetOrCreateShadowPipeline(const VulkanPBRMaterial& material)
{
    RHIPipeline* const default_pipeline = m_RenderPipelines[0].pipeline;
    const VulkanShaderPassData* const shadow_pass = FindShaderPassByLightMode(material, "ShadowCaster");
    if (!CanUseRuntimeShadowPass(m_Rhi, material, shadow_pass))
    {
        return default_pipeline;
    }

    const DirectionalLightShadowPass::ShadowPipelineKey pipeline_key = BuildShadowPipelineKey(material, *shadow_pass);
    if (const auto pipeline_it = m_MaterialPipelines.find(pipeline_key); pipeline_it != m_MaterialPipelines.end())
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
                    "fallback to built-in directional shadow pipeline for shader '{}', vert='{}', frag='{}'",
                    material.shader_name,
                    pipeline_key.vertex_shader_file,
                    pipeline_key.fragment_shader_file);
        m_MaterialPipelines[pipeline_key] = default_pipeline;
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
    vertex_input_state_create_info.vertexBindingDescriptionCount =
        static_cast<uint32_t>(vertex_binding_descriptions.size());
    vertex_input_state_create_info.pVertexBindingDescriptions = vertex_binding_descriptions.data();
    vertex_input_state_create_info.vertexAttributeDescriptionCount =
        static_cast<uint32_t>(vertex_attribute_descriptions.size());
    vertex_input_state_create_info.pVertexAttributeDescriptions = vertex_attribute_descriptions.data();

    RHIPipelineInputAssemblyStateCreateInfo input_assembly_create_info {};
    input_assembly_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly_create_info.topology = RHI_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    input_assembly_create_info.primitiveRestartEnable = RHI_FALSE;

    RHIViewport viewport = {
        0, 0, s_DirectionalLightShadowMapDimension, s_DirectionalLightShadowMapDimension, 0.0, 1.0};
    RHIRect2D scissor = {{0, 0}, {s_DirectionalLightShadowMapDimension, s_DirectionalLightShadowMapDimension}};

    RHIPipelineViewportStateCreateInfo viewport_state_create_info {};
    viewport_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state_create_info.viewportCount = 1;
    viewport_state_create_info.pViewports = &viewport;
    viewport_state_create_info.scissorCount = 1;
    viewport_state_create_info.pScissors = &scissor;

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

    RHIPipelineColorBlendAttachmentState color_blend_attachment_state {};
    color_blend_attachment_state.colorWriteMask =
        RHI_COLOR_COMPONENT_R_BIT | RHI_COLOR_COMPONENT_G_BIT | RHI_COLOR_COMPONENT_B_BIT | RHI_COLOR_COMPONENT_A_BIT;
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
    depth_stencil_create_info.depthTestEnable = RHI_TRUE;
    depth_stencil_create_info.depthWriteEnable = pipeline_key.zwrite ? RHI_TRUE : RHI_FALSE;
    depth_stencil_create_info.depthCompareOp = ParseCompareOp(pipeline_key.ztest);
    depth_stencil_create_info.depthBoundsTestEnable = RHI_FALSE;
    depth_stencil_create_info.stencilTestEnable = RHI_FALSE;

    RHIPipelineDynamicStateCreateInfo dynamic_state_create_info {};
    dynamic_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic_state_create_info.dynamicStateCount = 0;
    dynamic_state_create_info.pDynamicStates = NULL;

    RHIGraphicsPipelineCreateInfo pipeline_info {};
    pipeline_info.sType = RHI_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_info.stageCount = (sizeof(shader_stages) / sizeof(shader_stages[0]));
    pipeline_info.pStages = shader_stages;
    pipeline_info.pVertexInputState = &vertex_input_state_create_info;
    pipeline_info.pInputAssemblyState = &input_assembly_create_info;
    pipeline_info.pViewportState = &viewport_state_create_info;
    pipeline_info.pRasterizationState = &rasterization_state_create_info;
    pipeline_info.pMultisampleState = &multisample_state_create_info;
    pipeline_info.pColorBlendState = &color_blend_state_create_info;
    pipeline_info.pDepthStencilState = &depth_stencil_create_info;
    pipeline_info.layout = m_RenderPipelines[0].layout;
    pipeline_info.renderPass = m_Framebuffer.render_pass;
    pipeline_info.subpass = 0;
    pipeline_info.basePipelineHandle = RHI_NULL_HANDLE;
    pipeline_info.pDynamicState = &dynamic_state_create_info;

    RHIPipeline* material_pipeline = RHI_NULL_HANDLE;
    if (RHI_SUCCESS != m_Rhi->CreateGraphicsPipelines(RHI_NULL_HANDLE, 1, &pipeline_info, material_pipeline))
    {
        LOG_WARNING(ZRender,
                    "failed to create runtime directional shadow pipeline for shader '{}', fallback to built-in pipeline",
                    material.shader_name);
        material_pipeline = default_pipeline;
    }

    m_Rhi->DestroyShaderModule(vert_shader_module);
    m_Rhi->DestroyShaderModule(frag_shader_module);

    m_MaterialPipelines[pipeline_key] = material_pipeline != nullptr ? material_pipeline : default_pipeline;
    return material_pipeline != nullptr ? material_pipeline : default_pipeline;
}

void DirectionalLightShadowPass::Initialize(const RenderPassInitInfo* init_info)
{
    RenderPass::Initialize(nullptr);

    SetupAttachments();
    SetupRenderPass();
    SetupFramebuffer();
    SetupDescriptorSetLayout();
}
void DirectionalLightShadowPass::PostInitialize()
{
    SetupPipelines();

    if (m_Rhi && m_Rhi->getGraphicsAPI() == GraphicsAPI::DirectX12)
    {
        return;
    }

    if (!EnsureGlobalRenderResourceReady())
    {
        LOG_WARNING(ZRender,
                    "DirectionalLightShadowPass: global render resource unavailable; skipping descriptor "
                    "setup");
        return;
    }

    SetupDescriptorSet();
}

void DirectionalLightShadowPass::FinishDescriptorSetup()
{
    if (!m_Rhi || m_Rhi->getGraphicsAPI() != GraphicsAPI::DirectX12)
    {
        return;
    }

    if (!EnsureGlobalRenderResourceReady())
    {
        LOG_WARNING(ZRender,
                    "DirectionalLightShadowPass: global render resource unavailable; skipping descriptor setup");
        return;
    }

    SetupDescriptorSet();
}

void DirectionalLightShadowPass::PreparePassData(std::shared_ptr<RenderResourceBase> render_resource)
{
    const RenderResource* vulkan_resource = static_cast<const RenderResource*>(render_resource.get());
    if (vulkan_resource)
    {
        m_MeshDirectionalLightShadowPerframeStorageBufferObject =
            vulkan_resource->m_MeshDirectionalLightShadowPerframeStorageBufferObject;
    }
}
void DirectionalLightShadowPass::Draw()
{
    DrawModel();
}
void DirectionalLightShadowPass::SetupAttachments()
{
    // color and depth
    m_Framebuffer.attachments.resize(2);

    // color
    m_Framebuffer.attachments[0].format = RHI_FORMAT_R32_SFLOAT;
    m_Rhi->CreateImage(s_DirectionalLightShadowMapDimension,
                       s_DirectionalLightShadowMapDimension,
                       m_Framebuffer.attachments[0].format,
                       RHI_IMAGE_TILING_OPTIMAL,
                       RHI_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | RHI_IMAGE_USAGE_SAMPLED_BIT,
                       RHI_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                       m_Framebuffer.attachments[0].image,
                       m_Framebuffer.attachments[0].mem,
                       0,
                       1,
                       1);
    m_Rhi->CreateImageView(m_Framebuffer.attachments[0].image,
                           m_Framebuffer.attachments[0].format,
                           RHI_IMAGE_ASPECT_COLOR_BIT,
                           RHI_IMAGE_VIEW_TYPE_2D,
                           1,
                           1,
                           m_Framebuffer.attachments[0].view);

    // depth
    m_Framebuffer.attachments[1].format = m_Rhi->GetDepthImageInfo().depth_image_format;
    m_Rhi->CreateImage(s_DirectionalLightShadowMapDimension,
                       s_DirectionalLightShadowMapDimension,
                       m_Framebuffer.attachments[1].format,
                       RHI_IMAGE_TILING_OPTIMAL,
                       RHI_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | RHI_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
                       RHI_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                       m_Framebuffer.attachments[1].image,
                       m_Framebuffer.attachments[1].mem,
                       0,
                       1,
                       1);
    m_Rhi->CreateImageView(m_Framebuffer.attachments[1].image,
                           m_Framebuffer.attachments[1].format,
                           RHI_IMAGE_ASPECT_DEPTH_BIT,
                           RHI_IMAGE_VIEW_TYPE_2D,
                           1,
                           1,
                           m_Framebuffer.attachments[1].view);
}
void DirectionalLightShadowPass::SetupRenderPass()
{
    RHIAttachmentDescription attachments[2] = {};

    RHIAttachmentDescription& directional_light_shadow_color_attachment_description = attachments[0];
    directional_light_shadow_color_attachment_description.format = m_Framebuffer.attachments[0].format;
    directional_light_shadow_color_attachment_description.samples = RHI_SAMPLE_COUNT_1_BIT;
    directional_light_shadow_color_attachment_description.loadOp = RHI_ATTACHMENT_LOAD_OP_CLEAR;
    directional_light_shadow_color_attachment_description.storeOp = RHI_ATTACHMENT_STORE_OP_STORE;
    directional_light_shadow_color_attachment_description.stencilLoadOp = RHI_ATTACHMENT_LOAD_OP_DONT_CARE;
    directional_light_shadow_color_attachment_description.stencilStoreOp = RHI_ATTACHMENT_STORE_OP_DONT_CARE;
    directional_light_shadow_color_attachment_description.initialLayout = RHI_IMAGE_LAYOUT_UNDEFINED;
    directional_light_shadow_color_attachment_description.finalLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    RHIAttachmentDescription& directional_light_shadow_depth_attachment_description = attachments[1];
    directional_light_shadow_depth_attachment_description.format = m_Framebuffer.attachments[1].format;
    directional_light_shadow_depth_attachment_description.samples = RHI_SAMPLE_COUNT_1_BIT;
    directional_light_shadow_depth_attachment_description.loadOp = RHI_ATTACHMENT_LOAD_OP_CLEAR;
    directional_light_shadow_depth_attachment_description.storeOp = RHI_ATTACHMENT_STORE_OP_DONT_CARE;
    directional_light_shadow_depth_attachment_description.stencilLoadOp = RHI_ATTACHMENT_LOAD_OP_DONT_CARE;
    directional_light_shadow_depth_attachment_description.stencilStoreOp = RHI_ATTACHMENT_STORE_OP_DONT_CARE;
    directional_light_shadow_depth_attachment_description.initialLayout = RHI_IMAGE_LAYOUT_UNDEFINED;
    directional_light_shadow_depth_attachment_description.finalLayout =
        RHI_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    RHISubpassDescription subpasses[1] = {};

    RHIAttachmentReference shadow_pass_color_attachment_reference {};
    shadow_pass_color_attachment_reference.attachment = 0;
    shadow_pass_color_attachment_reference.layout = RHI_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    RHIAttachmentReference shadow_pass_depth_attachment_reference {};
    shadow_pass_depth_attachment_reference.attachment = 1;
    shadow_pass_depth_attachment_reference.layout = RHI_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    RHISubpassDescription& shadow_pass = subpasses[0];
    shadow_pass.pipelineBindPoint = RHI_PIPELINE_BIND_POINT_GRAPHICS;
    shadow_pass.colorAttachmentCount = 1;
    shadow_pass.pColorAttachments = &shadow_pass_color_attachment_reference;
    shadow_pass.pDepthStencilAttachment = &shadow_pass_depth_attachment_reference;

    RHISubpassDependency dependencies[1] = {};

    RHISubpassDependency& lighting_pass_dependency = dependencies[0];
    lighting_pass_dependency.srcSubpass = 0;
    lighting_pass_dependency.dstSubpass = RHI_SUBPASS_EXTERNAL;
    lighting_pass_dependency.srcStageMask = RHI_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    lighting_pass_dependency.dstStageMask = RHI_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    lighting_pass_dependency.srcAccessMask = RHI_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;  // STORE_OP_STORE
    lighting_pass_dependency.dstAccessMask = 0;
    lighting_pass_dependency.dependencyFlags = 0;  // NOT BY REGION

    RHIRenderPassCreateInfo renderpass_create_info {};
    renderpass_create_info.sType = RHI_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderpass_create_info.attachmentCount = (sizeof(attachments) / sizeof(attachments[0]));
    renderpass_create_info.pAttachments = attachments;
    renderpass_create_info.subpassCount = (sizeof(subpasses) / sizeof(subpasses[0]));
    renderpass_create_info.pSubpasses = subpasses;
    renderpass_create_info.dependencyCount = (sizeof(dependencies) / sizeof(dependencies[0]));
    renderpass_create_info.pDependencies = dependencies;

    if (RHI_SUCCESS != m_Rhi->CreateRenderPass(&renderpass_create_info, m_Framebuffer.render_pass))
    {
        throw std::runtime_error("create directional light shadow render pass");
    }
}
void DirectionalLightShadowPass::SetupFramebuffer()
{
    RHIImageView* attachments[2] = {m_Framebuffer.attachments[0].view, m_Framebuffer.attachments[1].view};

    RHIFramebufferCreateInfo framebuffer_create_info {};
    framebuffer_create_info.sType = RHI_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebuffer_create_info.flags = 0U;
    framebuffer_create_info.renderPass = m_Framebuffer.render_pass;
    framebuffer_create_info.attachmentCount = (sizeof(attachments) / sizeof(attachments[0]));
    framebuffer_create_info.pAttachments = attachments;
    framebuffer_create_info.width = s_DirectionalLightShadowMapDimension;
    framebuffer_create_info.height = s_DirectionalLightShadowMapDimension;
    framebuffer_create_info.layers = 1;

    if (RHI_SUCCESS != m_Rhi->CreateFramebuffer(&framebuffer_create_info, m_Framebuffer.framebuffer))
    {
        throw std::runtime_error("create directional light shadow framebuffer");
    }
}
void DirectionalLightShadowPass::SetupDescriptorSetLayout()
{
    m_DescriptorInfos.resize(1);

    RHIDescriptorSetLayoutBinding mesh_directional_light_shadow_global_layout_bindings[3];

    RHIDescriptorSetLayoutBinding& mesh_directional_light_shadow_global_layout_perframe_storage_buffer_binding =
        mesh_directional_light_shadow_global_layout_bindings[0];
    mesh_directional_light_shadow_global_layout_perframe_storage_buffer_binding.binding = 0;
    mesh_directional_light_shadow_global_layout_perframe_storage_buffer_binding.descriptorType =
        RHI_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
    mesh_directional_light_shadow_global_layout_perframe_storage_buffer_binding.descriptorCount = 1;
    mesh_directional_light_shadow_global_layout_perframe_storage_buffer_binding.stageFlags =
        RHI_SHADER_STAGE_VERTEX_BIT;

    RHIDescriptorSetLayoutBinding& mesh_directional_light_shadow_global_layout_perdrawcall_storage_buffer_binding =
        mesh_directional_light_shadow_global_layout_bindings[1];
    mesh_directional_light_shadow_global_layout_perdrawcall_storage_buffer_binding.binding = 1;
    mesh_directional_light_shadow_global_layout_perdrawcall_storage_buffer_binding.descriptorType =
        RHI_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
    mesh_directional_light_shadow_global_layout_perdrawcall_storage_buffer_binding.descriptorCount = 1;
    mesh_directional_light_shadow_global_layout_perdrawcall_storage_buffer_binding.stageFlags =
        RHI_SHADER_STAGE_VERTEX_BIT;

    RHIDescriptorSetLayoutBinding&
        mesh_directional_light_shadow_global_layout_per_drawcall_vertex_blending_storage_buffer_binding =
            mesh_directional_light_shadow_global_layout_bindings[2];
    mesh_directional_light_shadow_global_layout_per_drawcall_vertex_blending_storage_buffer_binding.binding = 2;
    mesh_directional_light_shadow_global_layout_per_drawcall_vertex_blending_storage_buffer_binding.descriptorType =
        RHI_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
    mesh_directional_light_shadow_global_layout_per_drawcall_vertex_blending_storage_buffer_binding.descriptorCount = 1;
    mesh_directional_light_shadow_global_layout_per_drawcall_vertex_blending_storage_buffer_binding.stageFlags =
        RHI_SHADER_STAGE_VERTEX_BIT;

    RHIDescriptorSetLayoutCreateInfo mesh_point_light_shadow_global_layout_create_info;
    mesh_point_light_shadow_global_layout_create_info.sType = RHI_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    mesh_point_light_shadow_global_layout_create_info.pNext = NULL;
    mesh_point_light_shadow_global_layout_create_info.flags = 0;

    if (m_Rhi->getGraphicsAPI() == GraphicsAPI::DirectX12)
    {
        // ShadowPassDx12Shaders.h: cbuffer PerFrame at b0, StructuredBuffer<MeshInstance> at t1.
        mesh_directional_light_shadow_global_layout_perframe_storage_buffer_binding.descriptorType =
            RHI_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        mesh_directional_light_shadow_global_layout_perdrawcall_storage_buffer_binding.descriptorType =
            RHI_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        mesh_point_light_shadow_global_layout_create_info.bindingCount = 2;
        mesh_point_light_shadow_global_layout_create_info.pBindings =
            mesh_directional_light_shadow_global_layout_bindings;
    }
    else
    {
        mesh_point_light_shadow_global_layout_create_info.bindingCount =
            (sizeof(mesh_directional_light_shadow_global_layout_bindings) /
             sizeof(mesh_directional_light_shadow_global_layout_bindings[0]));
        mesh_point_light_shadow_global_layout_create_info.pBindings =
            mesh_directional_light_shadow_global_layout_bindings;
    }

    if (RHI_SUCCESS != m_Rhi->CreateDescriptorSetLayout(&mesh_point_light_shadow_global_layout_create_info,
                                                        m_DescriptorInfos[0].layout))
    {
        throw std::runtime_error("create mesh directional light shadow global layout");
    }
}
void DirectionalLightShadowPass::SetupPipelines()
{
    m_RenderPipelines.resize(1);

    RHIDescriptorSetLayout* descriptorset_layouts[2] = {m_DescriptorInfos[0].layout, m_PerMeshLayout};
    uint32_t set_layout_count = 2;
    if (m_Rhi->getGraphicsAPI() == GraphicsAPI::DirectX12)
    {
        set_layout_count = 1;
    }

    RHIPipelineLayoutCreateInfo pipeline_layout_create_info {};
    pipeline_layout_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeline_layout_create_info.setLayoutCount = set_layout_count;
    pipeline_layout_create_info.pSetLayouts = descriptorset_layouts;

    if (RHI_SUCCESS != m_Rhi->CreatePipelineLayout(&pipeline_layout_create_info, m_RenderPipelines[0].layout))
    {
        throw std::runtime_error("create mesh directional light shadow pipeline layout");
    }

    RHIShader* vert_shader_module = nullptr;
    RHIShader* frag_shader_module = nullptr;
    if (m_Rhi->getGraphicsAPI() == GraphicsAPI::DirectX12)
    {
        vert_shader_module = m_Rhi->CreateShaderModuleFromSource(ShadowPassDx12Shaders::kDirectionalShadowVert,
                                                                 ShaderStage::Vertex,
                                                                 "mesh_directional_light_shadow.vert",
                                                                 {},
                                                                 {});
        frag_shader_module = m_Rhi->CreateShaderModuleFromSource(ShadowPassDx12Shaders::kDirectionalShadowFrag,
                                                                 ShaderStage::Fragment,
                                                                 "mesh_directional_light_shadow.frag",
                                                                 {},
                                                                 {});
    }
#if defined(Z_HAS_VULKAN)
    else
    {
        vert_shader_module = m_Rhi->CreateShaderModule(MESH_DIRECTIONAL_LIGHT_SHADOW_VERT);
        frag_shader_module = m_Rhi->CreateShaderModule(MESH_DIRECTIONAL_LIGHT_SHADOW_FRAG);
    }
#endif
    if (vert_shader_module == nullptr || frag_shader_module == nullptr)
    {
        throw std::runtime_error("DirectionalLightShadowPass: failed to create built-in shadow shaders");
    }

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
    vertex_input_state_create_info.vertexBindingDescriptionCount =
        static_cast<uint32_t>(vertex_binding_descriptions.size());
    vertex_input_state_create_info.pVertexBindingDescriptions = vertex_binding_descriptions.data();
    vertex_input_state_create_info.vertexAttributeDescriptionCount =
        static_cast<uint32_t>(vertex_attribute_descriptions.size());
    vertex_input_state_create_info.pVertexAttributeDescriptions = vertex_attribute_descriptions.data();

    RHIPipelineInputAssemblyStateCreateInfo input_assembly_create_info {};
    input_assembly_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly_create_info.topology = RHI_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    input_assembly_create_info.primitiveRestartEnable = RHI_FALSE;

    RHIViewport viewport = {
        0, 0, s_DirectionalLightShadowMapDimension, s_DirectionalLightShadowMapDimension, 0.0, 1.0};
    RHIRect2D scissor = {{0, 0}, {s_DirectionalLightShadowMapDimension, s_DirectionalLightShadowMapDimension}};

    RHIPipelineViewportStateCreateInfo viewport_state_create_info {};
    viewport_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state_create_info.viewportCount = 1;
    viewport_state_create_info.pViewports = &viewport;
    viewport_state_create_info.scissorCount = 1;
    viewport_state_create_info.pScissors = &scissor;

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

    RHIPipelineColorBlendAttachmentState color_blend_attachment_state {};
    color_blend_attachment_state.colorWriteMask =
        RHI_COLOR_COMPONENT_R_BIT | RHI_COLOR_COMPONENT_G_BIT | RHI_COLOR_COMPONENT_B_BIT | RHI_COLOR_COMPONENT_A_BIT;
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
    depth_stencil_create_info.depthTestEnable = RHI_TRUE;
    depth_stencil_create_info.depthWriteEnable = RHI_TRUE;
    depth_stencil_create_info.depthCompareOp = RHI_COMPARE_OP_LESS;
    depth_stencil_create_info.depthBoundsTestEnable = RHI_FALSE;
    depth_stencil_create_info.stencilTestEnable = RHI_FALSE;

    RHIPipelineDynamicStateCreateInfo dynamic_state_create_info {};
    dynamic_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic_state_create_info.dynamicStateCount = 0;
    dynamic_state_create_info.pDynamicStates = NULL;

    RHIGraphicsPipelineCreateInfo pipelineInfo {};
    pipelineInfo.sType = RHI_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = (sizeof(shader_stages) / sizeof(shader_stages[0]));
    pipelineInfo.pStages = shader_stages;
    pipelineInfo.pVertexInputState = &vertex_input_state_create_info;
    pipelineInfo.pInputAssemblyState = &input_assembly_create_info;
    pipelineInfo.pViewportState = &viewport_state_create_info;
    pipelineInfo.pRasterizationState = &rasterization_state_create_info;
    pipelineInfo.pMultisampleState = &multisample_state_create_info;
    pipelineInfo.pColorBlendState = &color_blend_state_create_info;
    pipelineInfo.pDepthStencilState = &depth_stencil_create_info;
    pipelineInfo.layout = m_RenderPipelines[0].layout;
    pipelineInfo.renderPass = m_Framebuffer.render_pass;
    pipelineInfo.subpass = 0;
    pipelineInfo.basePipelineHandle = RHI_NULL_HANDLE;
    pipelineInfo.pDynamicState = &dynamic_state_create_info;

    if (RHI_SUCCESS !=
        m_Rhi->CreateGraphicsPipelines(RHI_NULL_HANDLE, 1, &pipelineInfo, m_RenderPipelines[0].pipeline))
    {
        LOG_ERROR(ZRender,
                  "DirectionalLightShadowPass: CreateGraphicsPipelines failed (check RTV/DSV format vs render pass)");
        throw std::runtime_error("create mesh directional light shadow graphics pipeline");
    }

    m_Rhi->DestroyShaderModule(vert_shader_module);
    m_Rhi->DestroyShaderModule(frag_shader_module);
}
void DirectionalLightShadowPass::SetupDescriptorSet()
{
    RHIDescriptorSetAllocateInfo mesh_directional_light_shadow_global_descriptor_set_alloc_info;
    mesh_directional_light_shadow_global_descriptor_set_alloc_info.sType =
        RHI_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    mesh_directional_light_shadow_global_descriptor_set_alloc_info.pNext = NULL;
    mesh_directional_light_shadow_global_descriptor_set_alloc_info.descriptorPool = m_Rhi->GetDescriptorPoor();
    mesh_directional_light_shadow_global_descriptor_set_alloc_info.descriptorSetCount = 1;
    mesh_directional_light_shadow_global_descriptor_set_alloc_info.pSetLayouts = &m_DescriptorInfos[0].layout;

    if (RHI_SUCCESS != m_Rhi->AllocateDescriptorSets(&mesh_directional_light_shadow_global_descriptor_set_alloc_info,
                                                     m_DescriptorInfos[0].descriptor_set))
    {
        throw std::runtime_error("allocate mesh directional light shadow global descriptor set");
    }

    RHIDescriptorBufferInfo mesh_directional_light_shadow_perframe_storage_buffer_info = {};
    // this offset plus dynamic_offset should not be greater than the size of the buffer
    mesh_directional_light_shadow_perframe_storage_buffer_info.offset = 0;
    // the range means the size actually used by the shader per draw call
    mesh_directional_light_shadow_perframe_storage_buffer_info.range =
        sizeof(MeshDirectionalLightShadowPerframeStorageBufferObject);
    mesh_directional_light_shadow_perframe_storage_buffer_info.buffer =
        m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbuffer;
    assert(mesh_directional_light_shadow_perframe_storage_buffer_info.range <
           m_GlobalRenderResource->m_StorageBuffer.m_MaxStorageBufferRange);

    RHIDescriptorBufferInfo mesh_directional_light_shadow_perdrawcall_storage_buffer_info = {};
    mesh_directional_light_shadow_perdrawcall_storage_buffer_info.offset = 0;
    mesh_directional_light_shadow_perdrawcall_storage_buffer_info.range =
        sizeof(MeshDirectionalLightShadowPerdrawcallStorageBufferObject);
    mesh_directional_light_shadow_perdrawcall_storage_buffer_info.buffer =
        m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbuffer;
    assert(mesh_directional_light_shadow_perdrawcall_storage_buffer_info.range <
           m_GlobalRenderResource->m_StorageBuffer.m_MaxStorageBufferRange);

    RHIDescriptorBufferInfo mesh_directional_light_shadow_per_drawcall_vertex_blending_storage_buffer_info = {};
    mesh_directional_light_shadow_per_drawcall_vertex_blending_storage_buffer_info.offset = 0;
    mesh_directional_light_shadow_per_drawcall_vertex_blending_storage_buffer_info.range =
        sizeof(MeshDirectionalLightShadowPerdrawcallVertexBlendingStorageBufferObject);
    mesh_directional_light_shadow_per_drawcall_vertex_blending_storage_buffer_info.buffer =
        m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbuffer;
    assert(mesh_directional_light_shadow_per_drawcall_vertex_blending_storage_buffer_info.range <
           m_GlobalRenderResource->m_StorageBuffer.m_MaxStorageBufferRange);

    RHIDescriptorSet* descriptor_set_to_write = m_DescriptorInfos[0].descriptor_set;

    RHIWriteDescriptorSet descriptor_writes[3];

    RHIWriteDescriptorSet& mesh_directional_light_shadow_perframe_storage_buffer_write_info = descriptor_writes[0];
    mesh_directional_light_shadow_perframe_storage_buffer_write_info.sType = RHI_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    mesh_directional_light_shadow_perframe_storage_buffer_write_info.pNext = NULL;
    mesh_directional_light_shadow_perframe_storage_buffer_write_info.dstSet = descriptor_set_to_write;
    mesh_directional_light_shadow_perframe_storage_buffer_write_info.dstBinding = 0;
    mesh_directional_light_shadow_perframe_storage_buffer_write_info.dstArrayElement = 0;
    mesh_directional_light_shadow_perframe_storage_buffer_write_info.descriptorType =
        (m_Rhi->getGraphicsAPI() == GraphicsAPI::DirectX12) ? RHI_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC
                                                            : RHI_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
    mesh_directional_light_shadow_perframe_storage_buffer_write_info.descriptorCount = 1;
    mesh_directional_light_shadow_perframe_storage_buffer_write_info.pBufferInfo =
        &mesh_directional_light_shadow_perframe_storage_buffer_info;

    RHIWriteDescriptorSet& mesh_directional_light_shadow_perdrawcall_storage_buffer_write_info = descriptor_writes[1];
    mesh_directional_light_shadow_perdrawcall_storage_buffer_write_info.sType = RHI_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    mesh_directional_light_shadow_perdrawcall_storage_buffer_write_info.pNext = NULL;
    mesh_directional_light_shadow_perdrawcall_storage_buffer_write_info.dstSet = descriptor_set_to_write;
    mesh_directional_light_shadow_perdrawcall_storage_buffer_write_info.dstBinding = 1;
    mesh_directional_light_shadow_perdrawcall_storage_buffer_write_info.dstArrayElement = 0;
    mesh_directional_light_shadow_perdrawcall_storage_buffer_write_info.descriptorType =
        (m_Rhi->getGraphicsAPI() == GraphicsAPI::DirectX12) ? RHI_DESCRIPTOR_TYPE_STORAGE_BUFFER
                                                            : RHI_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
    mesh_directional_light_shadow_perdrawcall_storage_buffer_write_info.descriptorCount = 1;
    mesh_directional_light_shadow_perdrawcall_storage_buffer_write_info.pBufferInfo =
        &mesh_directional_light_shadow_perdrawcall_storage_buffer_info;

    RHIWriteDescriptorSet& mesh_directional_light_shadow_per_drawcall_vertex_blending_storage_buffer_write_info =
        descriptor_writes[2];
    mesh_directional_light_shadow_per_drawcall_vertex_blending_storage_buffer_write_info.sType =
        RHI_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    mesh_directional_light_shadow_per_drawcall_vertex_blending_storage_buffer_write_info.pNext = NULL;
    mesh_directional_light_shadow_per_drawcall_vertex_blending_storage_buffer_write_info.dstSet =
        descriptor_set_to_write;
    mesh_directional_light_shadow_per_drawcall_vertex_blending_storage_buffer_write_info.dstBinding = 2;
    mesh_directional_light_shadow_per_drawcall_vertex_blending_storage_buffer_write_info.dstArrayElement = 0;
    mesh_directional_light_shadow_per_drawcall_vertex_blending_storage_buffer_write_info.descriptorType =
        RHI_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
    mesh_directional_light_shadow_per_drawcall_vertex_blending_storage_buffer_write_info.descriptorCount = 1;
    mesh_directional_light_shadow_per_drawcall_vertex_blending_storage_buffer_write_info.pBufferInfo =
        &mesh_directional_light_shadow_per_drawcall_vertex_blending_storage_buffer_info;

    const uint32_t write_count = (m_Rhi->getGraphicsAPI() == GraphicsAPI::DirectX12) ? 2u : static_cast<uint32_t>(sizeof(descriptor_writes) / sizeof(descriptor_writes[0]));
    m_Rhi->UpdateDescriptorSets(write_count, descriptor_writes, 0, NULL);
}
void DirectionalLightShadowPass::DrawModel()
{
    struct MeshNode
    {
        const Matrix4x4* model_matrix {nullptr};
        const Matrix4x4* joint_matrices {nullptr};
        uint32_t joint_count {0};
    };

    std::map<VulkanPBRMaterial*, std::map<VulkanMesh*, std::vector<MeshNode>>> directional_light_mesh_drawcall_batch;

    // reorganize mesh
    const std::vector<RenderMeshNode>* visible_nodes = m_VisiableNodes.p_directional_light_visible_mesh_nodes;
    if (visible_nodes != nullptr)
    {
        for (const RenderMeshNode& node : *visible_nodes)
        {
            auto& mesh_instanced = directional_light_mesh_drawcall_batch[AsVulkanMaterialResource(node.ref_material)];
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
    }

    // Directional Light Shadow begin pass
    {
        RHIRenderPassBeginInfo renderpass_begin_info {};
        renderpass_begin_info.sType = RHI_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        renderpass_begin_info.renderPass = m_Framebuffer.render_pass;
        renderpass_begin_info.framebuffer = m_Framebuffer.framebuffer;
        renderpass_begin_info.renderArea.offset = {0, 0};
        renderpass_begin_info.renderArea.extent = {s_DirectionalLightShadowMapDimension,
                                                   s_DirectionalLightShadowMapDimension};

        RHIClearValue clear_values[2];
        clear_values[0].color = {1.0f};
        clear_values[1].depthStencil = {1.0f, 0};
        renderpass_begin_info.clearValueCount = (sizeof(clear_values) / sizeof(clear_values[0]));
        renderpass_begin_info.pClearValues = clear_values;

        m_Rhi->CmdBeginRenderPassPFN(
            m_Rhi->GetCurrentCommandBuffer(), &renderpass_begin_info, RHI_SUBPASS_CONTENTS_INLINE);

        float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        m_Rhi->PushEvent(m_Rhi->GetCurrentCommandBuffer(), "Directional Light Shadow", color);
    }

    // Mesh (DX12: shadow PSO exists but per-draw descriptor/bind path is still fragile;
    // clear-only until validated -- mirrors PointLightShadowPass DX12 policy.)
    if (m_Rhi->getGraphicsAPI() != GraphicsAPI::DirectX12 && m_Rhi->IsPointLightShadowEnabled() &&
        EnsureGlobalRenderResourceReady())
    {
        float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        m_Rhi->PushEvent(m_Rhi->GetCurrentCommandBuffer(), "Mesh", color);

        RHIPipeline* bound_pipeline = nullptr;

        // perframe storage buffer

        uint32_t perframe_dynamic_offset = RoundUp(
            m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbuffersEnd[m_Rhi->GetCurrentFrameIndex()],
            m_GlobalRenderResource->m_StorageBuffer.m_MinStorageBufferOffsetAlignment);
        m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbuffersEnd[m_Rhi->GetCurrentFrameIndex()] =
            perframe_dynamic_offset + sizeof(MeshDirectionalLightShadowPerframeStorageBufferObject);
        assert(
            m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbuffersEnd[m_Rhi->GetCurrentFrameIndex()] <=
            (m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbuffersBegin[m_Rhi->GetCurrentFrameIndex()] +
             m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbuffersSize[m_Rhi->GetCurrentFrameIndex()]));

        MeshDirectionalLightShadowPerframeStorageBufferObject& perframe_storage_buffer_object =
            (*reinterpret_cast<MeshDirectionalLightShadowPerframeStorageBufferObject*>(
                reinterpret_cast<uintptr_t>(
                    m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbufferMemoryPointer) +
                perframe_dynamic_offset));
        perframe_storage_buffer_object = m_MeshDirectionalLightShadowPerframeStorageBufferObject;

        for (auto& [material, mesh_instanced] : directional_light_mesh_drawcall_batch)
        {
            RHIPipeline* const material_pipeline = GetOrCreateShadowPipeline(*material);
            if (material_pipeline != bound_pipeline)
            {
                m_Rhi->CmdBindPipelinePFN(
                    m_Rhi->GetCurrentCommandBuffer(), RHI_PIPELINE_BIND_POINT_GRAPHICS, material_pipeline);
                bound_pipeline = material_pipeline;
            }

            // TODO: render from near to far

            for (auto& [mesh, mesh_nodes] : mesh_instanced)

            {
                uint32_t total_instance_count = static_cast<uint32_t>(mesh_nodes.size());
                if (total_instance_count > 0)
                {
                    // bind per mesh
                    if (mesh->mesh_vertex_blending_descriptor_set != nullptr)
                    {
                        m_Rhi->CmdBindDescriptorSetsPFN(m_Rhi->GetCurrentCommandBuffer(),
                                                        RHI_PIPELINE_BIND_POINT_GRAPHICS,
                                                        m_RenderPipelines[0].layout,
                                                        1,
                                                        1,
                                                        &mesh->mesh_vertex_blending_descriptor_set,
                                                        0,
                                                        NULL);
                    }

                    if (mesh->mesh_vertex_position_buffer == nullptr || mesh->mesh_index_buffer == nullptr ||
                        mesh->mesh_index_count == 0)
                    {
                        continue;
                    }

                    RHIBuffer* vertex_buffers[] = {mesh->mesh_vertex_position_buffer};
                    RHIDeviceSize offsets[] = {0};
                    m_Rhi->CmdBindVertexBuffersPFN(m_Rhi->GetCurrentCommandBuffer(), 0, 1, vertex_buffers, offsets);
                    m_Rhi->CmdBindIndexBufferPFN(
                        m_Rhi->GetCurrentCommandBuffer(), mesh->mesh_index_buffer, 0, RHI_INDEX_TYPE_UINT16);

                    uint32_t drawcall_max_instance_count =
                        (sizeof(MeshDirectionalLightShadowPerdrawcallStorageBufferObject::mesh_instances) /
                         sizeof(MeshDirectionalLightShadowPerdrawcallStorageBufferObject::mesh_instances[0]));
                    uint32_t drawcall_count =
                        RoundUp(total_instance_count, drawcall_max_instance_count) / drawcall_max_instance_count;

                    for (uint32_t drawcall_index = 0; drawcall_index < drawcall_count; ++drawcall_index)
                    {
                        uint32_t current_instance_count =
                            ((total_instance_count - drawcall_max_instance_count * drawcall_index) <
                             drawcall_max_instance_count)
                                ? (total_instance_count - drawcall_max_instance_count * drawcall_index)
                                : drawcall_max_instance_count;

                        // perdrawcall storage buffer
                        uint32_t perdrawcall_dynamic_offset =
                            RoundUp(m_GlobalRenderResource->m_StorageBuffer
                                        .m_GlobalUploadRingbuffersEnd[m_Rhi->GetCurrentFrameIndex()],
                                    m_GlobalRenderResource->m_StorageBuffer.m_MinStorageBufferOffsetAlignment);
                        m_GlobalRenderResource->m_StorageBuffer
                            .m_GlobalUploadRingbuffersEnd[m_Rhi->GetCurrentFrameIndex()] =
                            perdrawcall_dynamic_offset +
                            sizeof(MeshDirectionalLightShadowPerdrawcallStorageBufferObject);
                        assert(m_GlobalRenderResource->m_StorageBuffer
                                   .m_GlobalUploadRingbuffersEnd[m_Rhi->GetCurrentFrameIndex()] <=
                               (m_GlobalRenderResource->m_StorageBuffer
                                    .m_GlobalUploadRingbuffersBegin[m_Rhi->GetCurrentFrameIndex()] +
                                m_GlobalRenderResource->m_StorageBuffer
                                    .m_GlobalUploadRingbuffersSize[m_Rhi->GetCurrentFrameIndex()]));

                        MeshDirectionalLightShadowPerdrawcallStorageBufferObject& perdrawcall_storage_buffer_object =
                            (*reinterpret_cast<MeshDirectionalLightShadowPerdrawcallStorageBufferObject*>(
                                reinterpret_cast<uintptr_t>(m_GlobalRenderResource->m_StorageBuffer
                                                                .m_GlobalUploadRingbufferMemoryPointer) +
                                perdrawcall_dynamic_offset));
                        for (uint32_t i = 0; i < current_instance_count; ++i)
                        {
                            perdrawcall_storage_buffer_object.mesh_instances[i].model_matrix =
                                *mesh_nodes[drawcall_max_instance_count * drawcall_index + i].model_matrix;
                            perdrawcall_storage_buffer_object.mesh_instances[i].enable_vertex_blending =
                                mesh_nodes[drawcall_max_instance_count * drawcall_index + i].joint_matrices ? 1.0
                                                                                                            : -1.0;
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
                                sizeof(MeshDirectionalLightShadowPerdrawcallVertexBlendingStorageBufferObject);
                            assert(m_GlobalRenderResource->m_StorageBuffer
                                       .m_GlobalUploadRingbuffersEnd[m_Rhi->GetCurrentFrameIndex()] <=
                                   (m_GlobalRenderResource->m_StorageBuffer
                                        .m_GlobalUploadRingbuffersBegin[m_Rhi->GetCurrentFrameIndex()] +
                                    m_GlobalRenderResource->m_StorageBuffer
                                        .m_GlobalUploadRingbuffersSize[m_Rhi->GetCurrentFrameIndex()]));

                            MeshDirectionalLightShadowPerdrawcallVertexBlendingStorageBufferObject&
                                per_drawcall_vertex_blending_storage_buffer_object =
                                    (*reinterpret_cast<
                                        MeshDirectionalLightShadowPerdrawcallVertexBlendingStorageBufferObject*>(
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
                                            mesh_nodes[drawcall_max_instance_count * drawcall_index + i]
                                                .joint_matrices[j];
                                    }
                                }
                            }
                        }
                        else
                        {
                            per_drawcall_vertex_blending_dynamic_offset = 0;
                        }

                        // bind perdrawcall
                        if (m_Rhi->getGraphicsAPI() == GraphicsAPI::DirectX12)
                        {
                            RHIDescriptorBufferInfo perframe_info = {};
                            perframe_info.buffer = m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbuffer;
                            perframe_info.offset = perframe_dynamic_offset;
                            perframe_info.range =
                                sizeof(MeshDirectionalLightShadowPerframeStorageBufferObject);

                            RHIDescriptorBufferInfo perdrawcall_info = {};
                            perdrawcall_info.buffer = m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbuffer;
                            perdrawcall_info.offset = perdrawcall_dynamic_offset;
                            perdrawcall_info.range =
                                sizeof(MeshDirectionalLightShadowPerdrawcallStorageBufferObject);

                            RHIWriteDescriptorSet dx12_writes[2] = {};
                            dx12_writes[0].sType = RHI_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                            dx12_writes[0].dstSet = m_DescriptorInfos[0].descriptor_set;
                            dx12_writes[0].dstBinding = 0;
                            dx12_writes[0].descriptorType = RHI_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
                            dx12_writes[0].descriptorCount = 1;
                            dx12_writes[0].pBufferInfo = &perframe_info;

                            dx12_writes[1].sType = RHI_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                            dx12_writes[1].dstSet = m_DescriptorInfos[0].descriptor_set;
                            dx12_writes[1].dstBinding = 1;
                            dx12_writes[1].descriptorType = RHI_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                            dx12_writes[1].descriptorCount = 1;
                            dx12_writes[1].pBufferInfo = &perdrawcall_info;

                            m_Rhi->UpdateDescriptorSets(2, dx12_writes, 0, nullptr);
                            m_Rhi->CmdBindDescriptorSetsPFN(m_Rhi->GetCurrentCommandBuffer(),
                                                            RHI_PIPELINE_BIND_POINT_GRAPHICS,
                                                            m_RenderPipelines[0].layout,
                                                            0,
                                                            1,
                                                            &m_DescriptorInfos[0].descriptor_set,
                                                            0,
                                                            nullptr);
                        }
                        else
                        {
                            uint32_t dynamic_offsets[3] = {perframe_dynamic_offset,
                                                           perdrawcall_dynamic_offset,
                                                           per_drawcall_vertex_blending_dynamic_offset};
                            m_Rhi->CmdBindDescriptorSetsPFN(m_Rhi->GetCurrentCommandBuffer(),
                                                            RHI_PIPELINE_BIND_POINT_GRAPHICS,
                                                            m_RenderPipelines[0].layout,
                                                            0,
                                                            1,
                                                            &m_DescriptorInfos[0].descriptor_set,
                                                            (sizeof(dynamic_offsets) / sizeof(dynamic_offsets[0])),
                                                            dynamic_offsets);
                        }
                        m_Rhi->CmdDrawIndexedPFN(
                            m_Rhi->GetCurrentCommandBuffer(), mesh->mesh_index_count, current_instance_count, 0, 0, 0);
                    }
                }
            }
        }

        m_Rhi->PopEvent(m_Rhi->GetCurrentCommandBuffer());
    }

    // Directional Light Shadow end pass
    {
        m_Rhi->PopEvent(m_Rhi->GetCurrentCommandBuffer());

        m_Rhi->CmdEndRenderPassPFN(m_Rhi->GetCurrentCommandBuffer());
    }
}