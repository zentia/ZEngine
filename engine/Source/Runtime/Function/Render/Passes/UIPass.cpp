#include "Runtime/Function/Render/Passes/UIPass.h"

#include "Runtime/Core/Math/Vector2.h"
#include "Runtime/Function/Render/Interface/RHI.h"
#include "Runtime/UI/Render/BatchedUIRenderer.h"
#include "Runtime/UI/UISystem.h"
#include "Runtime/UI/Render/UiRenderBatch.h"
#include "Runtime/UI/Render/UiGpuResources.h"
#include "Runtime/UI/Core/WindowUI.h"
#include "core/Log/LogSystem.h"

#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(Z_HAS_VULKAN)
    #include <ui_batched_frag.h>
    #include <ui_batched_vert.h>
#endif

namespace
{
    // Vulkan uses explicit locations; DX12RHI maps location -> HLSL semantic via
    // ToDX12SemanticName (0=POSITION, 3=TEXCOORD, 4=COLOR). Keep these in sync
    // with ui_batched.vert / ui_batched.vert.hlsl.
    constexpr uint32_t kUiAttrPosition = 0;
    constexpr uint32_t kUiAttrColor = 4;
    constexpr uint32_t kUiAttrTexCoord = 3;

    void FillUiVertexAttributes(RHIVertexInputAttributeDescription out[3], GraphicsAPI api)
    {
        const uint32_t color_loc = (api == GraphicsAPI::DirectX12) ? kUiAttrColor : 1u;
        const uint32_t uv_loc = (api == GraphicsAPI::DirectX12) ? kUiAttrTexCoord : 2u;

        out[0].location = kUiAttrPosition;
        out[0].binding = 0;
        out[0].format = RHI_FORMAT_R32G32_SFLOAT;
        out[0].offset = offsetof(UiVertex, pos);

        out[1].location = color_loc;
        out[1].binding = 0;
        out[1].format = RHI_FORMAT_R32G32B32A32_SFLOAT;
        out[1].offset = offsetof(UiVertex, color);

        out[2].location = uv_loc;
        out[2].binding = 0;
        out[2].format = RHI_FORMAT_R32G32_SFLOAT;
        out[2].offset = offsetof(UiVertex, uv);
    }

    BatchedUIRenderer* ResolveBatchedRenderer(WindowUI* window_ui)
    {
        auto* ui_system = dynamic_cast<UISystem*>(window_ui);
        if (ui_system == nullptr)
        {
            return nullptr;
        }
        return dynamic_cast<BatchedUIRenderer*>(ui_system->getUiRenderer());
    }

    std::string GetShaderRoot()
    {
#ifdef ZENGINE_SHADER_ROOT
        return ZENGINE_SHADER_ROOT;
#else
        return "e:/Engine/ZEngine/engine/shader";
#endif
    }

    RHIShader* LoadDx12UiShader(RHI* rhi, const char* hlsl_relative_path, ShaderStage stage)
    {
        const std::string full_path = GetShaderRoot() + "/hlsl/rp2/" + hlsl_relative_path;
        std::vector<uint8_t> binary;
        return rhi->CreateShaderModuleFromFile(full_path, stage, {}, {}, binary);
    }
}  // namespace

REGISTER_FACTORY(RenderPass, UIPass, "UIPass");

void UIPass::Initialize(const RenderPassInitInfo* init_info)
{
    RenderPass::Initialize(nullptr);

    m_Framebuffer.render_pass = static_cast<const UIPassInitInfo*>(init_info)->render_pass;
    m_GpuResources.Initialize(m_Rhi);
    SetupPipeline();
}

void UIPass::InitializeUIRenderBackend(WindowUI* window_ui)
{
    m_WindowUi = window_ui;
}

void UIPass::SetupPipeline()
{
    if (m_Rhi == nullptr)
    {
        return;
    }

    // Vulkan 需要有效的 render_pass 来创建图形管线
    // DX12 不使用 render_pass 对象，传 nullptr 即可
    if (m_Rhi->getGraphicsAPI() == GraphicsAPI::Vulkan && m_Framebuffer.render_pass == nullptr)
    {
        return;
    }

    m_RenderPipelines.resize(1);

    RHIDescriptorSetLayout* texture_layout = m_GpuResources.GetTextureLayout();
    RHIPipelineLayoutCreateInfo pipeline_layout_create_info {};
    pipeline_layout_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeline_layout_create_info.setLayoutCount = texture_layout != nullptr ? 1 : 0;
    pipeline_layout_create_info.pSetLayouts = texture_layout != nullptr ? &texture_layout : nullptr;
    pipeline_layout_create_info.pushConstantRangeCount = 0;
    pipeline_layout_create_info.pPushConstantRanges = nullptr;

    if (RHI_SUCCESS != m_Rhi->CreatePipelineLayout(&pipeline_layout_create_info, m_RenderPipelines[0].layout))
    {
        throw std::runtime_error("UIPass: create pipeline layout failed");
    }

    RHIShader* vert_shader_module = nullptr;
    RHIShader* frag_shader_module = nullptr;

#if defined(Z_HAS_VULKAN)
    if (m_Rhi->getGraphicsAPI() == GraphicsAPI::Vulkan)
    {
        vert_shader_module = m_Rhi->CreateShaderModule(UI_BATCHED_VERT);
        frag_shader_module = m_Rhi->CreateShaderModule(UI_BATCHED_FRAG);
    }
#endif
#if defined(_WIN32)
    if (m_Rhi->getGraphicsAPI() == GraphicsAPI::DirectX12)
    {
        vert_shader_module = LoadDx12UiShader(m_Rhi, "ui_batched.vert.hlsl", ShaderStage::Vertex);
        frag_shader_module = LoadDx12UiShader(m_Rhi, "ui_batched.frag.hlsl", ShaderStage::Fragment);
    }
#endif

    if (vert_shader_module == nullptr || frag_shader_module == nullptr)
    {
        throw std::runtime_error("UIPass: create shader modules failed");
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

    RHIVertexInputBindingDescription binding_description {};
    binding_description.binding = 0;
    binding_description.stride = sizeof(UiVertex);
    binding_description.inputRate = RHI_VERTEX_INPUT_RATE_VERTEX;

    RHIVertexInputAttributeDescription attribute_descriptions[3] {};
    FillUiVertexAttributes(attribute_descriptions, m_Rhi->getGraphicsAPI());

    RHIPipelineVertexInputStateCreateInfo vertex_input {};
    vertex_input.sType = RHI_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount = 1;
    vertex_input.pVertexBindingDescriptions = &binding_description;
    vertex_input.vertexAttributeDescriptionCount = 3;
    vertex_input.pVertexAttributeDescriptions = attribute_descriptions;

    RHIPipelineInputAssemblyStateCreateInfo input_assembly {};
    input_assembly.sType = RHI_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = RHI_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    input_assembly.primitiveRestartEnable = RHI_FALSE;

    RHIPipelineViewportStateCreateInfo viewport_state {};
    viewport_state.sType = RHI_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;

    RHIPipelineRasterizationStateCreateInfo rasterization {};
    rasterization.sType = RHI_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization.depthClampEnable = RHI_FALSE;
    rasterization.rasterizerDiscardEnable = RHI_FALSE;
    rasterization.polygonMode = RHI_POLYGON_MODE_FILL;
    rasterization.lineWidth = 1.0f;
    rasterization.cullMode = RHI_CULL_MODE_NONE;
    rasterization.frontFace = RHI_FRONT_FACE_CLOCKWISE;
    rasterization.depthBiasEnable = RHI_FALSE;

    RHIPipelineMultisampleStateCreateInfo multisample {};
    multisample.sType = RHI_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.sampleShadingEnable = RHI_FALSE;
    multisample.rasterizationSamples = RHI_SAMPLE_COUNT_1_BIT;

    RHIPipelineColorBlendAttachmentState blend_attachment {};
    blend_attachment.colorWriteMask =
        RHI_COLOR_COMPONENT_R_BIT | RHI_COLOR_COMPONENT_G_BIT | RHI_COLOR_COMPONENT_B_BIT | RHI_COLOR_COMPONENT_A_BIT;
    blend_attachment.blendEnable = RHI_TRUE;
    blend_attachment.srcColorBlendFactor = RHI_BLEND_FACTOR_SRC_ALPHA;
    blend_attachment.dstColorBlendFactor = RHI_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend_attachment.colorBlendOp = RHI_BLEND_OP_ADD;
    blend_attachment.srcAlphaBlendFactor = RHI_BLEND_FACTOR_ONE;
    blend_attachment.dstAlphaBlendFactor = RHI_BLEND_FACTOR_ZERO;
    blend_attachment.alphaBlendOp = RHI_BLEND_OP_ADD;

    RHIPipelineColorBlendStateCreateInfo color_blend {};
    color_blend.sType = RHI_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    color_blend.logicOpEnable = RHI_FALSE;
    color_blend.attachmentCount = 1;
    color_blend.pAttachments = &blend_attachment;

    RHIPipelineDepthStencilStateCreateInfo depth_stencil {};
    depth_stencil.sType = RHI_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_stencil.depthTestEnable = RHI_FALSE;
    depth_stencil.depthWriteEnable = RHI_FALSE;

    RHIDynamicState dynamic_states[] = {RHI_DYNAMIC_STATE_VIEWPORT, RHI_DYNAMIC_STATE_SCISSOR};
    RHIPipelineDynamicStateCreateInfo dynamic_state {};
    dynamic_state.sType = RHI_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic_state.dynamicStateCount = 2;
    dynamic_state.pDynamicStates = dynamic_states;

    RHIGraphicsPipelineCreateInfo pipeline_info {};
    pipeline_info.sType = RHI_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_info.stageCount = 2;
    pipeline_info.pStages = shader_stages;
    pipeline_info.pVertexInputState = &vertex_input;
    pipeline_info.pInputAssemblyState = &input_assembly;
    pipeline_info.pViewportState = &viewport_state;
    pipeline_info.pRasterizationState = &rasterization;
    pipeline_info.pMultisampleState = &multisample;
    pipeline_info.pColorBlendState = &color_blend;
    pipeline_info.pDepthStencilState = &depth_stencil;
    pipeline_info.pDynamicState = &dynamic_state;
    pipeline_info.layout = m_RenderPipelines[0].layout;
    pipeline_info.renderPass = m_Framebuffer.render_pass;
    pipeline_info.subpass = _main_camera_subpass_ui;
    pipeline_info.basePipelineHandle = RHI_NULL_HANDLE;

    if (RHI_SUCCESS !=
        m_Rhi->CreateGraphicsPipelines(RHI_NULL_HANDLE, 1, &pipeline_info, m_RenderPipelines[0].pipeline))
    {
        LOG_ERROR(ZRender,
                  "UIPass: CreateGraphicsPipelines failed (API={}, subpass={}, check VS input layout vs "
                  "ui_batched.* shader semantics)",
                  static_cast<int>(m_Rhi->getGraphicsAPI()),
                  pipeline_info.subpass);
        throw std::runtime_error("UIPass: create graphics pipeline failed");
    }

    m_Rhi->DestroyShaderModule(vert_shader_module);
    m_Rhi->DestroyShaderModule(frag_shader_module);

    m_PipelineReady = true;
}

void UIPass::DestroyGpuBuffers()
{
    if (m_Rhi == nullptr)
    {
        return;
    }

    if (m_VertexBuffer != nullptr)
    {
        m_Rhi->DestroyBuffer(m_VertexBuffer);
        m_VertexBuffer = nullptr;
    }
    if (m_VertexMemory != nullptr)
    {
        m_Rhi->FreeMemory(m_VertexMemory);
        m_VertexMemory = nullptr;
    }
    if (m_IndexBuffer != nullptr)
    {
        m_Rhi->DestroyBuffer(m_IndexBuffer);
        m_IndexBuffer = nullptr;
    }
    if (m_IndexMemory != nullptr)
    {
        m_Rhi->FreeMemory(m_IndexMemory);
        m_IndexMemory = nullptr;
    }

    m_VertexCapacity = 0;
    m_IndexCapacity = 0;
}

void UIPass::EnsureGpuBuffers(size_t vertex_count, size_t index_count)
{
    if (m_Rhi == nullptr)
    {
        return;
    }

    if (vertex_count <= m_VertexCapacity && index_count <= m_IndexCapacity)
    {
        return;
    }

    DestroyGpuBuffers();

    const RHIDeviceSize vertex_bytes = static_cast<RHIDeviceSize>(vertex_count * sizeof(UiVertex));
    const RHIDeviceSize index_bytes = static_cast<RHIDeviceSize>(index_count * sizeof(uint16_t));

    m_Rhi->CreateBuffer(vertex_bytes,
                        RHI_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                        RHI_MEMORY_PROPERTY_HOST_VISIBLE_BIT | RHI_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        m_VertexBuffer,
                        m_VertexMemory);

    m_Rhi->CreateBuffer(index_bytes,
                        RHI_BUFFER_USAGE_INDEX_BUFFER_BIT,
                        RHI_MEMORY_PROPERTY_HOST_VISIBLE_BIT | RHI_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        m_IndexBuffer,
                        m_IndexMemory);

    m_VertexCapacity = vertex_count;
    m_IndexCapacity = index_count;
}

void UIPass::UploadBatch(const UiRenderBatch& batch, float display_width, float display_height)
{
    const std::vector<UiVertex>& src_vertices = batch.getVertices();
    const std::vector<uint16_t>& src_indices = batch.getIndices();
    if (src_vertices.empty() || src_indices.empty() || display_width <= 0.0f || display_height <= 0.0f)
    {
        return;
    }

    m_CpuVertices.resize(src_vertices.size());
    const float scale_x = 2.0f / display_width;
    const float scale_y = 2.0f / display_height;
    for (size_t i = 0; i < src_vertices.size(); ++i)
    {
        m_CpuVertices[i].pos[0] = src_vertices[i].pos[0] * scale_x - 1.0f;
        m_CpuVertices[i].pos[1] = 1.0f - src_vertices[i].pos[1] * scale_y;
        m_CpuVertices[i].color[0] = src_vertices[i].color[0];
        m_CpuVertices[i].color[1] = src_vertices[i].color[1];
        m_CpuVertices[i].color[2] = src_vertices[i].color[2];
        m_CpuVertices[i].color[3] = src_vertices[i].color[3];
        m_CpuVertices[i].uv[0] = src_vertices[i].uv[0];
        m_CpuVertices[i].uv[1] = src_vertices[i].uv[1];
    }

    EnsureGpuBuffers(src_vertices.size(), src_indices.size());

    void* vertex_data = nullptr;
    m_Rhi->MapMemory(m_VertexMemory,
                     0,
                     static_cast<RHIDeviceSize>(m_CpuVertices.size() * sizeof(UiVertex)),
                     0,
                     &vertex_data);
    memcpy(vertex_data, m_CpuVertices.data(), m_CpuVertices.size() * sizeof(UiVertex));
    m_Rhi->UnmapMemory(m_VertexMemory);

    void* index_data = nullptr;
    m_Rhi->MapMemory(m_IndexMemory,
                     0,
                     static_cast<RHIDeviceSize>(src_indices.size() * sizeof(uint16_t)),
                     0,
                     &index_data);
    memcpy(index_data, src_indices.data(), src_indices.size() * sizeof(uint16_t));
    m_Rhi->UnmapMemory(m_IndexMemory);
}

void UIPass::Draw()
{
    if (!m_PipelineReady || m_Rhi == nullptr || m_RenderPipelines.empty() ||
        m_RenderPipelines[0].pipeline == nullptr)
    {
        return;
    }

    if (m_WindowUi != nullptr)
    {
        m_WindowUi->PreRender();
    }

    BatchedUIRenderer* renderer = ResolveBatchedRenderer(m_WindowUi);
    if (renderer == nullptr)
    {
        return;
    }

    const UiRenderBatch& batch = renderer->getBatch();
    if (batch.empty())
    {
        return;
    }

    // All UI has recorded by now, so every requested glyph size is baked. Re-upload
    // any native font atlas that grew this frame before we draw from it.
    if (UiGpuResources* gpu = UiGpuResources::Get())
    {
        gpu->RefreshNativeFontAtlasesIfDirty();
    }

    const Vector2 display_size = renderer->getDisplaySize();
    UploadBatch(batch, display_size.x, display_size.y);

    if (m_VertexBuffer == nullptr || m_IndexBuffer == nullptr)
    {
        return;
    }

    float color[4] = {0.4f, 0.8f, 1.0f, 1.0f};
    RHICommandBuffer* command_buffer = m_Rhi->GetCurrentCommandBuffer();
    m_Rhi->PushEvent(command_buffer, "Runtime UGUI", color);

    RHIViewport viewport {0.0f,
                          0.0f,
                          static_cast<float>(m_Rhi->GetSwapchainInfo().extent.width),
                          static_cast<float>(m_Rhi->GetSwapchainInfo().extent.height),
                          0.0f,
                          1.0f};
    RHIRect2D scissor {0,
                       0,
                       static_cast<int32_t>(m_Rhi->GetSwapchainInfo().extent.width),
                       static_cast<int32_t>(m_Rhi->GetSwapchainInfo().extent.height)};

    m_Rhi->CmdBindPipelinePFN(command_buffer, RHI_PIPELINE_BIND_POINT_GRAPHICS, m_RenderPipelines[0].pipeline);
    m_Rhi->CmdSetViewportPFN(command_buffer, 0, 1, &viewport);
    m_Rhi->CmdSetScissorPFN(command_buffer, 0, 1, &scissor);

    RHIBuffer* vertex_buffers[] = {m_VertexBuffer};
    RHIDeviceSize offsets[] = {0};
    m_Rhi->CmdBindVertexBuffersPFN(command_buffer, 0, 1, vertex_buffers, offsets);
    m_Rhi->CmdBindIndexBufferPFN(command_buffer, m_IndexBuffer, 0, RHI_INDEX_TYPE_UINT16);

    const std::vector<UiDrawCommand>& commands = batch.getCommands();
    if (commands.empty())
    {
        RHIDescriptorSet* descriptor_set = m_GpuResources.GetDescriptorSet(m_GpuResources.GetWhiteTextureId());
        if (descriptor_set != nullptr)
        {
            m_Rhi->CmdBindDescriptorSetsPFN(command_buffer,
                                            RHI_PIPELINE_BIND_POINT_GRAPHICS,
                                            m_RenderPipelines[0].layout,
                                            0,
                                            1,
                                            &descriptor_set,
                                            0,
                                            nullptr);
        }
        m_Rhi->CmdDrawIndexedPFN(command_buffer,
                                 static_cast<uint32_t>(batch.getIndices().size()),
                                 1,
                                 0,
                                 0,
                                 0);
    }
    else
    {
        for (const UiDrawCommand& command : commands)
        {
            if (command.index_count == 0)
            {
                continue;
            }

            RHIDescriptorSet* descriptor_set = m_GpuResources.GetDescriptorSet(command.texture_id);
            if (descriptor_set != nullptr)
            {
                m_Rhi->CmdBindDescriptorSetsPFN(command_buffer,
                                                RHI_PIPELINE_BIND_POINT_GRAPHICS,
                                                m_RenderPipelines[0].layout,
                                                0,
                                                1,
                                                &descriptor_set,
                                                0,
                                                nullptr);
            }

            m_Rhi->CmdDrawIndexedPFN(command_buffer, command.index_count, 1, command.index_offset, 0, 0);
        }
    }

    m_Rhi->PopEvent(command_buffer);
}
