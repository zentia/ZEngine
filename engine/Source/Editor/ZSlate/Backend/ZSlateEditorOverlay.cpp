#include "Editor/ZSlate/Backend/ZSlateEditorOverlay.h"

#include "Editor/ZSlate/Backend/EditorSlateHost.h"
#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Core/Math/Vector2.h"
#include "Runtime/Function/Console/ConsoleManager.h"
#include "Runtime/Slate/Application/SlateApplication.h"
#include "Runtime/Slate/Backend/SlateUIRendererBackend.h"
#include "Runtime/Function/Render/WindowSystem.h"
#include "Runtime/UI/Core/UITypes.h"
#include "Runtime/UI/Render/UiGpuResources.h"
#include "core/Log/LogSystem.h"

#if defined(_WIN32)
    #include "Runtime/Function/Render/Interface/DX12/DX12RHI.h"
#endif

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>

#if defined(Z_HAS_VULKAN)
    #include <ui_batched_frag.h>
    #include <ui_batched_vert.h>
#endif

namespace ZSlate
{
namespace
{
    bool s_NativeSelfTestEnabled = false;
    bool s_CvarRegistered = false;

#if defined(_WIN32)
    struct ScopedOverlayDescriptorBind
    {
        DX12RHI* rhi {nullptr};
        explicit ScopedOverlayDescriptorBind(const std::shared_ptr<RHI>& rhi_ptr)
        {
            if (rhi_ptr != nullptr && rhi_ptr->getGraphicsAPI() == GraphicsAPI::DirectX12)
            {
                rhi = static_cast<DX12RHI*>(rhi_ptr.get());
                if (rhi->supportsBindlessTextures())
                {
                    rhi->SetOverlayDescriptorBindActive(true);
                }
                else
                {
                    rhi = nullptr;
                }
            }
        }
        ~ScopedOverlayDescriptorBind()
        {
            if (rhi != nullptr)
            {
                rhi->SetOverlayDescriptorBindActive(false);
            }
        }
    };
#endif

    // Mirror UIPass: Vulkan uses explicit locations; DX12 maps location -> HLSL
    // semantic (0=POSITION, 3=TEXCOORD, 4=COLOR).
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

    std::string GetShaderRoot()
    {
#ifdef ZENGINE_SHADER_ROOT
        return ZENGINE_SHADER_ROOT;
#else
        return "e:/Engine/ZEngine/engine/shader";
#endif
    }

    RHIShader* LoadDx12UiShader(const std::shared_ptr<RHI>& rhi, const char* hlsl_relative_path, ShaderStage stage)
    {
        const std::string full_path = GetShaderRoot() + "/hlsl/rp2/" + hlsl_relative_path;
        std::vector<uint8_t> binary;
        return rhi->CreateShaderModuleFromFile(full_path, stage, {}, {}, binary);
    }
}  // namespace

ZSlateEditorOverlay& ZSlateEditorOverlay::Get()
{
    static ZSlateEditorOverlay instance;
    return instance;
}

bool ZSlateEditorOverlay::IsNativeBackendEnabled()
{
    // P9: the ImGui editor fallback is retired -- the native RHI backend (BatchedUIRenderer)
    // is the sole renderer for the menu bar, dock, and every ZSlate panel. r.ZSlate.NativeBackend
    // and r.ZSlate.NativeMenuBar were removed; this is now unconditionally true. The dev-only
    // r.ZSlate.NativeSelfTest canary CVar is still registered here (first-call lazy init).
    if (!s_CvarRegistered)
    {
        if (auto console = GET_SYSTEM(ConsoleManager))
        {
            console->RegisterBoolVariable("r.ZSlate.NativeSelfTest",
                                          "Record the ZSlate native-backend canary quad/label each frame.",
                                          false,
                                          &s_NativeSelfTestEnabled);
            s_CvarRegistered = true;
        }
    }
    return true;
}

bool ZSlateEditorOverlay::IsSelfTestEnabled()
{
    // IsNativeBackendEnabled registers the self-test CVar on first call.
    IsNativeBackendEnabled();
    return s_NativeSelfTestEnabled;
}

bool ZSlateEditorOverlay::IsNativeMenuBarEnabled()
{
    return true;  // P9: ImGui menu-bar fallback retired.
}

void ZSlateEditorOverlay::BeginFrame()
{
    m_Renderer.beginFrame();
}

void ZSlateEditorOverlay::BeginFrameIfEnabled()
{
    if (!IsNativeBackendEnabled())
    {
        return;
    }

    m_Renderer.beginFrame();
    m_Groups.clear();

    // P10: snapshot the native input bus for this UI frame. Runs here because
    // BeginFrameIfEnabled is the once-per-frame hook that sits after glfwPollEvents
    // and right before WindowUI::PreRender (panel paint), on the panel-paint thread.
    EditorSlateHost::Get().NewFrame();

    // P10c: install the renderer-backed text measurer (forwards to the
    // BatchedUIRenderer, which rasterizes through the native ZFontAtlas) as the
    // process-wide ZSlate measurer. Done here -- before any window OnGUI paints --
    // so editor ZSlate layout measures against the SAME font it draws with. This
    // is now the SOLE editor measurer: the old per-panel ImGui measurers (which
    // measured ImGui's font even when the native atlas was painting) were removed.
    // Mirrors UISystem's runtime measurer.
    static SlateUIRendererTextMeasurer s_editor_measurer;
    s_editor_measurer.SetRenderer(&m_Renderer);
    SlateApplication::Get().SetTextMeasurer(&s_editor_measurer);

    // Capture the display size ZSlate windows are mapped to NDC against (see
    // m_EditorDisplay* in the header). The NDC divisor MUST equal the viewport
    // extent the overlay renders into (the live swapchain/framebuffer pixels in
    // DrawBatch), otherwise the whole UI -- fonts included -- is scaled by their
    // ratio. We therefore source it from the live framebuffer size rather than
    // the cached logical window size: on the Windows editor path GLFW window
    // size == framebuffer pixels == swapchain extent, so this equals the layout
    // space the panels record in, while also being immune to a missed window-
    // size event (defence in depth on top of the windowSizeCallback dispatch
    // fix). Falls back to the host's logical size if the framebuffer query is
    // unavailable. (The macOS Retina logical-vs-physical split is deferred until
    // the native Metal UI lands; that overlay is currently a clear/present stub.)
    Vector2 display = EditorSlateHost::Get().GetDisplaySize();
    if (auto window = GET_SYSTEM(WindowSystem))
    {
        const std::array<int, 2> fb = window->GetFramebufferSize();
        if (fb[0] > 0 && fb[1] > 0)
        {
            display.x = static_cast<float>(fb[0]);
            display.y = static_cast<float>(fb[1]);
        }
    }
    m_EditorDisplayWidth = display.x;
    m_EditorDisplayHeight = display.y;

    if (IsSelfTestEnabled())
    {
        RecordSelfTest();
    }
}

void ZSlateEditorOverlay::BeginWindowGroup(int z_order)
{
    // Break the current command so this window's first draw can't merge into the
    // previous window's last command (keeps group ranges disjoint).
    BatchedUIRenderer& renderer = GetRenderer();
    renderer.getBatch().forceNewCommand();
    // Window groups structure ONLY the main overlay batch (z-order sorting in
    // DrawBatch). A floating panel paints into its own batch drawn in natural
    // order, so skip group bookkeeping when a floating renderer is active --
    // otherwise the floating command indices would corrupt m_Groups ranges.
    if (m_CurrentRenderer != nullptr && m_CurrentRenderer != &m_Renderer)
        return;
    WindowGroup group {};
    group.z_order = z_order;
    group.first_command = static_cast<uint32_t>(renderer.getBatch().getCommands().size());
    m_Groups.push_back(group);
}

void ZSlateEditorOverlay::RecordSelfTest()
{
    // Fixed quad + label so the GPU path is provable without any real window.
    m_Renderer.drawQuad(UIRect(40.0f, 40.0f, 320.0f, 96.0f), UIColor(0.10f, 0.45f, 0.85f, 0.92f));
    m_Renderer.drawRect(UIRect(40.0f, 40.0f, 320.0f, 96.0f), UIColor(1.0f, 1.0f, 1.0f, 0.9f), 2.0f);
    // font_size 0 => use the font's baked LegacySize (16). The one-shot atlas
    // upload only contains glyphs baked at that size; arbitrary sizes need the
    // atlas-refresh added in M2 (ImGui 1.92 dynamic fonts bake on demand).
    m_Renderer.drawText(UIRect(56.0f, 64.0f, 288.0f, 48.0f),
                        "ZSlate native backend OK",
                        0.0f,
                        UIColor(1.0f, 1.0f, 1.0f, 1.0f),
                        TextAnchor::MiddleLeft,
                        TextWrapMode::NoWrap,
                        nullptr);
}

void ZSlateEditorOverlay::EnsurePipeline(const std::shared_ptr<RHI>& rhi, RHIRenderPass* render_pass, uint32_t subpass)
{
    if (m_PipelineReady || rhi == nullptr)
    {
        return;
    }

    // In pure edit mode no runtime UIPass runs, so the shared GPU resources may
    // not be initialized yet.
    UiGpuResources* gpu = UiGpuResources::Get();
    if (gpu != nullptr && !gpu->IsReady())
    {
        gpu->Initialize(rhi);
    }

    RHIDescriptorSetLayout* texture_layout = gpu != nullptr ? gpu->GetTextureLayout() : nullptr;
    RHIPipelineLayoutCreateInfo pipeline_layout_create_info {};
    pipeline_layout_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeline_layout_create_info.setLayoutCount = texture_layout != nullptr ? 1 : 0;
    pipeline_layout_create_info.pSetLayouts = texture_layout != nullptr ? &texture_layout : nullptr;
    pipeline_layout_create_info.pushConstantRangeCount = 0;
    pipeline_layout_create_info.pPushConstantRanges = nullptr;

    if (RHI_SUCCESS != rhi->CreatePipelineLayout(&pipeline_layout_create_info, m_Layout))
    {
        LOG_ERROR(ZEditor, "ZSlateEditorOverlay: create pipeline layout failed");
        return;
    }

    RHIShader* vert_shader_module = nullptr;
    RHIShader* frag_shader_module = nullptr;

#if defined(Z_HAS_VULKAN)
    if (rhi->getGraphicsAPI() == GraphicsAPI::Vulkan)
    {
        vert_shader_module = rhi->CreateShaderModule(UI_BATCHED_VERT);
        frag_shader_module = rhi->CreateShaderModule(UI_BATCHED_FRAG);
    }
#endif
#if defined(_WIN32)
    if (rhi->getGraphicsAPI() == GraphicsAPI::DirectX12)
    {
        vert_shader_module = LoadDx12UiShader(rhi, "ui_batched.vert.hlsl", ShaderStage::Vertex);
        frag_shader_module = LoadDx12UiShader(rhi, "ui_batched.frag.hlsl", ShaderStage::Fragment);
    }
#endif

    if (vert_shader_module == nullptr || frag_shader_module == nullptr)
    {
        LOG_ERROR(ZEditor, "ZSlateEditorOverlay: create shader modules failed");
        return;
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
    FillUiVertexAttributes(attribute_descriptions, rhi->getGraphicsAPI());

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
    pipeline_info.layout = m_Layout;
    pipeline_info.renderPass = render_pass;  // nullptr on DX12 => swapchain format PSO
    pipeline_info.subpass = subpass;
    pipeline_info.basePipelineHandle = RHI_NULL_HANDLE;

    if (RHI_SUCCESS != rhi->CreateGraphicsPipelines(RHI_NULL_HANDLE, 1, &pipeline_info, m_Pipeline))
    {
        LOG_ERROR(ZEditor,
                  "ZSlateEditorOverlay: CreateGraphicsPipelines failed (API={}, subpass={})",
                  static_cast<int>(rhi->getGraphicsAPI()),
                  subpass);
        rhi->DestroyShaderModule(vert_shader_module);
        rhi->DestroyShaderModule(frag_shader_module);
        return;
    }

    rhi->DestroyShaderModule(vert_shader_module);
    rhi->DestroyShaderModule(frag_shader_module);
    m_PipelineReady = true;
}

void ZSlateEditorOverlay::DestroyGpuBuffers(const std::shared_ptr<RHI>& rhi)
{
    if (rhi == nullptr)
    {
        return;
    }
    for (int slot = 0; slot < kOverlayFrameRing; ++slot)
    {
        if (m_VertexBuffer[slot] != nullptr)
        {
            rhi->DestroyBuffer(m_VertexBuffer[slot]);
            m_VertexBuffer[slot] = nullptr;
        }
        if (m_VertexMemory[slot] != nullptr)
        {
            rhi->FreeMemory(m_VertexMemory[slot]);
            m_VertexMemory[slot] = nullptr;
        }
        if (m_IndexBuffer[slot] != nullptr)
        {
            rhi->DestroyBuffer(m_IndexBuffer[slot]);
            m_IndexBuffer[slot] = nullptr;
        }
        if (m_IndexMemory[slot] != nullptr)
        {
            rhi->FreeMemory(m_IndexMemory[slot]);
            m_IndexMemory[slot] = nullptr;
        }
        m_VertexCapacity[slot] = 0;
        m_IndexCapacity[slot] = 0;
    }
}

void ZSlateEditorOverlay::EnsureGpuBuffers(const std::shared_ptr<RHI>& rhi, size_t vertex_count, size_t index_count)
{
    if (rhi == nullptr)
    {
        return;
    }
    const int slot = m_FrameSlot;
    if (vertex_count <= m_VertexCapacity[slot] && index_count <= m_IndexCapacity[slot])
    {
        return;
    }

    // Grow only the current ring slot. Other slots may still be referenced by an
    // in-flight frame, so they must not be destroyed here.
    if (m_VertexBuffer[slot] != nullptr)
    {
        rhi->DestroyBuffer(m_VertexBuffer[slot]);
        m_VertexBuffer[slot] = nullptr;
    }
    if (m_VertexMemory[slot] != nullptr)
    {
        rhi->FreeMemory(m_VertexMemory[slot]);
        m_VertexMemory[slot] = nullptr;
    }
    if (m_IndexBuffer[slot] != nullptr)
    {
        rhi->DestroyBuffer(m_IndexBuffer[slot]);
        m_IndexBuffer[slot] = nullptr;
    }
    if (m_IndexMemory[slot] != nullptr)
    {
        rhi->FreeMemory(m_IndexMemory[slot]);
        m_IndexMemory[slot] = nullptr;
    }

    const RHIDeviceSize vertex_bytes = static_cast<RHIDeviceSize>(vertex_count * sizeof(UiVertex));
    const RHIDeviceSize index_bytes = static_cast<RHIDeviceSize>(index_count * sizeof(uint16_t));

    rhi->CreateBuffer(vertex_bytes,
                      RHI_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                      RHI_MEMORY_PROPERTY_HOST_VISIBLE_BIT | RHI_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      m_VertexBuffer[slot],
                      m_VertexMemory[slot]);
    rhi->CreateBuffer(index_bytes,
                      RHI_BUFFER_USAGE_INDEX_BUFFER_BIT,
                      RHI_MEMORY_PROPERTY_HOST_VISIBLE_BIT | RHI_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      m_IndexBuffer[slot],
                      m_IndexMemory[slot]);

    m_VertexCapacity[slot] = vertex_count;
    m_IndexCapacity[slot] = index_count;
}

void ZSlateEditorOverlay::UploadBatch(const std::shared_ptr<RHI>& rhi, float display_width, float display_height,
                                      float display_pos_x, float display_pos_y)
{
    const UiRenderBatch& batch = m_Renderer.getBatch();
    const std::vector<UiVertex>& src_vertices = batch.getVertices();
    const std::vector<uint16_t>& src_indices = batch.getIndices();
    if (src_vertices.empty() || src_indices.empty() || display_width <= 0.0f || display_height <= 0.0f)
    {
        return;
    }

    // Pick this frame's ring slot. The RHI has already fenced this slot's previous
    // GPU usage (BeginFrame waits on it), so it is safe to overwrite now.
    m_FrameSlot = static_cast<int>(rhi->GetCurrentFrameIndex()) % kOverlayFrameRing;

    m_CpuVertices.resize(src_vertices.size());
    const float scale_x = 2.0f / display_width;
    const float scale_y = 2.0f / display_height;
    for (size_t i = 0; i < src_vertices.size(); ++i)
    {
        // ZSlate records vertices in ImGui screen coordinates, which are offset by
        // the main viewport position (DisplayPos). ImGui's own render backend maps
        // (pos - DisplayPos) -> NDC; mirror that here so ZSlate content aligns with
        // ImGui content AND with io.MousePos (also DisplayPos-relative). Without this
        // subtraction every ZSlate pixel is shifted down-right by DisplayPos, which
        // breaks hit-testing against the mouse (menu bar, console widgets, ...).
        m_CpuVertices[i].pos[0] = (src_vertices[i].pos[0] - display_pos_x) * scale_x - 1.0f;
        m_CpuVertices[i].pos[1] = 1.0f - (src_vertices[i].pos[1] - display_pos_y) * scale_y;
        m_CpuVertices[i].color[0] = src_vertices[i].color[0];
        m_CpuVertices[i].color[1] = src_vertices[i].color[1];
        m_CpuVertices[i].color[2] = src_vertices[i].color[2];
        m_CpuVertices[i].color[3] = src_vertices[i].color[3];
        m_CpuVertices[i].uv[0] = src_vertices[i].uv[0];
        m_CpuVertices[i].uv[1] = src_vertices[i].uv[1];
    }

    EnsureGpuBuffers(rhi, src_vertices.size(), src_indices.size());

    const int slot = m_FrameSlot;
    void* vertex_data = nullptr;
    rhi->MapMemory(m_VertexMemory[slot], 0, static_cast<RHIDeviceSize>(m_CpuVertices.size() * sizeof(UiVertex)), 0, &vertex_data);
    memcpy(vertex_data, m_CpuVertices.data(), m_CpuVertices.size() * sizeof(UiVertex));
    rhi->UnmapMemory(m_VertexMemory[slot]);

    void* index_data = nullptr;
    rhi->MapMemory(m_IndexMemory[slot], 0, static_cast<RHIDeviceSize>(src_indices.size() * sizeof(uint16_t)), 0, &index_data);
    memcpy(index_data, src_indices.data(), src_indices.size() * sizeof(uint16_t));
    rhi->UnmapMemory(m_IndexMemory[slot]);
}

void ZSlateEditorOverlay::DrawBatch(const std::shared_ptr<RHI>& rhi)
{
    if (!m_PipelineReady || rhi == nullptr || m_Pipeline == nullptr)
    {
        return;
    }

    const UiRenderBatch& batch = m_Renderer.getBatch();
    if (batch.empty())
    {
        return;
    }

#if defined(_WIN32)
    ScopedOverlayDescriptorBind overlay_bind(rhi);
#endif

    // All windows have recorded by now, so every requested glyph size is baked.
    // Re-upload any native glyph atlas that grew this frame (keeps each atlas'
    // handle_id stable so the commands above still resolve).
    if (UiGpuResources* gpu = UiGpuResources::Get())
    {
        gpu->RefreshNativeFontAtlasesIfDirty();
    }

    // ZSlate windows record in absolute screen coordinates (window client origin
    // + offset), the same space EditorSlateHost::GetPointerPos uses for input hit-
    // testing. Map to NDC using the native display metrics: size = logical window
    // size, pos = window client origin in desktop coords (the DisplayPos that must
    // be subtracted at NDC mapping time, see UploadBatch). Sourcing both from the
    // same host the input path uses guarantees rendered geometry and hit-testing
    // stay aligned without any ImGui dependency.
    float display_width = m_EditorDisplayWidth;
    float display_height = m_EditorDisplayHeight;
    const Vector2 display_pos = EditorSlateHost::Get().GetDisplayPos();
    float display_pos_x = display_pos.x;
    float display_pos_y = display_pos.y;
    if (display_width <= 0.0f || display_height <= 0.0f)
    {
        return;
    }
    UploadBatch(rhi, display_width, display_height, display_pos_x, display_pos_y);
    const int slot = m_FrameSlot;
    if (m_VertexBuffer[slot] == nullptr || m_IndexBuffer[slot] == nullptr)
    {
        return;
    }

    UiGpuResources* gpu = UiGpuResources::Get();
    if (gpu == nullptr)
    {
        return;
    }

    float color[4] = {0.4f, 0.8f, 1.0f, 1.0f};
    RHICommandBuffer* command_buffer = rhi->GetCurrentCommandBuffer();
    rhi->PushEvent(command_buffer, "ZSlate native overlay", color);

    const uint32_t fb_width = rhi->GetSwapchainInfo().extent.width;
    const uint32_t fb_height = rhi->GetSwapchainInfo().extent.height;

    RHIViewport viewport {
        0.0f, 0.0f, static_cast<float>(fb_width), static_cast<float>(fb_height), 0.0f, 1.0f};
    rhi->CmdBindPipelinePFN(command_buffer, RHI_PIPELINE_BIND_POINT_GRAPHICS, m_Pipeline);
    rhi->CmdSetViewportPFN(command_buffer, 0, 1, &viewport);

    RHIBuffer* vertex_buffers[] = {m_VertexBuffer[slot]};
    RHIDeviceSize offsets[] = {0};
    rhi->CmdBindVertexBuffersPFN(command_buffer, 0, 1, vertex_buffers, offsets);
    rhi->CmdBindIndexBufferPFN(command_buffer, m_IndexBuffer[slot], 0, RHI_INDEX_TYPE_UINT16);

    // Map logical clip rects to physical framebuffer pixels for scissor.
    const float scissor_scale_x =
        display_width > 0.0f ? static_cast<float>(fb_width) / display_width : 1.0f;
    const float scissor_scale_y =
        display_height > 0.0f ? static_cast<float>(fb_height) / display_height : 1.0f;

    const std::vector<UiDrawCommand>& commands = batch.getCommands();
    const uint32_t command_total = static_cast<uint32_t>(commands.size());

    auto draw_command = [&](const UiDrawCommand& command) {
        if (command.index_count == 0)
        {
            return;
        }

        RHIRect2D scissor {};
        scissor.offset.x = 0;
        scissor.offset.y = 0;
        scissor.extent.width = fb_width;
        scissor.extent.height = fb_height;
        if (command.has_clip)
        {
            // Clip rects are recorded in DisplayPos-offset ImGui screen space; map
            // to framebuffer pixels by subtracting DisplayPos before scaling (same
            // transform as the vertex NDC mapping in UploadBatch).
            float x0 = (command.clip_rect.x - display_pos_x) * scissor_scale_x;
            float y0 = (command.clip_rect.y - display_pos_y) * scissor_scale_y;
            float x1 = (command.clip_rect.x + command.clip_rect.width - display_pos_x) * scissor_scale_x;
            float y1 = (command.clip_rect.y + command.clip_rect.height - display_pos_y) * scissor_scale_y;
            x0 = std::clamp(x0, 0.0f, static_cast<float>(fb_width));
            y0 = std::clamp(y0, 0.0f, static_cast<float>(fb_height));
            x1 = std::clamp(x1, 0.0f, static_cast<float>(fb_width));
            y1 = std::clamp(y1, 0.0f, static_cast<float>(fb_height));
            scissor.offset.x = static_cast<int32_t>(x0);
            scissor.offset.y = static_cast<int32_t>(y0);
            scissor.extent.width = static_cast<uint32_t>(x1 > x0 ? x1 - x0 : 0.0f);
            scissor.extent.height = static_cast<uint32_t>(y1 > y0 ? y1 - y0 : 0.0f);
        }
        rhi->CmdSetScissorPFN(command_buffer, 0, 1, &scissor);

        RHIDescriptorSet* descriptor_set = gpu->GetDescriptorSet(command.texture_id);
        if (descriptor_set != nullptr)
        {
            rhi->CmdBindDescriptorSetsPFN(
                command_buffer, RHI_PIPELINE_BIND_POINT_GRAPHICS, m_Layout, 0, 1, &descriptor_set, 0, nullptr);
        }
        rhi->CmdDrawIndexedPFN(command_buffer, command.index_count, 1, command.index_offset, 0, 0);
    };

    auto draw_range = [&](uint32_t first, uint32_t last_exclusive) {
        for (uint32_t i = first; i < last_exclusive && i < command_total; ++i)
        {
            draw_command(commands[i]);
        }
    };

    if (m_Groups.empty())
    {
        // No per-window grouping (e.g. self-test only): draw in natural order.
        draw_range(0, command_total);
    }
    else
    {
        // Base layer: anything recorded before the first window group (self-test).
        draw_range(0, m_Groups.front().first_command);

        // P10b: sort groups by their explicit z-order layer (kZPanel < kZForeground).
        // stable_sort keeps equal-z groups in insertion (paint) order, which for the
        // tiled panel layer reproduces the ImGui NoBringToFrontOnFocus creation order
        // this replaced; foreground chrome (higher z) composites last (on top).
        std::vector<uint32_t> order(m_Groups.size());
        for (uint32_t i = 0; i < m_Groups.size(); ++i)
        {
            order[i] = i;
        }
        std::stable_sort(order.begin(), order.end(),
                         [&](uint32_t a, uint32_t b) { return m_Groups[a].z_order < m_Groups[b].z_order; });

        for (uint32_t gi : order)
        {
            const uint32_t first = m_Groups[gi].first_command;
            const uint32_t last =
                (gi + 1 < m_Groups.size()) ? m_Groups[gi + 1].first_command : command_total;
            draw_range(first, last);
        }
    }

    rhi->PopEvent(command_buffer);
}

void ZSlateEditorOverlay::DrawExternalBatchToFloatingSurface(const std::shared_ptr<RHI>& rhi,
                                                             const void* key,
                                                             const UiRenderBatch& batch,
                                                             uint32_t width,
                                                             uint32_t height)
{
    if (!m_PipelineReady || rhi == nullptr || m_Pipeline == nullptr || key == nullptr)
    {
        return;
    }
    const std::vector<UiVertex>& src_vertices = batch.getVertices();
    const std::vector<uint16_t>& src_indices = batch.getIndices();
    if (src_vertices.empty() || src_indices.empty() || width == 0 || height == 0)
    {
        return;
    }

    UiGpuResources* gpu = UiGpuResources::Get();
    if (gpu == nullptr)
    {
        return;
    }
    // Re-upload any glyph atlas that grew while the floating panel painted.
    gpu->RefreshNativeFontAtlasesIfDirty();

    FloatingRing& ring = m_FloatingRings[key];
    const int slot = static_cast<int>(rhi->GetCurrentFrameIndex()) % kOverlayFrameRing;

    // Grow this window's ring slot if needed.
    if (src_vertices.size() > ring.vertex_capacity[slot] || src_indices.size() > ring.index_capacity[slot])
    {
        if (ring.vertex_buffer[slot] != nullptr)
        {
            rhi->DestroyBuffer(ring.vertex_buffer[slot]);
            ring.vertex_buffer[slot] = nullptr;
        }
        if (ring.vertex_memory[slot] != nullptr)
        {
            rhi->FreeMemory(ring.vertex_memory[slot]);
            ring.vertex_memory[slot] = nullptr;
        }
        if (ring.index_buffer[slot] != nullptr)
        {
            rhi->DestroyBuffer(ring.index_buffer[slot]);
            ring.index_buffer[slot] = nullptr;
        }
        if (ring.index_memory[slot] != nullptr)
        {
            rhi->FreeMemory(ring.index_memory[slot]);
            ring.index_memory[slot] = nullptr;
        }
        rhi->CreateBuffer(static_cast<RHIDeviceSize>(src_vertices.size() * sizeof(UiVertex)),
                          RHI_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                          RHI_MEMORY_PROPERTY_HOST_VISIBLE_BIT | RHI_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          ring.vertex_buffer[slot],
                          ring.vertex_memory[slot]);
        rhi->CreateBuffer(static_cast<RHIDeviceSize>(src_indices.size() * sizeof(uint16_t)),
                          RHI_BUFFER_USAGE_INDEX_BUFFER_BIT,
                          RHI_MEMORY_PROPERTY_HOST_VISIBLE_BIT | RHI_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          ring.index_buffer[slot],
                          ring.index_memory[slot]);
        ring.vertex_capacity[slot] = src_vertices.size();
        ring.index_capacity[slot] = src_indices.size();
    }
    if (ring.vertex_buffer[slot] == nullptr || ring.index_buffer[slot] == nullptr)
    {
        return;
    }

    // Map to NDC against the floating window's own size; display origin is (0,0)
    // because a floating panel paints in its own client space (not main-window
    // screen space).
    const float display_width = static_cast<float>(width);
    const float display_height = static_cast<float>(height);
    const float scale_x = 2.0f / display_width;
    const float scale_y = 2.0f / display_height;
    m_CpuVertices.resize(src_vertices.size());
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

    void* vertex_data = nullptr;
    rhi->MapMemory(ring.vertex_memory[slot], 0,
                   static_cast<RHIDeviceSize>(m_CpuVertices.size() * sizeof(UiVertex)), 0, &vertex_data);
    memcpy(vertex_data, m_CpuVertices.data(), m_CpuVertices.size() * sizeof(UiVertex));
    rhi->UnmapMemory(ring.vertex_memory[slot]);

    void* index_data = nullptr;
    rhi->MapMemory(ring.index_memory[slot], 0,
                   static_cast<RHIDeviceSize>(src_indices.size() * sizeof(uint16_t)), 0, &index_data);
    memcpy(index_data, src_indices.data(), src_indices.size() * sizeof(uint16_t));
    rhi->UnmapMemory(ring.index_memory[slot]);

#if defined(_WIN32)
    ScopedOverlayDescriptorBind overlay_bind(rhi);
#endif

    RHICommandBuffer* command_buffer = rhi->GetCurrentCommandBuffer();
    float color[4] = {0.4f, 0.8f, 1.0f, 1.0f};
    rhi->PushEvent(command_buffer, "ZSlate floating overlay", color);

    RHIViewport viewport {0.0f, 0.0f, display_width, display_height, 0.0f, 1.0f};
    rhi->CmdBindPipelinePFN(command_buffer, RHI_PIPELINE_BIND_POINT_GRAPHICS, m_Pipeline);
    rhi->CmdSetViewportPFN(command_buffer, 0, 1, &viewport);

    RHIBuffer* vertex_buffers[] = {ring.vertex_buffer[slot]};
    RHIDeviceSize offsets[] = {0};
    rhi->CmdBindVertexBuffersPFN(command_buffer, 0, 1, vertex_buffers, offsets);
    rhi->CmdBindIndexBufferPFN(command_buffer, ring.index_buffer[slot], 0, RHI_INDEX_TYPE_UINT16);

    const std::vector<UiDrawCommand>& commands = batch.getCommands();
    for (const UiDrawCommand& command : commands)
    {
        if (command.index_count == 0)
        {
            continue;
        }
        RHIRect2D scissor {};
        scissor.offset.x = 0;
        scissor.offset.y = 0;
        scissor.extent.width = width;
        scissor.extent.height = height;
        if (command.has_clip)
        {
            float x0 = std::clamp(command.clip_rect.x, 0.0f, display_width);
            float y0 = std::clamp(command.clip_rect.y, 0.0f, display_height);
            float x1 = std::clamp(command.clip_rect.x + command.clip_rect.width, 0.0f, display_width);
            float y1 = std::clamp(command.clip_rect.y + command.clip_rect.height, 0.0f, display_height);
            scissor.offset.x = static_cast<int32_t>(x0);
            scissor.offset.y = static_cast<int32_t>(y0);
            scissor.extent.width = static_cast<uint32_t>(x1 > x0 ? x1 - x0 : 0.0f);
            scissor.extent.height = static_cast<uint32_t>(y1 > y0 ? y1 - y0 : 0.0f);
        }
        rhi->CmdSetScissorPFN(command_buffer, 0, 1, &scissor);

        RHIDescriptorSet* descriptor_set = gpu->GetDescriptorSet(command.texture_id);
        if (descriptor_set != nullptr)
        {
            rhi->CmdBindDescriptorSetsPFN(
                command_buffer, RHI_PIPELINE_BIND_POINT_GRAPHICS, m_Layout, 0, 1, &descriptor_set, 0, nullptr);
        }
        rhi->CmdDrawIndexedPFN(command_buffer, command.index_count, 1, command.index_offset, 0, 0);
    }

    rhi->PopEvent(command_buffer);
}

void ZSlateEditorOverlay::ReleaseFloatingRing(const std::shared_ptr<RHI>& rhi, const void* key)
{
    if (rhi == nullptr || key == nullptr)
    {
        return;
    }
    auto found = m_FloatingRings.find(key);
    if (found == m_FloatingRings.end())
    {
        return;
    }
    FloatingRing& ring = found->second;
    for (int slot = 0; slot < kOverlayFrameRing; ++slot)
    {
        if (ring.vertex_buffer[slot] != nullptr)
        {
            rhi->DestroyBuffer(ring.vertex_buffer[slot]);
        }
        if (ring.vertex_memory[slot] != nullptr)
        {
            rhi->FreeMemory(ring.vertex_memory[slot]);
        }
        if (ring.index_buffer[slot] != nullptr)
        {
            rhi->DestroyBuffer(ring.index_buffer[slot]);
        }
        if (ring.index_memory[slot] != nullptr)
        {
            rhi->FreeMemory(ring.index_memory[slot]);
        }
    }
    m_FloatingRings.erase(found);
}

void ZSlateEditorOverlay::Destroy(const std::shared_ptr<RHI>& rhi)
{
    // The RHI has no DestroyPipeline / DestroyPipelineLayout (the runtime UIPass
    // leaks its pipeline at shutdown too). Only the host-visible GPU buffers are
    // explicitly released; the pipeline/layout live for the editor lifetime.
    DestroyGpuBuffers(rhi);
}
}  // namespace ZSlate
