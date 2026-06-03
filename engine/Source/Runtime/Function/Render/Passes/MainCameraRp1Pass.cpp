#include "Runtime/Function/Render/Passes/MainCameraRp1Pass.h"

#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Function/Render/Interface/Vulkan/VulkanRenderResource.h"
#include "Runtime/Function/Render/RenderCamera.h"
#include "Runtime/Function/Render/RenderMesh.h"
#include "Runtime/Function/Render/MegaLights/MegaLightsSettings.h"
#include "Runtime/Function/Render/RenderResource.h"
#include "Runtime/Function/Render/RenderScene.h"
#include "Runtime/Function/Render/RenderSystem.h"
#include "Runtime/Utility/Utility.h"

#include <array>
#include <cstring>
#include <map>
#include <stdexcept>
#include <unordered_map>
#include <vector>

#if defined(Z_HAS_VULKAN)
    #include <deferred_lighting_frag.h>
    #include <deferred_lighting_vert.h>
    #include <mesh_frag.h>
    #include <mesh_gbuffer_frag.h>
    #include <mesh_vert.h>
#endif

namespace
{
    using namespace MainCameraPassShaderCommon;

    struct FallbackIblOwned
    {
        RHIImage* brdf_image {nullptr};
        RHIDeviceMemory* brdf_mem {nullptr};
        RHIImage* cube_image {nullptr};
        RHIDeviceMemory* cube_mem {nullptr};
    };

    std::unordered_map<MainCameraRp1Pass*, FallbackIblOwned> g_fallback_ibl_owned;

    std::string GetRp1ShaderRoot()
    {
#ifdef ZENGINE_SHADER_ROOT
        return ZENGINE_SHADER_ROOT;
#else
        return "e:/Engine/ZEngine/engine/shader";
#endif
    }

    bool MeshBuffersValid(const VulkanMesh& mesh)
    {
        return mesh.mesh_vertex_position_buffer != nullptr && mesh.mesh_vertex_varying_enable_blending_buffer != nullptr &&
               mesh.mesh_vertex_varying_buffer != nullptr && mesh.mesh_index_buffer != nullptr && mesh.mesh_index_count > 0;
    }

    RHIPipeline* CreateRuntimeMeshPipeline(std::shared_ptr<RHI>& rhi,
                                           MainCameraRp1Pass& pass,
                                           RHIShader* vert_shader_module,
                                           RHIShader* frag_shader_module,
                                           const MeshPipelineKey& pipeline_key,
                                           MainCameraRp1Pass::RenderPipelineType pipeline_type,
                                           RHIRenderPass* rp1_render_pass,
                                           uint32_t subpass,
                                           uint32_t color_attachment_count,
                                           bool transparent_pass)
    {
        RHIPipelineShaderStageCreateInfo vert_stage {};
        vert_stage.sType = RHI_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        vert_stage.stage = RHI_SHADER_STAGE_VERTEX_BIT;
        vert_stage.module = vert_shader_module;
        vert_stage.pName = pipeline_key.vertex_entry.c_str();

        RHIPipelineShaderStageCreateInfo frag_stage {};
        frag_stage.sType = RHI_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        frag_stage.stage = RHI_SHADER_STAGE_FRAGMENT_BIT;
        frag_stage.module = frag_shader_module;
        frag_stage.pName = pipeline_key.fragment_entry.c_str();

        RHIPipelineShaderStageCreateInfo shader_stages[] = {vert_stage, frag_stage};

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

        RHIPipelineViewportStateCreateInfo viewport_state_create_info {};
        viewport_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewport_state_create_info.viewportCount = 1;
        viewport_state_create_info.pViewports = rhi->GetSwapchainInfo().viewport;
        viewport_state_create_info.scissorCount = 1;
        viewport_state_create_info.pScissors = rhi->GetSwapchainInfo().scissor;

        RHIPipelineRasterizationStateCreateInfo rasterization_state_create_info {};
        rasterization_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterization_state_create_info.polygonMode = RHI_POLYGON_MODE_FILL;
        rasterization_state_create_info.cullMode = ParseCullMode(pipeline_key.cull);
        rasterization_state_create_info.frontFace = RHI_FRONT_FACE_COUNTER_CLOCKWISE;

        RHIPipelineMultisampleStateCreateInfo multisample_state_create_info {};
        multisample_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample_state_create_info.rasterizationSamples = RHI_SAMPLE_COUNT_1_BIT;

        std::array<RHIPipelineColorBlendAttachmentState, 3> gbuffer_blend_attachments {};
        std::array<RHIPipelineColorBlendAttachmentState, 1> forward_blend_attachments {};
        RHIPipelineColorBlendStateCreateInfo color_blend_state_create_info {};
        color_blend_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        if (color_attachment_count == 3)
        {
            ApplyBlendMode(pipeline_key.blend, gbuffer_blend_attachments);
            color_blend_state_create_info.attachmentCount = 3;
            color_blend_state_create_info.pAttachments = gbuffer_blend_attachments.data();
        }
        else
        {
            ApplyBlendMode(pipeline_key.blend, forward_blend_attachments);
            color_blend_state_create_info.attachmentCount = 1;
            color_blend_state_create_info.pAttachments = forward_blend_attachments.data();
        }

        RHIPipelineDepthStencilStateCreateInfo depth_stencil_create_info {};
        depth_stencil_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depth_stencil_create_info.depthTestEnable = RHI_TRUE;
        depth_stencil_create_info.depthWriteEnable = pipeline_key.zwrite ? RHI_TRUE : RHI_FALSE;
        depth_stencil_create_info.depthCompareOp = ParseCompareOp(pipeline_key.ztest);
        if (transparent_pass)
        {
            depth_stencil_create_info.depthWriteEnable = pipeline_key.zwrite ? RHI_TRUE : RHI_FALSE;
        }

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
        pipeline_info.layout = pass.m_RenderPipelines[static_cast<size_t>(pipeline_type)].layout;
        pipeline_info.renderPass = rp1_render_pass;
        pipeline_info.subpass = subpass;
        pipeline_info.pDynamicState = &dynamic_state_create_info;

        RHIPipeline* material_pipeline = RHI_NULL_HANDLE;
        if (rhi->CreateGraphicsPipelines(RHI_NULL_HANDLE, 1, &pipeline_info, material_pipeline) != RHI_SUCCESS)
        {
            material_pipeline = pass.m_RenderPipelines[static_cast<size_t>(pipeline_type)].pipeline;
        }
        return material_pipeline;
    }

}  // namespace

bool MainCameraRp1Pass::Initialize()
{
    if (m_Initialized || m_Rhi == nullptr || m_FbResources == nullptr)
    {
        return false;
    }

    // SetCommonInfo only fills RenderPassBase::m_RenderResource; mirror
    // RenderPass::Initialize() so SetupDescriptorSets can use the upload ringbuffer.
    const auto render_resource = std::static_pointer_cast<RenderResource>(m_RenderResource);
    m_GlobalRenderResource = render_resource ? &render_resource->m_GlobalRenderResource : nullptr;
    if (m_GlobalRenderResource == nullptr)
    {
        LOG_ERROR(ZRender,
                  "MainCameraRp1Pass::Initialize: GlobalRenderResource is null "
                  "(call SetCommonInfo with a RenderResource before Initialize)");
        return false;
    }

    m_DescriptorInfos.resize(_layout_type_count);
    m_RenderPipelines.resize(_render_pipeline_type_count);

    SetupDescriptorSetLayouts();
    m_MaterialDescriptorSetLayoutPtr = m_DescriptorInfos[_mesh_per_material].layout;
    SetupPipelines();
    SetupDescriptorSets();
    EnsureFallbackIblTextures();

    m_Initialized = true;
    return true;
}

void MainCameraRp1Pass::Shutdown()
{
    if (!m_Initialized)
    {
        return;
    }

    m_MeshGbufferMaterialPipelines.clear();
    m_MeshTransparentMaterialPipelines.clear();
    m_RenderPipelines.clear();
    m_DescriptorInfos.clear();
    m_MaterialDescriptorSetLayoutPtr = nullptr;

    if (const auto owned_it = g_fallback_ibl_owned.find(this); owned_it != g_fallback_ibl_owned.end())
    {
        FallbackIblOwned& owned = owned_it->second;
        if (owned.brdf_image != nullptr)
        {
            m_Rhi->DestroyImageView(m_FallbackBrdfView);
            m_Rhi->DestroyImage(owned.brdf_image);
            m_Rhi->FreeMemory(owned.brdf_mem);
        }
        if (owned.cube_image != nullptr)
        {
            m_Rhi->DestroyImageView(m_FallbackCubeView);
            m_Rhi->DestroyImage(owned.cube_image);
            m_Rhi->FreeMemory(owned.cube_mem);
        }
        g_fallback_ibl_owned.erase(owned_it);
    }

    m_FallbackSampler = nullptr;
    m_FallbackBrdfView = nullptr;
    m_FallbackCubeView = nullptr;
    m_Initialized = false;
}

void MainCameraRp1Pass::PreparePassData(std::shared_ptr<RenderResourceBase> render_resource)
{
    RenderResource* resource = static_cast<RenderResource*>(render_resource.get());
    if (resource != nullptr)
    {
        m_MeshPerframeStorageBufferObjects = resource->m_MeshPerframeStorageBufferObjects;
        m_MeshPerframeStorageBufferObject = resource->m_MeshPerframeStorageBufferObject;
        m_MegaLightsSystem = &resource->GetMegaLightsSystem();
    }

    m_ActiveMainCameraVisibleMeshNodes = m_VisiableNodes.p_main_camera_visible_mesh_nodes;
}

void MainCameraRp1Pass::EnsureFallbackIblTextures()
{
    if (m_FallbackBrdfView != nullptr || m_GlobalRenderResource == nullptr)
    {
        return;
    }

    const IBLResource& ibl = m_GlobalRenderResource->m_IblResource;
    if (ibl.m_BrdflutTextureImageView != nullptr && ibl.m_IrradianceTextureImageView != nullptr &&
        ibl.m_SpecularTextureImageView != nullptr)
    {
        return;
    }

    m_FallbackSampler = m_Rhi->GetOrCreateDefaultSampler(Default_Sampler_Linear);

    // Neutral gray -- NOT white. A white fallback blows out deferred lighting (HDR -> white screen).
    uint32_t neutral_pixel = 0xFF404040u;
    FallbackIblOwned owned {};

    m_Rhi->CreateGlobalImage(owned.brdf_image,
                             m_FallbackBrdfView,
                             owned.brdf_mem,
                             1,
                             1,
                             static_cast<void*>(&neutral_pixel),
                             RHI_FORMAT_R8G8B8A8_UNORM);

    m_Rhi->CreateImage(1,
                       1,
                       RHI_FORMAT_R8G8B8A8_UNORM,
                       RHI_IMAGE_TILING_OPTIMAL,
                       RHI_IMAGE_USAGE_SAMPLED_BIT | RHI_IMAGE_USAGE_TRANSFER_DST_BIT,
                       RHI_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                       owned.cube_image,
                       owned.cube_mem,
                       0,
                       6,
                       1);
    m_Rhi->CreateImageView(owned.cube_image,
                           RHI_FORMAT_R8G8B8A8_UNORM,
                           RHI_IMAGE_ASPECT_COLOR_BIT,
                           RHI_IMAGE_VIEW_TYPE_CUBE,
                           6,
                           1,
                           m_FallbackCubeView);

    g_fallback_ibl_owned[this] = owned;
}

RHIShader* MainCameraRp1Pass::LoadBuiltinShader(const char* hlsl_relative_path, ShaderStage stage)
{
    const std::string full_path = GetRp1ShaderRoot() + "/hlsl/rp1/" + hlsl_relative_path;
    std::vector<uint8_t> binary;
    return m_Rhi->CreateShaderModuleFromFile(full_path, stage, {}, {}, binary);
}

void MainCameraRp1Pass::SetupDescriptorSetLayouts()
{
    if (m_ExternalPerMeshLayout != nullptr)
    {
        m_DescriptorInfos[_per_mesh].layout = m_ExternalPerMeshLayout;
    }
    else
    {
        RHIDescriptorSetLayoutBinding mesh_mesh_layout_bindings[1] {};
        mesh_mesh_layout_bindings[0].binding = 0;
        mesh_mesh_layout_bindings[0].descriptorType = RHI_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        mesh_mesh_layout_bindings[0].descriptorCount = 1;
        mesh_mesh_layout_bindings[0].stageFlags = RHI_SHADER_STAGE_VERTEX_BIT;

        RHIDescriptorSetLayoutCreateInfo mesh_mesh_layout_create_info {};
        mesh_mesh_layout_create_info.sType = RHI_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        mesh_mesh_layout_create_info.bindingCount = 1;
        mesh_mesh_layout_create_info.pBindings = mesh_mesh_layout_bindings;

        if (m_Rhi->CreateDescriptorSetLayout(&mesh_mesh_layout_create_info, m_DescriptorInfos[_per_mesh].layout) !=
            RHI_SUCCESS)
        {
            throw std::runtime_error("MainCameraRp1Pass: create per-mesh layout");
        }
    }

    {
        RHIDescriptorSetLayoutBinding mesh_global_layout_bindings[13] {};

        mesh_global_layout_bindings[0].binding = 0;
        mesh_global_layout_bindings[0].descriptorType = RHI_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
        mesh_global_layout_bindings[0].descriptorCount = 1;
        mesh_global_layout_bindings[0].stageFlags =
            RHI_SHADER_STAGE_VERTEX_BIT | RHI_SHADER_STAGE_FRAGMENT_BIT;

        mesh_global_layout_bindings[1] = mesh_global_layout_bindings[0];
        mesh_global_layout_bindings[1].binding = 1;
        mesh_global_layout_bindings[1].stageFlags = RHI_SHADER_STAGE_VERTEX_BIT;

        mesh_global_layout_bindings[2] = mesh_global_layout_bindings[1];
        mesh_global_layout_bindings[2].binding = 2;

        mesh_global_layout_bindings[3].binding = 3;
        mesh_global_layout_bindings[3].descriptorType = RHI_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        mesh_global_layout_bindings[3].descriptorCount = 1;
        mesh_global_layout_bindings[3].stageFlags = RHI_SHADER_STAGE_FRAGMENT_BIT;

        mesh_global_layout_bindings[4] = mesh_global_layout_bindings[3];
        mesh_global_layout_bindings[4].binding = 4;
        mesh_global_layout_bindings[5] = mesh_global_layout_bindings[3];
        mesh_global_layout_bindings[5].binding = 5;
        mesh_global_layout_bindings[6] = mesh_global_layout_bindings[3];
        mesh_global_layout_bindings[6].binding = 6;
        mesh_global_layout_bindings[7] = mesh_global_layout_bindings[3];
        mesh_global_layout_bindings[7].binding = 7;

        RHIDescriptorSetLayoutBinding mesh_global_layout_megalights_binding = mesh_global_layout_bindings[1];
        mesh_global_layout_megalights_binding.binding = 8;
        mesh_global_layout_megalights_binding.descriptorCount = 1;
        mesh_global_layout_megalights_binding.stageFlags = RHI_SHADER_STAGE_FRAGMENT_BIT;
        mesh_global_layout_bindings[8] = mesh_global_layout_megalights_binding;
        mesh_global_layout_bindings[9] = mesh_global_layout_megalights_binding;
        mesh_global_layout_bindings[9].binding = 9;
        mesh_global_layout_bindings[10] = mesh_global_layout_megalights_binding;
        mesh_global_layout_bindings[10].binding = 10;

        mesh_global_layout_bindings[11].binding = 11;
        mesh_global_layout_bindings[11].descriptorType = RHI_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        mesh_global_layout_bindings[11].descriptorCount = 1;
        mesh_global_layout_bindings[11].stageFlags = RHI_SHADER_STAGE_FRAGMENT_BIT;

        mesh_global_layout_bindings[12].binding = 12;
        mesh_global_layout_bindings[12].descriptorType = RHI_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        mesh_global_layout_bindings[12].descriptorCount = 1;
        mesh_global_layout_bindings[12].stageFlags = RHI_SHADER_STAGE_FRAGMENT_BIT;

        if (m_Rhi->getGraphicsAPI() == GraphicsAPI::DirectX12)
        {
            // mesh.vert.hlsl / deferred_lighting.frag.hlsl: cbuffer PerFrame at b0,
            // StructuredBuffer<MeshInstance> at t1 (DirectionalLightPass uses the same split).
            mesh_global_layout_bindings[0].descriptorType = RHI_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            mesh_global_layout_bindings[1].descriptorType = RHI_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
            mesh_global_layout_bindings[2].descriptorType = RHI_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
        }

        if (m_Rhi->getGraphicsAPI() == GraphicsAPI::DirectX12)
        {
            mesh_global_layout_bindings[8].descriptorType = RHI_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            mesh_global_layout_bindings[9].descriptorType = RHI_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            mesh_global_layout_bindings[10].descriptorType = RHI_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        }

        RHIDescriptorSetLayoutCreateInfo mesh_global_layout_create_info {};
        mesh_global_layout_create_info.sType = RHI_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        mesh_global_layout_create_info.bindingCount = 13;
        mesh_global_layout_create_info.pBindings = mesh_global_layout_bindings;

        if (m_Rhi->CreateDescriptorSetLayout(&mesh_global_layout_create_info,
                                             m_DescriptorInfos[_mesh_global].layout) != RHI_SUCCESS)
        {
            throw std::runtime_error("MainCameraRp1Pass: create mesh global layout");
        }
    }

    {
        RHIDescriptorSetLayoutBinding mesh_material_layout_bindings[6] {};
        mesh_material_layout_bindings[0].binding = 0;
        mesh_material_layout_bindings[0].descriptorType = RHI_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        mesh_material_layout_bindings[0].descriptorCount = 1;
        mesh_material_layout_bindings[0].stageFlags = RHI_SHADER_STAGE_FRAGMENT_BIT;

        mesh_material_layout_bindings[1].binding = 1;
        mesh_material_layout_bindings[1].descriptorType = RHI_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        mesh_material_layout_bindings[1].descriptorCount = 1;
        mesh_material_layout_bindings[1].stageFlags = RHI_SHADER_STAGE_FRAGMENT_BIT;

        mesh_material_layout_bindings[2] = mesh_material_layout_bindings[1];
        mesh_material_layout_bindings[2].binding = 2;
        mesh_material_layout_bindings[3] = mesh_material_layout_bindings[1];
        mesh_material_layout_bindings[3].binding = 3;
        mesh_material_layout_bindings[4] = mesh_material_layout_bindings[1];
        mesh_material_layout_bindings[4].binding = 4;
        mesh_material_layout_bindings[5] = mesh_material_layout_bindings[1];
        mesh_material_layout_bindings[5].binding = 5;

        RHIDescriptorSetLayoutCreateInfo mesh_material_layout_create_info {};
        mesh_material_layout_create_info.sType = RHI_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        mesh_material_layout_create_info.bindingCount = 6;
        mesh_material_layout_create_info.pBindings = mesh_material_layout_bindings;

        if (m_Rhi->CreateDescriptorSetLayout(&mesh_material_layout_create_info,
                                             m_DescriptorInfos[_mesh_per_material].layout) != RHI_SUCCESS)
        {
            throw std::runtime_error("MainCameraRp1Pass: create mesh material layout");
        }
    }

    {
        RHIDescriptorSetLayoutBinding gbuffer_lighting_global_layout_bindings[4] {};
        for (uint32_t binding_index = 0; binding_index < 4; ++binding_index)
        {
            gbuffer_lighting_global_layout_bindings[binding_index].binding = binding_index;
            gbuffer_lighting_global_layout_bindings[binding_index].descriptorType = RHI_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
            gbuffer_lighting_global_layout_bindings[binding_index].descriptorCount = 1;
            gbuffer_lighting_global_layout_bindings[binding_index].stageFlags = RHI_SHADER_STAGE_FRAGMENT_BIT;
        }

        RHIDescriptorSetLayoutCreateInfo gbuffer_lighting_global_layout_create_info {};
        gbuffer_lighting_global_layout_create_info.sType = RHI_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        gbuffer_lighting_global_layout_create_info.bindingCount = 4;
        gbuffer_lighting_global_layout_create_info.pBindings = gbuffer_lighting_global_layout_bindings;

        if (m_Rhi->CreateDescriptorSetLayout(&gbuffer_lighting_global_layout_create_info,
                                             m_DescriptorInfos[_deferred_lighting].layout) != RHI_SUCCESS)
        {
            throw std::runtime_error("MainCameraRp1Pass: create deferred lighting layout");
        }
    }
}

namespace
{
    RHIShader* LoadRp1ShaderFromFile(std::shared_ptr<RHI>& rhi, const char* hlsl_relative_path, ShaderStage stage)
    {
        const std::string full_path = GetRp1ShaderRoot() + "/hlsl/rp1/" + hlsl_relative_path;
        const std::vector<std::string> include_paths = {GetRp1ShaderRoot() + "/hlsl/rp1",
                                                        GetRp1ShaderRoot() + "/hlsl"};
        std::vector<uint8_t> binary;
        return rhi->CreateShaderModuleFromFile(full_path, stage, include_paths, {}, binary);
    }

    RHIShader* CreateBuiltinVertShader(std::shared_ptr<RHI>& rhi)
    {
        if (rhi->getGraphicsAPI() == GraphicsAPI::DirectX12)
        {
            return LoadRp1ShaderFromFile(rhi, "mesh.vert.hlsl", ShaderStage::Vertex);
        }
#if defined(Z_HAS_VULKAN)
        return rhi->CreateShaderModule(MESH_VERT);
#else
        return nullptr;
#endif
    }

    RHIShader* CreateBuiltinFragShader(std::shared_ptr<RHI>& rhi, const char* dx12_hlsl_path)
    {
        if (rhi->getGraphicsAPI() == GraphicsAPI::DirectX12)
        {
            return LoadRp1ShaderFromFile(rhi, dx12_hlsl_path, ShaderStage::Fragment);
        }
#if defined(Z_HAS_VULKAN)
        if (std::strcmp(dx12_hlsl_path, "mesh_gbuffer.frag.hlsl") == 0)
        {
            return rhi->CreateShaderModule(MESH_GBUFFER_FRAG);
        }
        if (std::strcmp(dx12_hlsl_path, "mesh_forward.frag.hlsl") == 0)
        {
            return rhi->CreateShaderModule(MESH_FRAG);
        }
        if (std::strcmp(dx12_hlsl_path, "deferred_lighting.frag.hlsl") == 0)
        {
            return rhi->CreateShaderModule(DEFERRED_LIGHTING_FRAG);
        }
#endif
        return nullptr;
    }

    RHIShader* CreateBuiltinDeferredVertShader(std::shared_ptr<RHI>& rhi)
    {
        if (rhi->getGraphicsAPI() == GraphicsAPI::DirectX12)
        {
            return LoadRp1ShaderFromFile(rhi, "deferred_lighting.vert.hlsl", ShaderStage::Vertex);
        }
#if defined(Z_HAS_VULKAN)
        return rhi->CreateShaderModule(DEFERRED_LIGHTING_VERT);
#else
        return nullptr;
#endif
    }

}  // namespace

void MainCameraRp1Pass::SetupPipelines()
{
    RHIRenderPass* rp1_render_pass = m_FbResources->getRP1RenderPass();

    auto create_mesh_pipeline = [&](RenderPipelineType pipeline_type,
                                    const char* frag_hlsl_path,
                                    uint32_t subpass,
                                    uint32_t color_attachment_count,
                                    bool transparent_pass,
                                    RHICullModeFlags cull_mode) {
        RHIDescriptorSetLayout* descriptorset_layouts[3] = {m_DescriptorInfos[_mesh_global].layout,
                                                            m_DescriptorInfos[_per_mesh].layout,
                                                            m_DescriptorInfos[_mesh_per_material].layout};
        RHIPipelineLayoutCreateInfo pipeline_layout_create_info {};
        pipeline_layout_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipeline_layout_create_info.setLayoutCount = 3;
        pipeline_layout_create_info.pSetLayouts = descriptorset_layouts;

        if (m_Rhi->CreatePipelineLayout(&pipeline_layout_create_info, m_RenderPipelines[pipeline_type].layout) !=
            RHI_SUCCESS)
        {
            throw std::runtime_error("MainCameraRp1Pass: create mesh pipeline layout");
        }

        RHIShader* vert_shader_module = CreateBuiltinVertShader(m_Rhi);
        RHIShader* frag_shader_module = CreateBuiltinFragShader(m_Rhi, frag_hlsl_path);
        if (vert_shader_module == nullptr || frag_shader_module == nullptr)
        {
            throw std::runtime_error("MainCameraRp1Pass: failed to load builtin mesh shaders");
        }

        RHIPipelineShaderStageCreateInfo vert_stage {};
        vert_stage.sType = RHI_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        vert_stage.stage = RHI_SHADER_STAGE_VERTEX_BIT;
        vert_stage.module = vert_shader_module;
        vert_stage.pName = "main";

        RHIPipelineShaderStageCreateInfo frag_stage {};
        frag_stage.sType = RHI_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        frag_stage.stage = RHI_SHADER_STAGE_FRAGMENT_BIT;
        frag_stage.module = frag_shader_module;
        frag_stage.pName = "main";

        RHIPipelineShaderStageCreateInfo shader_stages[] = {vert_stage, frag_stage};

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

        RHIPipelineViewportStateCreateInfo viewport_state_create_info {};
        viewport_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewport_state_create_info.viewportCount = 1;
        viewport_state_create_info.pViewports = m_Rhi->GetSwapchainInfo().viewport;
        viewport_state_create_info.scissorCount = 1;
        viewport_state_create_info.pScissors = m_Rhi->GetSwapchainInfo().scissor;

        RHIPipelineRasterizationStateCreateInfo rasterization_state_create_info {};
        rasterization_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterization_state_create_info.polygonMode = RHI_POLYGON_MODE_FILL;
        rasterization_state_create_info.cullMode = cull_mode;
        rasterization_state_create_info.frontFace = RHI_FRONT_FACE_COUNTER_CLOCKWISE;

        RHIPipelineMultisampleStateCreateInfo multisample_state_create_info {};
        multisample_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample_state_create_info.rasterizationSamples = RHI_SAMPLE_COUNT_1_BIT;

        RHIPipelineDepthStencilStateCreateInfo depth_stencil_create_info {};
        depth_stencil_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depth_stencil_create_info.depthTestEnable = RHI_TRUE;
        depth_stencil_create_info.depthWriteEnable = transparent_pass ? RHI_FALSE : RHI_TRUE;
        depth_stencil_create_info.depthCompareOp =
            transparent_pass ? RHI_COMPARE_OP_LESS_OR_EQUAL : RHI_COMPARE_OP_LESS;

        RHIPipelineColorBlendStateCreateInfo color_blend_state_create_info {};
        color_blend_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;

        std::array<RHIPipelineColorBlendAttachmentState, 3> gbuffer_blend_attachments {};
        std::array<RHIPipelineColorBlendAttachmentState, 1> forward_blend_attachments {};
        if (color_attachment_count == 3)
        {
            color_blend_state_create_info.attachmentCount = 3;
            color_blend_state_create_info.pAttachments = gbuffer_blend_attachments.data();
        }
        else
        {
            ApplyBlendMode("Transparent", forward_blend_attachments);
            color_blend_state_create_info.attachmentCount = 1;
            color_blend_state_create_info.pAttachments = forward_blend_attachments.data();
        }

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
        pipeline_info.layout = m_RenderPipelines[pipeline_type].layout;
        pipeline_info.renderPass = rp1_render_pass;
        pipeline_info.subpass = subpass;
        pipeline_info.pDynamicState = &dynamic_state_create_info;

        if (m_Rhi->CreateGraphicsPipelines(RHI_NULL_HANDLE, 1, &pipeline_info, m_RenderPipelines[pipeline_type].pipeline) !=
            RHI_SUCCESS)
        {
            throw std::runtime_error("MainCameraRp1Pass: create mesh graphics pipeline");
        }

        m_Rhi->DestroyShaderModule(vert_shader_module);
        m_Rhi->DestroyShaderModule(frag_shader_module);
    };

    create_mesh_pipeline(_render_pipeline_type_mesh_gbuffer,
                         "mesh_gbuffer.frag.hlsl",
                         _main_camera_subpass_basepass,
                         3,
                         false,
                         RHI_CULL_MODE_BACK_BIT);
    create_mesh_pipeline(_render_pipeline_type_mesh_gbuffer_nocull,
                         "mesh_gbuffer.frag.hlsl",
                         _main_camera_subpass_basepass,
                         3,
                         false,
                         RHI_CULL_MODE_NONE);
    create_mesh_pipeline(_render_pipeline_type_mesh_transparent,
                         "mesh_forward.frag.hlsl",
                         _main_camera_subpass_forward_lighting,
                         1,
                         true,
                         RHI_CULL_MODE_BACK_BIT);

    {
        RHIDescriptorSetLayout* descriptorset_layouts[2] = {m_DescriptorInfos[_mesh_global].layout,
                                                            m_DescriptorInfos[_deferred_lighting].layout};
        RHIPipelineLayoutCreateInfo pipeline_layout_create_info {};
        pipeline_layout_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipeline_layout_create_info.setLayoutCount = 2;
        pipeline_layout_create_info.pSetLayouts = descriptorset_layouts;

        if (m_Rhi->CreatePipelineLayout(&pipeline_layout_create_info,
                                        m_RenderPipelines[_render_pipeline_type_deferred_lighting].layout) !=
            RHI_SUCCESS)
        {
            throw std::runtime_error("MainCameraRp1Pass: create deferred lighting pipeline layout");
        }

        RHIShader* vert_shader_module = CreateBuiltinDeferredVertShader(m_Rhi);
        RHIShader* frag_shader_module = CreateBuiltinFragShader(m_Rhi, "deferred_lighting.frag.hlsl");
        if (vert_shader_module == nullptr || frag_shader_module == nullptr)
        {
            throw std::runtime_error("MainCameraRp1Pass: failed to load deferred lighting shaders");
        }

        RHIPipelineShaderStageCreateInfo vert_stage {};
        vert_stage.sType = RHI_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        vert_stage.stage = RHI_SHADER_STAGE_VERTEX_BIT;
        vert_stage.module = vert_shader_module;
        vert_stage.pName = "main";

        RHIPipelineShaderStageCreateInfo frag_stage {};
        frag_stage.sType = RHI_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        frag_stage.stage = RHI_SHADER_STAGE_FRAGMENT_BIT;
        frag_stage.module = frag_shader_module;
        frag_stage.pName = "main";

        RHIPipelineShaderStageCreateInfo shader_stages[] = {vert_stage, frag_stage};

        RHIPipelineVertexInputStateCreateInfo vertex_input_state_create_info {};
        vertex_input_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

        RHIPipelineInputAssemblyStateCreateInfo input_assembly_create_info {};
        input_assembly_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        input_assembly_create_info.topology = RHI_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        RHIPipelineViewportStateCreateInfo viewport_state_create_info {};
        viewport_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewport_state_create_info.viewportCount = 1;
        viewport_state_create_info.pViewports = m_Rhi->GetSwapchainInfo().viewport;
        viewport_state_create_info.scissorCount = 1;
        viewport_state_create_info.pScissors = m_Rhi->GetSwapchainInfo().scissor;

        RHIPipelineRasterizationStateCreateInfo rasterization_state_create_info {};
        rasterization_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterization_state_create_info.polygonMode = RHI_POLYGON_MODE_FILL;
        rasterization_state_create_info.cullMode = RHI_CULL_MODE_BACK_BIT;
        rasterization_state_create_info.frontFace = RHI_FRONT_FACE_CLOCKWISE;

        RHIPipelineMultisampleStateCreateInfo multisample_state_create_info {};
        multisample_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample_state_create_info.rasterizationSamples = RHI_SAMPLE_COUNT_1_BIT;

        RHIPipelineColorBlendAttachmentState color_blend_attachment {};
        color_blend_attachment.colorWriteMask = RHI_COLOR_COMPONENT_R_BIT | RHI_COLOR_COMPONENT_G_BIT |
                                                RHI_COLOR_COMPONENT_B_BIT | RHI_COLOR_COMPONENT_A_BIT;

        RHIPipelineColorBlendStateCreateInfo color_blend_state_create_info {};
        color_blend_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        color_blend_state_create_info.attachmentCount = 1;
        color_blend_state_create_info.pAttachments = &color_blend_attachment;

        RHIPipelineDepthStencilStateCreateInfo depth_stencil_create_info {};
        depth_stencil_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;

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
        pipeline_info.layout = m_RenderPipelines[_render_pipeline_type_deferred_lighting].layout;
        pipeline_info.renderPass = rp1_render_pass;
        pipeline_info.subpass = _main_camera_subpass_deferred_lighting;
        pipeline_info.pDynamicState = &dynamic_state_create_info;

        if (m_Rhi->CreateGraphicsPipelines(RHI_NULL_HANDLE,
                                           1,
                                           &pipeline_info,
                                           m_RenderPipelines[_render_pipeline_type_deferred_lighting].pipeline) !=
            RHI_SUCCESS)
        {
            throw std::runtime_error("MainCameraRp1Pass: create deferred lighting pipeline");
        }

        m_Rhi->DestroyShaderModule(vert_shader_module);
        m_Rhi->DestroyShaderModule(frag_shader_module);
    }

    if (m_Rhi->getGraphicsAPI() == GraphicsAPI::DirectX12)
    {
        RHIDescriptorSetLayout* descriptorset_layouts[2] = {m_DescriptorInfos[_mesh_global].layout,
                                                            m_DescriptorInfos[_deferred_lighting].layout};
        RHIPipelineLayoutCreateInfo pipeline_layout_create_info {};
        pipeline_layout_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipeline_layout_create_info.setLayoutCount = 2;
        pipeline_layout_create_info.pSetLayouts = descriptorset_layouts;

        if (m_Rhi->CreatePipelineLayout(&pipeline_layout_create_info,
                                        m_RenderPipelines[_render_pipeline_type_megalights_deferred].layout) !=
            RHI_SUCCESS)
        {
            throw std::runtime_error("MainCameraRp1Pass: create megalights deferred pipeline layout");
        }

        RHIShader* vert_shader_module = CreateBuiltinDeferredVertShader(m_Rhi);
        RHIShader* frag_shader_module = LoadRp1ShaderFromFile(m_Rhi, "megalights_deferred.frag.hlsl", ShaderStage::Fragment);
        if (vert_shader_module == nullptr || frag_shader_module == nullptr)
        {
            throw std::runtime_error("MainCameraRp1Pass: failed to load megalights deferred shaders");
        }

        RHIPipelineShaderStageCreateInfo vert_stage {};
        vert_stage.sType = RHI_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        vert_stage.stage = RHI_SHADER_STAGE_VERTEX_BIT;
        vert_stage.module = vert_shader_module;
        vert_stage.pName = "main";

        RHIPipelineShaderStageCreateInfo frag_stage {};
        frag_stage.sType = RHI_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        frag_stage.stage = RHI_SHADER_STAGE_FRAGMENT_BIT;
        frag_stage.module = frag_shader_module;
        frag_stage.pName = "main";

        RHIPipelineShaderStageCreateInfo shader_stages[] = {vert_stage, frag_stage};

        RHIPipelineVertexInputStateCreateInfo vertex_input_state_create_info {};
        vertex_input_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

        RHIPipelineInputAssemblyStateCreateInfo input_assembly_create_info {};
        input_assembly_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        input_assembly_create_info.topology = RHI_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        RHIPipelineViewportStateCreateInfo viewport_state_create_info {};
        viewport_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewport_state_create_info.viewportCount = 1;
        viewport_state_create_info.pViewports = m_Rhi->GetSwapchainInfo().viewport;
        viewport_state_create_info.scissorCount = 1;
        viewport_state_create_info.pScissors = m_Rhi->GetSwapchainInfo().scissor;

        RHIPipelineRasterizationStateCreateInfo rasterization_state_create_info {};
        rasterization_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterization_state_create_info.polygonMode = RHI_POLYGON_MODE_FILL;
        rasterization_state_create_info.cullMode = RHI_CULL_MODE_BACK_BIT;
        rasterization_state_create_info.frontFace = RHI_FRONT_FACE_CLOCKWISE;

        RHIPipelineMultisampleStateCreateInfo multisample_state_create_info {};
        multisample_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample_state_create_info.rasterizationSamples = RHI_SAMPLE_COUNT_1_BIT;

        RHIPipelineColorBlendAttachmentState color_blend_attachment {};
        color_blend_attachment.colorWriteMask = RHI_COLOR_COMPONENT_R_BIT | RHI_COLOR_COMPONENT_G_BIT |
                                                RHI_COLOR_COMPONENT_B_BIT | RHI_COLOR_COMPONENT_A_BIT;

        RHIPipelineColorBlendStateCreateInfo color_blend_state_create_info {};
        color_blend_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        color_blend_state_create_info.attachmentCount = 1;
        color_blend_state_create_info.pAttachments = &color_blend_attachment;

        RHIPipelineDepthStencilStateCreateInfo depth_stencil_create_info {};
        depth_stencil_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;

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
        pipeline_info.layout = m_RenderPipelines[_render_pipeline_type_megalights_deferred].layout;
        pipeline_info.renderPass = rp1_render_pass;
        pipeline_info.subpass = _main_camera_subpass_deferred_lighting;
        pipeline_info.pDynamicState = &dynamic_state_create_info;

        if (m_Rhi->CreateGraphicsPipelines(RHI_NULL_HANDLE,
                                           1,
                                           &pipeline_info,
                                           m_RenderPipelines[_render_pipeline_type_megalights_deferred].pipeline) !=
            RHI_SUCCESS)
        {
            throw std::runtime_error("MainCameraRp1Pass: create megalights deferred pipeline");
        }

        m_Rhi->DestroyShaderModule(vert_shader_module);
        m_Rhi->DestroyShaderModule(frag_shader_module);
    }

    if (m_Rhi->getGraphicsAPI() == GraphicsAPI::DirectX12)
    {
        RHIDescriptorSetLayout* descriptorset_layouts[2] = {m_DescriptorInfos[_mesh_global].layout,
                                                            m_DescriptorInfos[_deferred_lighting].layout};
        RHIPipelineLayoutCreateInfo pipeline_layout_create_info {};
        pipeline_layout_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipeline_layout_create_info.setLayoutCount = 2;
        pipeline_layout_create_info.pSetLayouts = descriptorset_layouts;

        if (m_Rhi->CreatePipelineLayout(&pipeline_layout_create_info,
                                        m_RenderPipelines[_render_pipeline_type_megalights_spatial].layout) !=
            RHI_SUCCESS)
        {
            throw std::runtime_error("MainCameraRp1Pass: create megalights spatial pipeline layout");
        }

        RHIShader* vert_shader_module = CreateBuiltinDeferredVertShader(m_Rhi);
        RHIShader* frag_shader_module = LoadRp1ShaderFromFile(m_Rhi, "megalights_spatial.frag.hlsl", ShaderStage::Fragment);
        if (vert_shader_module == nullptr || frag_shader_module == nullptr)
        {
            throw std::runtime_error("MainCameraRp1Pass: failed to load megalights spatial shaders");
        }

        RHIPipelineShaderStageCreateInfo vert_stage {};
        vert_stage.sType = RHI_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        vert_stage.stage = RHI_SHADER_STAGE_VERTEX_BIT;
        vert_stage.module = vert_shader_module;
        vert_stage.pName = "main";

        RHIPipelineShaderStageCreateInfo frag_stage {};
        frag_stage.sType = RHI_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        frag_stage.stage = RHI_SHADER_STAGE_FRAGMENT_BIT;
        frag_stage.module = frag_shader_module;
        frag_stage.pName = "main";

        RHIPipelineShaderStageCreateInfo shader_stages[] = {vert_stage, frag_stage};

        RHIPipelineVertexInputStateCreateInfo vertex_input_state_create_info {};
        vertex_input_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

        RHIPipelineInputAssemblyStateCreateInfo input_assembly_create_info {};
        input_assembly_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        input_assembly_create_info.topology = RHI_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        RHIPipelineViewportStateCreateInfo viewport_state_create_info {};
        viewport_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewport_state_create_info.viewportCount = 1;
        viewport_state_create_info.pViewports = m_Rhi->GetSwapchainInfo().viewport;
        viewport_state_create_info.scissorCount = 1;
        viewport_state_create_info.pScissors = m_Rhi->GetSwapchainInfo().scissor;

        RHIPipelineRasterizationStateCreateInfo rasterization_state_create_info {};
        rasterization_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterization_state_create_info.polygonMode = RHI_POLYGON_MODE_FILL;
        rasterization_state_create_info.cullMode = RHI_CULL_MODE_BACK_BIT;
        rasterization_state_create_info.frontFace = RHI_FRONT_FACE_CLOCKWISE;

        RHIPipelineMultisampleStateCreateInfo multisample_state_create_info {};
        multisample_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample_state_create_info.rasterizationSamples = RHI_SAMPLE_COUNT_1_BIT;

        RHIPipelineColorBlendAttachmentState color_blend_attachment {};
        color_blend_attachment.colorWriteMask = RHI_COLOR_COMPONENT_R_BIT | RHI_COLOR_COMPONENT_G_BIT |
                                                RHI_COLOR_COMPONENT_B_BIT | RHI_COLOR_COMPONENT_A_BIT;

        RHIPipelineColorBlendStateCreateInfo color_blend_state_create_info {};
        color_blend_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
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
        pipeline_info.layout = m_RenderPipelines[_render_pipeline_type_megalights_spatial].layout;
        pipeline_info.renderPass = rp1_render_pass;
        pipeline_info.subpass = _main_camera_subpass_deferred_lighting;
        pipeline_info.pDynamicState = &dynamic_state_create_info;

        if (m_Rhi->CreateGraphicsPipelines(RHI_NULL_HANDLE,
                                           1,
                                           &pipeline_info,
                                           m_RenderPipelines[_render_pipeline_type_megalights_spatial].pipeline) !=
            RHI_SUCCESS)
        {
            throw std::runtime_error("MainCameraRp1Pass: create megalights spatial pipeline");
        }

        m_Rhi->DestroyShaderModule(vert_shader_module);
        m_Rhi->DestroyShaderModule(frag_shader_module);
    }
}

void MainCameraRp1Pass::SetupDescriptorSets()
{
    RHIDescriptorSetAllocateInfo mesh_global_descriptor_set_alloc_info {};
    mesh_global_descriptor_set_alloc_info.sType = RHI_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    mesh_global_descriptor_set_alloc_info.descriptorPool = m_Rhi->GetDescriptorPoor();
    mesh_global_descriptor_set_alloc_info.descriptorSetCount = 1;
    mesh_global_descriptor_set_alloc_info.pSetLayouts = &m_DescriptorInfos[_mesh_global].layout;

    if (m_Rhi->AllocateDescriptorSets(&mesh_global_descriptor_set_alloc_info,
                                      m_DescriptorInfos[_mesh_global].descriptor_set) != RHI_SUCCESS)
    {
        throw std::runtime_error("MainCameraRp1Pass: allocate mesh global descriptor set");
    }
    RHIDescriptorBufferInfo mesh_perframe_storage_buffer_info {};
    mesh_perframe_storage_buffer_info.offset = 0;
    mesh_perframe_storage_buffer_info.range = sizeof(MeshPerframeStorageBufferObject);
    mesh_perframe_storage_buffer_info.buffer = m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbuffer;

    RHIDescriptorBufferInfo mesh_perdrawcall_storage_buffer_info {};
    mesh_perdrawcall_storage_buffer_info.offset = 0;
    mesh_perdrawcall_storage_buffer_info.range = sizeof(MeshPerdrawcallStorageBufferObject);
    mesh_perdrawcall_storage_buffer_info.buffer = m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbuffer;

    RHIDescriptorBufferInfo mesh_per_drawcall_vertex_blending_storage_buffer_info {};
    mesh_per_drawcall_vertex_blending_storage_buffer_info.offset = 0;
    mesh_per_drawcall_vertex_blending_storage_buffer_info.range =
        sizeof(MeshPerdrawcallVertexBlendingStorageBufferObject);
    mesh_per_drawcall_vertex_blending_storage_buffer_info.buffer =
        m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbuffer;

    const IBLResource& ibl = m_GlobalRenderResource->m_IblResource;

    RHIDescriptorImageInfo brdf_texture_image_info {};
    brdf_texture_image_info.sampler = ibl.m_BrdflutTextureSampler != nullptr ? ibl.m_BrdflutTextureSampler : m_FallbackSampler;
    brdf_texture_image_info.imageView = ibl.m_BrdflutTextureImageView != nullptr ? ibl.m_BrdflutTextureImageView : m_FallbackBrdfView;
    brdf_texture_image_info.imageLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    RHIDescriptorImageInfo irradiance_texture_image_info {};
    irradiance_texture_image_info.sampler = ibl.m_IrradianceTextureSampler != nullptr ? ibl.m_IrradianceTextureSampler : m_FallbackSampler;
    irradiance_texture_image_info.imageView = ibl.m_IrradianceTextureImageView != nullptr ? ibl.m_IrradianceTextureImageView : m_FallbackCubeView;
    irradiance_texture_image_info.imageLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    RHIDescriptorImageInfo specular_texture_image_info {};
    specular_texture_image_info.sampler = ibl.m_SpecularTextureSampler != nullptr ? ibl.m_SpecularTextureSampler : m_FallbackSampler;
    specular_texture_image_info.imageView = ibl.m_SpecularTextureImageView != nullptr ? ibl.m_SpecularTextureImageView : m_FallbackCubeView;
    specular_texture_image_info.imageLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    RHIDescriptorImageInfo point_light_shadow_texture_image_info {};
    point_light_shadow_texture_image_info.sampler = m_Rhi->GetOrCreateDefaultSampler(Default_Sampler_Nearest);
    point_light_shadow_texture_image_info.imageView = m_PointLightShadowColorImageView;
    point_light_shadow_texture_image_info.imageLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    RHIDescriptorImageInfo directional_light_shadow_texture_image_info {};
    directional_light_shadow_texture_image_info.sampler = m_Rhi->GetOrCreateDefaultSampler(Default_Sampler_Nearest);
    directional_light_shadow_texture_image_info.imageView = m_DirectionalLightShadowColorImageView;
    directional_light_shadow_texture_image_info.imageLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    const bool is_dx12 = m_Rhi->getGraphicsAPI() == GraphicsAPI::DirectX12;

    RHIWriteDescriptorSet mesh_descriptor_writes_info[13] {};
    for (uint32_t write_index = 0; write_index < 3; ++write_index)
    {
        mesh_descriptor_writes_info[write_index].sType = RHI_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        mesh_descriptor_writes_info[write_index].dstSet = m_DescriptorInfos[_mesh_global].descriptor_set;
        mesh_descriptor_writes_info[write_index].dstBinding = write_index;
        mesh_descriptor_writes_info[write_index].descriptorType =
            is_dx12 ? (write_index == 0 ? RHI_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : RHI_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC) : RHI_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
        mesh_descriptor_writes_info[write_index].descriptorCount = 1;
    }
    mesh_descriptor_writes_info[0].pBufferInfo = &mesh_perframe_storage_buffer_info;
    mesh_descriptor_writes_info[1].pBufferInfo = &mesh_perdrawcall_storage_buffer_info;
    mesh_descriptor_writes_info[2].pBufferInfo = &mesh_per_drawcall_vertex_blending_storage_buffer_info;

    for (uint32_t write_index = 3; write_index < 8; ++write_index)
    {
        mesh_descriptor_writes_info[write_index].sType = RHI_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        mesh_descriptor_writes_info[write_index].dstSet = m_DescriptorInfos[_mesh_global].descriptor_set;
        mesh_descriptor_writes_info[write_index].dstBinding = write_index;
        mesh_descriptor_writes_info[write_index].descriptorType = RHI_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        mesh_descriptor_writes_info[write_index].descriptorCount = 1;
    }
    mesh_descriptor_writes_info[3].pImageInfo = &brdf_texture_image_info;
    mesh_descriptor_writes_info[4].pImageInfo = &irradiance_texture_image_info;
    mesh_descriptor_writes_info[5].pImageInfo = &specular_texture_image_info;
    mesh_descriptor_writes_info[6].pImageInfo = &point_light_shadow_texture_image_info;
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

    for (uint32_t write_index = 8; write_index < 11; ++write_index)
    {
        mesh_descriptor_writes_info[write_index].sType = RHI_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        mesh_descriptor_writes_info[write_index].dstSet = m_DescriptorInfos[_mesh_global].descriptor_set;
        mesh_descriptor_writes_info[write_index].dstBinding = write_index;
        mesh_descriptor_writes_info[write_index].descriptorType = RHI_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        mesh_descriptor_writes_info[write_index].descriptorCount = 1;
    }
    mesh_descriptor_writes_info[8].pBufferInfo = &megalights_lights_buffer_info;
    mesh_descriptor_writes_info[9].pBufferInfo = &megalights_indices_buffer_info;
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

    m_Rhi->UpdateDescriptorSets(13, mesh_descriptor_writes_info, 0, nullptr);

    RHIDescriptorSetAllocateInfo gbuffer_light_global_descriptor_set_alloc_info {};
    gbuffer_light_global_descriptor_set_alloc_info.sType = RHI_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    gbuffer_light_global_descriptor_set_alloc_info.descriptorPool = m_Rhi->GetDescriptorPoor();
    gbuffer_light_global_descriptor_set_alloc_info.descriptorSetCount = 1;
    gbuffer_light_global_descriptor_set_alloc_info.pSetLayouts = &m_DescriptorInfos[_deferred_lighting].layout;

    if (m_Rhi->AllocateDescriptorSets(&gbuffer_light_global_descriptor_set_alloc_info,
                                      m_DescriptorInfos[_deferred_lighting].descriptor_set) != RHI_SUCCESS)
    {
        throw std::runtime_error("MainCameraRp1Pass: allocate deferred lighting descriptor set");
    }

    RefreshDeferredLightingInputAttachments();
}

void MainCameraRp1Pass::RefreshDeferredLightingInputAttachments()
{
    // Rebinds the G-buffer (+ depth) input-attachment descriptors of the deferred-lighting set.
    // These views are owned by the MainCamera framebuffer / the RHI depth target, both of which
    // are destroyed and recreated on a window resize. Without re-running these writes after a
    // recreate, the set keeps pointing at freed views and the lit scene renders blank (white).
    if (m_Rhi == nullptr || m_FbResources == nullptr ||
        m_DescriptorInfos.size() <= _deferred_lighting ||
        m_DescriptorInfos[_deferred_lighting].descriptor_set == nullptr)
    {
        return;
    }

    const auto& attachments = m_FbResources->getAttachments();

    RHIDescriptorImageInfo gbuffer_normal_input_attachment_info {};
    gbuffer_normal_input_attachment_info.sampler = m_Rhi->GetOrCreateDefaultSampler(Default_Sampler_Nearest);
    gbuffer_normal_input_attachment_info.imageView = attachments[_main_camera_pass_gbuffer_a].view;
    gbuffer_normal_input_attachment_info.imageLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    RHIDescriptorImageInfo gbuffer_metallic_roughness_shadingmodeid_input_attachment_info {};
    gbuffer_metallic_roughness_shadingmodeid_input_attachment_info.sampler =
        m_Rhi->GetOrCreateDefaultSampler(Default_Sampler_Nearest);
    gbuffer_metallic_roughness_shadingmodeid_input_attachment_info.imageView =
        attachments[_main_camera_pass_gbuffer_b].view;
    gbuffer_metallic_roughness_shadingmodeid_input_attachment_info.imageLayout =
        RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    RHIDescriptorImageInfo gbuffer_albedo_input_attachment_info {};
    gbuffer_albedo_input_attachment_info.sampler = m_Rhi->GetOrCreateDefaultSampler(Default_Sampler_Nearest);
    gbuffer_albedo_input_attachment_info.imageView = attachments[_main_camera_pass_gbuffer_c].view;
    gbuffer_albedo_input_attachment_info.imageLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    RHIDescriptorImageInfo depth_input_attachment_info {};
    depth_input_attachment_info.sampler = m_Rhi->GetOrCreateDefaultSampler(Default_Sampler_Nearest);
    depth_input_attachment_info.imageView = m_Rhi->GetDepthImageInfo().depth_image_view;
    depth_input_attachment_info.imageLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    RHIWriteDescriptorSet deferred_lighting_descriptor_writes_info[4] {};
    deferred_lighting_descriptor_writes_info[0].sType = RHI_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    deferred_lighting_descriptor_writes_info[0].dstSet = m_DescriptorInfos[_deferred_lighting].descriptor_set;
    deferred_lighting_descriptor_writes_info[0].dstBinding = 0;
    deferred_lighting_descriptor_writes_info[0].descriptorType = RHI_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
    deferred_lighting_descriptor_writes_info[0].descriptorCount = 1;
    deferred_lighting_descriptor_writes_info[0].pImageInfo = &gbuffer_normal_input_attachment_info;

    deferred_lighting_descriptor_writes_info[1] = deferred_lighting_descriptor_writes_info[0];
    deferred_lighting_descriptor_writes_info[1].dstBinding = 1;
    deferred_lighting_descriptor_writes_info[1].pImageInfo =
        &gbuffer_metallic_roughness_shadingmodeid_input_attachment_info;

    deferred_lighting_descriptor_writes_info[2] = deferred_lighting_descriptor_writes_info[0];
    deferred_lighting_descriptor_writes_info[2].dstBinding = 2;
    deferred_lighting_descriptor_writes_info[2].pImageInfo = &gbuffer_albedo_input_attachment_info;

    deferred_lighting_descriptor_writes_info[3] = deferred_lighting_descriptor_writes_info[0];
    deferred_lighting_descriptor_writes_info[3].dstBinding = 3;
    deferred_lighting_descriptor_writes_info[3].pImageInfo = &depth_input_attachment_info;

    m_Rhi->UpdateDescriptorSets(4, deferred_lighting_descriptor_writes_info, 0, nullptr);
}

void MainCameraRp1Pass::RefreshMeshGlobalIblDescriptors()
{
    if (!m_Initialized || m_Rhi == nullptr || m_GlobalRenderResource == nullptr ||
        m_DescriptorInfos.size() <= _mesh_global ||
        m_DescriptorInfos[_mesh_global].descriptor_set == nullptr)
    {
        return;
    }

    const IBLResource& ibl = m_GlobalRenderResource->m_IblResource;

    RHIDescriptorImageInfo brdf_texture_image_info {};
    brdf_texture_image_info.sampler = ibl.m_BrdflutTextureSampler != nullptr ? ibl.m_BrdflutTextureSampler : m_FallbackSampler;
    brdf_texture_image_info.imageView = ibl.m_BrdflutTextureImageView != nullptr ? ibl.m_BrdflutTextureImageView : m_FallbackBrdfView;
    brdf_texture_image_info.imageLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    RHIDescriptorImageInfo irradiance_texture_image_info {};
    irradiance_texture_image_info.sampler = ibl.m_IrradianceTextureSampler != nullptr ? ibl.m_IrradianceTextureSampler : m_FallbackSampler;
    irradiance_texture_image_info.imageView = ibl.m_IrradianceTextureImageView != nullptr ? ibl.m_IrradianceTextureImageView : m_FallbackCubeView;
    irradiance_texture_image_info.imageLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    RHIDescriptorImageInfo specular_texture_image_info {};
    specular_texture_image_info.sampler = ibl.m_SpecularTextureSampler != nullptr ? ibl.m_SpecularTextureSampler : m_FallbackSampler;
    specular_texture_image_info.imageView = ibl.m_SpecularTextureImageView != nullptr ? ibl.m_SpecularTextureImageView : m_FallbackCubeView;
    specular_texture_image_info.imageLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    RHIDescriptorImageInfo point_light_shadow_texture_image_info {};
    point_light_shadow_texture_image_info.sampler = m_Rhi->GetOrCreateDefaultSampler(Default_Sampler_Nearest);
    point_light_shadow_texture_image_info.imageView = m_PointLightShadowColorImageView;
    point_light_shadow_texture_image_info.imageLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    RHIDescriptorImageInfo directional_light_shadow_texture_image_info {};
    directional_light_shadow_texture_image_info.sampler = m_Rhi->GetOrCreateDefaultSampler(Default_Sampler_Nearest);
    directional_light_shadow_texture_image_info.imageView = m_DirectionalLightShadowColorImageView;
    directional_light_shadow_texture_image_info.imageLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    RHIWriteDescriptorSet mesh_descriptor_writes_info[5] {};
    for (uint32_t write_index = 0; write_index < 5; ++write_index)
    {
        mesh_descriptor_writes_info[write_index].sType = RHI_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        mesh_descriptor_writes_info[write_index].dstSet = m_DescriptorInfos[_mesh_global].descriptor_set;
        mesh_descriptor_writes_info[write_index].dstBinding = write_index + 3;
        mesh_descriptor_writes_info[write_index].descriptorType = RHI_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        mesh_descriptor_writes_info[write_index].descriptorCount = 1;
    }
    mesh_descriptor_writes_info[0].pImageInfo = &brdf_texture_image_info;
    mesh_descriptor_writes_info[1].pImageInfo = &irradiance_texture_image_info;
    mesh_descriptor_writes_info[2].pImageInfo = &specular_texture_image_info;
    mesh_descriptor_writes_info[3].pImageInfo = &point_light_shadow_texture_image_info;
    mesh_descriptor_writes_info[4].pImageInfo = &directional_light_shadow_texture_image_info;

    m_Rhi->UpdateDescriptorSets(5, mesh_descriptor_writes_info, 0, nullptr);
}

bool MainCameraRp1Pass::IsViewportValid(ViewportType viewport_type) const
{
    const RHIViewport* viewport = m_Rhi->GetViewport(viewport_type);
    return viewport && viewport->width > 0.0f && viewport->height > 0.0f;
}

void MainCameraRp1Pass::SetViewportScissor(ViewportType viewport_type)
{
    RHICommandBuffer* current_command_buffer = m_Rhi->GetCurrentCommandBuffer();
    RHIViewport* viewport = m_Rhi->GetViewport(viewport_type);
    RHIRect2D scissor = m_Rhi->GetSwapchainInfo().scissor[static_cast<uint32_t>(viewport_type)];
    m_Rhi->CmdSetViewportPFN(current_command_buffer, 0, 1, viewport);
    m_Rhi->CmdSetScissorPFN(current_command_buffer, 0, 1, &scissor);
}

void MainCameraRp1Pass::SetPerViewportData(ViewportType viewport_type)
{
    m_MeshPerframeStorageBufferObject = m_MeshPerframeStorageBufferObjects[static_cast<size_t>(viewport_type)];

    if (RenderScene* render_scene = GET_SYSTEM(RenderSystem)->getRenderScene().get())
    {
        m_ActiveMainCameraVisibleMeshNodes = &render_scene->GetMainCameraVisibleMeshNodes(viewport_type);
    }
    else
    {
        m_ActiveMainCameraVisibleMeshNodes = m_VisiableNodes.p_main_camera_visible_mesh_nodes;
    }
}

RHIPipeline* MainCameraRp1Pass::GetOrCreateMeshGBufferPipeline(const VulkanPBRMaterial& material)
{
    RHIPipeline* const default_pipeline = m_RenderPipelines[_render_pipeline_type_mesh_gbuffer].pipeline;
    RHIPipeline* const nocull_pipeline = m_RenderPipelines[_render_pipeline_type_mesh_gbuffer_nocull].pipeline;
    if (!CanUseRuntimePrimaryShaderPass(m_Rhi, material))
    {
        if (EqualsIgnoreCase(material.cull, "OFF") || EqualsIgnoreCase(material.cull, "NONE"))
        {
            return nocull_pipeline != nullptr ? nocull_pipeline : default_pipeline;
        }
        return default_pipeline;
    }

    const MeshPipelineKey pipeline_key = BuildPipelineKey(material);
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

    if ((vert_shader_module == nullptr || frag_shader_module == nullptr) &&
        m_Rhi->getGraphicsAPI() == GraphicsAPI::DirectX12)
    {
        if (vert_shader_module != nullptr)
        {
            m_Rhi->DestroyShaderModule(vert_shader_module);
        }
        if (frag_shader_module != nullptr)
        {
            m_Rhi->DestroyShaderModule(frag_shader_module);
        }
        vert_shader_module = LoadBuiltinShader("mesh.vert.hlsl", ShaderStage::Vertex);
        frag_shader_module = LoadBuiltinShader("mesh_gbuffer.frag.hlsl", ShaderStage::Fragment);
    }

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
        m_MeshGbufferMaterialPipelines[pipeline_key] = default_pipeline;
        return default_pipeline;
    }

    RHIPipeline* material_pipeline = CreateRuntimeMeshPipeline(m_Rhi,
                                                               *this,
                                                               vert_shader_module,
                                                               frag_shader_module,
                                                               pipeline_key,
                                                               _render_pipeline_type_mesh_gbuffer,
                                                               m_FbResources->getRP1RenderPass(),
                                                               _main_camera_subpass_basepass,
                                                               3,
                                                               false);
    m_Rhi->DestroyShaderModule(vert_shader_module);
    m_Rhi->DestroyShaderModule(frag_shader_module);
    m_MeshGbufferMaterialPipelines[pipeline_key] = material_pipeline != nullptr ? material_pipeline : default_pipeline;
    return material_pipeline != nullptr ? material_pipeline : default_pipeline;
}

RHIPipeline* MainCameraRp1Pass::GetOrCreateMeshTransparentPipeline(const VulkanPBRMaterial& material)
{
    RHIPipeline* const default_pipeline = m_RenderPipelines[_render_pipeline_type_mesh_transparent].pipeline;
    const VulkanShaderPassData* const transparent_pass = FindTransparentShaderPass(material);
    if (!CanUseRuntimeShaderPass(m_Rhi, material, transparent_pass))
    {
        return default_pipeline;
    }

    const MeshPipelineKey pipeline_key = BuildPipelineKey(material, transparent_pass);
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

    if ((vert_shader_module == nullptr || frag_shader_module == nullptr) &&
        m_Rhi->getGraphicsAPI() == GraphicsAPI::DirectX12)
    {
        if (vert_shader_module != nullptr)
        {
            m_Rhi->DestroyShaderModule(vert_shader_module);
        }
        if (frag_shader_module != nullptr)
        {
            m_Rhi->DestroyShaderModule(frag_shader_module);
        }
        vert_shader_module = LoadBuiltinShader("mesh.vert.hlsl", ShaderStage::Vertex);
        frag_shader_module = LoadBuiltinShader("mesh_forward.frag.hlsl", ShaderStage::Fragment);
    }

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
        m_MeshTransparentMaterialPipelines[pipeline_key] = default_pipeline;
        return default_pipeline;
    }

    RHIPipeline* material_pipeline = CreateRuntimeMeshPipeline(m_Rhi,
                                                               *this,
                                                               vert_shader_module,
                                                               frag_shader_module,
                                                               pipeline_key,
                                                               _render_pipeline_type_mesh_transparent,
                                                               m_FbResources->getRP1RenderPass(),
                                                               _main_camera_subpass_forward_lighting,
                                                               1,
                                                               true);
    m_Rhi->DestroyShaderModule(vert_shader_module);
    m_Rhi->DestroyShaderModule(frag_shader_module);
    m_MeshTransparentMaterialPipelines[pipeline_key] =
        material_pipeline != nullptr ? material_pipeline : default_pipeline;
    return material_pipeline != nullptr ? material_pipeline : default_pipeline;
}

void MainCameraRp1Pass::DrawRP1(const std::array<bool, 2>& skybox_visible)
{
    constexpr float k_scene_clear_r = 0.29f;
    constexpr float k_scene_clear_g = 0.345f;
    constexpr float k_scene_clear_b = 0.435f;

    RHIRenderPassBeginInfo renderpass_begin_info {};
    renderpass_begin_info.sType = RHI_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderpass_begin_info.renderPass = m_FbResources->getRP1RenderPass();
    renderpass_begin_info.framebuffer = m_FbResources->getRP1Framebuffer();
    renderpass_begin_info.renderArea.offset = {0, 0};
    renderpass_begin_info.renderArea.extent = m_Rhi->GetSwapchainInfo().extent;

    RHIClearValue clear_values[5];
    clear_values[0].color = {{0.0f, 0.0f, 0.0f, 0.0f}};
    clear_values[1].color = {{0.0f, 0.0f, 0.0f, 0.0f}};
    clear_values[2].color = {{0.0f, 0.0f, 0.0f, 0.0f}};
    clear_values[3].color = {{k_scene_clear_r, k_scene_clear_g, k_scene_clear_b, 1.0f}};
    clear_values[4].depthStencil = {1.0f, 0};
    renderpass_begin_info.clearValueCount = 5;
    renderpass_begin_info.pClearValues = clear_values;

    m_Rhi->CmdBeginRenderPassPFN(m_Rhi->GetCurrentCommandBuffer(), &renderpass_begin_info, RHI_SUBPASS_CONTENTS_INLINE);

    constexpr ViewportType k_viewports[] = {ViewportType::game, ViewportType::scene};
    float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};

    m_Rhi->PushEvent(m_Rhi->GetCurrentCommandBuffer(), "BasePass", color);
    for (ViewportType viewport_type : k_viewports)
    {
        if (!IsViewportValid(viewport_type))
        {
            continue;
        }
        SetPerViewportData(viewport_type);
        m_MeshPerframeStorageBufferObject.show_skybox =
            skybox_visible[static_cast<size_t>(viewport_type)] ? 1U : 0U;
        DrawMeshGbuffer(viewport_type);
    }
    m_Rhi->PopEvent(m_Rhi->GetCurrentCommandBuffer());

    m_Rhi->CmdNextSubpassPFN(m_Rhi->GetCurrentCommandBuffer(), RHI_SUBPASS_CONTENTS_INLINE);

    m_Rhi->PushEvent(m_Rhi->GetCurrentCommandBuffer(), "Deferred Lighting", color);
    for (ViewportType viewport_type : k_viewports)
    {
        if (!IsViewportValid(viewport_type))
        {
            continue;
        }
        SetPerViewportData(viewport_type);
        m_MeshPerframeStorageBufferObject.show_skybox =
            skybox_visible[static_cast<size_t>(viewport_type)] ? 1U : 0U;
        DrawDeferredLighting(viewport_type);
    }
    m_Rhi->PopEvent(m_Rhi->GetCurrentCommandBuffer());

    m_Rhi->CmdNextSubpassPFN(m_Rhi->GetCurrentCommandBuffer(), RHI_SUBPASS_CONTENTS_INLINE);

    m_Rhi->PushEvent(m_Rhi->GetCurrentCommandBuffer(), "Forward Lighting", color);
    for (ViewportType viewport_type : k_viewports)
    {
        if (!IsViewportValid(viewport_type))
        {
            continue;
        }
        SetPerViewportData(viewport_type);
        m_MeshPerframeStorageBufferObject.show_skybox =
            skybox_visible[static_cast<size_t>(viewport_type)] ? 1U : 0U;
        if (skybox_visible[static_cast<size_t>(viewport_type)] && m_SkyboxDrawCallback)
        {
            m_SkyboxDrawCallback(viewport_type);
        }
        DrawMeshTransparent(viewport_type);
    }
    m_Rhi->PopEvent(m_Rhi->GetCurrentCommandBuffer());

    m_Rhi->CmdEndRenderPassPFN(m_Rhi->GetCurrentCommandBuffer());
}

void MainCameraRp1Pass::DrawMeshGbuffer(ViewportType viewport_type)
{
    struct MeshNode
    {
        const Matrix4x4* model_matrix {nullptr};
        const Matrix4x4* joint_matrices {nullptr};
        uint32_t joint_count {0};
    };

    const RenderScene* render_scene = GET_SYSTEM(RenderSystem)->getRenderScene().get();
    const auto& visible_nodes =
        render_scene ? render_scene->GetMainCameraOpaqueMeshNodes(viewport_type) : (m_ActiveMainCameraVisibleMeshNodes ? *m_ActiveMainCameraVisibleMeshNodes : *(m_VisiableNodes.p_main_camera_visible_mesh_nodes));

    std::map<VulkanPBRMaterial*, std::map<VulkanMesh*, std::vector<MeshNode>>> draw_batches;
    for (const RenderMeshNode& node : visible_nodes)
    {
        auto& mesh_nodes = draw_batches[AsVulkanMaterialource(node.ref_material)][AsVulkanMeshResource(node.ref_mesh)];
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

    const uint32_t perframe_dynamic_offset =
        RoundUp(m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbuffersEnd[m_Rhi->GetCurrentFrameIndex()],
                m_GlobalRenderResource->m_StorageBuffer.m_MinStorageBufferOffsetAlignment);
    m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbuffersEnd[m_Rhi->GetCurrentFrameIndex()] =
        perframe_dynamic_offset + sizeof(MeshPerframeStorageBufferObject);

    (*reinterpret_cast<MeshPerframeStorageBufferObject*>(
        reinterpret_cast<uintptr_t>(m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbufferMemoryPointer) +
        perframe_dynamic_offset)) = m_MeshPerframeStorageBufferObject;

    for (auto& material_batch : draw_batches)
    {
        VulkanPBRMaterial& material = *material_batch.first;
        RHIPipeline* const material_pipeline = GetOrCreateMeshGBufferPipeline(material);
        if (material_pipeline != bound_pipeline)
        {
            m_Rhi->CmdBindPipelinePFN(
                m_Rhi->GetCurrentCommandBuffer(), RHI_PIPELINE_BIND_POINT_GRAPHICS, material_pipeline);
            bound_pipeline = material_pipeline;
        }

        m_Rhi->CmdBindDescriptorSetsPFN(m_Rhi->GetCurrentCommandBuffer(),
                                        RHI_PIPELINE_BIND_POINT_GRAPHICS,
                                        pipeline_layout,
                                        2,
                                        1,
                                        &material.material_descriptor_set,
                                        0,
                                        nullptr);

        for (auto& mesh_batch : material_batch.second)
        {
            VulkanMesh& mesh = *mesh_batch.first;
            auto& mesh_nodes = mesh_batch.second;
            if (!MeshBuffersValid(mesh))
            {
                continue;
            }

            const uint32_t total_instance_count = static_cast<uint32_t>(mesh_nodes.size());
            if (total_instance_count == 0)
            {
                continue;
            }

            m_Rhi->CmdBindDescriptorSetsPFN(m_Rhi->GetCurrentCommandBuffer(),
                                            RHI_PIPELINE_BIND_POINT_GRAPHICS,
                                            pipeline_layout,
                                            1,
                                            1,
                                            &mesh.mesh_vertex_blending_descriptor_set,
                                            0,
                                            nullptr);

            RHIBuffer* vertex_buffers[] = {mesh.mesh_vertex_position_buffer,
                                           mesh.mesh_vertex_varying_enable_blending_buffer,
                                           mesh.mesh_vertex_varying_buffer};
            RHIDeviceSize offsets[] = {0, 0, 0};
            m_Rhi->CmdBindVertexBuffersPFN(m_Rhi->GetCurrentCommandBuffer(),
                                           0,
                                           static_cast<uint32_t>(sizeof(vertex_buffers) / sizeof(vertex_buffers[0])),
                                           vertex_buffers,
                                           offsets);
            m_Rhi->CmdBindIndexBufferPFN(
                m_Rhi->GetCurrentCommandBuffer(), mesh.mesh_index_buffer, 0, RHI_INDEX_TYPE_UINT16);

            const uint32_t drawcall_max_instance_count =
                static_cast<uint32_t>(sizeof(MeshPerdrawcallStorageBufferObject::mesh_instances) /
                                      sizeof(MeshPerdrawcallStorageBufferObject::mesh_instances[0]));
            const uint32_t drawcall_count =
                RoundUp(total_instance_count, drawcall_max_instance_count) / drawcall_max_instance_count;

            for (uint32_t drawcall_index = 0; drawcall_index < drawcall_count; ++drawcall_index)
            {
                const uint32_t current_instance_count =
                    ((total_instance_count - drawcall_max_instance_count * drawcall_index) < drawcall_max_instance_count) ? (total_instance_count - drawcall_max_instance_count * drawcall_index) : drawcall_max_instance_count;

                const uint32_t perdrawcall_dynamic_offset =
                    RoundUp(m_GlobalRenderResource->m_StorageBuffer
                                .m_GlobalUploadRingbuffersEnd[m_Rhi->GetCurrentFrameIndex()],
                            m_GlobalRenderResource->m_StorageBuffer.m_MinStorageBufferOffsetAlignment);
                m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbuffersEnd[m_Rhi->GetCurrentFrameIndex()] =
                    perdrawcall_dynamic_offset + sizeof(MeshPerdrawcallStorageBufferObject);

                MeshPerdrawcallStorageBufferObject& perdrawcall_storage_buffer_object =
                    *reinterpret_cast<MeshPerdrawcallStorageBufferObject*>(reinterpret_cast<uintptr_t>(
                                                                               m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbufferMemoryPointer) +
                                                                           perdrawcall_dynamic_offset);
                for (uint32_t instance_index = 0; instance_index < current_instance_count; ++instance_index)
                {
                    const uint32_t node_index = drawcall_max_instance_count * drawcall_index + instance_index;
                    perdrawcall_storage_buffer_object.mesh_instances[instance_index].model_matrix =
                        *mesh_nodes[node_index].model_matrix;
                    perdrawcall_storage_buffer_object.mesh_instances[instance_index].enable_vertex_blending =
                        mesh_nodes[node_index].joint_matrices ? 1.0f : -1.0f;
                }

                uint32_t per_drawcall_vertex_blending_dynamic_offset = 0;
                bool any_vertex_blending = false;
                for (uint32_t instance_index = 0; instance_index < current_instance_count; ++instance_index)
                {
                    if (mesh_nodes[drawcall_max_instance_count * drawcall_index + instance_index].joint_matrices)
                    {
                        any_vertex_blending = true;
                        break;
                    }
                }
                if (any_vertex_blending)
                {
                    per_drawcall_vertex_blending_dynamic_offset =
                        RoundUp(m_GlobalRenderResource->m_StorageBuffer
                                    .m_GlobalUploadRingbuffersEnd[m_Rhi->GetCurrentFrameIndex()],
                                m_GlobalRenderResource->m_StorageBuffer.m_MinStorageBufferOffsetAlignment);
                    m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbuffersEnd[m_Rhi->GetCurrentFrameIndex()] =
                        per_drawcall_vertex_blending_dynamic_offset +
                        sizeof(MeshPerdrawcallVertexBlendingStorageBufferObject);

                    MeshPerdrawcallVertexBlendingStorageBufferObject& vertex_blending_object =
                        *reinterpret_cast<MeshPerdrawcallVertexBlendingStorageBufferObject*>(reinterpret_cast<uintptr_t>(
                                                                                                 m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbufferMemoryPointer) +
                                                                                             per_drawcall_vertex_blending_dynamic_offset);
                    for (uint32_t instance_index = 0; instance_index < current_instance_count; ++instance_index)
                    {
                        const uint32_t node_index = drawcall_max_instance_count * drawcall_index + instance_index;
                        if (mesh_nodes[node_index].joint_matrices)
                        {
                            for (uint32_t joint_index = 0; joint_index < mesh_nodes[node_index].joint_count; ++joint_index)
                            {
                                vertex_blending_object.joint_matrices[s_MeshVertexBlendingMaxJointCount * instance_index +
                                                                      joint_index] =
                                    mesh_nodes[node_index].joint_matrices[joint_index];
                            }
                        }
                    }
                }

                const uint32_t dynamic_offsets[3] = {perframe_dynamic_offset,
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

    m_Rhi->PopEvent(m_Rhi->GetCurrentCommandBuffer());
}

void MainCameraRp1Pass::DrawMeshTransparent(ViewportType viewport_type)
{
    struct MeshNode
    {
        const Matrix4x4* model_matrix {nullptr};
        const Matrix4x4* joint_matrices {nullptr};
        uint32_t joint_count {0};
    };

    const RenderScene* render_scene = GET_SYSTEM(RenderSystem)->getRenderScene().get();
    const auto& visible_nodes =
        render_scene ? render_scene->GetMainCameraTransparentMeshNodes(viewport_type) : (m_ActiveMainCameraVisibleMeshNodes ? *m_ActiveMainCameraVisibleMeshNodes : *(m_VisiableNodes.p_main_camera_visible_mesh_nodes));

    std::map<VulkanPBRMaterial*, std::map<VulkanMesh*, std::vector<MeshNode>>> draw_batches;
    for (const RenderMeshNode& node : visible_nodes)
    {
        auto& mesh_nodes = draw_batches[AsVulkanMaterialource(node.ref_material)][AsVulkanMeshResource(node.ref_mesh)];
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

    const uint32_t perframe_dynamic_offset =
        RoundUp(m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbuffersEnd[m_Rhi->GetCurrentFrameIndex()],
                m_GlobalRenderResource->m_StorageBuffer.m_MinStorageBufferOffsetAlignment);
    m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbuffersEnd[m_Rhi->GetCurrentFrameIndex()] =
        perframe_dynamic_offset + sizeof(MeshPerframeStorageBufferObject);

    (*reinterpret_cast<MeshPerframeStorageBufferObject*>(
        reinterpret_cast<uintptr_t>(m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbufferMemoryPointer) +
        perframe_dynamic_offset)) = m_MeshPerframeStorageBufferObject;

    for (auto& material_batch : draw_batches)
    {
        VulkanPBRMaterial& material = *material_batch.first;
        RHIPipeline* const material_pipeline = GetOrCreateMeshTransparentPipeline(material);
        if (material_pipeline != bound_pipeline)
        {
            m_Rhi->CmdBindPipelinePFN(
                m_Rhi->GetCurrentCommandBuffer(), RHI_PIPELINE_BIND_POINT_GRAPHICS, material_pipeline);
            bound_pipeline = material_pipeline;
        }

        m_Rhi->CmdBindDescriptorSetsPFN(m_Rhi->GetCurrentCommandBuffer(),
                                        RHI_PIPELINE_BIND_POINT_GRAPHICS,
                                        pipeline_layout,
                                        2,
                                        1,
                                        &material.material_descriptor_set,
                                        0,
                                        nullptr);

        for (auto& mesh_batch : material_batch.second)
        {
            VulkanMesh& mesh = *mesh_batch.first;
            auto& mesh_nodes = mesh_batch.second;
            if (!MeshBuffersValid(mesh))
            {
                continue;
            }

            const uint32_t total_instance_count = static_cast<uint32_t>(mesh_nodes.size());
            if (total_instance_count == 0)
            {
                continue;
            }

            m_Rhi->CmdBindDescriptorSetsPFN(m_Rhi->GetCurrentCommandBuffer(),
                                            RHI_PIPELINE_BIND_POINT_GRAPHICS,
                                            pipeline_layout,
                                            1,
                                            1,
                                            &mesh.mesh_vertex_blending_descriptor_set,
                                            0,
                                            nullptr);

            RHIBuffer* vertex_buffers[] = {mesh.mesh_vertex_position_buffer,
                                           mesh.mesh_vertex_varying_enable_blending_buffer,
                                           mesh.mesh_vertex_varying_buffer};
            RHIDeviceSize offsets[] = {0, 0, 0};
            m_Rhi->CmdBindVertexBuffersPFN(m_Rhi->GetCurrentCommandBuffer(),
                                           0,
                                           static_cast<uint32_t>(sizeof(vertex_buffers) / sizeof(vertex_buffers[0])),
                                           vertex_buffers,
                                           offsets);
            m_Rhi->CmdBindIndexBufferPFN(
                m_Rhi->GetCurrentCommandBuffer(), mesh.mesh_index_buffer, 0, RHI_INDEX_TYPE_UINT16);

            const uint32_t drawcall_max_instance_count =
                static_cast<uint32_t>(sizeof(MeshPerdrawcallStorageBufferObject::mesh_instances) /
                                      sizeof(MeshPerdrawcallStorageBufferObject::mesh_instances[0]));
            const uint32_t drawcall_count =
                RoundUp(total_instance_count, drawcall_max_instance_count) / drawcall_max_instance_count;

            for (uint32_t drawcall_index = 0; drawcall_index < drawcall_count; ++drawcall_index)
            {
                const uint32_t current_instance_count =
                    ((total_instance_count - drawcall_max_instance_count * drawcall_index) < drawcall_max_instance_count) ? (total_instance_count - drawcall_max_instance_count * drawcall_index) : drawcall_max_instance_count;

                const uint32_t perdrawcall_dynamic_offset =
                    RoundUp(m_GlobalRenderResource->m_StorageBuffer
                                .m_GlobalUploadRingbuffersEnd[m_Rhi->GetCurrentFrameIndex()],
                            m_GlobalRenderResource->m_StorageBuffer.m_MinStorageBufferOffsetAlignment);
                m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbuffersEnd[m_Rhi->GetCurrentFrameIndex()] =
                    perdrawcall_dynamic_offset + sizeof(MeshPerdrawcallStorageBufferObject);

                MeshPerdrawcallStorageBufferObject& perdrawcall_storage_buffer_object =
                    *reinterpret_cast<MeshPerdrawcallStorageBufferObject*>(reinterpret_cast<uintptr_t>(
                                                                               m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbufferMemoryPointer) +
                                                                           perdrawcall_dynamic_offset);
                for (uint32_t instance_index = 0; instance_index < current_instance_count; ++instance_index)
                {
                    const uint32_t node_index = drawcall_max_instance_count * drawcall_index + instance_index;
                    perdrawcall_storage_buffer_object.mesh_instances[instance_index].model_matrix =
                        *mesh_nodes[node_index].model_matrix;
                    perdrawcall_storage_buffer_object.mesh_instances[instance_index].enable_vertex_blending =
                        mesh_nodes[node_index].joint_matrices ? 1.0f : -1.0f;
                }

                uint32_t per_drawcall_vertex_blending_dynamic_offset = 0;
                bool any_vertex_blending = false;
                for (uint32_t instance_index = 0; instance_index < current_instance_count; ++instance_index)
                {
                    if (mesh_nodes[drawcall_max_instance_count * drawcall_index + instance_index].joint_matrices)
                    {
                        any_vertex_blending = true;
                        break;
                    }
                }
                if (any_vertex_blending)
                {
                    per_drawcall_vertex_blending_dynamic_offset =
                        RoundUp(m_GlobalRenderResource->m_StorageBuffer
                                    .m_GlobalUploadRingbuffersEnd[m_Rhi->GetCurrentFrameIndex()],
                                m_GlobalRenderResource->m_StorageBuffer.m_MinStorageBufferOffsetAlignment);
                    m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbuffersEnd[m_Rhi->GetCurrentFrameIndex()] =
                        per_drawcall_vertex_blending_dynamic_offset +
                        sizeof(MeshPerdrawcallVertexBlendingStorageBufferObject);

                    MeshPerdrawcallVertexBlendingStorageBufferObject& vertex_blending_object =
                        *reinterpret_cast<MeshPerdrawcallVertexBlendingStorageBufferObject*>(reinterpret_cast<uintptr_t>(
                                                                                                 m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbufferMemoryPointer) +
                                                                                             per_drawcall_vertex_blending_dynamic_offset);
                    for (uint32_t instance_index = 0; instance_index < current_instance_count; ++instance_index)
                    {
                        const uint32_t node_index = drawcall_max_instance_count * drawcall_index + instance_index;
                        if (mesh_nodes[node_index].joint_matrices)
                        {
                            for (uint32_t joint_index = 0; joint_index < mesh_nodes[node_index].joint_count; ++joint_index)
                            {
                                vertex_blending_object.joint_matrices[s_MeshVertexBlendingMaxJointCount * instance_index +
                                                                      joint_index] =
                                    mesh_nodes[node_index].joint_matrices[joint_index];
                            }
                        }
                    }
                }

                const uint32_t dynamic_offsets[3] = {perframe_dynamic_offset,
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

    m_Rhi->PopEvent(m_Rhi->GetCurrentCommandBuffer());
}

void MainCameraRp1Pass::UpdateMegaLightsDescriptorSets(ViewportType viewport_type)
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

void MainCameraRp1Pass::UpdateMegaLightsSpatialDescriptorSets(ViewportType viewport_type)
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

void MainCameraRp1Pass::DrawDeferredLighting(ViewportType viewport_type)
{
    const bool use_megalights = m_Rhi->getGraphicsAPI() == GraphicsAPI::DirectX12 && MegaLights::IsEnabled() &&
                                m_MegaLightsSystem != nullptr && m_MegaLightsSystem->HasGpuData();
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

    const uint32_t perframe_dynamic_offset =
        RoundUp(m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbuffersEnd[m_Rhi->GetCurrentFrameIndex()],
                m_GlobalRenderResource->m_StorageBuffer.m_MinStorageBufferOffsetAlignment);
    m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbuffersEnd[m_Rhi->GetCurrentFrameIndex()] =
        perframe_dynamic_offset + sizeof(MeshPerframeStorageBufferObject);

    (*reinterpret_cast<MeshPerframeStorageBufferObject*>(
        reinterpret_cast<uintptr_t>(m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbufferMemoryPointer) +
        perframe_dynamic_offset)) = m_MeshPerframeStorageBufferObject;

    RHIDescriptorSet* descriptor_sets[2] = {m_DescriptorInfos[_mesh_global].descriptor_set,
                                            m_DescriptorInfos[_deferred_lighting].descriptor_set};
    const uint32_t dynamic_offsets[2] = {perframe_dynamic_offset, 0};
    m_Rhi->CmdBindDescriptorSetsPFN(m_Rhi->GetCurrentCommandBuffer(),
                                    RHI_PIPELINE_BIND_POINT_GRAPHICS,
                                    m_RenderPipelines[pipeline_index].layout,
                                    0,
                                    2,
                                    descriptor_sets,
                                    2,
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

void MainCameraRp1Pass::DrawMegaLightsSpatialDenoise(ViewportType viewport_type)
{
    m_Rhi->CmdBindPipelinePFN(m_Rhi->GetCurrentCommandBuffer(),
                              RHI_PIPELINE_BIND_POINT_GRAPHICS,
                              m_RenderPipelines[_render_pipeline_type_megalights_spatial].pipeline);
    SetViewportScissor(viewport_type);

    const uint32_t perframe_dynamic_offset =
        RoundUp(m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbuffersEnd[m_Rhi->GetCurrentFrameIndex()],
                m_GlobalRenderResource->m_StorageBuffer.m_MinStorageBufferOffsetAlignment);
    m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbuffersEnd[m_Rhi->GetCurrentFrameIndex()] =
        perframe_dynamic_offset + sizeof(MeshPerframeStorageBufferObject);

    (*reinterpret_cast<MeshPerframeStorageBufferObject*>(
        reinterpret_cast<uintptr_t>(m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbufferMemoryPointer) +
        perframe_dynamic_offset)) = m_MeshPerframeStorageBufferObject;

    RHIDescriptorSet* descriptor_sets[2] = {m_DescriptorInfos[_mesh_global].descriptor_set,
                                            m_DescriptorInfos[_deferred_lighting].descriptor_set};
    const uint32_t dynamic_offsets[2] = {perframe_dynamic_offset, 0};
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
