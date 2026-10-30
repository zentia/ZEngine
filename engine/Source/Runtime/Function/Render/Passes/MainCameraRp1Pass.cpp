#include "Runtime/Function/Render/Passes/MainCameraRp1Pass.h"
#include "Runtime/Function/Render/Passes/MainCameraRp1GraphDecl.h"
#include "Runtime/Function/Render/MeshMaterialLayout.h"

#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Function/Render/AppleSsaoPass.h"
#include "Runtime/Function/Render/RenderGpuResources.h"
#include "Runtime/Function/Render/RenderCamera.h"
#include "Runtime/Function/Render/RenderMesh.h"
#include "Runtime/Function/Render/MegaLights/MegaLightsSettings.h"
#include "Runtime/Function/Render/RenderResource.h"
#include "Runtime/Function/Render/RenderScene.h"
#include "Runtime/Function/Render/RenderSystem.h"
#include "Runtime/Function/Render/SsaoSettings.h"
#include "Runtime/Function/Render/Nanite/NaniteRenderer.h"
#include "Runtime/Function/Render/Spine/SpineGpuRenderer.h"
#include "Runtime/Utility/Utility.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <map>
#include <stdexcept>
#include <unordered_map>
#include <vector>

// Z_HAS_SPIRV_SHADERS, not Z_HAS_VULKAN: these are just the SPIR-V bytes of the
// engine's GLSL, and MetalRHI runs them through SPIRV-Cross to get MSL. Gating on
// Z_HAS_VULKAN left Apple with no mesh shaders at all -- the pass threw on startup
// and the scene was never drawn.
#if defined(Z_HAS_SPIRV_SHADERS)
    #include <axis_frag.h>
    #include <axis_vert.h>
    #include <deferred_lighting_frag.h>
    #include <deferred_lighting_ssao_frag.h>
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

    constexpr uint32_t kDx12CbvSlotAlignmentBytes = 256u;
    // Must match DX12RHI::CreateDynamicBufferGpuHandle kRenderMeshInstanceStride.
    constexpr uint32_t kDx12MeshInstanceStructBytes = 80u;
    static_assert(sizeof(RenderMeshInstance) == kDx12MeshInstanceStructBytes,
                  "RenderMeshInstance must stay 80 bytes for DX12 structured-buffer SRV stride");

    uint32_t Dx12MeshSlotOffsetAfterPerFrame(uint32_t perframe_dynamic_offset)
    {
        const uint32_t after_cbv =
            perframe_dynamic_offset +
            RoundUp(static_cast<uint32_t>(sizeof(MainCameraPerFrame)), kDx12CbvSlotAlignmentBytes);
        const uint32_t mesh_slot = RoundUp(after_cbv, kDx12MeshInstanceStructBytes);
        assert((mesh_slot % kDx12MeshInstanceStructBytes) == 0);
        return mesh_slot;
    }

    uint32_t AlignMeshDrawDynamicOffset(RHI* rhi, uint32_t perdrawcall_dynamic_offset)
    {
        if (rhi != nullptr && rhi->getGraphicsAPI() == GraphicsAPI::DirectX12)
        {
            return RoundUp(perdrawcall_dynamic_offset, kDx12MeshInstanceStructBytes);
        }
        return perdrawcall_dynamic_offset;
    }

    uint32_t Dx12MainCameraPerFrameRingStrideBytes(uint32_t perframe_dynamic_offset)
    {
        const uint32_t mesh_slot_begin = Dx12MeshSlotOffsetAfterPerFrame(perframe_dynamic_offset);
        const uint32_t slot_bytes = mesh_slot_begin - perframe_dynamic_offset;
        // Keep the ring cursor 256-byte aligned so the next PerFrame CBV dynamic offset is valid.
        return RoundUp(slot_bytes, kDx12CbvSlotAlignmentBytes);
    }

    uint32_t MeshPerDrawcallOffsetAfterPerFrame(RHI* rhi, uint32_t perframe_dynamic_offset)
    {
        if (rhi != nullptr && rhi->getGraphicsAPI() == GraphicsAPI::DirectX12)
        {
            return AlignMeshDrawDynamicOffset(rhi, Dx12MeshSlotOffsetAfterPerFrame(perframe_dynamic_offset));
        }
        return perframe_dynamic_offset + static_cast<uint32_t>(sizeof(MainCameraPerFrame));
    }

    uint32_t AlignPerFrameRingOffset(RHI* rhi, uint32_t end_offset, uint32_t storage_buffer_alignment)
    {
        if (rhi != nullptr && rhi->getGraphicsAPI() == GraphicsAPI::DirectX12)
        {
            return RoundUp(end_offset, kDx12CbvSlotAlignmentBytes);
        }
        return RoundUp(end_offset, storage_buffer_alignment);
    }

    uint32_t PerFrameUploadRingStrideBytes(RHI* rhi, uint32_t perframe_dynamic_offset)
    {
        if (rhi != nullptr && rhi->getGraphicsAPI() == GraphicsAPI::DirectX12)
        {
            return Dx12MainCameraPerFrameRingStrideBytes(perframe_dynamic_offset);
        }
        return static_cast<uint32_t>(sizeof(MainCameraPerFrame));
    }

    RHIPipeline* CreateRuntimeMeshPipeline(RHI* rhi,
                                           MainCameraRp1Pass& pass,
                                           RHIShader* vert_shader_module,
                                           RHIShader* frag_shader_module,
                                           const MeshPipelineKey& pipeline_key,
                                           MainCameraRp1Pass::RenderPipelineType pipeline_type,
                                           RHIRenderPass* render_pass,  // Changed: pass render_pass instead of rp1_render_pass
                                           uint32_t subpass_index,         // Changed: subpass index (now always 0)
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
        pipeline_info.renderPass = render_pass;  // Use parameter instead of rp1_render_pass
        pipeline_info.subpass = subpass_index;          // Use parameter instead of subpass
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
    const auto render_resource = static_cast<RenderResource*>(m_RenderResource.get());
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
    SetupRenderGraphPath();

    m_Initialized = true;
    return true;
}

void MainCameraRp1Pass::Shutdown()
{
    if (!m_Initialized)
    {
        return;
    }

    // Reverse creation order: the executor and the pool only borrow the device.
    m_Rp1Graph.reset();
    m_RgExecutor.reset();
    m_RgPool.reset();
    m_RgDevice.reset();
    m_UseRenderGraph = false;
    m_RgFallbackLogged = false;

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
    m_PerFrameSource = nullptr;

    // SSAO：pass 本身归管线模块，这里只解借用；1x1 兜底图是自己建的。
    if (m_AmbientOcclusionFallbackView != nullptr)
    {
        m_Rhi->DestroyImageView(m_AmbientOcclusionFallbackView);
    }
    if (m_AmbientOcclusionFallbackImage != nullptr)
    {
        m_Rhi->DestroyImage(m_AmbientOcclusionFallbackImage);
    }
    m_AmbientOcclusionFallbackView = nullptr;
    m_AmbientOcclusionFallbackImage = nullptr;
    m_AmbientOcclusionPass = nullptr;
    m_AmbientOcclusionBound = false;

    // 解掉 Nanite 的注入目标：它指向的 render pass / framebuffer 属于帧缓冲资源，
    // 这次 Shutdown（以及窗口 resize 触发的重建）之后就是悬垂指针了。Nanite 只比较
    // 不解引用，但留下一个指向已销毁对象的目标迟早会出事 —— 解掉之后它退回离屏。
    if (m_NaniteRenderer != nullptr)
    {
        m_NaniteRenderer->SetRasterRenderPass(nullptr, nullptr);
        m_NaniteRenderer = nullptr;
    }

    m_Initialized = false;
}

void MainCameraRp1Pass::PreparePassData(std::shared_ptr<RenderResourceBase> render_resource)
{
    RenderResource* resource = static_cast<RenderResource*>(render_resource.get());
    m_PerFrameSource = resource;
    if (resource != nullptr)
    {
        m_MainCameraPerFrameByViewport = resource->m_MainCameraPerFrameByViewport;
        m_MainCameraPerFrame = resource->m_MainCameraPerFrame;
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

    // Guard: CreateGlobalImage / CreateCubeMap may fail if the D3D12 device
    // has been removed (e.g. TDR under RenderDoc).  Without these checks
    // subsequent rendering code would dereference null image/view pointers.
    if (owned.brdf_image == nullptr)
    {
        LOG_ERROR(ZRender,
                  "EnsureFallbackIblTextures: CreateGlobalImage (BRDF LUT) "
                  "failed — D3D12 device may have been removed");
        return;
    }

    // NOTE: a 1x1 fallback cubemap was tried first and reproducibly removed the
    // D3D12 device on this machine (the fault surfaced later as a failed 1x1 UI
    // white-texture creation in UIGpuResources::Initialize).  Use a 4x4 face
    // instead: large enough to avoid the 1x1 degenerate path, still negligible.
    constexpr uint32_t kFallbackCubeSize = 4u;
    constexpr uint32_t kFaceBytes = kFallbackCubeSize * kFallbackCubeSize * 4u;
    static std::array<uint8_t, kFaceBytes> s_neutral_face {};
    if (s_neutral_face[0] == 0u)
    {
        const uint32_t neutral_pixel_value = 0xFF404040u;
        for (uint32_t i = 0; i < kFallbackCubeSize * kFallbackCubeSize; ++i)
        {
            std::memcpy(&s_neutral_face[i * 4u], &neutral_pixel_value, sizeof(neutral_pixel_value));
        }
    }

    std::array<void*, 6> neutral_faces = {
        s_neutral_face.data(), s_neutral_face.data(), s_neutral_face.data(),
        s_neutral_face.data(), s_neutral_face.data(), s_neutral_face.data(),
    };
    m_Rhi->CreateCubeMap(owned.cube_image,
                         m_FallbackCubeView,
                         nullptr,
                         kFallbackCubeSize,
                         kFallbackCubeSize,
                         neutral_faces,
                         RHI_FORMAT_R8G8B8A8_UNORM,
                         1);
    owned.cube_mem = nullptr;

    if (owned.cube_image == nullptr)
    {
        LOG_ERROR(ZRender,
                  "EnsureFallbackIblTextures: CreateCubeMap (fallback cubemap) "
                  "failed — cleaning up BRDF LUT and returning");
        m_Rhi->DestroyImageView(m_FallbackBrdfView);
        m_Rhi->DestroyImage(owned.brdf_image);
        m_Rhi->FreeMemory(owned.brdf_mem);
        m_FallbackBrdfView = nullptr;
        return;
    }

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
        // 布局来自单一来源（MeshMaterialLayout.h），VT 材质 A/B 测试用同一个函数建同一份布局。
        // DX12 多 binding 6/7（VT 页表 / 页池）；别的后端的 GLSL / SPIR-V gbuffer 着色器不认这两个 binding，
        // 布局保持 6 个，行为一字不变。
        RHIDescriptorSetLayoutBinding mesh_material_layout_bindings[8] {};
        const uint32_t mesh_material_binding_count = BuildMeshMaterialLayoutBindings(
            m_Rhi->getGraphicsAPI() == GraphicsAPI::DirectX12, mesh_material_layout_bindings);

        RHIDescriptorSetLayoutCreateInfo mesh_material_layout_create_info {};
        mesh_material_layout_create_info.sType = RHI_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        mesh_material_layout_create_info.bindingCount = mesh_material_binding_count;
        mesh_material_layout_create_info.pBindings = mesh_material_layout_bindings;

        if (m_Rhi->CreateDescriptorSetLayout(&mesh_material_layout_create_info,
                                             m_DescriptorInfos[_mesh_per_material].layout) != RHI_SUCCESS)
        {
            throw std::runtime_error("MainCameraRp1Pass: create mesh material layout");
        }
    }

    {
        RHIDescriptorSetLayoutBinding gbuffer_lighting_global_layout_bindings[5] {};
        for (uint32_t binding_index = 0; binding_index < 4; ++binding_index)
        {
            gbuffer_lighting_global_layout_bindings[binding_index].binding = binding_index;
            gbuffer_lighting_global_layout_bindings[binding_index].descriptorType = RHI_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
            gbuffer_lighting_global_layout_bindings[binding_index].descriptorCount = 1;
            gbuffer_lighting_global_layout_bindings[binding_index].stageFlags = RHI_SHADER_STAGE_FRAGMENT_BIT;
        }

        // Metal：多一个 binding 4 = SSAO 结果（deferred_lighting_ssao.frag 的
        // in_screen_space_ao）。⚠️ 这一条是**生效那一步**：MetalRHI::UpdateDescriptorSets
        // 只更新布局里存在的 binding，布局里没有 4 的话 RefreshDeferredLightingInputAttachments
        // 写 AO 图会被静默丢掉 ⇒ shader 采到 nil 纹理 = 0 ⇒ 整个间接光变黑，且不报错。
        // 其余后端仍是 4 个 binding，行为一字不变。
        uint32_t gbuffer_lighting_binding_count = 4;
        if (m_Rhi->getGraphicsAPI() == GraphicsAPI::Metal)
        {
            gbuffer_lighting_global_layout_bindings[4].binding = 4;
            gbuffer_lighting_global_layout_bindings[4].descriptorType = RHI_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            gbuffer_lighting_global_layout_bindings[4].descriptorCount = 1;
            gbuffer_lighting_global_layout_bindings[4].stageFlags = RHI_SHADER_STAGE_FRAGMENT_BIT;
            gbuffer_lighting_binding_count = 5;
        }

        RHIDescriptorSetLayoutCreateInfo gbuffer_lighting_global_layout_create_info {};
        gbuffer_lighting_global_layout_create_info.sType = RHI_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        gbuffer_lighting_global_layout_create_info.bindingCount = gbuffer_lighting_binding_count;
        gbuffer_lighting_global_layout_create_info.pBindings = gbuffer_lighting_global_layout_bindings;

        if (m_Rhi->CreateDescriptorSetLayout(&gbuffer_lighting_global_layout_create_info,
                                             m_DescriptorInfos[_deferred_lighting].layout) != RHI_SUCCESS)
        {
            throw std::runtime_error("MainCameraRp1Pass: create deferred lighting layout");
        }
    }

    // 编辑器 gizmo（axis）。和 Vulkan 的 MainCameraPass 同一套布局：
    //   binding 0 = per-frame（proj_view_matrix 等），dynamic —— 每帧在 ring buffer
    //               上的偏移不同，靠 CmdBindDescriptorSets 的 dynamic offset 传进来。
    //   binding 1 = 一个 axis 的 AxisDrawStorage（model 矩阵 + 选中哪根轴）。
    {
        RHIDescriptorSetLayoutBinding axis_layout_bindings[2] {};
        axis_layout_bindings[0].binding         = 0;
        axis_layout_bindings[0].descriptorType  = RHI_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
        axis_layout_bindings[0].descriptorCount = 1;
        axis_layout_bindings[0].stageFlags      = RHI_SHADER_STAGE_VERTEX_BIT;

        axis_layout_bindings[1].binding         = 1;
        axis_layout_bindings[1].descriptorType  = RHI_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        axis_layout_bindings[1].descriptorCount = 1;
        axis_layout_bindings[1].stageFlags      = RHI_SHADER_STAGE_VERTEX_BIT;

        RHIDescriptorSetLayoutCreateInfo axis_layout_create_info {};
        axis_layout_create_info.sType        = RHI_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        axis_layout_create_info.bindingCount = 2;
        axis_layout_create_info.pBindings    = axis_layout_bindings;

        if (m_Rhi->CreateDescriptorSetLayout(&axis_layout_create_info, m_DescriptorInfos[_axis].layout) != RHI_SUCCESS)
        {
            throw std::runtime_error("MainCameraRp1Pass: create axis layout");
        }
    }
}

namespace
{
    RHIShader* LoadRp1ShaderFromFile(RHI* rhi, const char* hlsl_relative_path, ShaderStage stage)
    {
        const std::string full_path = GetRp1ShaderRoot() + "/hlsl/rp1/" + hlsl_relative_path;
        const std::vector<std::string> include_paths = {GetRp1ShaderRoot() + "/hlsl/rp1",
                                                        GetRp1ShaderRoot() + "/hlsl"};
        std::vector<uint8_t> binary;
        LOG_INFO(ZRender, "LoadRp1ShaderFromFile: loading shader from {}", full_path);
        RHIShader* shader = rhi->CreateShaderModuleFromFile(full_path, stage, include_paths, {}, binary);
        if (shader == nullptr)
        {
            LOG_ERROR(ZRender, "LoadRp1ShaderFromFile: failed to load shader from {}", full_path);
        }
        return shader;
    }

    RHIShader* CreateBuiltinVertShader(RHI* rhi)
    {
        if (rhi->getGraphicsAPI() == GraphicsAPI::DirectX12)
        {
            return LoadRp1ShaderFromFile(rhi, "mesh.vert.hlsl", ShaderStage::Vertex);
        }
#if defined(Z_HAS_SPIRV_SHADERS)
        return rhi->CreateShaderModule(MESH_VERT);
#else
        return nullptr;
#endif
    }

    RHIShader* CreateBuiltinFragShader(RHI* rhi, const char* dx12_hlsl_path)
    {
        if (rhi->getGraphicsAPI() == GraphicsAPI::DirectX12)
        {
            return LoadRp1ShaderFromFile(rhi, dx12_hlsl_path, ShaderStage::Fragment);
        }
#if defined(Z_HAS_SPIRV_SHADERS)
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
            // Metal 用带 SSAO 的那一份（同一份 GLSL 主体，多定义一个宏 ⇒ 多一个
            // set 1 / binding 4 的 AO 采样，见 deferred_lighting_ssao.frag）。其余
            // 后端仍是原版：它们的 set 1 布局里没有 binding 4。
            if (rhi->getGraphicsAPI() == GraphicsAPI::Metal)
            {
                return rhi->CreateShaderModule(DEFERRED_LIGHTING_SSAO_FRAG);
            }
            return rhi->CreateShaderModule(DEFERRED_LIGHTING_FRAG);
        }
#endif
        return nullptr;
    }

    RHIShader* CreateBuiltinDeferredVertShader(RHI* rhi)
    {
        if (rhi->getGraphicsAPI() == GraphicsAPI::DirectX12)
        {
            return LoadRp1ShaderFromFile(rhi, "deferred_lighting.vert.hlsl", ShaderStage::Vertex);
        }
#if defined(Z_HAS_SPIRV_SHADERS)
        return rhi->CreateShaderModule(DEFERRED_LIGHTING_VERT);
#else
        return nullptr;
#endif
    }

}  // namespace

void MainCameraRp1Pass::SetupPipelines()
{
    // Simplified: 3 independent render passes (no subpasses).
    // Each pipeline type uses the correct render pass.
    RHIRenderPass* gbuffer_render_pass = m_FbResources->getGBufferRenderPass();           // GBufferPass
    RHIRenderPass* deferred_lighting_render_pass = m_FbResources->getDeferredLightingRenderPass(); // DeferredLightingPass
    RHIRenderPass* forward_lighting_render_pass = m_FbResources->getForwardLightingRenderPass(); // ForwardLightingPass

    auto create_mesh_pipeline = [&](RenderPipelineType pipeline_type,
                                    const char* frag_hlsl_path,
                                    RHIRenderPass* render_pass,  // Changed: pass render_pass instead of subpass
                                    uint32_t color_attachment_count,
                                    bool transparent_pass,
                                    RHICullModeFlags cull_mode,
                                    const char* forward_blend_mode = nullptr,
                                    const char* vert_hlsl_path = "mesh.vert.hlsl") {
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

        const std::string vert_shader_path = GetRp1ShaderRoot() + "/hlsl/rp1/" + vert_hlsl_path;
        const std::string frag_shader_path = GetRp1ShaderRoot() + "/hlsl/rp1/" + frag_hlsl_path;
        RHIShader* vert_shader_module =
            m_Rhi->getGraphicsAPI() == GraphicsAPI::DirectX12
                ? LoadRp1ShaderFromFile(m_Rhi, vert_hlsl_path, ShaderStage::Vertex)
                : CreateBuiltinVertShader(m_Rhi);
        RHIShader* frag_shader_module = CreateBuiltinFragShader(m_Rhi, frag_hlsl_path);
        if (vert_shader_module == nullptr || frag_shader_module == nullptr)
        {
            LOG_ERROR(ZRender,
                      "MainCameraRp1Pass: failed to load builtin mesh shaders (vert={} frag={}; paths: {} | {})",
                      vert_shader_module != nullptr,
                      frag_shader_module != nullptr,
                      vert_shader_path,
                      frag_shader_path);
            throw std::runtime_error("MainCameraRp1Pass: failed to load builtin mesh shaders");
        }
        else
        {
            LOG_INFO(ZRender, "MainCameraRp1Pass: shader loaded successfully (vert={}, frag={})",
                     vert_shader_path.c_str(), frag_shader_path);
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
        // Sky: disable depth test (ALWAYS) so sky renders after all opaque objects.
        // UE-style: sky is drawn at far plane, no need to test against depth buffer.
        bool is_sky_mesh = (pipeline_type == _render_pipeline_type_sky_mesh);
        depth_stencil_create_info.depthTestEnable = RHI_TRUE;  // Enable depth test
        depth_stencil_create_info.depthWriteEnable = RHI_FALSE;  // Never write depth for sky
        if (is_sky_mesh)
        {
            depth_stencil_create_info.depthCompareOp = RHI_COMPARE_OP_ALWAYS;  // ALWAYS for sky (UE-style)
        }
        else
        {
            depth_stencil_create_info.depthCompareOp =
                (transparent_pass) ? RHI_COMPARE_OP_LESS_OR_EQUAL : RHI_COMPARE_OP_LESS;
        }

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
            const char* blend_mode =
                forward_blend_mode != nullptr ? forward_blend_mode
                                              : (transparent_pass ? "Transparent" : "OFF");
            ApplyBlendMode(blend_mode, forward_blend_attachments);
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
        pipeline_info.renderPass = render_pass;  // Use parameter instead of rp1_render_pass
        pipeline_info.subpass = 0;               // Simplified: each render pass has only 1 subpass
        pipeline_info.pDynamicState = &dynamic_state_create_info;

        if (m_Rhi->CreateGraphicsPipelines(RHI_NULL_HANDLE, 1, &pipeline_info, m_RenderPipelines[pipeline_type].pipeline) !=
            RHI_SUCCESS)
        {
            throw std::runtime_error(std::string("MainCameraRp1Pass: create mesh graphics pipeline (frag=") +
                                     frag_hlsl_path + ", subpass=0)");
        }

        m_Rhi->DestroyShaderModule(vert_shader_module);
        m_Rhi->DestroyShaderModule(frag_shader_module);
    };

    // GBufferPass: writes GBufferA, GBufferB, GBufferC.
    create_mesh_pipeline(_render_pipeline_type_mesh_gbuffer,
                         "mesh_gbuffer.frag.hlsl",
                         gbuffer_render_pass,  // Pass GBuffer render pass
                         3,
                         false,
                         RHI_CULL_MODE_BACK_BIT);
    create_mesh_pipeline(_render_pipeline_type_mesh_gbuffer_nocull,
                         "mesh_gbuffer.frag.hlsl",
                         gbuffer_render_pass,  // Pass GBuffer render pass
                         3,
                         false,
                         RHI_CULL_MODE_NONE);

    // ForwardLightingPass: reads/writes BackupOdd.
    create_mesh_pipeline(_render_pipeline_type_mesh_transparent,
                         "mesh_forward.frag.hlsl",
                         forward_lighting_render_pass,  // Pass ForwardLighting render pass
                         1,
                         true,
                         RHI_CULL_MODE_BACK_BIT);

    if (m_Rhi->getGraphicsAPI() == GraphicsAPI::DirectX12)
    {
        // DeferredLightingPass: reads G-Buffer, writes BackupOdd (includes sky).
        // Scene sky mesh: sky_mesh.vert pushes to far depth; LEQUAL depth test, no depth write.
        create_mesh_pipeline(_render_pipeline_type_sky_mesh,
                             "sky_mesh.frag.hlsl",
                             deferred_lighting_render_pass,  // Pass DeferredLighting render pass
                             1,
                             true,
                             RHI_CULL_MODE_NONE,
                             "OFF",
                             "sky_mesh.vert.hlsl");
    }

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
        pipeline_info.renderPass = deferred_lighting_render_pass;  // Use DeferredLightingPass
        pipeline_info.subpass = 0;                                          // Simplified: only 1 subpass
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

    // MegaLights shaders use HLSL 2018-incompatible patterns when disabled at runtime; only
    // compile DX12 pipelines when the feature is on (default off). Core mesh/deferred still run.
    if (m_Rhi->getGraphicsAPI() == GraphicsAPI::DirectX12 && MegaLights::IsEnabled())
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
        pipeline_info.renderPass = deferred_lighting_render_pass;  // Use DeferredLightingPass
        pipeline_info.subpass = 0;                                           // Simplified: only 1 subpass
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

        RHIDescriptorSetLayout* spatial_descriptorset_layouts[2] = {m_DescriptorInfos[_mesh_global].layout,
                                                                  m_DescriptorInfos[_deferred_lighting].layout};
        RHIPipelineLayoutCreateInfo spatial_pipeline_layout_create_info {};
        spatial_pipeline_layout_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        spatial_pipeline_layout_create_info.setLayoutCount = 2;
        spatial_pipeline_layout_create_info.pSetLayouts = spatial_descriptorset_layouts;

        if (m_Rhi->CreatePipelineLayout(&spatial_pipeline_layout_create_info,
                                        m_RenderPipelines[_render_pipeline_type_megalights_spatial].layout) !=
            RHI_SUCCESS)
        {
            throw std::runtime_error("MainCameraRp1Pass: create megalights spatial pipeline layout");
        }

        vert_shader_module = CreateBuiltinDeferredVertShader(m_Rhi);
        frag_shader_module = LoadRp1ShaderFromFile(m_Rhi, "megalights_spatial.frag.hlsl", ShaderStage::Fragment);
        if (vert_shader_module == nullptr || frag_shader_module == nullptr)
        {
            throw std::runtime_error("MainCameraRp1Pass: failed to load megalights spatial shaders");
        }

        RHIPipelineShaderStageCreateInfo spatial_vert_stage {};
        spatial_vert_stage.sType = RHI_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        spatial_vert_stage.stage = RHI_SHADER_STAGE_VERTEX_BIT;
        spatial_vert_stage.module = vert_shader_module;
        spatial_vert_stage.pName = "main";

        RHIPipelineShaderStageCreateInfo spatial_frag_stage {};
        spatial_frag_stage.sType = RHI_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        spatial_frag_stage.stage = RHI_SHADER_STAGE_FRAGMENT_BIT;
        spatial_frag_stage.module = frag_shader_module;
        spatial_frag_stage.pName = "main";

        RHIPipelineShaderStageCreateInfo spatial_shader_stages[] = {spatial_vert_stage, spatial_frag_stage};

        RHIPipelineVertexInputStateCreateInfo spatial_vertex_input_state_create_info {};
        spatial_vertex_input_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

        RHIPipelineInputAssemblyStateCreateInfo spatial_input_assembly_create_info {};
        spatial_input_assembly_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        spatial_input_assembly_create_info.topology = RHI_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        RHIPipelineViewportStateCreateInfo spatial_viewport_state_create_info {};
        spatial_viewport_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        spatial_viewport_state_create_info.viewportCount = 1;
        spatial_viewport_state_create_info.pViewports = m_Rhi->GetSwapchainInfo().viewport;
        spatial_viewport_state_create_info.scissorCount = 1;
        spatial_viewport_state_create_info.pScissors = m_Rhi->GetSwapchainInfo().scissor;

        RHIPipelineRasterizationStateCreateInfo spatial_rasterization_state_create_info {};
        spatial_rasterization_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        spatial_rasterization_state_create_info.polygonMode = RHI_POLYGON_MODE_FILL;
        spatial_rasterization_state_create_info.cullMode = RHI_CULL_MODE_BACK_BIT;
        spatial_rasterization_state_create_info.frontFace = RHI_FRONT_FACE_CLOCKWISE;

        RHIPipelineMultisampleStateCreateInfo spatial_multisample_state_create_info {};
        spatial_multisample_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        spatial_multisample_state_create_info.rasterizationSamples = RHI_SAMPLE_COUNT_1_BIT;

        RHIPipelineColorBlendAttachmentState spatial_color_blend_attachment {};
        spatial_color_blend_attachment.colorWriteMask = RHI_COLOR_COMPONENT_R_BIT | RHI_COLOR_COMPONENT_G_BIT |
                                                        RHI_COLOR_COMPONENT_B_BIT | RHI_COLOR_COMPONENT_A_BIT;

        RHIPipelineColorBlendStateCreateInfo spatial_color_blend_state_create_info {};
        spatial_color_blend_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        spatial_color_blend_state_create_info.attachmentCount = 1;
        spatial_color_blend_state_create_info.pAttachments = &spatial_color_blend_attachment;

        RHIPipelineDepthStencilStateCreateInfo spatial_depth_stencil_create_info {};
        spatial_depth_stencil_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        spatial_depth_stencil_create_info.depthTestEnable = RHI_FALSE;
        spatial_depth_stencil_create_info.depthWriteEnable = RHI_FALSE;

        RHIDynamicState spatial_dynamic_states[] = {RHI_DYNAMIC_STATE_VIEWPORT, RHI_DYNAMIC_STATE_SCISSOR};
        RHIPipelineDynamicStateCreateInfo spatial_dynamic_state_create_info {};
        spatial_dynamic_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        spatial_dynamic_state_create_info.dynamicStateCount = 2;
        spatial_dynamic_state_create_info.pDynamicStates = spatial_dynamic_states;

        RHIGraphicsPipelineCreateInfo spatial_pipeline_info {};
        spatial_pipeline_info.sType = RHI_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        spatial_pipeline_info.stageCount = 2;
        spatial_pipeline_info.pStages = spatial_shader_stages;
        spatial_pipeline_info.pVertexInputState = &spatial_vertex_input_state_create_info;
        spatial_pipeline_info.pInputAssemblyState = &spatial_input_assembly_create_info;
        spatial_pipeline_info.pViewportState = &spatial_viewport_state_create_info;
        spatial_pipeline_info.pRasterizationState = &spatial_rasterization_state_create_info;
        spatial_pipeline_info.pMultisampleState = &spatial_multisample_state_create_info;
        spatial_pipeline_info.pColorBlendState = &spatial_color_blend_state_create_info;
        spatial_pipeline_info.pDepthStencilState = &spatial_depth_stencil_create_info;
        spatial_pipeline_info.layout = m_RenderPipelines[_render_pipeline_type_megalights_spatial].layout;
        spatial_pipeline_info.renderPass = deferred_lighting_render_pass;  // Use DeferredLightingPass
        spatial_pipeline_info.subpass = 0;                                           // Simplified: only 1 subpass
        spatial_pipeline_info.pDynamicState = &spatial_dynamic_state_create_info;

        if (m_Rhi->CreateGraphicsPipelines(RHI_NULL_HANDLE,
                                           1,
                                           &spatial_pipeline_info,
                                           m_RenderPipelines[_render_pipeline_type_megalights_spatial].pipeline) !=
            RHI_SUCCESS)
        {
            throw std::runtime_error("MainCameraRp1Pass: create megalights spatial pipeline");
        }

        m_Rhi->DestroyShaderModule(vert_shader_module);
        m_Rhi->DestroyShaderModule(frag_shader_module);
    }

    // Procedural sky cube (no vertex buffers). Scene sky mesh uses create_mesh_pipeline above.
    if (m_Rhi->getGraphicsAPI() == GraphicsAPI::DirectX12)
    {
        RenderPipelineType pipeline_type = _render_pipeline_type_sky_procedural;
        RHIDescriptorSetLayout* descriptorset_layouts[1] = {m_DescriptorInfos[_mesh_global].layout};
        RHIPipelineLayoutCreateInfo pipeline_layout_create_info {};
        pipeline_layout_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipeline_layout_create_info.setLayoutCount = 1;
        pipeline_layout_create_info.pSetLayouts = descriptorset_layouts;

        if (m_Rhi->CreatePipelineLayout(&pipeline_layout_create_info, m_RenderPipelines[pipeline_type].layout) !=
            RHI_SUCCESS)
        {
            throw std::runtime_error("MainCameraRp1Pass: create sky procedural pipeline layout");
        }

        RHIShader* vert_shader_module = LoadRp1ShaderFromFile(m_Rhi, "sky_procedural.vert.hlsl", ShaderStage::Vertex);
        RHIShader* frag_shader_module = LoadRp1ShaderFromFile(m_Rhi, "sky_mesh.frag.hlsl", ShaderStage::Fragment);
        if (vert_shader_module == nullptr || frag_shader_module == nullptr)
        {
            throw std::runtime_error("MainCameraRp1Pass: failed to load sky procedural shaders");
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
        rasterization_state_create_info.cullMode = RHI_CULL_MODE_NONE;
        rasterization_state_create_info.frontFace = RHI_FRONT_FACE_COUNTER_CLOCKWISE;

        RHIPipelineMultisampleStateCreateInfo multisample_state_create_info {};
        multisample_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample_state_create_info.rasterizationSamples = RHI_SAMPLE_COUNT_1_BIT;

        std::array<RHIPipelineColorBlendAttachmentState, 1> color_blend_attachments {};
        ApplyBlendMode("Transparent", color_blend_attachments);
        RHIPipelineColorBlendStateCreateInfo color_blend_state_create_info {};
        color_blend_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        color_blend_state_create_info.attachmentCount = 1;
        color_blend_state_create_info.pAttachments = color_blend_attachments.data();

        RHIPipelineDepthStencilStateCreateInfo depth_stencil_create_info {};
        depth_stencil_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        // Sky: disable depth test (ALWAYS) so sky renders after all opaque objects.
        // UE-style: sky is drawn at far plane, no need to test against depth buffer.
        depth_stencil_create_info.depthTestEnable = RHI_TRUE;  // Enable depth test
        depth_stencil_create_info.depthWriteEnable = RHI_FALSE;  // Never write depth for sky
        depth_stencil_create_info.depthCompareOp = RHI_COMPARE_OP_ALWAYS;  // ALWAYS for sky (UE-style)

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
        pipeline_info.renderPass = deferred_lighting_render_pass;  // Use DeferredLightingPass
        pipeline_info.subpass = 0;                                           // Simplified: only 1 subpass
        pipeline_info.pDynamicState = &dynamic_state_create_info;

        if (m_Rhi->CreateGraphicsPipelines(RHI_NULL_HANDLE, 1, &pipeline_info, m_RenderPipelines[pipeline_type].pipeline) !=
            RHI_SUCCESS)
        {
            throw std::runtime_error("MainCameraRp1Pass: create sky procedural graphics pipeline");
        }

        m_Rhi->DestroyShaderModule(vert_shader_module);
        m_Rhi->DestroyShaderModule(frag_shader_module);
    }

    SetupAxisPipeline();
}

void MainCameraRp1Pass::SetupAxisPipeline()
{
    // 编辑器 gizmo（axis）的管线。
    //
    // 只给非 DX12 建：DX12 走 `DX12MainCameraPass::DrawAxis`（HLSL + root constant），
    // Vulkan 走 `MainCameraPass::DrawAxis`。RP1 这一份是给 Apple / Metal 补的 ——
    // 那里既没有 Vulkan 的 MainCameraPass，也没有 DX12 那份，于是 axis 一个都没人画。
    //
    // 失败**不抛**：gizmo 缺席不该让整个编辑器起不来。以前这里整条 SetupPipelines
    // 是一串 throw，加一个"可选"的管线进去必须自己兜住，否则 SPIR-V 转 MSL 出一点
    // 问题就把场景一起拖死。
    if (m_Rhi->getGraphicsAPI() == GraphicsAPI::DirectX12)
    {
        return;
    }

    RHIDescriptorSetLayout* descriptorset_layouts[1] = {m_DescriptorInfos[_axis].layout};
    RHIPipelineLayoutCreateInfo pipeline_layout_create_info {};
    pipeline_layout_create_info.sType        = RHI_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeline_layout_create_info.setLayoutCount = 1;
    pipeline_layout_create_info.pSetLayouts  = descriptorset_layouts;

    if (m_Rhi->CreatePipelineLayout(&pipeline_layout_create_info,
                                    m_RenderPipelines[_render_pipeline_type_axis].layout) != RHI_SUCCESS)
    {
        LOG_ERROR(ZRender, "MainCameraRp1Pass: create axis pipeline layout failed -- editor gizmo stays hidden");
        return;
    }

#if !defined(Z_HAS_SPIRV_SHADERS)
    LOG_WARNING(ZRender,
                "MainCameraRp1Pass: axis shaders unavailable (SPIR-V builtins not compiled in) -- "
                "editor gizmo stays hidden");
    return;
#else
    RHIShader* vert_shader_module = m_Rhi->CreateShaderModule(AXIS_VERT);
    RHIShader* frag_shader_module = m_Rhi->CreateShaderModule(AXIS_FRAG);
    if (vert_shader_module == nullptr || frag_shader_module == nullptr)
    {
        LOG_ERROR(ZRender,
                  "MainCameraRp1Pass: axis shader module failed (vert={} frag={}) -- editor gizmo stays hidden",
                  vert_shader_module != nullptr,
                  frag_shader_module != nullptr);
        return;
    }

    RHIPipelineShaderStageCreateInfo shader_stages[2] {};
    shader_stages[0].sType  = RHI_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shader_stages[0].stage  = RHI_SHADER_STAGE_VERTEX_BIT;
    shader_stages[0].module = vert_shader_module;
    shader_stages[0].pName  = "main";
    shader_stages[1].sType  = RHI_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shader_stages[1].stage  = RHI_SHADER_STAGE_FRAGMENT_BIT;
    shader_stages[1].module = frag_shader_module;
    shader_stages[1].pName  = "main";

    auto vertex_binding_descriptions   = MeshVertex::GetBindingDescriptions();
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
    input_assembly_create_info.sType     = RHI_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly_create_info.topology  = RHI_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    RHIPipelineViewportStateCreateInfo viewport_state_create_info {};
    viewport_state_create_info.sType        = RHI_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state_create_info.viewportCount = 1;
    viewport_state_create_info.pViewports   = m_Rhi->GetSwapchainInfo().viewport;
    viewport_state_create_info.scissorCount = 1;
    viewport_state_create_info.pScissors    = m_Rhi->GetSwapchainInfo().scissor;

    RHIPipelineRasterizationStateCreateInfo rasterization_state_create_info {};
    rasterization_state_create_info.sType      = RHI_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization_state_create_info.polygonMode = RHI_POLYGON_MODE_FILL;
    // gizmo 是一组细长的锥/柱，双面都要看得见。
    rasterization_state_create_info.cullMode   = RHI_CULL_MODE_NONE;
    rasterization_state_create_info.frontFace  = RHI_FRONT_FACE_COUNTER_CLOCKWISE;

    RHIPipelineMultisampleStateCreateInfo multisample_state_create_info {};
    multisample_state_create_info.sType               = RHI_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample_state_create_info.rasterizationSamples = RHI_SAMPLE_COUNT_1_BIT;

    RHIPipelineColorBlendAttachmentState color_blend_attachment {};
    color_blend_attachment.colorWriteMask = RHI_COLOR_COMPONENT_R_BIT | RHI_COLOR_COMPONENT_G_BIT |
                                            RHI_COLOR_COMPONENT_B_BIT | RHI_COLOR_COMPONENT_A_BIT;
    color_blend_attachment.blendEnable    = RHI_FALSE;

    RHIPipelineColorBlendStateCreateInfo color_blend_state_create_info {};
    color_blend_state_create_info.sType           = RHI_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    color_blend_state_create_info.attachmentCount = 1;
    color_blend_state_create_info.pAttachments    = &color_blend_attachment;

    // 深度全关：axis.vert 已经把 clip z 压到 0.0001（贴到最近平面），gizmo 必须永远
    // 盖在场景之上，不能被自己的几何或场景深度吃掉。
    RHIPipelineDepthStencilStateCreateInfo depth_stencil_create_info {};
    depth_stencil_create_info.sType            = RHI_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_stencil_create_info.depthTestEnable  = RHI_FALSE;
    depth_stencil_create_info.depthWriteEnable = RHI_FALSE;

    RHIDynamicState dynamic_states[] = {RHI_DYNAMIC_STATE_VIEWPORT, RHI_DYNAMIC_STATE_SCISSOR};
    RHIPipelineDynamicStateCreateInfo dynamic_state_create_info {};
    dynamic_state_create_info.sType            = RHI_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic_state_create_info.dynamicStateCount = 2;
    dynamic_state_create_info.pDynamicStates   = dynamic_states;

    RHIGraphicsPipelineCreateInfo pipeline_info {};
    pipeline_info.sType               = RHI_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_info.stageCount          = 2;
    pipeline_info.pStages             = shader_stages;
    pipeline_info.pVertexInputState   = &vertex_input_state_create_info;
    pipeline_info.pInputAssemblyState = &input_assembly_create_info;
    pipeline_info.pViewportState      = &viewport_state_create_info;
    pipeline_info.pRasterizationState = &rasterization_state_create_info;
    pipeline_info.pMultisampleState   = &multisample_state_create_info;
    pipeline_info.pColorBlendState    = &color_blend_state_create_info;
    pipeline_info.pDepthStencilState  = &depth_stencil_create_info;
    pipeline_info.layout              = m_RenderPipelines[_render_pipeline_type_axis].layout;
    pipeline_info.renderPass          = m_FbResources->getForwardLightingRenderPass();
    pipeline_info.subpass             = 0;
    pipeline_info.pDynamicState       = &dynamic_state_create_info;

    if (m_Rhi->CreateGraphicsPipelines(RHI_NULL_HANDLE,
                                       1,
                                       &pipeline_info,
                                       m_RenderPipelines[_render_pipeline_type_axis].pipeline) != RHI_SUCCESS)
    {
        LOG_ERROR(ZRender, "MainCameraRp1Pass: create axis graphics pipeline failed -- editor gizmo stays hidden");
        return;
    }

    m_Rhi->DestroyShaderModule(vert_shader_module);
    m_Rhi->DestroyShaderModule(frag_shader_module);

    LOG_INFO(ZRender, "MainCameraRp1Pass: axis pipeline ready (editor gizmo can be drawn)");
#endif
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
    mesh_perframe_storage_buffer_info.range =
        m_Rhi->getGraphicsAPI() == GraphicsAPI::DirectX12
            ? RoundUp(static_cast<uint32_t>(sizeof(MainCameraPerFrame)), kDx12CbvSlotAlignmentBytes)
            : sizeof(MainCameraPerFrame);
    mesh_perframe_storage_buffer_info.buffer = m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbuffer;

    RHIDescriptorBufferInfo mesh_perdrawcall_storage_buffer_info {};
    mesh_perdrawcall_storage_buffer_info.offset = 0;
    mesh_perdrawcall_storage_buffer_info.range = sizeof(MeshDrawPerDrawcall);
    mesh_perdrawcall_storage_buffer_info.buffer = m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbuffer;
    // Explicit stride: DX12 builds a structured SRV from this, and its fallback
    // heuristic (80 bytes when the range divides by 80) would be wrong for any
    // per-instance struct that is not exactly 80 bytes.
    mesh_perdrawcall_storage_buffer_info.stride = sizeof(RenderMeshInstance);

    RHIDescriptorBufferInfo mesh_per_drawcall_vertex_blending_storage_buffer_info {};
    mesh_per_drawcall_vertex_blending_storage_buffer_info.offset = 0;
    mesh_per_drawcall_vertex_blending_storage_buffer_info.range =
        sizeof(MeshDrawPerDrawcallVertexBlending);
    mesh_per_drawcall_vertex_blending_storage_buffer_info.buffer =
        m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbuffer;
    // The joint palette is a flat array of float4x4, not a struct, so the
    // stride must be spelled out for DX12 (see the HLSL declaration in
    // mesh.vert.hlsl: `StructuredBuffer<float4x4> joint_matrices`).
    mesh_per_drawcall_vertex_blending_storage_buffer_info.stride = sizeof(Matrix4x4);

    const IBLResource& ibl = m_GlobalRenderResource->m_IblResource;

    // Guard: if neither real IBL textures nor fallback textures are available,
    // skip descriptor setup.
    //
    // 这条日志以前写的是 "(device removed?)"，把"根本没上传"和"设备掉了"混成了一
    // 件事 —— Apple 上它每帧必现，害得每次看日志都要先怀疑一次 TDR。实际上来到
    // 这里的绝大多数情况是 IBL 还没上传（`RenderSystem::Initialize` 没走
    // `UploadGlobalRenderResource`），而真正的设备丢失会先在
    // `EnsureFallbackIblTextures` 的 CreateGlobalImage / CreateCubeMap 上报出来。
    // 措辞改成"还没上传"，并把它降级成 WARN —— descriptor 跳过是**有兜底**的降级
    // 路径（不画环境光），不是渲染失败。
    RHIImageView* setup_brdf = ibl.m_BrdflutTextureImageView != nullptr
        ? ibl.m_BrdflutTextureImageView : m_FallbackBrdfView;
    RHIImageView* setup_irradiance = ibl.m_IrradianceTextureImageView != nullptr
        ? ibl.m_IrradianceTextureImageView : m_FallbackCubeView;
    RHIImageView* setup_specular = ibl.m_SpecularTextureImageView != nullptr
        ? ibl.m_SpecularTextureImageView : m_FallbackCubeView;
    if (setup_brdf == nullptr || setup_irradiance == nullptr ||
        setup_specular == nullptr)
    {
        LOG_WARNING(ZRender,
                    "MainCameraRp1Pass::SetupDescriptorSets: no IBL textures bound yet (neither the real "
                    "HDR cubemaps nor the neutral fallback exist) -- skipping the mesh global descriptor "
                    "set; indirect lighting will be flat until RenderResource uploads the IBL");
        return;
    }

    // 真 IBL 还是兜底 IBL，这一行是唯一的对外交代。以前这条路径上只有"兜底为空
    // 时报错"这一条日志，于是"环境光其实是 4x4 中性灰"这件事在日志里完全看不出
    // 来 —— Apple 上挂了很久就是没人发现。现在每次建 descriptor 都打一次。
    LOG_INFO(ZRender,
             "MainCameraRp1Pass::SetupDescriptorSets: IBL bound (irradiance={}, specular={}, brdf={}) -- "
             "'real' = the uploaded HDR cubemaps, 'fallback' = the 4x4 neutral stand-in",
             ibl.m_IrradianceTextureImageView != nullptr ? "real" : "fallback",
             ibl.m_SpecularTextureImageView != nullptr ? "real" : "fallback",
             ibl.m_BrdflutTextureImageView != nullptr ? "real" : "fallback");

    RHIDescriptorImageInfo brdf_texture_image_info {};
    brdf_texture_image_info.sampler = ibl.m_BrdflutTextureSampler != nullptr ? ibl.m_BrdflutTextureSampler : m_FallbackSampler;
    brdf_texture_image_info.imageView = setup_brdf;
    brdf_texture_image_info.imageLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    RHIDescriptorImageInfo irradiance_texture_image_info {};
    irradiance_texture_image_info.sampler = ibl.m_IrradianceTextureSampler != nullptr ? ibl.m_IrradianceTextureSampler : m_FallbackSampler;
    irradiance_texture_image_info.imageView = setup_irradiance;
    irradiance_texture_image_info.imageLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    RHIDescriptorImageInfo specular_texture_image_info {};
    specular_texture_image_info.sampler = ibl.m_SpecularTextureSampler != nullptr ? ibl.m_SpecularTextureSampler : m_FallbackSampler;
    specular_texture_image_info.imageView = setup_specular;
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

    // See MainCameraPass::SetupModelGlobalDescriptorSet: while MegaLights owns no
    // resources (r.MegaLights.Enable = 0 by default) bindings 8..12 have nothing
    // to point at, and only the MegaLights deferred shader declares them.
    uint32_t mesh_descriptor_write_count = 13;
    if (megalights_lights_buffer_info.buffer == nullptr ||
        megalights_indices_buffer_info.buffer == nullptr ||
        megalights_ranges_buffer_info.buffer == nullptr ||
        megalights_history_read_info.imageView == nullptr ||
        megalights_history_write_info.imageView == nullptr)
    {
        // Writes 8..12 are the last five entries, in binding order.
        mesh_descriptor_write_count = 8;
    }

    m_Rhi->UpdateDescriptorSets(mesh_descriptor_write_count, mesh_descriptor_writes_info, 0, nullptr);

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

    // Allocate a default _mesh_per_material descriptor set.
    // DX12 requires ALL descriptor sets in the root signature to be bound;
    // a null set causes the GPU to return 0 (black) for any access.
    // Sky mesh doesn't use per-material params, but the set must be valid.
    if (m_DescriptorInfos[_mesh_per_material].layout != nullptr)
    {
        RHIDescriptorSetAllocateInfo mesh_per_material_alloc_info {};
        mesh_per_material_alloc_info.sType = RHI_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        mesh_per_material_alloc_info.descriptorPool = m_Rhi->GetDescriptorPoor();
        mesh_per_material_alloc_info.descriptorSetCount = 1;
        mesh_per_material_alloc_info.pSetLayouts = &m_DescriptorInfos[_mesh_per_material].layout;

        if (m_Rhi->AllocateDescriptorSets(&mesh_per_material_alloc_info,
                                          m_DescriptorInfos[_mesh_per_material].descriptor_set) != RHI_SUCCESS)
        {
            LOG_WARNING(ZRender, "MainCameraRp1Pass: failed to allocate default mesh_per_material descriptor set");
        }
        else
        {
            LOG_INFO(ZRender, "MainCameraRp1Pass: allocated default mesh_per_material descriptor set (handle={:016X})",
                     (uint64_t)(uintptr_t)m_DescriptorInfos[_mesh_per_material].descriptor_set);
            // NOTE: We do NOT write descriptors here. The sky mesh shader does not
            // access set 2 (_mesh_per_material). The only requirement is that the
            // descriptor set handle is non-null so DX12 doesn't bind garbage.
            // When real mesh nodes are drawn later, RefreshMeshPerMaterialDescriptor()
            // will overwrite this set with real descriptors.
        }
    }

    SetupAxisDescriptorSet();
}

void MainCameraRp1Pass::SetupAxisDescriptorSet()
{
    // 编辑器 gizmo（axis）的 descriptor set：binding 0 指 per-frame 的 ring buffer
    // （static offset 写 0，真正的偏移每帧由 CmdBindDescriptorSets 的 dynamic offset
    // 带进来），binding 1 指那一个 axis 的 AxisDrawStorage。
    if (m_DescriptorInfos[_axis].layout == nullptr)
    {
        return;
    }

    RHIDescriptorSetAllocateInfo axis_descriptor_set_alloc_info {};
    axis_descriptor_set_alloc_info.sType            = RHI_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    axis_descriptor_set_alloc_info.descriptorPool   = m_Rhi->GetDescriptorPoor();
    axis_descriptor_set_alloc_info.descriptorSetCount = 1;
    axis_descriptor_set_alloc_info.pSetLayouts      = &m_DescriptorInfos[_axis].layout;

    if (m_Rhi->AllocateDescriptorSets(&axis_descriptor_set_alloc_info, m_DescriptorInfos[_axis].descriptor_set) !=
        RHI_SUCCESS)
    {
        LOG_ERROR(ZRender, "MainCameraRp1Pass: allocate axis descriptor set failed -- editor gizmo stays hidden");
        return;
    }

    RHIDescriptorBufferInfo axis_perframe_storage_buffer_info {};
    axis_perframe_storage_buffer_info.offset = 0;
    axis_perframe_storage_buffer_info.range  = sizeof(MainCameraPerFrame);
    axis_perframe_storage_buffer_info.buffer =
        m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbuffer;

    RHIDescriptorBufferInfo axis_storage_buffer_info {};
    axis_storage_buffer_info.offset = 0;
    axis_storage_buffer_info.range  = sizeof(AxisDrawStorage);
    axis_storage_buffer_info.buffer = m_GlobalRenderResource->m_StorageBuffer.m_AxisInefficientStorageBuffer;

    RHIWriteDescriptorSet axis_descriptor_writes_info[2] {};
    axis_descriptor_writes_info[0].sType           = RHI_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    axis_descriptor_writes_info[0].dstSet          = m_DescriptorInfos[_axis].descriptor_set;
    axis_descriptor_writes_info[0].dstBinding      = 0;
    axis_descriptor_writes_info[0].descriptorType  = RHI_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
    axis_descriptor_writes_info[0].descriptorCount = 1;
    axis_descriptor_writes_info[0].pBufferInfo     = &axis_perframe_storage_buffer_info;

    axis_descriptor_writes_info[1].sType           = RHI_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    axis_descriptor_writes_info[1].dstSet          = m_DescriptorInfos[_axis].descriptor_set;
    axis_descriptor_writes_info[1].dstBinding      = 1;
    axis_descriptor_writes_info[1].descriptorType  = RHI_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    axis_descriptor_writes_info[1].descriptorCount = 1;
    axis_descriptor_writes_info[1].pBufferInfo     = &axis_storage_buffer_info;

    m_Rhi->UpdateDescriptorSets(2, axis_descriptor_writes_info, 0, nullptr);
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

    // Metal：binding 4 = SSAO（真 AO 图或 1x1 白图兜底）。每帧都写 —— 这一帧 AO 跑没
    // 跑、resize 之后目标换没换，都只有这一刻知道。
    if (m_Rhi->getGraphicsAPI() == GraphicsAPI::Metal)
    {
        RHIDescriptorImageInfo ambient_occlusion_info {};
        ambient_occlusion_info.sampler = m_Rhi->GetOrCreateDefaultSampler(Default_Sampler_Nearest);
        ambient_occlusion_info.imageView = ResolveAmbientOcclusionView();
        ambient_occlusion_info.imageLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        RHIWriteDescriptorSet ambient_occlusion_write {};
        ambient_occlusion_write.sType = RHI_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        ambient_occlusion_write.dstSet = m_DescriptorInfos[_deferred_lighting].descriptor_set;
        ambient_occlusion_write.dstBinding = 4;
        ambient_occlusion_write.descriptorType = RHI_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        ambient_occlusion_write.descriptorCount = 1;
        ambient_occlusion_write.pImageInfo = &ambient_occlusion_info;
        m_Rhi->UpdateDescriptorSets(1, &ambient_occlusion_write, 0, nullptr);
    }
}

RHIImageView* MainCameraRp1Pass::ResolveAmbientOcclusionView()
{
#if defined(__APPLE__)
    // 判据只有一个：AppleSsaoPass::GetOutputView()。它在 Record() 开头就被清掉，所以
    // "这一帧没跑"（关掉 / 视口无效 / 输入缺失）一定拿到 nullptr，不会读到上一帧
    // 或 resize 之前的旧图。
    if (m_AmbientOcclusionPass != nullptr)
    {
        if (RHIImageView* ao_view = m_AmbientOcclusionPass->GetOutputView())
        {
            m_AmbientOcclusionBound = true;
            return ao_view;
        }
    }
#endif
    m_AmbientOcclusionBound = false;

    // 兜底：1x1 白图（r = 间接光可见度 = 1，g = 直接光乘子 = 1）。少了它 binding 4 是
    // 空的，Metal 采样返回 0 ⇒ 环境光 / IBL 整片消失，而且日志里什么都没有。
    if (m_AmbientOcclusionFallbackView == nullptr && m_Rhi != nullptr)
    {
        const uint8_t white[] = {255, 255, 255, 255};
        RHIImage* image = nullptr;
        m_Rhi->CreateGlobalImage(image,
                                 m_AmbientOcclusionFallbackView,
                                 nullptr,
                                 1,
                                 1,
                                 const_cast<uint8_t*>(white),
                                 RHI_FORMAT_R8G8B8A8_UNORM,
                                 1);
        m_AmbientOcclusionFallbackImage = image;
        if (m_AmbientOcclusionFallbackView == nullptr)
        {
            LOG_ERROR(ZRender, "MainCameraRp1Pass: SSAO 1x1 fallback texture creation failed");
        }
    }
    return m_AmbientOcclusionFallbackView;
}

void MainCameraRp1Pass::DrawAmbientOcclusionPass()
{
#if defined(__APPLE__)
    if (m_AmbientOcclusionPass == nullptr || m_Rhi == nullptr || m_FbResources == nullptr ||
        m_Rhi->getGraphicsAPI() != GraphicsAPI::Metal)
    {
        return;
    }

    AppleSsaoSettings settings {};
    settings.radius                   = SsaoSettings::GetRadius();
    settings.intensity                = SsaoSettings::GetIntensity();
    settings.direct_lighting_strength = SsaoSettings::GetDirectLightingStrength();
    settings.sample_count             = SsaoSettings::GetSampleCount();
    settings.blur                     = SsaoSettings::IsBlurEnabled();

    // 视口 / 相机：和延迟光照同一套（ResolveViewportScissor + SetPerViewportData），
    // 否则 AO 和光照落在不同的像素上 —— 编辑器里 Scene 视口是窗口的一个子矩形。
    AppleSsaoViewport viewports[AppleSsaoPass::kMaxViewports] {};
    uint32_t          viewport_count = 0;
    constexpr ViewportType k_viewports[] = {ViewportType::game, ViewportType::scene};
    for (ViewportType viewport_type : k_viewports)
    {
        if (!IsViewportValid(viewport_type) || viewport_count >= AppleSsaoPass::kMaxViewports)
        {
            continue;
        }
        SetPerViewportData(viewport_type);
        AppleSsaoViewport& out = viewports[viewport_count++];
        ResolveViewportScissor(viewport_type, out.viewport, out.scissor);
        out.proj_view       = m_MainCameraPerFrame.proj_view_matrix;
        out.camera_position = m_MainCameraPerFrame.camera_position;
    }

    // 关掉时也要调 Record（传 0 个视口）：它负责把"这一帧有没有 AO"清零，
    // GetOutputView() 才会如实返回 nullptr。
    const bool enabled = SsaoSettings::IsEnabled();
    m_AmbientOcclusionPass->Record(m_Rhi->GetCurrentCommandBuffer(),
                                   m_Rhi->GetDepthImageInfo().depth_image_view,
                                   m_FbResources->getAttachmentView(_main_camera_pass_gbuffer_a),
                                   m_Rhi->GetSwapchainInfo().extent,
                                   viewports,
                                   enabled ? viewport_count : 0u,
                                   settings);
#endif
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

    // Guard: if neither real IBL textures nor fallback textures are available
    // (e.g. D3D12 device removed / TDR under RenderDoc), skip descriptor updates
    // to avoid passing null imageViews into UpdateDescriptorSets.
    RHIImageView* effective_brdf = ibl.m_BrdflutTextureImageView != nullptr
        ? ibl.m_BrdflutTextureImageView : m_FallbackBrdfView;
    RHIImageView* effective_irradiance = ibl.m_IrradianceTextureImageView != nullptr
        ? ibl.m_IrradianceTextureImageView : m_FallbackCubeView;
    RHIImageView* effective_specular = ibl.m_SpecularTextureImageView != nullptr
        ? ibl.m_SpecularTextureImageView : m_FallbackCubeView;
    if (effective_brdf == nullptr || effective_irradiance == nullptr ||
        effective_specular == nullptr)
    {
        return;
    }

    RHIDescriptorImageInfo brdf_texture_image_info {};
    brdf_texture_image_info.sampler = ibl.m_BrdflutTextureSampler != nullptr ? ibl.m_BrdflutTextureSampler : m_FallbackSampler;
    brdf_texture_image_info.imageView = effective_brdf;
    brdf_texture_image_info.imageLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    RHIDescriptorImageInfo irradiance_texture_image_info {};
    irradiance_texture_image_info.sampler = ibl.m_IrradianceTextureSampler != nullptr ? ibl.m_IrradianceTextureSampler : m_FallbackSampler;
    irradiance_texture_image_info.imageView = effective_irradiance;
    irradiance_texture_image_info.imageLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    RHIDescriptorImageInfo specular_texture_image_info {};
    specular_texture_image_info.sampler = ibl.m_SpecularTextureSampler != nullptr ? ibl.m_SpecularTextureSampler : m_FallbackSampler;
    const bool use_fallback_specular = ibl.m_SpecularTextureImageView == nullptr;
    specular_texture_image_info.imageView = effective_specular;
    specular_texture_image_info.imageLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    if (use_fallback_specular)
    {
        static bool warned_fallback = false;
        if (!warned_fallback)
        {
            LOG_WARNING(ZRender,
                        "MainCameraRp1Pass: specular IBL cubemap not ready; deferred sky uses neutral fallback");
            warned_fallback = true;
        }
    }

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
    RHIViewport viewport {};
    RHIRect2D scissor {};
    ResolveViewportScissor(viewport_type, viewport, scissor);

    RHICommandBuffer* current_command_buffer = m_Rhi->GetCurrentCommandBuffer();
    m_Rhi->CmdSetViewportPFN(current_command_buffer, 0, 1, &viewport);
    m_Rhi->CmdSetScissorPFN(current_command_buffer, 0, 1, &scissor);
}

void MainCameraRp1Pass::ResolveViewportScissor(ViewportType viewport_type,
                                               RHIViewport& out_viewport,
                                               RHIRect2D& out_scissor)
{
    if (const RHIViewport* viewport = m_Rhi->GetViewport(viewport_type))
    {
        out_viewport = *viewport;
    }
    out_scissor = m_Rhi->GetSwapchainInfo().scissor[static_cast<uint32_t>(viewport_type)];

    if (viewport_type == ViewportType::scene)
    {
        if (auto* render_system = GET_SYSTEM(RenderSystem))
        {
            RHIViewport scene_viewport {};
            RHIRect2D scene_scissor {};
            if (render_system->TryGetRenderSceneViewport(scene_viewport, scene_scissor) &&
                scene_viewport.width > 0.0f && scene_viewport.height > 0.0f)
            {
                out_viewport = scene_viewport;
                out_scissor = scene_scissor;
            }
        }
    }
}

void MainCameraRp1Pass::SetPerViewportData(ViewportType viewport_type)
{
    if (m_PerFrameSource != nullptr)
    {
        m_MainCameraPerFrame =
            m_PerFrameSource->m_MainCameraPerFrameByViewport[static_cast<size_t>(viewport_type)];
    }
    else
    {
        m_MainCameraPerFrame = m_MainCameraPerFrameByViewport[static_cast<size_t>(viewport_type)];
    }

    if (RenderScene* render_scene = GET_SYSTEM(RenderSystem)->getRenderScene().get())
    {
        m_ActiveMainCameraVisibleMeshNodes = &render_scene->GetMainCameraVisibleMeshNodes(viewport_type);
    }
    else
    {
        m_ActiveMainCameraVisibleMeshNodes = m_VisiableNodes.p_main_camera_visible_mesh_nodes;
    }
}

RHIPipeline* MainCameraRp1Pass::GetOrCreateMeshGBufferPipeline(const GpuPBRMaterial& material)
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
                                                               m_FbResources->getGBufferRenderPass(),  // Pass GBufferPass render pass
                                                               0,                                            // Simplified: only 1 subpass
                                                               3,
                                                               false);
    m_Rhi->DestroyShaderModule(vert_shader_module);
    m_Rhi->DestroyShaderModule(frag_shader_module);
    m_MeshGbufferMaterialPipelines[pipeline_key] = material_pipeline != nullptr ? material_pipeline : default_pipeline;
    return material_pipeline != nullptr ? material_pipeline : default_pipeline;
}

RHIPipeline* MainCameraRp1Pass::GetOrCreateMeshTransparentPipeline(const GpuPBRMaterial& material)
{
    RHIPipeline* const default_pipeline = m_RenderPipelines[_render_pipeline_type_mesh_transparent].pipeline;
    const GpuShaderPassData* const transparent_pass = FindTransparentShaderPass(material);
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
                                                               m_FbResources->getForwardLightingRenderPass(),  // Pass ForwardLightingPass render pass
                                                               0,                                            // Simplified: only 1 subpass
                                                               1,
                                                               true);
    m_Rhi->DestroyShaderModule(vert_shader_module);
    m_Rhi->DestroyShaderModule(frag_shader_module);
    m_MeshTransparentMaterialPipelines[pipeline_key] =
        material_pipeline != nullptr ? material_pipeline : default_pipeline;
    return material_pipeline != nullptr ? material_pipeline : default_pipeline;
}

void MainCameraRp1Pass::RefreshNaniteInjection()
{
    if (m_NaniteRenderer == nullptr || m_FbResources == nullptr)
    {
        return;
    }

    RHIRenderPass* render_pass = m_FbResources->getGBufferRenderPass();
    RHIFramebuffer* framebuffer = m_FbResources->getGBufferFramebuffer();
    m_NaniteRenderer->SetRasterRenderPass(render_pass, framebuffer);
}

void MainCameraRp1Pass::DrawGBufferPass(const std::array<bool, 2>& skybox_visible)
{
    // G-buffer pass: writes GBufferA, GBufferB, GBufferC, Depth.
    RHIRenderPassBeginInfo renderpass_begin_info {};
    renderpass_begin_info.sType = RHI_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderpass_begin_info.renderPass = m_FbResources->getGBufferRenderPass();
    renderpass_begin_info.framebuffer = m_FbResources->getGBufferFramebuffer();
    renderpass_begin_info.renderArea.offset = {0, 0};
    renderpass_begin_info.renderArea.extent = m_Rhi->GetSwapchainInfo().extent;

    RHIClearValue clear_values[4];
    clear_values[0].color = {{0.0f, 0.0f, 0.0f, 0.0f}};  // GBufferA
    clear_values[1].color = {{0.0f, 0.0f, 0.0f, 0.0f}};  // GBufferB
    clear_values[2].color = {{0.0f, 0.0f, 0.0f, 0.0f}};  // GBufferC
    clear_values[3].depthStencil = {1.0f, 0};                 // Depth
    renderpass_begin_info.clearValueCount = 4;
    renderpass_begin_info.pClearValues = clear_values;

    m_Rhi->CmdBeginRenderPassPFN(m_Rhi->GetCurrentCommandBuffer(), &renderpass_begin_info, RHI_SUBPASS_CONTENTS_INLINE);

    // ---- Nanite 的注入 --------------------------------------------------------
    //
    // Nanite 的剔除在帧里更早的位置跑（RenderNaniteCmd → UpdateNanite），那时一个
    // pass 都还没开，它只能把间接命令写好。绘制必须发生在**某个 GBuffer pass 开着
    // 的时候** —— 也就是这里。
    //
    // 注入是**跨帧保持**的，不是"这一帧画完就解掉"：下一帧 Nanite 的 Render() 会比
    // 这个 pass 更早执行，它在那里看到"已注入"就会跳过自己的离屏 target —— 那条路
    // 是 BeginSingleTimeCommands + 提交 + 等待，等于每帧一次全量 GPU 同步。代价是
    // 第一帧（以及 GBuffer pass 不跑的那些帧）Nanite 可能画进没人读的离屏图，这比
    // 每帧一次全量同步划算。只在 Shutdown 时解掉。
    if (m_NaniteRenderer != nullptr)
    {
        m_NaniteRenderer->SetRasterRenderPass(renderpass_begin_info.renderPass,
                                             renderpass_begin_info.framebuffer);
    }

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
        m_MainCameraPerFrame.show_skybox =
            skybox_visible[static_cast<size_t>(viewport_type)] ? 1U : 0U;
        DrawMeshGbuffer(viewport_type);

        // Nanite 只在**游戏视口**里画：它的剔除用的就是 GetRenderCamera(game) 那台
        // 相机，拿同一批可见集群去画编辑视口会得到按别的视锥裁过的结果。
        //
        // 视口 / scissor 是 DrawMeshGbuffer 刚设好的（SetViewportScissor），Nanite
        // 不许碰 —— DrawClustersIndirectInPass 只追加绘制，不改任何 pass 状态。
        if (viewport_type == ViewportType::game && m_NaniteRenderer != nullptr)
        {
            m_NaniteRenderer->DrawClustersIndirectInPass(m_Rhi->GetCurrentCommandBuffer());
        }
    }
    m_Rhi->PopEvent(m_Rhi->GetCurrentCommandBuffer());

    m_Rhi->CmdEndRenderPassPFN(m_Rhi->GetCurrentCommandBuffer());
}

void MainCameraRp1Pass::DrawDeferredLightingPass(const std::array<bool, 2>& skybox_visible)
{
    // Deferred Lighting pass: reads G-Buffer + Depth (as input attachments), writes BackupOdd.
    // Includes Sky Pass (draws sky mesh).

    // Refresh G-Buffer input attachments (they were written by GBufferPass).
    RefreshDeferredLightingInputAttachments();
    RefreshMeshGlobalIblDescriptors();

    RHIRenderPassBeginInfo renderpass_begin_info {};
    renderpass_begin_info.sType = RHI_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderpass_begin_info.renderPass = m_FbResources->getDeferredLightingRenderPass();
    renderpass_begin_info.framebuffer = m_FbResources->getDeferredLightingFramebuffer();
    renderpass_begin_info.renderArea.offset = {0, 0};
    renderpass_begin_info.renderArea.extent = m_Rhi->GetSwapchainInfo().extent;

    constexpr float k_scene_clear_r = 0.29f;
    constexpr float k_scene_clear_g = 0.345f;
    constexpr float k_scene_clear_b = 0.435f;

    // ⚠️ clear 值按 framebuffer 附件**下标**取（CmdBeginRenderPass 用 pClearValues[i]，
    // i 是 framebuffer 附件下标，不是颜色槽下标）。Metal 的 deferred framebuffer 只有两个
    // 附件：[0]=BackupOdd、[1]=Depth（见 MainCameraFramebufferResources::SetupFramebuffers
    // 的 Metal 分支），所以场景清屏色必须在 [0]。
    const bool is_metal = (m_Rhi != nullptr && m_Rhi->getGraphicsAPI() == GraphicsAPI::Metal);
    RHIClearValue clear_values_metal[2];
    RHIClearValue clear_values[5];
    if (is_metal)
    {
        clear_values_metal[0].color = {{k_scene_clear_r, k_scene_clear_g, k_scene_clear_b, 1.0f}};  // BackupOdd (clear)
        clear_values_metal[1].depthStencil = {1.0f, 0};                                            // Depth (load)
        renderpass_begin_info.clearValueCount = 2;
        renderpass_begin_info.pClearValues = clear_values_metal;
    }
    else
    {
        clear_values[0].color = {{0.0f, 0.0f, 0.0f, 0.0f}};  // GBufferA (load)
        clear_values[1].color = {{0.0f, 0.0f, 0.0f, 0.0f}};  // GBufferB (load)
        clear_values[2].color = {{0.0f, 0.0f, 0.0f, 0.0f}};  // GBufferC (load)
        clear_values[3].depthStencil = {1.0f, 0};                 // Depth (load)
        clear_values[4].color = {{k_scene_clear_r, k_scene_clear_g, k_scene_clear_b, 1.0f}};  // BackupOdd (clear)
        renderpass_begin_info.clearValueCount = 5;
        renderpass_begin_info.pClearValues = clear_values;
    }

    m_Rhi->CmdBeginRenderPassPFN(m_Rhi->GetCurrentCommandBuffer(), &renderpass_begin_info, RHI_SUBPASS_CONTENTS_INLINE);

    constexpr ViewportType k_viewports[] = {ViewportType::game, ViewportType::scene};
    float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};

    // Deferred Lighting (must run before Sky Pass).
    m_Rhi->PushEvent(m_Rhi->GetCurrentCommandBuffer(), "Deferred Lighting", color);
    for (ViewportType viewport_type : k_viewports)
    {
        if (!IsViewportValid(viewport_type))
        {
            continue;
        }
        SetPerViewportData(viewport_type);
        m_MainCameraPerFrame.show_skybox =
            skybox_visible[static_cast<size_t>(viewport_type)] ? 1U : 0U;
        DrawDeferredLighting(viewport_type);
    }
    m_Rhi->PopEvent(m_Rhi->GetCurrentCommandBuffer());

    // Sky Pass (draws sky mesh).
    m_Rhi->PushEvent(m_Rhi->GetCurrentCommandBuffer(), "Sky Pass", color);
    for (ViewportType viewport_type : k_viewports)
    {
        if (!IsViewportValid(viewport_type))
        {
            continue;
        }
        SetPerViewportData(viewport_type);
        m_MainCameraPerFrame.show_skybox =
            skybox_visible[static_cast<size_t>(viewport_type)] ? 1U : 0U;
        DrawSkyMeshPass(viewport_type, skybox_visible[static_cast<size_t>(viewport_type)]);
    }
    m_Rhi->PopEvent(m_Rhi->GetCurrentCommandBuffer());

    m_Rhi->CmdEndRenderPassPFN(m_Rhi->GetCurrentCommandBuffer());
}

void MainCameraRp1Pass::DrawForwardLightingPass(const std::array<bool, 2>& skybox_visible)
{
    // Forward Lighting pass: reads/writes BackupOdd (transparent objects), reads Depth.
    RHIRenderPassBeginInfo renderpass_begin_info {};
    renderpass_begin_info.sType = RHI_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderpass_begin_info.renderPass = m_FbResources->getForwardLightingRenderPass();
    renderpass_begin_info.framebuffer = m_FbResources->getForwardLightingFramebuffer();
    renderpass_begin_info.renderArea.offset = {0, 0};
    renderpass_begin_info.renderArea.extent = m_Rhi->GetSwapchainInfo().extent;

    RHIClearValue clear_values[2];
    clear_values[0].color = {{0.0f, 0.0f, 0.0f, 0.0f}};  // BackupOdd (load)
    clear_values[1].depthStencil = {1.0f, 0};                 // Depth (load)
    renderpass_begin_info.clearValueCount = 2;
    renderpass_begin_info.pClearValues = clear_values;

    m_Rhi->CmdBeginRenderPassPFN(m_Rhi->GetCurrentCommandBuffer(), &renderpass_begin_info, RHI_SUBPASS_CONTENTS_INLINE);

    constexpr ViewportType k_viewports[] = {ViewportType::game, ViewportType::scene};
    float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};

    m_Rhi->PushEvent(m_Rhi->GetCurrentCommandBuffer(), "Forward Lighting", color);
    for (ViewportType viewport_type : k_viewports)
    {
        if (!IsViewportValid(viewport_type))
        {
            continue;
        }
        SetPerViewportData(viewport_type);
        m_MainCameraPerFrame.show_skybox =
            skybox_visible[static_cast<size_t>(viewport_type)] ? 1U : 0U;
        DrawMeshTransparent(viewport_type);
        // axis 挂在 forward 这一站：它是最后开的那个 pass，深度测试关掉、写进
        // BackupOdd，gizmo 因此永远盖在场景之上。
        DrawAxis(viewport_type);
    }
    // 粒子的 billboard 也画在 forward 这一站（和 Vulkan 的 MainCameraPass 一致：
    // 那里也是 forward lighting 的 viewport 循环之后调 `m_ParticlePass->Draw()`）。
    // 它自己内部会按 game / scene 两个视口各画一遍，所以这里只在 pass 级调一次。
    if (m_ParticlePass != nullptr)
    {
        m_ParticlePass->Draw();
    }
#if !defined(ZENGINE_SHARED_NO_SPINE)
    // GPU-Spine 世界实例（SpineRenderer 组件）：透明物体同一站 —— 测深度、不写深度。只画一个视口
    // （Scene 开着画 Scene，否则 Game）；视口取法与 SetViewportScissor 完全一致（Scene 走 render rect），
    // 否则深度和网格对不上。DX12 / Apple 都走这里（Vulkan 编辑器走 MainCameraPass::DrawRp1）。
    {
        const ViewportType spine_viewport =
            IsViewportValid(ViewportType::scene) ? ViewportType::scene : ViewportType::game;
        if (IsViewportValid(spine_viewport))
        {
            RHIViewport viewport = *m_Rhi->GetViewport(spine_viewport);
            if (spine_viewport == ViewportType::scene)
            {
                if (auto* render_system = GET_SYSTEM(RenderSystem))
                {
                    RHIViewport scene_viewport {};
                    RHIRect2D scene_scissor {};
                    if (render_system->TryGetRenderSceneViewport(scene_viewport, scene_scissor) &&
                        scene_viewport.width > 0.0f && scene_viewport.height > 0.0f)
                    {
                        viewport = scene_viewport;
                    }
                }
            }
            SpineGpuRenderer::Get().DrawWorld(m_Rhi, m_FbResources->getForwardLightingRenderPass(), 0, viewport,
                                              spine_viewport);
        }
    }
#endif  // !ZENGINE_SHARED_NO_SPINE
    m_Rhi->PopEvent(m_Rhi->GetCurrentCommandBuffer());

    m_Rhi->CmdEndRenderPassPFN(m_Rhi->GetCurrentCommandBuffer());
}

// =====================================================================================
// Render graph path (ZENGINE_RG_RP1)
// =====================================================================================

void MainCameraRp1Pass::SetupRenderGraphPath()
{
    // Default ON: the render-graph RP1 path was verified bit-identical to the hand-written
    // chain by the in-frame A/B capture (DX12RenderGraphAbCapture + scripts/rg_ab_compare.py)
    // on 2026-10-26 -- rp1.backup_odd readbacks matched to the byte, zero debug-layer
    // errors. Set ZENGINE_RG_RP1=0 to force the hand-written chain (kept as the fallback
    // and for A/B comparison; the capture drives both paths directly regardless of this).
    const char* env = std::getenv("ZENGINE_RG_RP1");
    const bool disabled = (env != nullptr && env[0] == '0');
    m_UseRenderGraph = false;
    if (disabled)
    {
        return;
    }

    if (!SetRenderGraphPathEnabled(true))
    {
        LOG_WARNING(ZRender,
                    "MainCameraRp1Pass: render graph RP1 path is DX12-only; "
                    "keeping the hand-written RP1 chain");
        return;
    }
    LOG_INFO(ZRender, "MainCameraRp1Pass: RP1 is driven by the render graph");
}

bool MainCameraRp1Pass::SetRenderGraphPathEnabled(bool enabled)
{
    if (!enabled)
    {
        // Keep the graph objects: the A/B capture flips this off and back on in one frame.
        m_UseRenderGraph = false;
        return true;
    }

    // DX12 only. MainCameraRp1Pass also runs on Metal, but Metal's CmdPipelineBarrier is a
    // no-op (it tracks hazards per encoder), so the graph would add nothing there. And the
    // RP1 import states rely on DX12RHI::CmdPipelineBarrier transitioning from its OWN
    // tracked state (see MainCameraRp1GraphDecl.h) -- not something to assume elsewhere.
    if (m_Rhi == nullptr || m_Rhi->getGraphicsAPI() != GraphicsAPI::DirectX12)
    {
        m_UseRenderGraph = false;
        return false;
    }

    if (m_Rp1Graph == nullptr)
    {
        m_RgDevice = std::make_unique<RGRhiDeviceAdapter>(m_Rhi);
        m_RgPool = std::make_unique<RenderGraphResourcePool>(m_RgDevice.get());
        m_RgExecutor = std::make_unique<RenderGraphExecutor>(m_Rhi, m_RgDevice.get(), m_RgPool.get());
        m_Rp1Graph = std::make_unique<RenderGraph>("RP1");
        m_RgFallbackLogged = false;
    }
    m_UseRenderGraph = true;
    return true;
}

bool MainCameraRp1Pass::DrawRP1WithGraph(const std::array<bool, 2>& skybox_visible)
{
    if (!m_UseRenderGraph || m_RgExecutor == nullptr || m_Rp1Graph == nullptr || m_FbResources == nullptr)
    {
        return false;
    }

    RHICommandBuffer* cmd = m_Rhi->GetCurrentCommandBuffer();
    if (cmd == nullptr)
    {
        return false;
    }

    // ---- validate before recording ---------------------------------------------------
    // Everything that can reject the graph path is checked here. Once Execute() starts
    // recording there is no way back: the caller's fallback would append the whole
    // hand-written chain after whatever the graph already put in the buffer.
    const auto reject_once = [this](const char* why) {
        if (!m_RgFallbackLogged)
        {
            m_RgFallbackLogged = true;
            LOG_WARNING(ZRender, "MainCameraRp1Pass: {}, falling back to the hand-written RP1 chain", why);
        }
        return false;
    };

    if (m_FbResources->getGBufferRenderPass() == nullptr || m_FbResources->getGBufferFramebuffer() == nullptr ||
        m_FbResources->getDeferredLightingRenderPass() == nullptr ||
        m_FbResources->getDeferredLightingFramebuffer() == nullptr ||
        m_FbResources->getForwardLightingRenderPass() == nullptr ||
        m_FbResources->getForwardLightingFramebuffer() == nullptr)
    {
        return reject_once("an RP1 render pass / framebuffer is missing");
    }

    const auto external = [this](uint32_t attachment, RGFormat format) {
        RGExternalTexture texture {};
        texture.image = m_FbResources->getAttachmentImage(attachment);
        texture.view = m_FbResources->getAttachmentView(attachment);
        texture.format = format;
        return texture;
    };

    Rp1GraphTextures textures {};
    textures.gbuffer_a = external(_main_camera_pass_gbuffer_a, RGFormat::R8G8B8A8_UNORM);
    textures.gbuffer_b = external(_main_camera_pass_gbuffer_b, RGFormat::R8G8B8A8_UNORM);
    textures.gbuffer_c = external(_main_camera_pass_gbuffer_c, RGFormat::R8G8B8A8_SRGB);
    textures.backup_odd = external(_main_camera_pass_backup_buffer_odd, RGFormat::R16G16B16A16_SFLOAT);
    if (textures.gbuffer_a.image == nullptr || textures.gbuffer_b.image == nullptr ||
        textures.gbuffer_c.image == nullptr || textures.backup_odd.image == nullptr)
    {
        return reject_once("a GBuffer / BackupOdd attachment exposes no RHIImage");
    }

    // Scene depth is tracked only when the backend publishes an image for it. DX12's
    // GetDepthImageInfo() currently does not (depth_image == nullptr), so the graph leaves
    // depth entirely to the render passes -- exactly what the hand-written chain does,
    // whose explicit depth barrier hits the same null image and is dropped by the RHI.
    const RHIDepthImageDesc depth_info = m_Rhi->GetDepthImageInfo();
    if (depth_info.depth_image != nullptr)
    {
        textures.depth.image = depth_info.depth_image;
        textures.depth.view = depth_info.depth_image_view;
        switch (depth_info.depth_image_format)
        {
            case RHI_FORMAT_D24_UNORM_S8_UINT:
                textures.depth.format = RGFormat::D24_UNORM_S8_UINT;
                break;
            case RHI_FORMAT_D32_SFLOAT:
                textures.depth.format = RGFormat::D32_SFLOAT;
                break;
            default:
                // A depth format the graph has no enum for (e.g. D32S8) would get a COLOR
                // aspect barrier; refuse rather than emit an invalid one.
                return reject_once("the scene depth format is not representable in the render graph");
        }
    }

    textures = MakeRp1GraphImportStates(textures);

    // ---- declare ---------------------------------------------------------------------
    // The callbacks are the very same Draw*Pass() functions the hand-written chain calls,
    // in the same order; each one begins and ends its own render pass. The graph only
    // inserts its barriers between them.
    m_Rp1Graph->Reset();
    Rp1GraphCallbacks callbacks;
    callbacks.gbuffer = [this, &skybox_visible](RenderGraphExecuteContext&) { DrawGBufferPass(skybox_visible); };
    callbacks.deferred_lighting = [this, &skybox_visible](RenderGraphExecuteContext&) {
        DrawDeferredLightingPass(skybox_visible);
    };
    callbacks.forward_lighting = [this, &skybox_visible](RenderGraphExecuteContext&) {
        DrawForwardLightingPass(skybox_visible);
    };
    DeclareRp1Graph(*m_Rp1Graph, textures, std::move(callbacks));

    // The render passes record their own GPU events ("BasePass", "Deferred Lighting", ...);
    // the executor's per-pass markers would only nest a duplicate level around them.
    m_RgExecutor->SetDebugEventsEnabled(false);

    const RGCompiledGraph& compiled = m_Rp1Graph->Compile();
    if (!compiled.valid)
    {
        if (!m_RgFallbackLogged)
        {
            m_RgFallbackLogged = true;
            LOG_ERROR(ZRender,
                      "MainCameraRp1Pass: the RP1 render graph is invalid, falling back to the hand-written path");
            for (const std::string& error : compiled.errors)
            {
                LOG_ERROR(ZRender, "  render graph: {}", error);
            }
        }
        return false;
    }

    // Execute() can only fail before it records anything (no device, no pool, invalid
    // graph -- the last of which was just ruled out above), so falling back here is safe.
    if (!m_RgExecutor->Execute(*m_Rp1Graph, cmd))
    {
        LOG_ERROR(ZRender,
                  "MainCameraRp1Pass: render graph execution failed ({}), falling back to the hand-written path",
                  m_RgExecutor->GetLastError());
        return false;
    }
    return true;
}

void MainCameraRp1Pass::SetupParticlePass()
{
    // 和 Vulkan 的 `MainCameraPass::SetupParticlePass()` 一一对应：
    //   · 深度图 → 粒子碰撞用的深度
    //   · GBufferA → 法线（Vulkan 那边传的是 main_camera 自己 framebuffer 的
    //     `_main_camera_pass_gbuffer_a`，这里就是同一张图，只是从 RP1 的
    //     framebuffer 资源里取）
    //   · forward lighting 的 render pass → billboard 画在这一站里
    if (m_ParticlePass == nullptr || m_FbResources == nullptr)
    {
        LOG_WARNING(ZRender,
                    "MainCameraRp1Pass::SetupParticlePass: skipped (particle pass or framebuffer resources "
                    "missing) -- particles stay disabled");
        return;
    }

    // ⚠️ 这里有两个同名函数，极易漏掉其中一个：
    //   · `ParticlePass::SetupParticlePass()` —— pass 自己的重活：建 uniform buffer
    //     （`m_SceneUniformBufferMapped` / `m_ParticleBillboardUniformBufferMapped`
    //     就是在这里映射出来的）、descriptor layout、三条 compute pipeline、深度/法线
    //     副本图、命令缓冲、fence。
    //   · 本函数 —— 只是把 RP1 的深度图 / GBufferA / forward render pass 递过去。
    //
    // 只做第二件是**不够的**：漏掉第一件时 `Draw()` 会往两个 nullptr 里 memcpy
    // （SIGSEGV / KERN_INVALID_ADDRESS at 0x0，栈落在 `_platform_memmove`）。这个
    // 崩溃我实打实踩过一次，所以两件都放在这里，外面只调一次。
    m_ParticlePass->SetupParticlePass();

    m_ParticlePass->SetDepthAndNormalImage(m_Rhi->GetDepthImageInfo().depth_image,
                                           m_FbResources->getAttachmentImage(_main_camera_pass_gbuffer_a));
    m_ParticlePass->SetRenderPassHandle(m_FbResources->getForwardLightingRenderPass());

    LOG_INFO(ZRender, "MainCameraRp1Pass: particle pass wired into the forward lighting render pass");
}

void MainCameraRp1Pass::DrawAxis(ViewportType viewport_type)
{
    // 编辑器 gizmo（axis）的绘制。
    //
    // 两半必须都在：上传（`RenderSystem::CreateAxis`，已经补成平台无关实现）+ 这里的
    // 绘制。以前 Apple 上两半都没有 —— 编辑器里选中物体拿不到坐标轴，而且不报任何错。
    //
    // 每一次提前返回都记一条**一次性**日志：gizmo 不出现的可能原因有五六个（没开、
    // 管线没建起来、没选中物体、视口是 0、mesh 没上传），不分开打就永远只能靠猜。
    if (!m_IsShowAxis)
    {
        return;
    }

    auto skip = [this](const char* reason) {
        if (!m_AxisSkippedLogged)
        {
            m_AxisSkippedLogged = true;
            LOG_WARNING(ZRender, "DrawAxis: skipped ({})", reason);
        }
    };

    if (m_RenderPipelines[_render_pipeline_type_axis].pipeline == nullptr ||
        m_DescriptorInfos[_axis].descriptor_set == nullptr)
    {
        skip("axis pipeline / descriptor set unavailable");
        return;
    }
    if (m_VisiableNodes.p_axis_node == nullptr)
    {
        skip("no axis node (nothing selected)");
        return;
    }

    RenderSystem* render_system = GET_SYSTEM(RenderSystem);
    if (render_system == nullptr)
    {
        skip("RenderSystem unavailable");
        return;
    }
    const std::shared_ptr<RenderScene>& render_scene = render_system->getRenderScene();
    if (render_scene == nullptr || !render_scene->m_RenderAxis.has_value())
    {
        skip("no axis entity was ever submitted through RenderSystem::SetVisibleAxis");
        return;
    }

    // axis 只画在编辑器场景视口上。这个视口是 0x0 是**正常**的：Scene 和 Game 是
    // 同一个 tab 组的两个 tab，只有当前激活的那个才有 rect。激活的是 Game 时这里
    // 就该跳过 —— 但要说出来，否则"gizmo 不显示"和"gizmo 代码没跑"在日志里一样安静。
    RHIViewport scene_viewport {};
    RHIRect2D   scene_scissor {};
    if (!render_system->TryGetRenderSceneViewport(scene_viewport, scene_scissor) ||
        scene_viewport.width <= 0.0f || scene_viewport.height <= 0.0f)
    {
        if (!m_AxisSkippedLogged)
        {
            m_AxisSkippedLogged = true;
            LOG_WARNING(ZRender,
                        "DrawAxis: skipped (scene viewport is {:.0f}x{:.0f} -- the Scene panel is not the active tab, "
                        "so there is nowhere to draw the gizmo)",
                        scene_viewport.width,
                        scene_viewport.height);
        }
        return;
    }

    const MeshDrawData axis_mesh = getMeshDrawData(m_VisiableNodes.p_axis_node->ref_mesh);
    if (!axis_mesh)
    {
        skip("axis mesh was never uploaded to the GPU (RenderSystem::CreateAxis)");
        return;
    }

    SetPerViewportData(viewport_type);
    if (viewport_type == ViewportType::scene)
    {
        m_MainCameraPerFrame.viewport_rect =
            Vector4(scene_viewport.x, scene_viewport.y, scene_viewport.width, scene_viewport.height);
    }

    const uint32_t perframe_dynamic_offset = AlignPerFrameRingOffset(
        m_Rhi,
        m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbuffersEnd[m_Rhi->GetCurrentFrameIndex()],
        m_GlobalRenderResource->m_StorageBuffer.m_MinStorageBufferOffsetAlignment);
    m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbuffersEnd[m_Rhi->GetCurrentFrameIndex()] =
        perframe_dynamic_offset + PerFrameUploadRingStrideBytes(m_Rhi, perframe_dynamic_offset);

    (*reinterpret_cast<MainCameraPerFrame*>(
        reinterpret_cast<uintptr_t>(m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbufferMemoryPointer) +
        perframe_dynamic_offset)) = m_MainCameraPerFrame;

    m_AxisDrawStorage.selected_axis = m_SelectedAxis;
    m_AxisDrawStorage.model_matrix  = m_VisiableNodes.p_axis_node->model_matrix;
    (*reinterpret_cast<AxisDrawStorage*>(
        reinterpret_cast<uintptr_t>(
            m_GlobalRenderResource->m_StorageBuffer.m_AxisInefficientStorageBufferMemoryPointer))) =
        m_AxisDrawStorage;

    float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    m_Rhi->PushEvent(m_Rhi->GetCurrentCommandBuffer(), "Axis", color);

    m_Rhi->CmdBindPipelinePFN(
        m_Rhi->GetCurrentCommandBuffer(), RHI_PIPELINE_BIND_POINT_GRAPHICS,
        m_RenderPipelines[_render_pipeline_type_axis].pipeline);
    SetViewportScissor(viewport_type);
    m_Rhi->CmdBindDescriptorSetsPFN(m_Rhi->GetCurrentCommandBuffer(),
                                    RHI_PIPELINE_BIND_POINT_GRAPHICS,
                                    m_RenderPipelines[_render_pipeline_type_axis].layout,
                                    0,
                                    1,
                                    &m_DescriptorInfos[_axis].descriptor_set,
                                    1,
                                    &perframe_dynamic_offset);

    RHIBuffer* vertex_buffers[3] = {axis_mesh.position_buffer,
                                    axis_mesh.varying_blending_buffer,
                                    axis_mesh.varying_buffer};
    RHIDeviceSize offsets[3]     = {0, 0, 0};
    m_Rhi->CmdBindVertexBuffersPFN(m_Rhi->GetCurrentCommandBuffer(),
                                   0,
                                   static_cast<uint32_t>(sizeof(vertex_buffers) / sizeof(vertex_buffers[0])),
                                   vertex_buffers,
                                   offsets);
    m_Rhi->CmdBindIndexBufferPFN(
        m_Rhi->GetCurrentCommandBuffer(), axis_mesh.index_buffer, 0, RHI_INDEX_TYPE_UINT16);

    m_Rhi->CmdDrawIndexedPFN(m_Rhi->GetCurrentCommandBuffer(), axis_mesh.index_count, 1, 0, 0, 0);

    m_Rhi->PopEvent(m_Rhi->GetCurrentCommandBuffer());

    if (!m_AxisDrawnLogged)
    {
        m_AxisDrawnLogged = true;
        LOG_INFO(ZRender,
                 "DrawAxis: editor gizmo drawn (indices={} selected={} viewport={:.0f}x{:.0f})",
                 axis_mesh.index_count,
                 m_SelectedAxis,
                 scene_viewport.width,
                 scene_viewport.height);
    }
}

void MainCameraRp1Pass::DrawSkyMeshPass(ViewportType viewport_type, bool skybox_visible)
{
    
    if (!skybox_visible || m_Rhi->getGraphicsAPI() != GraphicsAPI::DirectX12)
    {
        return;
    }

    const RenderScene* render_scene = GET_SYSTEM(RenderSystem)->getRenderScene().get();
    static const std::vector<RenderMeshNode> k_empty_sky_nodes;
    const std::vector<RenderMeshNode>& sky_nodes =
        render_scene ? render_scene->GetMainCameraSkyMeshNodes(viewport_type) : k_empty_sky_nodes;

    // Refresh from RenderResource immediately before ring-buffer upload (DrawRP1 also calls this).
    SetPerViewportData(viewport_type);
    m_MainCameraPerFrame.show_skybox = skybox_visible ? 1U : 0U;
    SetViewportScissor(viewport_type);

    if (viewport_type == ViewportType::scene)
    {
        if (auto* render_system = GET_SYSTEM(RenderSystem))
        {
            RHIViewport scene_viewport {};
            RHIRect2D scene_scissor {};
            if (render_system->TryGetRenderSceneViewport(scene_viewport, scene_scissor) &&
                scene_viewport.width > 0.0f && scene_viewport.height > 0.0f)
            {
                m_MainCameraPerFrame.viewport_rect =
                    Vector4(scene_viewport.x, scene_viewport.y, scene_viewport.width, scene_viewport.height);
            }
        }
    }

    const uint32_t perframe_dynamic_offset = AlignPerFrameRingOffset(
        m_Rhi,
        m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbuffersEnd[m_Rhi->GetCurrentFrameIndex()],
        m_GlobalRenderResource->m_StorageBuffer.m_MinStorageBufferOffsetAlignment);
    m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbuffersEnd[m_Rhi->GetCurrentFrameIndex()] =
        perframe_dynamic_offset + PerFrameUploadRingStrideBytes(m_Rhi, perframe_dynamic_offset);

    (*reinterpret_cast<MainCameraPerFrame*>(
        reinterpret_cast<uintptr_t>(m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbufferMemoryPointer) +
        perframe_dynamic_offset)) = m_MainCameraPerFrame;

    if (!sky_nodes.empty())
    {
        struct MeshNode
        {
            const Matrix4x4* model_matrix {nullptr};
            const Matrix4x4* joint_matrices {nullptr};
            uint32_t joint_count {0};
        };

        std::map<GpuMesh*, std::vector<MeshNode>> draw_batches;
        for (const RenderMeshNode& node : sky_nodes)
        {
            auto& mesh_nodes = draw_batches[AsGpuMesh(node.ref_mesh)];
            MeshNode temp;
            temp.model_matrix = node.model_matrix;
            if (node.enable_vertex_blending)
            {
                temp.joint_matrices = node.joint_matrices;
                temp.joint_count = node.joint_count;
            }
            mesh_nodes.push_back(temp);
        }

        RHIPipeline* sky_mesh_pipeline = m_RenderPipelines[_render_pipeline_type_sky_mesh].pipeline;
        RHIPipelineLayout* pipeline_layout = m_RenderPipelines[_render_pipeline_type_sky_mesh].layout;
        
        if (sky_mesh_pipeline == nullptr)
        {
            LOG_ERROR(ZRender, "DrawSkyMeshPass: sky_mesh_pipeline is null!");
            return;
        }
        
        m_Rhi->CmdBindPipelinePFN(m_Rhi->GetCurrentCommandBuffer(), RHI_PIPELINE_BIND_POINT_GRAPHICS, sky_mesh_pipeline);

        for (auto& mesh_batch : draw_batches)
        {
            GpuMesh& mesh = *mesh_batch.first;
            auto& mesh_nodes = mesh_batch.second;

            const uint32_t total_instance_count = static_cast<uint32_t>(mesh_nodes.size());
            if (total_instance_count == 0)
            {
                continue;
            }

            const MeshDrawData mesh_draw = getMeshDrawData(&mesh);
            if (!mesh_draw)
            {
                LOG_ERROR(ZRender, "DrawSkyMeshPass: mesh_draw invalid for mesh={:016X}", (uint64_t)(uintptr_t)&mesh);
                continue;
            }

            // Must bind binding 0 (_mesh_global) BEFORE binding binding 1.
            // In DX12, all descriptor tables referenced by the root signature must be bound,
            // otherwise GPU resource access faults cause the draw call to be skipped.
            // Binding 0 provides access to: per-frame UBO (b0), IBL cubemaps (t3-t7), etc.
            uint32_t perframe_dynamic_offset = AlignPerFrameRingOffset(
                m_Rhi,
                m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbuffersEnd[m_Rhi->GetCurrentFrameIndex()],
                m_GlobalRenderResource->m_StorageBuffer.m_MinStorageBufferOffsetAlignment);
            m_Rhi->CmdBindDescriptorSetsPFN(m_Rhi->GetCurrentCommandBuffer(),
                                            RHI_PIPELINE_BIND_POINT_GRAPHICS,
                                            pipeline_layout,
                                            0,
                                            1,
                                            &m_DescriptorInfos[_mesh_global].descriptor_set,
                                            1,
                                            &perframe_dynamic_offset);

            m_Rhi->CmdBindDescriptorSetsPFN(m_Rhi->GetCurrentCommandBuffer(),
                                            RHI_PIPELINE_BIND_POINT_GRAPHICS,
                                            pipeline_layout,
                                            1,
                                            1,
                                            &mesh.mesh_vertex_blending_descriptor_set,
                                            0,
                                            nullptr);

            // DX12: must bind ALL descriptor sets in the root signature.
            // The pipeline layout has 3 sets (0/1/2). Set 2 (_mesh_per_material)
            // must be bound even if the sky shader doesn't use per-material data,
            // otherwise the DX12 root signature is incomplete and the draw call
            // is silently skipped.
            m_Rhi->CmdBindDescriptorSetsPFN(m_Rhi->GetCurrentCommandBuffer(),
                                            RHI_PIPELINE_BIND_POINT_GRAPHICS,
                                            pipeline_layout,
                                            2,
                                            1,
                                            &m_DescriptorInfos[_mesh_per_material].descriptor_set,
                                            0,
                                            nullptr);

            RHIBuffer* vertex_buffers[] = {mesh_draw.position_buffer,
                                           mesh_draw.varying_blending_buffer,
                                           mesh_draw.varying_buffer};
            RHIDeviceSize offsets[] = {0, 0, 0};
            m_Rhi->CmdBindVertexBuffersPFN(m_Rhi->GetCurrentCommandBuffer(),
                                           0,
                                           static_cast<uint32_t>(sizeof(vertex_buffers) / sizeof(vertex_buffers[0])),
                                           vertex_buffers,
                                           offsets);
            m_Rhi->CmdBindIndexBufferPFN(
                m_Rhi->GetCurrentCommandBuffer(), mesh_draw.index_buffer, 0, RHI_INDEX_TYPE_UINT16);

            const uint32_t drawcall_max_instance_count =
                static_cast<uint32_t>(sizeof(MeshDrawPerDrawcall::mesh_instances) /
                                      sizeof(MeshDrawPerDrawcall::mesh_instances[0]));
            const uint32_t drawcall_count =
                RoundUp(total_instance_count, drawcall_max_instance_count) / drawcall_max_instance_count;

            for (uint32_t drawcall_index = 0; drawcall_index < drawcall_count; ++drawcall_index)
            {
                const uint32_t current_instance_count =
                    ((total_instance_count - drawcall_max_instance_count * drawcall_index) < drawcall_max_instance_count)
                        ? (total_instance_count - drawcall_max_instance_count * drawcall_index)
                        : drawcall_max_instance_count;

                const uint32_t perdrawcall_dynamic_offset =
                    drawcall_index == 0
                        ? MeshPerDrawcallOffsetAfterPerFrame(m_Rhi, perframe_dynamic_offset)
                        : RoundUp(m_GlobalRenderResource->m_StorageBuffer
                                      .m_GlobalUploadRingbuffersEnd[m_Rhi->GetCurrentFrameIndex()],
                                  m_GlobalRenderResource->m_StorageBuffer.m_MinStorageBufferOffsetAlignment);
                if (m_Rhi->getGraphicsAPI() == GraphicsAPI::DirectX12)
                {
                    assert((perdrawcall_dynamic_offset % kDx12MeshInstanceStructBytes) == 0);
                    assert(perdrawcall_dynamic_offset ==
                           Dx12MeshSlotOffsetAfterPerFrame(perframe_dynamic_offset));
                }
                m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbuffersEnd[m_Rhi->GetCurrentFrameIndex()] =
                    perdrawcall_dynamic_offset + sizeof(MeshDrawPerDrawcall);

                RenderMeshInstance* const mesh_instances =
                    reinterpret_cast<RenderMeshInstance*>(reinterpret_cast<uintptr_t>(
                        m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbufferMemoryPointer) +
                        perdrawcall_dynamic_offset);
                for (uint32_t instance_index = 0; instance_index < current_instance_count; ++instance_index)
                {
                    const uint32_t node_index = drawcall_max_instance_count * drawcall_index + instance_index;
                    RenderMeshInstance& instance = mesh_instances[instance_index];
                    instance.enable_vertex_blending = mesh_nodes[node_index].joint_matrices ? 1.0f : -1.0f;
                    instance.model_matrix = *mesh_nodes[node_index].model_matrix;
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
                        per_drawcall_vertex_blending_dynamic_offset + sizeof(MeshDrawPerDrawcallVertexBlending);

                    MeshDrawPerDrawcallVertexBlending& vertex_blending_object =
                        *reinterpret_cast<MeshDrawPerDrawcallVertexBlending*>(reinterpret_cast<uintptr_t>(
                            m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbufferMemoryPointer) +
                            per_drawcall_vertex_blending_dynamic_offset);
                    for (uint32_t instance_index = 0; instance_index < current_instance_count; ++instance_index)
                    {
                        const uint32_t node_index = drawcall_max_instance_count * drawcall_index + instance_index;
                        if (mesh_nodes[node_index].joint_matrices)
                        {
                            for (uint32_t joint_index = 0; joint_index < mesh_nodes[node_index].joint_count; ++joint_index)
                            {
                                vertex_blending_object
                                    .joint_matrices[s_MeshVertexBlendingMaxJointCount * instance_index + joint_index] =
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

                if (mesh_draw.index_count == 0)
                {
                    LOG_ERROR(ZRender, "DrawSkyMeshPass: index_count is 0!");
                }
                m_Rhi->CmdDrawIndexedPFN(m_Rhi->GetCurrentCommandBuffer(),
                                         mesh_draw.index_count,
                                         current_instance_count,
                                         0,
                                         0,
                                         0);
            }
        }
        return;
    }

    RHIPipeline* sky_procedural_pipeline = m_RenderPipelines[_render_pipeline_type_sky_procedural].pipeline;
    RHIPipelineLayout* procedural_layout = m_RenderPipelines[_render_pipeline_type_sky_procedural].layout;
    
    if (sky_procedural_pipeline == nullptr)
    {
        LOG_ERROR(ZRender, "DrawSkyMeshPass: sky_procedural_pipeline is null!");
        return;
    }
    
    m_Rhi->CmdBindPipelinePFN(m_Rhi->GetCurrentCommandBuffer(), RHI_PIPELINE_BIND_POINT_GRAPHICS, sky_procedural_pipeline);
    
    m_Rhi->CmdBindDescriptorSetsPFN(m_Rhi->GetCurrentCommandBuffer(),
                                    RHI_PIPELINE_BIND_POINT_GRAPHICS,
                                    procedural_layout,
                                    0,
                                    1,
                                    &m_DescriptorInfos[_mesh_global].descriptor_set,
                                    1,
                                    &perframe_dynamic_offset);

    // DX12: bind ALL descriptor sets in the root signature.
    m_Rhi->CmdBindDescriptorSetsPFN(m_Rhi->GetCurrentCommandBuffer(),
                                    RHI_PIPELINE_BIND_POINT_GRAPHICS,
                                    procedural_layout,
                                    1,
                                    1,
                                    &m_DescriptorInfos[_per_mesh].descriptor_set,
                                    0,
                                    nullptr);
    m_Rhi->CmdBindDescriptorSetsPFN(m_Rhi->GetCurrentCommandBuffer(),
                                    RHI_PIPELINE_BIND_POINT_GRAPHICS,
                                    procedural_layout,
                                    2,
                                    1,
                                    &m_DescriptorInfos[_mesh_per_material].descriptor_set,
                                    0,
                                    nullptr);

    m_Rhi->CmdDraw(m_Rhi->GetCurrentCommandBuffer(), 36, 1, 0, 0);
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

    std::map<GpuPBRMaterial*, std::map<GpuMesh*, std::vector<MeshNode>>> draw_batches;
    for (const RenderMeshNode& node : visible_nodes)
    {
        auto& mesh_nodes = draw_batches[AsGpuMaterial(node.ref_material)][AsGpuMesh(node.ref_mesh)];
        MeshNode temp;
        temp.model_matrix = node.model_matrix;
        if (node.enable_vertex_blending)
        {
            temp.joint_matrices = node.joint_matrices;
            temp.joint_count = node.joint_count;
        }
        mesh_nodes.push_back(temp);
    }

    if (visible_nodes.empty() && render_scene != nullptr && !render_scene->m_RenderEntities.empty())
    {
        const std::vector<RenderMeshNode>& sky_nodes = render_scene->GetMainCameraSkyMeshNodes(viewport_type);
        const std::vector<RenderMeshNode>& transparent_nodes =
            render_scene->GetMainCameraTransparentMeshNodes(viewport_type);
        const std::vector<RenderMeshNode>& all_visible_nodes =
            render_scene->GetMainCameraVisibleMeshNodes(viewport_type);

        // Sky materials intentionally skip the GBuffer pass; only warn when entities exist but
        // none survived visibility (frustum cull) or routing (opaque / transparent / sky).
        if (all_visible_nodes.empty())
        {
            static std::array<bool, 2> warned_culled {};
            const size_t viewport_index = static_cast<size_t>(viewport_type);
            if (viewport_index < warned_culled.size() && !warned_culled[viewport_index])
            {
                LOG_WARNING(ZRender,
                            "MainCameraRp1Pass: {} render entities exist but none are visible for viewport={} "
                            "(check frustum cull / enabled state)",
                            render_scene->m_RenderEntities.size(),
                            static_cast<int>(viewport_type));
                warned_culled[viewport_index] = true;
            }
        }
        else if (sky_nodes.empty() && transparent_nodes.empty())
        {
            static std::array<bool, 2> warned_routing {};
            const size_t viewport_index = static_cast<size_t>(viewport_type);
            if (viewport_index < warned_routing.size() && !warned_routing[viewport_index])
            {
                LOG_WARNING(ZRender,
                            "MainCameraRp1Pass: {} visible mesh nodes but 0 opaque GBuffer nodes for viewport={} "
                            "(forward-only materials?)",
                            all_visible_nodes.size(),
                            static_cast<int>(viewport_type));
                warned_routing[viewport_index] = true;
            }
        }
    }

    float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    m_Rhi->PushEvent(m_Rhi->GetCurrentCommandBuffer(), "Mesh GBuffer", color);
    SetPerViewportData(viewport_type);
    SetViewportScissor(viewport_type);

    RHIPipeline* bound_pipeline = nullptr;
    RHIPipelineLayout* pipeline_layout = m_RenderPipelines[_render_pipeline_type_mesh_gbuffer].layout;

    const uint32_t perframe_dynamic_offset = AlignPerFrameRingOffset(
        m_Rhi,
        m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbuffersEnd[m_Rhi->GetCurrentFrameIndex()],
        m_GlobalRenderResource->m_StorageBuffer.m_MinStorageBufferOffsetAlignment);
    m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbuffersEnd[m_Rhi->GetCurrentFrameIndex()] =
        perframe_dynamic_offset + PerFrameUploadRingStrideBytes(m_Rhi, perframe_dynamic_offset);

    (*reinterpret_cast<MainCameraPerFrame*>(
        reinterpret_cast<uintptr_t>(m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbufferMemoryPointer) +
        perframe_dynamic_offset)) = m_MainCameraPerFrame;

    for (auto& material_batch : draw_batches)
    {
        GpuPBRMaterial& material = *material_batch.first;
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
            GpuMesh& mesh = *mesh_batch.first;
            auto& mesh_nodes = mesh_batch.second;

            const uint32_t total_instance_count = static_cast<uint32_t>(mesh_nodes.size());
            if (total_instance_count == 0)
            {
                continue;
            }

            const MeshDrawData mesh_draw = getMeshDrawData(&mesh);
            if (!mesh_draw)
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

            RHIBuffer* vertex_buffers[] = {mesh_draw.position_buffer,
                                           mesh_draw.varying_blending_buffer,
                                           mesh_draw.varying_buffer};
            RHIDeviceSize offsets[] = {0, 0, 0};
            m_Rhi->CmdBindVertexBuffersPFN(m_Rhi->GetCurrentCommandBuffer(),
                                           0,
                                           static_cast<uint32_t>(sizeof(vertex_buffers) / sizeof(vertex_buffers[0])),
                                           vertex_buffers,
                                           offsets);
            m_Rhi->CmdBindIndexBufferPFN(
                m_Rhi->GetCurrentCommandBuffer(), mesh_draw.index_buffer, 0, RHI_INDEX_TYPE_UINT16);

            const uint32_t drawcall_max_instance_count =
                static_cast<uint32_t>(sizeof(MeshDrawPerDrawcall::mesh_instances) /
                                      sizeof(MeshDrawPerDrawcall::mesh_instances[0]));
            const uint32_t drawcall_count =
                RoundUp(total_instance_count, drawcall_max_instance_count) / drawcall_max_instance_count;

            for (uint32_t drawcall_index = 0; drawcall_index < drawcall_count; ++drawcall_index)
            {
                const uint32_t current_instance_count =
                    ((total_instance_count - drawcall_max_instance_count * drawcall_index) < drawcall_max_instance_count) ? (total_instance_count - drawcall_max_instance_count * drawcall_index) : drawcall_max_instance_count;

                const uint32_t perdrawcall_dynamic_offset =
                    drawcall_index == 0
                        ? MeshPerDrawcallOffsetAfterPerFrame(m_Rhi, perframe_dynamic_offset)
                        : RoundUp(m_GlobalRenderResource->m_StorageBuffer
                                      .m_GlobalUploadRingbuffersEnd[m_Rhi->GetCurrentFrameIndex()],
                                  m_GlobalRenderResource->m_StorageBuffer.m_MinStorageBufferOffsetAlignment);
                m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbuffersEnd[m_Rhi->GetCurrentFrameIndex()] =
                    perdrawcall_dynamic_offset + sizeof(MeshDrawPerDrawcall);

                MeshDrawPerDrawcall& perdrawcall_storage_buffer_object =
                    *reinterpret_cast<MeshDrawPerDrawcall*>(reinterpret_cast<uintptr_t>(
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
                        sizeof(MeshDrawPerDrawcallVertexBlending);

                    MeshDrawPerDrawcallVertexBlending& vertex_blending_object =
                        *reinterpret_cast<MeshDrawPerDrawcallVertexBlending*>(reinterpret_cast<uintptr_t>(
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
                    m_Rhi->GetCurrentCommandBuffer(), mesh_draw.index_count, current_instance_count, 0, 0, 0);
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

    std::map<GpuPBRMaterial*, std::map<GpuMesh*, std::vector<MeshNode>>> draw_batches;
    for (const RenderMeshNode& node : visible_nodes)
    {
        auto& mesh_nodes = draw_batches[AsGpuMaterial(node.ref_material)][AsGpuMesh(node.ref_mesh)];
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

    const uint32_t perframe_dynamic_offset = AlignPerFrameRingOffset(
        m_Rhi,
        m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbuffersEnd[m_Rhi->GetCurrentFrameIndex()],
        m_GlobalRenderResource->m_StorageBuffer.m_MinStorageBufferOffsetAlignment);
    m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbuffersEnd[m_Rhi->GetCurrentFrameIndex()] =
        perframe_dynamic_offset + PerFrameUploadRingStrideBytes(m_Rhi, perframe_dynamic_offset);

    (*reinterpret_cast<MainCameraPerFrame*>(
        reinterpret_cast<uintptr_t>(m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbufferMemoryPointer) +
        perframe_dynamic_offset)) = m_MainCameraPerFrame;

    for (auto& material_batch : draw_batches)
    {
        GpuPBRMaterial& material = *material_batch.first;
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
            GpuMesh& mesh = *mesh_batch.first;
            auto& mesh_nodes = mesh_batch.second;

            const uint32_t total_instance_count = static_cast<uint32_t>(mesh_nodes.size());
            if (total_instance_count == 0)
            {
                continue;
            }

            const MeshDrawData mesh_draw = getMeshDrawData(&mesh);
            if (!mesh_draw)
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

            RHIBuffer* vertex_buffers[] = {mesh_draw.position_buffer,
                                           mesh_draw.varying_blending_buffer,
                                           mesh_draw.varying_buffer};
            RHIDeviceSize offsets[] = {0, 0, 0};
            m_Rhi->CmdBindVertexBuffersPFN(m_Rhi->GetCurrentCommandBuffer(),
                                           0,
                                           static_cast<uint32_t>(sizeof(vertex_buffers) / sizeof(vertex_buffers[0])),
                                           vertex_buffers,
                                           offsets);
            m_Rhi->CmdBindIndexBufferPFN(
                m_Rhi->GetCurrentCommandBuffer(), mesh_draw.index_buffer, 0, RHI_INDEX_TYPE_UINT16);

            const uint32_t drawcall_max_instance_count =
                static_cast<uint32_t>(sizeof(MeshDrawPerDrawcall::mesh_instances) /
                                      sizeof(MeshDrawPerDrawcall::mesh_instances[0]));
            const uint32_t drawcall_count =
                RoundUp(total_instance_count, drawcall_max_instance_count) / drawcall_max_instance_count;

            for (uint32_t drawcall_index = 0; drawcall_index < drawcall_count; ++drawcall_index)
            {
                const uint32_t current_instance_count =
                    ((total_instance_count - drawcall_max_instance_count * drawcall_index) < drawcall_max_instance_count) ? (total_instance_count - drawcall_max_instance_count * drawcall_index) : drawcall_max_instance_count;

                const uint32_t perdrawcall_dynamic_offset =
                    drawcall_index == 0
                        ? MeshPerDrawcallOffsetAfterPerFrame(m_Rhi, perframe_dynamic_offset)
                        : RoundUp(m_GlobalRenderResource->m_StorageBuffer
                                      .m_GlobalUploadRingbuffersEnd[m_Rhi->GetCurrentFrameIndex()],
                                  m_GlobalRenderResource->m_StorageBuffer.m_MinStorageBufferOffsetAlignment);
                m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbuffersEnd[m_Rhi->GetCurrentFrameIndex()] =
                    perdrawcall_dynamic_offset + sizeof(MeshDrawPerDrawcall);

                MeshDrawPerDrawcall& perdrawcall_storage_buffer_object =
                    *reinterpret_cast<MeshDrawPerDrawcall*>(reinterpret_cast<uintptr_t>(
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
                        sizeof(MeshDrawPerDrawcallVertexBlending);

                    MeshDrawPerDrawcallVertexBlending& vertex_blending_object =
                        *reinterpret_cast<MeshDrawPerDrawcallVertexBlending*>(reinterpret_cast<uintptr_t>(
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
                    m_Rhi->GetCurrentCommandBuffer(), mesh_draw.index_count, current_instance_count, 0, 0, 0);
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

    // Same guard as MainCameraPass::SetupModelGlobalDescriptorSet: MegaLights
    // only allocates its buffers/history images once Update() runs, and Update()
    // returns early while r.MegaLights.Enable is 0.
    if (megalights_lights_buffer_info.buffer == nullptr ||
        megalights_indices_buffer_info.buffer == nullptr ||
        megalights_ranges_buffer_info.buffer == nullptr ||
        megalights_history_read_info.imageView == nullptr ||
        megalights_history_write_info.imageView == nullptr)
    {
        return;
    }

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

    if (megalights_lights_buffer_info.buffer == nullptr ||
        megalights_indices_buffer_info.buffer == nullptr ||
        megalights_ranges_buffer_info.buffer == nullptr ||
        megalights_direct_sample_info.imageView == nullptr ||
        megalights_history_write_info.imageView == nullptr)
    {
        return;
    }

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
    if (viewport_type == ViewportType::scene)
    {
        if (auto* render_system = GET_SYSTEM(RenderSystem))
        {
            RHIViewport scene_viewport {};
            RHIRect2D scene_scissor {};
            if (render_system->TryGetRenderSceneViewport(scene_viewport, scene_scissor) &&
                scene_viewport.width > 0.0f && scene_viewport.height > 0.0f)
            {
                m_MainCameraPerFrame.viewport_rect =
                    Vector4(scene_viewport.x, scene_viewport.y, scene_viewport.width, scene_viewport.height);
            }
        }
    }

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

    const uint32_t perframe_dynamic_offset = AlignPerFrameRingOffset(
        m_Rhi,
        m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbuffersEnd[m_Rhi->GetCurrentFrameIndex()],
        m_GlobalRenderResource->m_StorageBuffer.m_MinStorageBufferOffsetAlignment);
    m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbuffersEnd[m_Rhi->GetCurrentFrameIndex()] =
        perframe_dynamic_offset + PerFrameUploadRingStrideBytes(m_Rhi, perframe_dynamic_offset);

    (*reinterpret_cast<MainCameraPerFrame*>(
        reinterpret_cast<uintptr_t>(m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbufferMemoryPointer) +
        perframe_dynamic_offset)) = m_MainCameraPerFrame;

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
        m_MegaLightsSystem->EndDeferredPass(viewport_type, m_MainCameraPerFrame.proj_view_matrix);
    }
}

void MainCameraRp1Pass::DrawMegaLightsSpatialDenoise(ViewportType viewport_type)
{
    m_Rhi->CmdBindPipelinePFN(m_Rhi->GetCurrentCommandBuffer(),
                              RHI_PIPELINE_BIND_POINT_GRAPHICS,
                              m_RenderPipelines[_render_pipeline_type_megalights_spatial].pipeline);
    SetViewportScissor(viewport_type);

    const uint32_t perframe_dynamic_offset = AlignPerFrameRingOffset(
        m_Rhi,
        m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbuffersEnd[m_Rhi->GetCurrentFrameIndex()],
        m_GlobalRenderResource->m_StorageBuffer.m_MinStorageBufferOffsetAlignment);
    m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbuffersEnd[m_Rhi->GetCurrentFrameIndex()] =
        perframe_dynamic_offset + PerFrameUploadRingStrideBytes(m_Rhi, perframe_dynamic_offset);

    (*reinterpret_cast<MainCameraPerFrame*>(
        reinterpret_cast<uintptr_t>(m_GlobalRenderResource->m_StorageBuffer.m_GlobalUploadRingbufferMemoryPointer) +
        perframe_dynamic_offset)) = m_MainCameraPerFrame;

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
