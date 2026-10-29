#include "Runtime/Function/Render/Passes/DX12MainCameraPass.h"
#include "Runtime/Function/Render/MegaLights/MegaLightsSystem.h"

#include "Runtime/Function/Render/Interface/DX12/DX12BindlessTextureManager.h"
#include "Runtime/Function/Render/Interface/DX12/DX12RHI.h"
#include "Runtime/Function/Render/Interface/DX12/DX12RHIResource.h"
#include "Runtime/Function/Render/Interface/DX12/DX12ShaderCompiler.h"
#include "Runtime/Function/Render/Passes/ShadowPassShared.h"
#include "Runtime/Function/Render/RenderCamera.h"
#include "Runtime/Function/Render/RenderMesh.h"
#include "Runtime/Function/Render/RenderResource.h"
#include "Runtime/Function/Render/RenderScene.h"
#include "Runtime/Function/Render/RenderSystem.h"
#include "Runtime/Function/Render/RenderType.h"
#include "Runtime/Function/Render/Spine/SpineGpuRenderer.h"
#if defined(Z_HAS_VULKAN)
    // VT's .cpp files only build where the Vulkan backend is compiled in (see Runtime/CMakeLists.txt).
    #include "Runtime/Function/Render/Texture/VirtualTextureManager.h"
    #include "Runtime/Function/Render/Texture/VirtualTextureResource.h"
    #include "Runtime/Function/Render/Texture/VirtualTextureSettings.h"
    #include "Runtime/Function/Render/Texture/VirtualTextureMath.h"
    #include "Runtime/Function/Render/Texture/VirtualTextureTypes.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <exception>
#include <format>
#include <string>
#include <vector>

namespace
{
    // =====================================================================
    // PR-DX1: Bindless production HLSL
    // ---------------------------------------------------------------------
    // Both skybox and scene_grid use the same root signature template:
    //   b0 = 32-bit bindless packed index
    //   b1 = root CBV for per-draw UBO
    //   s0..s3 = bindless static sampler bank
    //
    // The skybox shader uses ResourceDescriptorHeap[] for the cubemap;
    // the scene_grid shader ignores the bindless index (consistent layout
    // across all production passes).
    // =====================================================================

    const char* k_dx12_skybox_vertex_shader = R"(
struct VSOutput
{
    float4 position : SV_POSITION;
    float3 direction : TEXCOORD0;
};

VSOutput main(uint vertex_id : SV_VertexID)
{
    float2 positions[3] = {
        float2(-1.0f, -1.0f),
        float2(-1.0f,  3.0f),
        float2( 3.0f, -1.0f)
    };

    float2 p = positions[vertex_id];
    VSOutput output;
    output.position = float4(p, 1.0f, 1.0f);
    output.direction = float3(p.x, -p.y, 1.0f);
    return output;
}
)";

    const char* k_dx12_skybox_fragment_shader = R"(
// PR-DX1: bindless skybox pixel shader (SM 6.6 + HLSL 2021)
// Reads cubemap from the global bindless heap via
// ResourceDescriptorHeap[NonUniformResourceIndex(texture_index)].

cbuffer BindlessIndices : register(b0)
{
    uint g_packed_indices;
};

cbuffer SkyboxConstants : register(b1)
{
    float4 g_camera_right_tan_aspect;
    float4 g_camera_up_tan;
    float4 g_camera_forward_padding;
};

// Static samplers from the bindless root signature.
// s1 = LinearClamp — suitable for cubemap sampling.
SamplerState g_linear_clamp : register(s1);

struct PSInput
{
    float4 position : SV_POSITION;
    float3 direction : TEXCOORD0;
};

float4 main(PSInput input) : SV_TARGET
{
    float3 camera_right = g_camera_right_tan_aspect.xyz;
    float  right_w = g_camera_right_tan_aspect.w;
    float3 camera_up = g_camera_up_tan.xyz;
    float  up_w = g_camera_up_tan.w;
    float3 camera_forward = g_camera_forward_padding.xyz;
    float3 world_direction = normalize(camera_forward + camera_right * input.direction.x * right_w -
                                       camera_up * input.direction.y * up_w);

    float3 sample_direction = float3(world_direction.x, world_direction.z, world_direction.y);

    // Unpack bindless index and sample from the global heap.
    const uint texture_index = g_packed_indices & 0xFFFFu;
    TextureCube g_skybox =
        ResourceDescriptorHeap[NonUniformResourceIndex(texture_index)];
    float3 color = g_skybox.SampleLevel(g_linear_clamp, sample_direction, 0.0f).rgb;

    color = color / (color + 1.0f);
    color = pow(saturate(color), 1.0f / 2.2f);
    return float4(color, 1.0f);
}
)";

    const char* k_dx12_scene_grid_vertex_shader = R"(
struct VSOutput
{
    float4 position : SV_POSITION;
};

VSOutput main(uint vertex_id : SV_VertexID)
{
    float2 positions[3] = {
        float2(-1.0f, -1.0f),
        float2(-1.0f,  3.0f),
        float2( 3.0f, -1.0f)
    };

    VSOutput output;
    output.position = float4(positions[vertex_id], 0.0f, 1.0f);
    return output;
}
)";

    const char* k_dx12_axis_vertex_shader = R"(
cbuffer PerFrame : register(b0)
{
    float4x4 proj_view_matrix;
};

cbuffer AxisDraw : register(b1)
{
    float4x4 model_matrix;
    uint selected_axis;
    uint3 _padding;
};

struct VsInput
{
    float3 position : POSITION;
    float2 texcoord : TEXCOORD0;
};

struct VsOutput
{
    float4 position : SV_POSITION;
    float3 color : COLOR0;
};

VsOutput main(VsInput input)
{
    float4 world = mul(model_matrix, float4(input.position, 1.0f));
    float4 clip_position = mul(proj_view_matrix, world);
    // Match GLSL axis.vert: push overlay geometry to the near plane in clip space.
    clip_position.z = clip_position.z * 0.0001f;

    float3 color = float3(1.0f, 1.0f, 1.0f);
    if (input.texcoord.x < 0.01f)
    {
        color = (selected_axis == 0u) ? float3(1.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f);
    }
    else if (input.texcoord.x < 1.01f)
    {
        color = (selected_axis == 1u) ? float3(1.0f, 1.0f, 0.0f) : float3(0.0f, 1.0f, 0.0f);
    }
    else if (input.texcoord.x < 2.01f)
    {
        color = (selected_axis == 2u) ? float3(1.0f, 1.0f, 0.0f) : float3(0.0f, 0.0f, 1.0f);
    }

    VsOutput output;
    output.position = clip_position;
    output.color = color;
    return output;
}
)";

    const char* k_dx12_axis_fragment_shader = R"(
struct PsInput
{
    float4 position : SV_POSITION;
    float3 color : COLOR0;
};

float4 main(PsInput input) : SV_TARGET
{
    return float4(input.color, 1.0f);
}
)";

    struct alignas(16) DX12AxisDrawConstants
    {
        Matrix4x4 model_matrix = Matrix4x4::IDENTITY;
        uint32_t selected_axis = 3;
        uint32_t padding[3] = {};
    };

    static_assert(sizeof(DX12AxisDrawConstants) == 80, "DX12AxisDrawConstants must match HLSL cbuffer layout");

    struct DX12AxisPerFrameConstants
    {
        Matrix4x4 proj_view_matrix = Matrix4x4::IDENTITY;
    };

    static_assert(sizeof(DX12AxisPerFrameConstants) == sizeof(Matrix4x4),
                  "DX12AxisPerFrameConstants must contain only proj_view_matrix");

    const char* k_dx12_scene_grid_fragment_shader = R"(
// PR-DX1: bindless scene_grid pixel shader.
// Uses the same root signature template as skybox for consistency,
// but does not sample any bindless texture (g_packed_indices is unused).

cbuffer BindlessIndices : register(b0)
{
    uint g_packed_indices; // unused — kept for root-signature consistency
};

cbuffer SceneGridConstants : register(b1)
{
    float4 g_camera_position_plane_height;
    float4 g_camera_right_tan_aspect;
    float4 g_camera_up_tan;
    float4 g_camera_forward_opacity;
    float4 g_viewport;
    float4 g_grid_params;
};

struct PSInput
{
    float4 position : SV_POSITION;
};

float gridLineAxis(float coord, float spacing, float derivative)
{
    float grid = abs(frac(coord / spacing + 0.5f) - 0.5f) / max(derivative, 0.0001f);
    return 1.0f - saturate(grid);
}

float gridLine(float2 world_x_y, float spacing)
{
    // Per-axis lines then combine with max. Using min() suppresses one family
    // when fwidth differs strongly across the view (looks like horizontal moire only).
    float minor_x = gridLineAxis(world_x_y.x, spacing, fwidth(world_x_y.x));
    float minor_y = gridLineAxis(world_x_y.y, spacing, fwidth(world_x_y.y));
    return max(minor_x, minor_y);
}

float axisLine(float coord)
{
    return 1.0f - saturate(abs(coord) / max(fwidth(coord), 0.0001f));
}

float4 main(PSInput input) : SV_TARGET
{
    float2 uv = (input.position.xy - g_viewport.xy) / max(g_viewport.zw, float2(1.0f, 1.0f));
    if (any(uv < 0.0f) || any(uv > 1.0f))
    {
        discard;
    }

    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float3 camera_position = g_camera_position_plane_height.xyz;
    float  plane_height = g_camera_position_plane_height.w;
    float3 camera_right = g_camera_right_tan_aspect.xyz;
    float  tan_aspect = g_camera_right_tan_aspect.w;
    float3 camera_up = g_camera_up_tan.xyz;
    float  tan_half_fovy = g_camera_up_tan.w;
    float3 camera_forward = g_camera_forward_opacity.xyz;
    float  opacity = g_camera_forward_opacity.w;

    float3 ray = normalize(camera_forward + camera_right * ndc.x * tan_aspect + camera_up * ndc.y * tan_half_fovy);
    float denom = ray.z;
    if (abs(denom) < 0.0001f)
    {
        discard;
    }

    float t = (plane_height - camera_position.z) / denom;
    if (t <= 0.0f)
    {
        discard;
    }

    // Fade with distance along the view ray (not just XY distance from camera).
    float distance_along_ray = t * length(ray);
    float3 world_position = camera_position + ray * t;
    float2 world_x_y = world_position.xy;
    float distance_to_camera = distance_along_ray;

    float minor = gridLine(world_x_y, g_grid_params.x) * saturate(1.0f - distance_to_camera / g_grid_params.z);
    float major = gridLine(world_x_y, g_grid_params.y) * saturate(1.0f - distance_to_camera / g_grid_params.w);
    float axis_x = axisLine(world_x_y.y) * saturate(1.0f - distance_to_camera / g_grid_params.w);
    float axis_y = axisLine(world_x_y.x) * saturate(1.0f - distance_to_camera / g_grid_params.w);

    float3 grid_color = float3(0.47f, 0.52f, 0.60f) * minor * 0.22f;
    grid_color = max(grid_color, float3(0.58f, 0.63f, 0.72f) * major * 0.42f);
    grid_color = lerp(grid_color, float3(0.85f, 0.35f, 0.35f), axis_x * 0.85f);
    grid_color = lerp(grid_color, float3(0.35f, 0.80f, 0.45f), axis_y * 0.85f);

    float alpha = max(max(minor * 0.22f, major * 0.42f), max(axis_x, axis_y) * 0.85f) * opacity;
    if (alpha <= 0.001f)
    {
        discard;
    }

    return float4(grid_color, alpha);
}
)";

    struct DX12SkyboxConstants
    {
        Vector4 camera_right_tan_aspect;
        Vector4 camera_up_tan;
        Vector4 camera_forward_padding;
    };

    struct DX12SceneGridConstants
    {
        Vector4 camera_position_plane_height;
        Vector4 camera_right_tan_aspect;
        Vector4 camera_up_tan;
        Vector4 camera_forward_opacity;
        Vector4 viewport;
        Vector4 grid_params;
    };

    // PR-DX1: Pin the constant buffer layouts against the HLSL cbuffer
    // declarations. Both shaders declare float4 fields in cbuffer(b1);
    // HLSL packs float4 at 16-byte boundaries with no padding. If the
    // C++ struct size or alignment drifts (e.g. someone adds a padding
    // field, changes Vector4 to Vector3), these fire at compile time
    // instead of silently uploading misaligned data.
    static_assert(sizeof(DX12SkyboxConstants) == 3 * sizeof(Vector4),
                  "PR-DX1: DX12SkyboxConstants must be exactly 3 float4s "
                  "(camera_right_tan_aspect, camera_up_tan, camera_forward_padding)");
    static_assert(alignof(DX12SkyboxConstants) == alignof(Vector4),
                  "PR-DX1: DX12SkyboxConstants alignment must match Vector4 (HLSL cbuffer row alignment)");
    static_assert(sizeof(DX12SceneGridConstants) == 6 * sizeof(Vector4),
                  "PR-DX1: DX12SceneGridConstants must be exactly 6 float4s "
                  "(camera_position_plane_height, camera_right_tan_aspect, camera_up_tan, "
                  "camera_forward_opacity, viewport, grid_params)");
    static_assert(alignof(DX12SceneGridConstants) == alignof(Vector4),
                  "PR-DX1: DX12SceneGridConstants alignment must match Vector4 (HLSL cbuffer row alignment)");

    constexpr float k_scene_grid_plane_height = -0.05f;
    constexpr float k_scene_grid_opacity = 0.80f;
    constexpr int k_scene_grid_major_line_every = 10;
    constexpr float k_scene_grid_minor_fade_distance = 80.0f;
    constexpr float k_scene_grid_major_fade_distance = 800.0f;

    float getSceneGridMinorSpacing(float camera_height_above_plane)
    {
        const float safe_height = std::max(camera_height_above_plane, 1.0f);
        const float major_spacing = std::pow(10.0f, std::floor(std::log10(safe_height)));
        return std::max(1.0f, major_spacing / static_cast<float>(k_scene_grid_major_line_every));
    }

    // Helper: minimal CheckDX12 for the pass (matches the RHI's pattern).
    bool PassCheckDX12(HRESULT result, const char* message)
    {
        if (FAILED(result))
        {
            LOG_ERROR(ZRender, "{} HRESULT=0x{:08X}", message, static_cast<unsigned int>(result));
            return false;
        }
        return true;
    }

    bool ResolveSceneOverlayViewport(RHI* rhi, RHIViewport& out_viewport, RHIRect2D& out_scissor)
    {
        if (auto* render_system = GET_SYSTEM(RenderSystem))
        {
            if (render_system->TryGetRenderSceneViewport(out_viewport, out_scissor))
            {
                return out_viewport.width > 0.0f && out_viewport.height > 0.0f;
            }
        }

        if (rhi == nullptr)
        {
            return false;
        }

        RHIViewport* viewport_ptr = rhi->GetViewport(ViewportType::scene);
        if (viewport_ptr == nullptr || viewport_ptr->width <= 0.0f || viewport_ptr->height <= 0.0f)
        {
            return false;
        }

        out_viewport = *viewport_ptr;
        out_scissor = rhi->GetSwapchainInfo().scissor[static_cast<uint32_t>(ViewportType::scene)];
        return true;
    }

    void LogAxisDrawDiagnosticsOnce(size_t axis_key,
                                    const RHIViewport& scene_viewport,
                                    const RHIViewport& draw_viewport,
                                    uint32_t swap_width,
                                    uint32_t swap_height,
                                    uint32_t index_count,
                                    const Matrix4x4& proj_view_matrix,
                                    const Matrix4x4& model_matrix)
    {
        static size_t s_last_logged_axis_key = static_cast<size_t>(-1);
        if (axis_key == s_last_logged_axis_key)
        {
            return;
        }
        s_last_logged_axis_key = axis_key;

        const Vector4 origin {0.0f, 0.0f, 0.0f, 1.0f};
        // Match HLSL: mul(proj_view, mul(model, position)) and ZSlate grid (Matrix * Vector4).
        const Vector4 clip = proj_view_matrix * (model_matrix * origin);
        const float w = std::abs(clip.w) > 1e-6f ? clip.w : 1.0f;
        const float ndc_x = clip.x / w;
        const float ndc_y = clip.y / w;
        const float pixel_x = (ndc_x + 1.0f) * 0.5f * draw_viewport.width + draw_viewport.x;
        const float pixel_y = (1.0f - ndc_y) * 0.5f * draw_viewport.height + draw_viewport.y;

        const float scene_left = scene_viewport.x;
        const float scene_top = scene_viewport.y;
        const float scene_right = scene_viewport.x + scene_viewport.width;
        const float scene_bottom = scene_viewport.y + scene_viewport.height;
        const bool inside_scene = pixel_x >= scene_left && pixel_x < scene_right && pixel_y >= scene_top &&
                                  pixel_y < scene_bottom;

        LOG_INFO(ZRender,
                 "DrawAxis diag: indices={} swapchain={}x{} scene_vp=({:.0f},{:.0f}) {:.0f}x{:.0f} "
                 "draw_vp=({:.0f},{:.0f}) {:.0f}x{:.0f} origin_ndc=({:.3f},{:.3f}) origin_px=({:.0f},{:.0f}) "
                 "inside_scene={}",
                 index_count,
                 swap_width,
                 swap_height,
                 scene_viewport.x,
                 scene_viewport.y,
                 scene_viewport.width,
                 scene_viewport.height,
                 draw_viewport.x,
                 draw_viewport.y,
                 draw_viewport.width,
                 draw_viewport.height,
                 ndc_x,
                 ndc_y,
                 pixel_x,
                 pixel_y,
                 inside_scene ? 1 : 0);
    }

    bool EnsureAxisHostVisibleBuffer(RHI* rhi,
                                     RHIBufferUsageFlags usage,
                                     RHIBuffer*& buffer,
                                     RHIDeviceMemory*& memory,
                                     size_t& capacity_bytes,
                                     const void* cpu_data,
                                     size_t byte_size)
    {
        if (rhi == nullptr || cpu_data == nullptr || byte_size == 0)
        {
            return false;
        }

        if (capacity_bytes < byte_size)
        {
            if (buffer != nullptr)
            {
                rhi->DestroyBuffer(buffer);
                buffer = nullptr;
            }
            if (memory != nullptr)
            {
                rhi->FreeMemory(memory);
                memory = nullptr;
            }

            rhi->CreateBuffer(static_cast<RHIDeviceSize>(byte_size),
                              usage,
                              RHI_MEMORY_PROPERTY_HOST_VISIBLE_BIT | RHI_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                              buffer,
                              memory);
            if (buffer == nullptr || memory == nullptr)
            {
                capacity_bytes = 0;
                return false;
            }
            capacity_bytes = byte_size;
        }

        void* mapped_data = nullptr;
        if (!rhi->MapMemory(memory, 0, static_cast<RHIDeviceSize>(byte_size), 0, &mapped_data) || mapped_data == nullptr)
        {
            return false;
        }

        std::memcpy(mapped_data, cpu_data, byte_size);
        rhi->UnmapMemory(memory);
        return true;
    }
}  // namespace

void DX12MainCameraPass::Initialize(const RenderPassInitInfo* init_info)
{
    RenderPass::Initialize(init_info);

    if (!BuildBindlessProductionRootSignature())
    {
        LOG_ERROR(ZRender, "DX12MainCameraPass: failed to build bindless production root signature");
        return;
    }

    m_EnableFxaa = false;
    if (init_info != nullptr)
    {
        const MainCameraPassInitInfo* camera_init = static_cast<const MainCameraPassInitInfo*>(init_info);
        m_EnableFxaa = camera_init->enable_fxaa;
    }

    m_FramebufferResourcesReady = m_FramebufferResources.Initialize(m_Rhi, m_EnableFxaa);
    if (!m_FramebufferResourcesReady)
    {
        LOG_ERROR(ZRender, "DX12MainCameraPass: MainCameraFramebufferResources::initialize failed");
    }

    m_SkyboxReady = false;
    m_SkyboxSetupAborted = false;
    m_SceneGridReady = SetupSceneGridResources();

    if (m_FramebufferResourcesReady)
    {
        RenderPassCommonInfo rp1_common {};
        rp1_common.rhi = m_Rhi;
        rp1_common.render_resource = m_RenderResource;
        m_Rp1Pass.SetCommonInfo(rp1_common);
        m_Rp1Pass.SetFramebufferResources(&m_FramebufferResources);
        m_Rp1Pass.SetShadowImageViews(m_DirectionalLightShadowColorImageView, m_PointLightShadowColorImageView);
        m_Rp1Pass.SetPerMeshLayout(ShadowPassShared::GetPerMeshLayoutPtr());
        try
        {
            m_Rp1Ready = m_Rp1Pass.Initialize();
            m_RgAbCapture.Initialize(m_Rhi);
            LOG_INFO(ZRender, "DX12MainCameraPass: MainCameraRp1Pass.Initialize() returned {}", m_Rp1Ready);
            if (m_Rp1Ready)
            {
                LOG_INFO(ZRender, "DX12MainCameraPass: MainCameraRp1Pass initialized (Plan C mesh SkyPass)");
            }
            else
            {
                LOG_WARNING(ZRender, "DX12MainCameraPass: MainCameraRp1Pass FAILED to initialize (check MainCameraRp1Pass::Initialize logs)");
            }
        }
        catch (const std::exception& ex)
        {
            LOG_ERROR(ZRender, "DX12MainCameraPass: MainCameraRp1Pass init EXCEPTION: {}", ex.what());
            m_Rp1Ready = false;
        }

        RenderPassCommonInfo tonemap_common {};
        tonemap_common.rhi = m_Rhi;
        tonemap_common.render_resource = m_RenderResource;
        m_TonemapPass.SetCommonInfo(tonemap_common);

        const auto extent = m_Rhi->GetSwapchainInfo().extent;
        BindlessTonemapPassInitInfo tonemap_init {};
        tonemap_init.source_hdr_view =
            m_FramebufferResources.getAttachmentView(_main_camera_pass_backup_buffer_odd);
        tonemap_init.target_ldr_view =
            m_FramebufferResources.getAttachmentView(_main_camera_pass_backup_buffer_even);
        tonemap_init.target_ldr_format = RHI_FORMAT_R16G16B16A16_SFLOAT;
        tonemap_init.width = extent.width;
        tonemap_init.height = extent.height;
#ifdef ZENGINE_DX12_UTILITY_SHADER_ROOT
        tonemap_init.hlsl_search_root = ZENGINE_DX12_UTILITY_SHADER_ROOT;
#endif
        try
        {
            m_TonemapPass.Initialize(&tonemap_init);
            m_TonemapReady = m_TonemapPass.isReady();
            if (m_TonemapReady)
            {
                LOG_INFO(ZRender, "DX12MainCameraPass: BindlessTonemapPass initialized");
            }
        }
        catch (const std::exception& ex)
        {
            LOG_ERROR(ZRender, "DX12MainCameraPass: BindlessTonemapPass init failed: {}", ex.what());
            m_TonemapReady = false;
        }

        RenderPassCommonInfo rp2_common {};
        rp2_common.rhi = m_Rhi;
        rp2_common.render_resource = m_RenderResource;
        m_Rp2Pass.SetCommonInfo(rp2_common);
        m_Rp2Pass.SetFramebufferResources(&m_FramebufferResources);
        // GPU-Spine（控制台 `spine.load` 才有东西画）：DX12 编辑器没有 UI 层可画（RP2 的 UI 层是一张
        // 透明兜底图，ZSlate / ImGui 在 RP2 之后直接画 swapchain），所以画在 Combine UI 之后、同一个
        // swapchain pass 里 —— 预乘 alpha 叠在合成好的场景上，面板随后盖在它上面（与 Vulkan 的层次一致）。
        m_Rp2Pass.SetCombineOverlay([this](RHIRenderPass* ldr_render_pass) {
            SpineGpuRenderer::Get().DrawInEditorViewport(m_Rhi, ldr_render_pass, 0);
        });
        try
        {
            m_Rp2Ready = m_Rp2Pass.Initialize(m_EnableFxaa);
            if (m_Rp2Ready)
            {
                LOG_INFO(ZRender, "DX12MainCameraPass: MainCameraRp2Pass initialized");
            }
        }
        catch (const std::exception& ex)
        {
            LOG_ERROR(ZRender, "DX12MainCameraPass: MainCameraRp2Pass init failed: {}", ex.what());
            m_Rp2Ready = false;
        }
    }

    if (!m_AxisReady)
    {
        m_AxisReady = SetupAxisResources();
        if (m_AxisReady)
        {
            LOG_INFO(ZRender, "DX12MainCameraPass: editor axis gizmo initialized");
        }
    }
}

RHIRenderPass* DX12MainCameraPass::getRp2HdrRenderPass() const
{
    return m_FramebufferResourcesReady ? m_FramebufferResources.getRp2HdrRenderPass() : nullptr;
}

RHIRenderPass* DX12MainCameraPass::getRp2LdrRenderPass() const
{
    return m_FramebufferResourcesReady ? m_FramebufferResources.getRp2LdrRenderPass() : nullptr;
}

RHIImageView* DX12MainCameraPass::getUiLayerColorView() const
{
    if (!m_FramebufferResourcesReady)
    {
        return nullptr;
    }
    return m_FramebufferResources.getAttachmentView(_main_camera_pass_backup_buffer_even);
}

std::vector<RHIImageView*> DX12MainCameraPass::getFramebufferImageViews() const
{
    std::vector<RHIImageView*> views;
    if (!m_FramebufferResourcesReady)
    {
        return views;
    }
    const auto& attachments = m_FramebufferResources.getAttachments();
    views.reserve(attachments.size());
    for (const auto& attachment : attachments)
    {
        views.push_back(attachment.view);
    }
    return views;
}

void DX12MainCameraPass::UpdateAfterFramebufferRecreate()
{
    if (!m_FramebufferResourcesReady)
    {
        return;
    }
    m_FramebufferResources.UpdateAfterFramebufferRecreate();

    const auto extent = m_Rhi->GetSwapchainInfo().extent;

    if (m_TonemapReady)
    {
        m_TonemapPass.UpdateAfterFramebufferRecreate(
            m_FramebufferResources.getAttachmentView(_main_camera_pass_backup_buffer_odd),
            m_FramebufferResources.getAttachmentView(_main_camera_pass_backup_buffer_even),
            extent.width,
            extent.height);
    }

    if (m_Rp2Ready)
    {
        m_Rp2Pass.UpdateAfterFramebufferRecreate();
    }

    if (m_Rp1Ready)
    {
        // G-buffer views are recreated above; deferred lighting input attachments must be
        // re-written or the lit pass binds stale/freed SRVs (RenderDoc AV on startup resize).
        m_Rp1Pass.RefreshDeferredLightingInputAttachments();
        // Nanite hand-holds the GBuffer framebuffer across frames (so its own Render() skips
        // the offscreen target); the handle it holds was just destroyed and rebuilt.
        m_Rp1Pass.RefreshNaniteInjection();
    }
}

void DX12MainCameraPass::PreparePassData(std::shared_ptr<RenderResourceBase> render_resource)
{
    if (RenderResource* resource = dynamic_cast<RenderResource*>(render_resource.get()))
    {
        m_MainCameraPerFrameByViewport = resource->m_MainCameraPerFrameByViewport;
    }

    if (m_Rp1Ready)
    {
        m_Rp1Pass.PreparePassData(render_resource);
    }
}

// =====================================================================
// PR-DX1: Bindless production root signature
// =====================================================================
// Shared between skybox and scene_grid. Layout:
//   Root param 0: 32-bit constants (1 DWORD) at b0/space0 — packed bindless index
//   Root param 1: Root CBV at b1/space0 — per-draw UBO
//   Static samplers: s0..s3 (same bank as bindless_blit_ps.hlsl)
//   Flags: CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
// =====================================================================
bool DX12MainCameraPass::BuildBindlessProductionRootSignature()
{
    auto* dx12_rhi = dynamic_cast<DX12RHI*>(m_Rhi);
    if (!dx12_rhi || !dx12_rhi->getDevice())
    {
        return false;
    }

    ID3D12Device* device = dx12_rhi->getDevice();

    // Root parameter 0: bindless packed index (1 × 32-bit constant)
    D3D12_ROOT_PARAMETER root_params[2] = {};
    root_params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    root_params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    root_params[0].Constants.ShaderRegister = 0;
    root_params[0].Constants.RegisterSpace = 0;
    root_params[0].Constants.Num32BitValues = 1;

    // Root parameter 1: root CBV for per-draw UBO
    root_params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    root_params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    root_params[1].Descriptor.ShaderRegister = 1;
    root_params[1].Descriptor.RegisterSpace = 0;

    // Static sampler bank: same layout as DX12RHI::createPipelineLayout's
    // bindless_static_samplers (s0..s3). MUST match BindlessBlitSampler enum
    // and bindless_blit_ps.hlsl.
    D3D12_STATIC_SAMPLER_DESC static_samplers[DX12RHI::kBindlessStaticSamplerCount] = {};
    auto fill_sampler = [](D3D12_STATIC_SAMPLER_DESC& s,
                           D3D12_FILTER filter,
                           D3D12_TEXTURE_ADDRESS_MODE addr,
                           UINT slot) {
        s.Filter = filter;
        s.AddressU = addr;
        s.AddressV = addr;
        s.AddressW = addr;
        s.MipLODBias = 0.0f;
        s.MaxAnisotropy = 1;
        s.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        s.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
        s.MinLOD = 0.0f;
        s.MaxLOD = D3D12_FLOAT32_MAX;
        s.ShaderRegister = slot;
        s.RegisterSpace = 0;
        s.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    };
    fill_sampler(static_samplers[0], D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_WRAP, 0);
    fill_sampler(static_samplers[1], D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, 1);
    fill_sampler(static_samplers[2], D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_WRAP, 2);
    fill_sampler(static_samplers[3], D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, 3);

    D3D12_ROOT_SIGNATURE_DESC root_sig_desc = {};
    root_sig_desc.NumParameters = 2;
    root_sig_desc.pParameters = root_params;
    root_sig_desc.NumStaticSamplers = DX12RHI::kBindlessStaticSamplerCount;
    root_sig_desc.pStaticSamplers = static_samplers;
    root_sig_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
                          D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;

    ComPtr<ID3DBlob> signature_blob;
    ComPtr<ID3DBlob> error_blob;
    HRESULT hr = D3D12SerializeRootSignature(&root_sig_desc,
                                             D3D_ROOT_SIGNATURE_VERSION_1,
                                             signature_blob.GetAddressOf(),
                                             error_blob.GetAddressOf());
    if (FAILED(hr))
    {
        if (error_blob)
        {
            LOG_ERROR(ZRender,
                      "DX12MainCameraPass: D3D12SerializeRootSignature failed: {}",
                      static_cast<const char*>(error_blob->GetBufferPointer()));
        }
        return false;
    }

    if (!PassCheckDX12(device->CreateRootSignature(0,
                                                   signature_blob->GetBufferPointer(),
                                                   signature_blob->GetBufferSize(),
                                                   IID_PPV_ARGS(&m_BindlessRootSignature)),
                       "DX12MainCameraPass: CreateRootSignature failed"))
    {
        return false;
    }

    LOG_INFO(ZRender, "DX12MainCameraPass: bindless production root signature built successfully");
    return true;
}

bool DX12MainCameraPass::BuildSkyboxOverlayRootSignature()
{
    auto* dx12_rhi = dynamic_cast<DX12RHI*>(m_Rhi);
    if (!dx12_rhi || !dx12_rhi->getDevice())
    {
        return false;
    }

    ID3D12Device* device = dx12_rhi->getDevice();

    D3D12_DESCRIPTOR_RANGE srv_range {};
    srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srv_range.NumDescriptors = 1;
    srv_range.BaseShaderRegister = 0;
    srv_range.RegisterSpace = 0;
    srv_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER root_params[2] = {};
    root_params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    root_params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    root_params[0].Descriptor.ShaderRegister = 0;
    root_params[0].Descriptor.RegisterSpace = 0;

    root_params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    root_params[1].DescriptorTable.NumDescriptorRanges = 1;
    root_params[1].DescriptorTable.pDescriptorRanges = &srv_range;

    D3D12_STATIC_SAMPLER_DESC static_samplers[DX12RHI::kBindlessStaticSamplerCount] = {};
    auto fill_sampler = [](D3D12_STATIC_SAMPLER_DESC& s,
                           D3D12_FILTER filter,
                           D3D12_TEXTURE_ADDRESS_MODE addr,
                           UINT slot) {
        s.Filter = filter;
        s.AddressU = addr;
        s.AddressV = addr;
        s.AddressW = addr;
        s.MipLODBias = 0.0f;
        s.MaxAnisotropy = 1;
        s.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        s.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
        s.MinLOD = 0.0f;
        s.MaxLOD = D3D12_FLOAT32_MAX;
        s.ShaderRegister = slot;
        s.RegisterSpace = 0;
        s.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    };
    fill_sampler(static_samplers[0], D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_WRAP, 0);
    fill_sampler(static_samplers[1], D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, 1);
    fill_sampler(static_samplers[2], D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_WRAP, 2);
    fill_sampler(static_samplers[3], D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, 3);

    D3D12_ROOT_SIGNATURE_DESC root_sig_desc = {};
    root_sig_desc.NumParameters = 2;
    root_sig_desc.pParameters = root_params;
    root_sig_desc.NumStaticSamplers = DX12RHI::kBindlessStaticSamplerCount;
    root_sig_desc.pStaticSamplers = static_samplers;
    root_sig_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> signature_blob;
    ComPtr<ID3DBlob> error_blob;
    HRESULT hr = D3D12SerializeRootSignature(&root_sig_desc,
                                             D3D_ROOT_SIGNATURE_VERSION_1,
                                             signature_blob.GetAddressOf(),
                                             error_blob.GetAddressOf());
    if (FAILED(hr))
    {
        if (error_blob)
        {
            LOG_ERROR(ZRender,
                      "DX12MainCameraPass: skybox overlay root signature failed: {}",
                      static_cast<const char*>(error_blob->GetBufferPointer()));
        }
        return false;
    }

    return PassCheckDX12(device->CreateRootSignature(0,
                                                     signature_blob->GetBufferPointer(),
                                                     signature_blob->GetBufferSize(),
                                                     IID_PPV_ARGS(&m_SkyboxOverlayRootSignature)),
                         "DX12MainCameraPass: skybox overlay CreateRootSignature failed");
}

bool DX12MainCameraPass::SetupSkyboxResources()
{
    auto* dx12_rhi = dynamic_cast<DX12RHI*>(m_Rhi);
    if (!dx12_rhi)
    {
        return false;
    }

    if (m_GlobalRenderResource == nullptr ||
        m_GlobalRenderResource->m_IblResource.m_SpecularTextureImageView == nullptr)
    {
        return false;
    }

    if (!BuildSkyboxOverlayRootSignature())
    {
        return false;
    }

    ID3D12Device* device = dx12_rhi->getDevice();

    DX12ShaderCompiler vs_compiler;
    DX12ShaderCompileResult vs_result =
        vs_compiler.CompileFromSource(k_dx12_skybox_vertex_shader,
                                      ShaderStage::Vertex,
                                      "dx12_skybox_overlay_vs");
    if (!vs_result.success)
    {
        LOG_ERROR(ZRender, "DX12MainCameraPass: skybox VS compile failed: {}", vs_result.error_message);
        return false;
    }

#ifdef ZENGINE_SHADER_ROOT
    const std::string shader_root = std::string(ZENGINE_SHADER_ROOT) + "/hlsl/rp1/";
#else
    const std::string shader_root = "e:/Engine/ZEngine/engine/shader/hlsl/rp1/";
#endif
    const std::string ps_path = shader_root + "skybox_overlay.frag.hlsl";

    DX12ShaderCompiler ps_compiler;
    DX12ShaderCompileResult ps_result = ps_compiler.CompileFromFile(ps_path,
                                                                    ShaderStage::Fragment,
                                                                    {},
                                                                    {},
                                                                    "main",
                                                                    "ps_6_0",
                                                                    "2021");
    if (!ps_result.success)
    {
        LOG_ERROR(ZRender, "DX12MainCameraPass: skybox PS compile failed: {}", ps_result.error_message);
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc = {};
    pso_desc.pRootSignature = m_SkyboxOverlayRootSignature.Get();

    pso_desc.VS = {vs_result.dxil_code.data(), vs_result.dxil_code.size()};
    pso_desc.PS = {ps_result.dxil_code.data(), ps_result.dxil_code.size()};

    pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso_desc.NumRenderTargets = 1;
    pso_desc.RTVFormats[0] = dx12_rhi->GetSwapchainDXGIFormat();
    pso_desc.SampleDesc.Count = 1;
    pso_desc.SampleDesc.Quality = 0;

    // No depth, no stencil, no blend (skybox overwrites).
    pso_desc.DepthStencilState.DepthEnable = FALSE;
    pso_desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    pso_desc.BlendState.AlphaToCoverageEnable = FALSE;
    pso_desc.BlendState.IndependentBlendEnable = FALSE;
    D3D12_RENDER_TARGET_BLEND_DESC& rt_blend = pso_desc.BlendState.RenderTarget[0];
    rt_blend.BlendEnable = FALSE;
    rt_blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso_desc.RasterizerState.DepthClipEnable = TRUE;
    pso_desc.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

    HRESULT hr = device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&m_SkyboxPso));
    if (FAILED(hr))
    {
        static bool s_logged_skybox_pso_failure = false;
        if (!s_logged_skybox_pso_failure)
        {
            s_logged_skybox_pso_failure = true;
            LOG_ERROR(ZRender,
                      "DX12MainCameraPass: skybox overlay CreateGraphicsPipelineState failed HRESULT=0x{:08X}",
                      static_cast<unsigned int>(hr));
        }
        return false;
    }

    const std::string forward_ps_path = shader_root + "skybox_forward.frag.hlsl";
    DX12ShaderCompileResult forward_ps_result = ps_compiler.CompileFromFile(forward_ps_path,
                                                                            ShaderStage::Fragment,
                                                                            {},
                                                                            {},
                                                                            "main",
                                                                            "ps_6_0",
                                                                            "2021");
    if (!forward_ps_result.success)
    {
        LOG_ERROR(ZRender, "DX12MainCameraPass: skybox forward PS compile failed: {}", forward_ps_result.error_message);
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC forward_pso_desc = pso_desc;
    forward_pso_desc.PS = {forward_ps_result.dxil_code.data(), forward_ps_result.dxil_code.size()};
    forward_pso_desc.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    forward_pso_desc.DepthStencilState.DepthEnable = TRUE;
    forward_pso_desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    forward_pso_desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_EQUAL;
    forward_pso_desc.DepthStencilState.StencilEnable = FALSE;
    forward_pso_desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;

    hr = device->CreateGraphicsPipelineState(&forward_pso_desc, IID_PPV_ARGS(&m_SkyboxForwardPso));
    if (FAILED(hr))
    {
        static bool s_logged_forward_pso_failure = false;
        if (!s_logged_forward_pso_failure)
        {
            s_logged_forward_pso_failure = true;
            LOG_ERROR(ZRender,
                      "DX12MainCameraPass: skybox forward CreateGraphicsPipelineState failed HRESULT=0x{:08X}",
                      static_cast<unsigned int>(hr));
        }
        return false;
    }

    // Create per-viewport constant buffers (game, scene, preview).
    for (size_t i = 0; i < kSkyboxViewportCount; ++i)
    {
        m_Rhi->CreateBuffer(sizeof(DX12SkyboxConstants),
                            RHI_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                            RHI_MEMORY_PROPERTY_HOST_VISIBLE_BIT | RHI_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                            m_SkyboxConstantBuffers[i],
                            m_SkyboxConstantBufferMemories[i]);
    }

    LOG_INFO(ZRender,
             "DX12MainCameraPass: skybox initialized (overlay + RP1 forward HDR, descriptor cubemap SRV)");
    return true;
}

void DX12MainCameraPass::RefreshSkyboxCubemapDescriptor()
{
    // Cubemap SRV is bound per-draw from the live ImageView GPU handle (no bindless heap copy).
}

bool DX12MainCameraPass::SetupSceneGridResources()
{
    auto* dx12_rhi = dynamic_cast<DX12RHI*>(m_Rhi);
    if (!dx12_rhi || !m_BindlessRootSignature)
    {
        return false;
    }

    ID3D12Device* device = dx12_rhi->getDevice();

    // Compile shaders. Scene grid doesn't use ResourceDescriptorHeap,
    // so SM 6.0 default is fine. But we compile with SM 6.6 + HV 2021
    // for consistency (the root signature has DIRECTLY_INDEXED).
    DX12ShaderCompiler grid_vs_compiler;
    DX12ShaderCompileResult grid_vs_result =
        grid_vs_compiler.CompileFromSource(k_dx12_scene_grid_vertex_shader,
                                           ShaderStage::Vertex,
                                           "dx12_scene_grid_bindless_vs");
    if (!grid_vs_result.success)
    {
        LOG_ERROR(ZRender, "DX12MainCameraPass: scene_grid VS compile failed: {}", grid_vs_result.error_message);
        return false;
    }

    DX12ShaderCompiler grid_ps_compiler;
    DX12ShaderCompileResult grid_ps_result =
        grid_ps_compiler.CompileFromSource(k_dx12_scene_grid_fragment_shader,
                                           ShaderStage::Fragment,
                                           "dx12_scene_grid_bindless_ps");
    if (!grid_ps_result.success)
    {
        LOG_ERROR(ZRender, "DX12MainCameraPass: scene_grid PS compile failed: {}", grid_ps_result.error_message);
        return false;
    }

    // Build PSO — same root signature as skybox.
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc = {};
    pso_desc.pRootSignature = m_BindlessRootSignature.Get();

    pso_desc.VS = {grid_vs_result.dxil_code.data(), grid_vs_result.dxil_code.size()};
    pso_desc.PS = {grid_ps_result.dxil_code.data(), grid_ps_result.dxil_code.size()};

    pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso_desc.NumRenderTargets = 1;
    pso_desc.RTVFormats[0] = dx12_rhi->GetSwapchainDXGIFormat();
    pso_desc.SampleDesc.Count = 1;
    pso_desc.SampleDesc.Quality = 0;

    pso_desc.DepthStencilState.DepthEnable = FALSE;
    pso_desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;

    // Alpha blend (same as legacy scene_grid).
    pso_desc.BlendState.AlphaToCoverageEnable = FALSE;
    pso_desc.BlendState.IndependentBlendEnable = FALSE;
    D3D12_RENDER_TARGET_BLEND_DESC& rt_blend = pso_desc.BlendState.RenderTarget[0];
    rt_blend.BlendEnable = TRUE;
    rt_blend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
    rt_blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    rt_blend.BlendOp = D3D12_BLEND_OP_ADD;
    rt_blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    rt_blend.DestBlendAlpha = D3D12_BLEND_ZERO;
    rt_blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    rt_blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso_desc.RasterizerState.DepthClipEnable = TRUE;
    pso_desc.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

    HRESULT hr = device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&m_SceneGridPso));
    // No shader modules to destroy — inline-compiled DXIL.

    if (!PassCheckDX12(hr, "DX12MainCameraPass: scene_grid CreateGraphicsPipelineState failed"))
    {
        return false;
    }

    // Create constant buffer.
    m_Rhi->CreateBuffer(sizeof(DX12SceneGridConstants),
                        RHI_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                        RHI_MEMORY_PROPERTY_HOST_VISIBLE_BIT | RHI_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        m_SceneGridConstantBuffer,
                        m_SceneGridConstantBufferMemory);

    LOG_INFO(ZRender, "DX12MainCameraPass: bindless scene_grid initialized");
    return true;
}

bool DX12MainCameraPass::SetupAxisResources()
{
    auto* dx12_rhi = dynamic_cast<DX12RHI*>(m_Rhi);
    if (!dx12_rhi || !dx12_rhi->getDevice())
    {
        return false;
    }

    ID3D12Device* device = dx12_rhi->getDevice();

    D3D12_ROOT_PARAMETER root_params[2] = {};
    root_params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    root_params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    root_params[0].Descriptor.ShaderRegister = 0;
    root_params[0].Descriptor.RegisterSpace = 0;

    root_params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    root_params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    root_params[1].Descriptor.ShaderRegister = 1;
    root_params[1].Descriptor.RegisterSpace = 0;

    D3D12_ROOT_SIGNATURE_DESC root_sig_desc = {};
    root_sig_desc.NumParameters = 2;
    root_sig_desc.pParameters = root_params;
    root_sig_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> signature_blob;
    ComPtr<ID3DBlob> error_blob;
    HRESULT hr = D3D12SerializeRootSignature(&root_sig_desc,
                                             D3D_ROOT_SIGNATURE_VERSION_1,
                                             signature_blob.GetAddressOf(),
                                             error_blob.GetAddressOf());
    if (FAILED(hr))
    {
        if (error_blob)
        {
            LOG_ERROR(ZRender,
                      "DX12MainCameraPass: axis root signature serialize failed: {}",
                      static_cast<const char*>(error_blob->GetBufferPointer()));
        }
        return false;
    }

    if (!PassCheckDX12(device->CreateRootSignature(0,
                                                   signature_blob->GetBufferPointer(),
                                                   signature_blob->GetBufferSize(),
                                                   IID_PPV_ARGS(&m_AxisRootSignature)),
                       "DX12MainCameraPass: axis CreateRootSignature failed"))
    {
        return false;
    }

    DX12ShaderCompiler axis_vs_compiler;
    DX12ShaderCompileResult axis_vs_result =
        axis_vs_compiler.CompileFromSource(k_dx12_axis_vertex_shader, ShaderStage::Vertex, "dx12_axis_vs", {}, {}, "main", "", "", true);
    if (!axis_vs_result.success)
    {
        LOG_ERROR(ZRender, "DX12MainCameraPass: axis VS compile failed: {}", axis_vs_result.error_message);
        return false;
    }

    DX12ShaderCompiler axis_ps_compiler;
    DX12ShaderCompileResult axis_ps_result =
        axis_ps_compiler.CompileFromSource(k_dx12_axis_fragment_shader, ShaderStage::Fragment, "dx12_axis_ps", {}, {}, "main", "", "", true);
    if (!axis_ps_result.success)
    {
        LOG_ERROR(ZRender, "DX12MainCameraPass: axis PS compile failed: {}", axis_ps_result.error_message);
        return false;
    }

    static_assert(sizeof(MeshVertexDataDefinition) == 44, "Axis interleaved layout out of sync with Axis.cpp");
    static constexpr uint32_t k_axis_interleaved_texcoord_offset =
        static_cast<uint32_t>(offsetof(MeshVertexDataDefinition, u));

    D3D12_INPUT_ELEMENT_DESC input_elements[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD",
         0,
         DXGI_FORMAT_R32G32_FLOAT,
         0,
         k_axis_interleaved_texcoord_offset,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
         0},
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc = {};
    pso_desc.pRootSignature = m_AxisRootSignature.Get();
    pso_desc.VS = {axis_vs_result.dxil_code.data(), axis_vs_result.dxil_code.size()};
    pso_desc.PS = {axis_ps_result.dxil_code.data(), axis_ps_result.dxil_code.size()};
    pso_desc.InputLayout = {input_elements, 2u};
    pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso_desc.NumRenderTargets = 1;
    pso_desc.RTVFormats[0] = dx12_rhi->GetSwapchainDXGIFormat();
    pso_desc.SampleDesc.Count = 1;
    pso_desc.SampleDesc.Quality = 0;
    pso_desc.DepthStencilState.DepthEnable = FALSE;
    pso_desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    pso_desc.BlendState.AlphaToCoverageEnable = FALSE;
    pso_desc.BlendState.IndependentBlendEnable = FALSE;
    pso_desc.BlendState.RenderTarget[0].BlendEnable = FALSE;
    pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso_desc.RasterizerState.DepthClipEnable = TRUE;
    pso_desc.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

    hr = device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&m_AxisPso));
    if (!PassCheckDX12(hr, "DX12MainCameraPass: axis CreateGraphicsPipelineState failed"))
    {
        return false;
    }

    m_Rhi->CreateBuffer(sizeof(DX12AxisPerFrameConstants),
                        RHI_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                        RHI_MEMORY_PROPERTY_HOST_VISIBLE_BIT | RHI_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        m_AxisPerFrameConstantBuffer,
                        m_AxisPerFrameConstantBufferMemory);
    m_Rhi->CreateBuffer(sizeof(DX12AxisDrawConstants),
                        RHI_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                        RHI_MEMORY_PROPERTY_HOST_VISIBLE_BIT | RHI_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        m_AxisDrawConstantBuffer,
                        m_AxisDrawConstantBufferMemory);

    return m_AxisPerFrameConstantBuffer != nullptr && m_AxisDrawConstantBuffer != nullptr;
}

void DX12MainCameraPass::DrawAxis()
{
    if (!m_IsShowAxis)
    {
        return;
    }

    if (!m_AxisReady)
    {
        m_AxisReady = SetupAxisResources();
    }

    if (!m_AxisReady || !m_AxisPso || !m_AxisRootSignature)
    {
        static bool warned = false;
        if (!warned)
        {
            LOG_WARNING(ZRender, "DrawAxis: axis PSO/resources not ready");
            warned = true;
        }
        return;
    }

    const std::shared_ptr<RenderScene> render_scene = GET_SYSTEM(RenderSystem)->getRenderScene();
    if (!render_scene || !render_scene->m_RenderAxis.has_value())
    {
        return;
    }

    if (m_VisiableNodes.p_axis_node == nullptr)
    {
        return;
    }

    auto* dx12_rhi = dynamic_cast<DX12RHI*>(m_Rhi);
    if (!dx12_rhi || dx12_rhi->IsDeviceRemoved(" before axis draw"))
    {
        return;
    }

    const std::shared_ptr<RenderCamera> scene_camera = GET_SYSTEM(RenderSystem)->GetCamera(ViewportType::scene);
    if (!scene_camera)
    {
        return;
    }

    RHIViewport scene_viewport {};
    RHIRect2D unused_scissor {};
    if (!ResolveSceneOverlayViewport(m_Rhi, scene_viewport, unused_scissor))
    {
        static bool warned = false;
        if (!warned)
        {
            LOG_WARNING(ZRender,
                        "DrawAxis: skipped (scene viewport {:.0f}x{:.0f})",
                        scene_viewport.width,
                        scene_viewport.height);
            warned = true;
        }
        return;
    }

    size_t axis_mesh_asset_id = render_scene->m_RenderAxis->m_MeshAssetId;
    if (m_VisiableNodes.p_axis_node->mesh_asset_id != 0)
    {
        axis_mesh_asset_id = m_VisiableNodes.p_axis_node->mesh_asset_id;
    }

    const RenderMeshData* axis_source = render_scene->FindAxisMeshSourceData(axis_mesh_asset_id);

    if (axis_source == nullptr || axis_source->m_StaticMeshData.m_VertexBuffer == nullptr ||
        !axis_source->m_StaticMeshData.m_VertexBuffer->IsValid() ||
        axis_source->m_StaticMeshData.m_IndexBuffer == nullptr ||
        !axis_source->m_StaticMeshData.m_IndexBuffer->IsValid())
    {
        static bool warned = false;
        if (!warned)
        {
            LOG_WARNING(ZRender,
                        "DrawAxis: skipped (axis CPU mesh source missing for asset id {})",
                        axis_mesh_asset_id);
            warned = true;
        }
        return;
    }

    const size_t axis_vertex_byte_size = axis_source->m_StaticMeshData.m_VertexBuffer->m_Size;
    const size_t axis_index_byte_size = axis_source->m_StaticMeshData.m_IndexBuffer->m_Size;
    if (axis_vertex_byte_size == 0 || axis_index_byte_size == 0 ||
        axis_index_byte_size % sizeof(uint16_t) != 0)
    {
        return;
    }

    if (!EnsureAxisHostVisibleBuffer(m_Rhi,
                                     RHI_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                     m_AxisHostVisibleVertexBuffer,
                                     m_AxisHostVisibleVertexBufferMemory,
                                     m_AxisHostVisibleVertexCapacity,
                                     axis_source->m_StaticMeshData.m_VertexBuffer->m_Data,
                                     axis_vertex_byte_size))
    {
        static bool warned = false;
        if (!warned)
        {
            LOG_WARNING(ZRender, "DrawAxis: skipped (failed to upload axis vertex buffer)");
            warned = true;
        }
        return;
    }

    if (!EnsureAxisHostVisibleBuffer(m_Rhi,
                                     RHI_BUFFER_USAGE_INDEX_BUFFER_BIT,
                                     m_AxisHostVisibleIndexBuffer,
                                     m_AxisHostVisibleIndexBufferMemory,
                                     m_AxisHostVisibleIndexCapacity,
                                     axis_source->m_StaticMeshData.m_IndexBuffer->m_Data,
                                     axis_index_byte_size))
    {
        static bool warned = false;
        if (!warned)
        {
            LOG_WARNING(ZRender, "DrawAxis: skipped (failed to upload axis index buffer)");
            warned = true;
        }
        return;
    }

    const uint32_t axis_index_count = static_cast<uint32_t>(axis_index_byte_size / sizeof(uint16_t));

    DX12AxisPerFrameConstants per_frame_constants {};
    // Same contract as ZSlateSceneWindow grid overlay: proj(aspect) * view, column-vector path.
    const float scene_aspect =
        scene_viewport.height > 0.0f ? scene_viewport.width / scene_viewport.height : scene_camera->getAspect();
    per_frame_constants.proj_view_matrix =
        scene_camera->GetProjectionMatrixForAspect(scene_aspect) * scene_camera->GetViewMatrix();

    DX12AxisDrawConstants draw_constants {};
    draw_constants.model_matrix = m_VisiableNodes.p_axis_node->model_matrix;
    draw_constants.selected_axis = static_cast<uint32_t>(m_SelectedAxis);

    void* mapped_data = nullptr;
    if (!m_Rhi->MapMemory(m_AxisPerFrameConstantBufferMemory, 0, sizeof(DX12AxisPerFrameConstants), 0, &mapped_data) ||
        mapped_data == nullptr)
    {
        return;
    }
    std::memcpy(mapped_data, &per_frame_constants, sizeof(DX12AxisPerFrameConstants));
    m_Rhi->UnmapMemory(m_AxisPerFrameConstantBufferMemory);

    mapped_data = nullptr;
    if (!m_Rhi->MapMemory(m_AxisDrawConstantBufferMemory, 0, sizeof(DX12AxisDrawConstants), 0, &mapped_data) ||
        mapped_data == nullptr)
    {
        return;
    }
    std::memcpy(mapped_data, &draw_constants, sizeof(DX12AxisDrawConstants));
    m_Rhi->UnmapMemory(m_AxisDrawConstantBufferMemory);

    const auto swap_extent = dx12_rhi->GetSwapchainInfo().extent;

    RHIViewport draw_viewport = scene_viewport;
    RHIRect2D axis_scissor {};
    axis_scissor.offset.x = static_cast<int32_t>(draw_viewport.x);
    axis_scissor.offset.y = static_cast<int32_t>(draw_viewport.y);
    axis_scissor.extent.width = static_cast<uint32_t>(draw_viewport.width);
    axis_scissor.extent.height = static_cast<uint32_t>(draw_viewport.height);

    LogAxisDrawDiagnosticsOnce(axis_mesh_asset_id,
                               scene_viewport,
                               draw_viewport,
                               swap_extent.width,
                               swap_extent.height,
                               axis_index_count,
                               per_frame_constants.proj_view_matrix,
                               draw_constants.model_matrix);

    RHICommandBuffer* command_buffer = m_Rhi->GetCurrentCommandBuffer();
    float event_color[4] = {1.0f, 0.85f, 0.2f, 1.0f};
    m_Rhi->PushEvent(command_buffer, "Axis", event_color);
    dx12_rhi->BeginSwapchainOverlayDraw();

    m_Rhi->CmdSetViewportPFN(command_buffer, 0, 1, &draw_viewport);
    m_Rhi->CmdSetScissorPFN(command_buffer, 0, 1, &axis_scissor);

    auto* dx12_cmd = dx12_rhi->getCurrentCommandList();
    dx12_cmd->SetPipelineState(m_AxisPso.Get());
    dx12_cmd->SetGraphicsRootSignature(m_AxisRootSignature.Get());

    auto* per_frame_buffer = static_cast<DX12Buffer*>(m_AxisPerFrameConstantBuffer);
    auto* draw_buffer = static_cast<DX12Buffer*>(m_AxisDrawConstantBuffer);
    dx12_cmd->SetGraphicsRootConstantBufferView(0, per_frame_buffer->getResource()->GetGPUVirtualAddress());
    dx12_cmd->SetGraphicsRootConstantBufferView(1, draw_buffer->getResource()->GetGPUVirtualAddress());

    auto* vertex_buffer = static_cast<DX12Buffer*>(m_AxisHostVisibleVertexBuffer);
    auto* index_buffer = static_cast<DX12Buffer*>(m_AxisHostVisibleIndexBuffer);
    if (!vertex_buffer || !index_buffer)
    {
        m_Rhi->PopEvent(command_buffer);
        return;
    }

    D3D12_VERTEX_BUFFER_VIEW vertex_buffer_view = {};
    vertex_buffer_view.BufferLocation = vertex_buffer->getResource()->GetGPUVirtualAddress();
    vertex_buffer_view.SizeInBytes = static_cast<UINT>(axis_vertex_byte_size);
    vertex_buffer_view.StrideInBytes = static_cast<UINT>(sizeof(MeshVertexDataDefinition));

    D3D12_INDEX_BUFFER_VIEW index_buffer_view = {};
    index_buffer_view.BufferLocation = index_buffer->getResource()->GetGPUVirtualAddress();
    index_buffer_view.SizeInBytes = static_cast<UINT>(axis_index_byte_size);
    index_buffer_view.Format = DXGI_FORMAT_R16_UINT;

    dx12_cmd->IASetVertexBuffers(0, 1, &vertex_buffer_view);
    dx12_cmd->IASetIndexBuffer(&index_buffer_view);
    dx12_cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    dx12_cmd->DrawIndexedInstanced(axis_index_count, 1, 0, 0, 0);
    m_Rhi->PopEvent(command_buffer);
}

void DX12MainCameraPass::DrawSkybox(ViewportType viewport_type)
{
    const size_t viewport_index = static_cast<size_t>(viewport_type);
    if (!m_SkyboxReady || viewport_index >= 2)
    {
        return;
    }

    RHIViewport* viewport = m_Rhi->GetViewport(viewport_type);
    RHIViewport viewport_for_draw {};
    if (viewport_type == ViewportType::scene)
    {
        RHIRect2D scissor {};
        if (!ResolveSceneOverlayViewport(m_Rhi, viewport_for_draw, scissor))
        {
            static bool warned = false;
            if (!warned)
            {
                LOG_WARNING(ZRender,
                            "DrawSkybox(scene): skipped (scene viewport {:.0f}x{:.0f})",
                            viewport_for_draw.width,
                            viewport_for_draw.height);
                warned = true;
            }
            return;
        }

        DrawSkyboxWithCamera(GET_SYSTEM(RenderSystem)->GetCamera(viewport_type),
                             viewport_for_draw,
                             viewport_index,
                             true);
        return;
    }

    DrawSkyboxWithCamera(GET_SYSTEM(RenderSystem)->GetCamera(viewport_type),
                         viewport != nullptr ? *viewport : RHIViewport {},
                         viewport_index,
                         true);
}

void DX12MainCameraPass::DrawSkyboxPreview()
{
    CameraPreviewRequest request = GET_SYSTEM(RenderSystem)->getCameraPreviewRequest();
    if (!request.enabled)
    {
        return;
    }

    RHIViewport viewport {};
    viewport.x = request.viewport.x;
    viewport.y = request.viewport.y;
    viewport.width = request.viewport.width;
    viewport.height = request.viewport.height;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    DrawSkyboxWithCamera(request.camera, viewport, 2, true);  // slot 2 = preview
}

void DX12MainCameraPass::DrawSkyboxInRp1Forward(ViewportType viewport_type)
{
    if (!m_SkyboxReady || !m_SkyboxForwardPso || !m_SkyboxOverlayRootSignature)
    {
        return;
    }

    const size_t viewport_index = static_cast<size_t>(viewport_type);
    if (viewport_index >= kSkyboxViewportCount)
    {
        return;
    }

    RHIViewport viewport_for_draw {};
    if (viewport_type == ViewportType::scene)
    {
        RHIRect2D scissor {};
        if (!ResolveSceneOverlayViewport(m_Rhi, viewport_for_draw, scissor))
        {
            return;
        }
    }
    else
    {
        RHIViewport* viewport = m_Rhi->GetViewport(viewport_type);
        if (!viewport || viewport->width <= 0.0f || viewport->height <= 0.0f)
        {
            return;
        }
        viewport_for_draw = *viewport;
    }

    DrawSkyboxWithCamera(GET_SYSTEM(RenderSystem)->GetCamera(viewport_type),
                         viewport_for_draw,
                         viewport_index,
                         false);
}

void DX12MainCameraPass::DrawSkyboxWithCamera(const std::shared_ptr<RenderCamera>& camera,
                                              const RHIViewport& viewport,
                                              size_t viewport_slot,
                                              bool swapchain_overlay)
{
    ID3D12PipelineState* active_pso =
        swapchain_overlay ? m_SkyboxPso.Get() : m_SkyboxForwardPso.Get();
    if (!m_SkyboxReady || active_pso == nullptr || !m_SkyboxOverlayRootSignature)
    {
        return;
    }

    auto* dx12_rhi = dynamic_cast<DX12RHI*>(m_Rhi);
    if (!dx12_rhi)
    {
        return;
    }

    if (dx12_rhi->IsDeviceRemoved(" before skybox draw"))
    {
        return;
    }

    if (m_GlobalRenderResource == nullptr ||
        m_GlobalRenderResource->m_IblResource.m_SpecularTextureImageView == nullptr)
    {
        return;
    }

    auto* cubemap_view =
        static_cast<DX12ImageView*>(m_GlobalRenderResource->m_IblResource.m_SpecularTextureImageView);

    // Allocate a fresh per-frame SRV slot for the cubemap. The image view's
    // stored GPU handle may point to a per-frame CBV/SRV/UAV heap slot that
    // was valid at CreateImageView time but has since been reused after the
    // frame's WaitForFences counter reset. Creating a new SRV in the current
    // frame's partition ensures the GPU reads valid descriptor data.
    D3D12_CPU_DESCRIPTOR_HANDLE cubemap_cpu {};
    D3D12_GPU_DESCRIPTOR_HANDLE cubemap_srv {};
    if (cubemap_view == nullptr || cubemap_view->getImage() == nullptr)
    {
        return;
    }
    if (!dx12_rhi->AllocateCbvSrvUavDescriptor(cubemap_cpu, cubemap_srv))
    {
        LOG_WARNING(ZRender, "DrawSkyboxWithCamera: failed to allocate per-frame cubemap SRV");
        return;
    }
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
        srv_desc.Format = cubemap_view->getFormat();
        srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        srv_desc.TextureCube.MostDetailedMip = 0;
        srv_desc.TextureCube.MipLevels = cubemap_view->getMipLevels();
        srv_desc.TextureCube.ResourceMinLODClamp = 0.0f;
        dx12_rhi->getDevice()->CreateShaderResourceView(
            cubemap_view->getImage()->getResource(), &srv_desc, cubemap_cpu);
    }

    if (!camera || viewport.width <= 0.0f || viewport.height <= 0.0f)
    {
        return;
    }

    if (viewport_slot >= kSkyboxViewportCount ||
        m_SkyboxConstantBufferMemories[viewport_slot] == nullptr ||
        m_SkyboxConstantBuffers[viewport_slot] == nullptr)
    {
        return;
    }

    // Upload constants to the UBO.
    const float aspect =
        viewport.height > 0.0f ? std::max(viewport.width / viewport.height, 0.01f) : std::max(camera->getAspect(), 0.01f);
    const Vector3 camera_right = camera->right();
    const Vector3 camera_up = camera->up();
    const Vector3 camera_forward = camera->forward();

    DX12SkyboxConstants constants {};
    if (camera->IsOrthographic())
    {
        const float half_h = camera->GetOrthoHalfHeight();
        const float half_w = half_h * aspect;
        constants.camera_right_tan_aspect = Vector4(camera_right.x, camera_right.y, camera_right.z, half_w);
        constants.camera_up_tan = Vector4(camera_up.x, camera_up.y, camera_up.z, half_h);
        constants.camera_forward_padding = Vector4(camera_forward.x, camera_forward.y, camera_forward.z, 1.0f);
    }
    else
    {
        const float fovy_radians =
            Radian(Math::atan(Math::tan(Radian(Degree(camera->getFOV().x) * 0.5f)) / aspect) * 2.0f).valueRadians();
        const float tan_half_fovy = std::tan(fovy_radians * 0.5f);
        constants.camera_right_tan_aspect =
            Vector4(camera_right.x, camera_right.y, camera_right.z, tan_half_fovy * aspect);
        constants.camera_up_tan = Vector4(camera_up.x, camera_up.y, camera_up.z, tan_half_fovy);
        constants.camera_forward_padding = Vector4(camera_forward.x, camera_forward.y, camera_forward.z, 0.0f);
    }

    void* mapped_data = nullptr;
    if (!m_Rhi->MapMemory(m_SkyboxConstantBufferMemories[viewport_slot], 0, sizeof(DX12SkyboxConstants), 0, &mapped_data) ||
        mapped_data == nullptr)
    {
        return;
    }
    std::memcpy(mapped_data, &constants, sizeof(DX12SkyboxConstants));
    m_Rhi->UnmapMemory(m_SkyboxConstantBufferMemories[viewport_slot]);

    RHICommandBuffer* command_buffer = m_Rhi->GetCurrentCommandBuffer();
    RHIRect2D scissor {{static_cast<int32_t>(viewport.x), static_cast<int32_t>(viewport.y)},
                       {static_cast<uint32_t>(viewport.width), static_cast<uint32_t>(viewport.height)}};

    if (swapchain_overlay)
    {
        dx12_rhi->BeginSwapchainOverlayDraw();

        ID3D12DescriptorHeap* descriptor_heap = dx12_rhi->GetCbvSrvUavDescriptorHeap();
        if (descriptor_heap != nullptr)
        {
            ID3D12GraphicsCommandList* command_list = dx12_rhi->getCurrentCommandList();
            command_list->SetDescriptorHeaps(1, &descriptor_heap);
        }
    }
    else
    {
        ID3D12DescriptorHeap* descriptor_heap = dx12_rhi->GetCbvSrvUavDescriptorHeap();
        if (descriptor_heap != nullptr)
        {
            ID3D12GraphicsCommandList* command_list = dx12_rhi->getCurrentCommandList();
            command_list->SetDescriptorHeaps(1, &descriptor_heap);
        }
    }

    m_Rhi->CmdSetViewportPFN(command_buffer, 0, 1, &viewport);
    m_Rhi->CmdSetScissorPFN(command_buffer, 0, 1, &scissor);

    auto* dx12_cmd = dx12_rhi->getCurrentCommandList();
    dx12_cmd->SetGraphicsRootSignature(m_SkyboxOverlayRootSignature.Get());
    dx12_cmd->SetPipelineState(active_pso);

    auto* dx12_ubo = static_cast<DX12Buffer*>(m_SkyboxConstantBuffers[viewport_slot]);
    dx12_cmd->SetGraphicsRootConstantBufferView(0, dx12_ubo->getResource()->GetGPUVirtualAddress());
    dx12_cmd->SetGraphicsRootDescriptorTable(1, cubemap_srv);

    dx12_cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    dx12_cmd->DrawInstanced(3, 1, 0, 0);
}

void DX12MainCameraPass::DrawSceneGrid()
{
    if (!m_SceneGridReady || !m_SceneGridPso || !m_BindlessRootSignature)
    {
        return;
    }

    auto* dx12_rhi = dynamic_cast<DX12RHI*>(m_Rhi);
    if (!dx12_rhi)
    {
        return;
    }

    const RHISwapChainDesc& swapchain_info = m_Rhi->GetSwapchainInfo();
    RHIViewport* scene_viewport = m_Rhi->GetViewport(ViewportType::scene);
    if (!scene_viewport || scene_viewport->width <= 0.0f || scene_viewport->height <= 0.0f)
    {
        return;
    }

    std::shared_ptr<RenderCamera> scene_camera = GET_SYSTEM(RenderSystem)->GetCamera(ViewportType::scene);
    if (!scene_camera)
    {
        return;
    }

    // Upload constants.
    const Vector3 camera_position = scene_camera->position();
    const float aspect = std::max(scene_camera->getAspect(), 0.01f);
    const float fovy_radians = scene_camera->getFOV().y * 3.14159265358979323846f / 180.0f;
    const float tan_half_fovy = std::tan(fovy_radians * 0.5f);

    DX12SceneGridConstants constants {};
    constants.camera_position_plane_height = Vector4(camera_position.x,
                                                     camera_position.y,
                                                     camera_position.z,
                                                     k_scene_grid_plane_height);
    Vector3 camera_right = scene_camera->right();
    Vector3 camera_up = scene_camera->up();
    Vector3 camera_forward = scene_camera->forward();
    constants.camera_right_tan_aspect = Vector4(camera_right.x, camera_right.y, camera_right.z, tan_half_fovy * aspect);
    constants.camera_up_tan = Vector4(camera_up.x, camera_up.y, camera_up.z, tan_half_fovy);
    constants.camera_forward_opacity = Vector4(camera_forward.x,
                                               camera_forward.y,
                                               camera_forward.z,
                                               k_scene_grid_opacity);
    constants.viewport = Vector4(scene_viewport->x, scene_viewport->y, scene_viewport->width, scene_viewport->height);
    const float minor_spacing =
        getSceneGridMinorSpacing(std::abs(camera_position.z - k_scene_grid_plane_height));
    const float major_spacing = minor_spacing * static_cast<float>(k_scene_grid_major_line_every);
    constants.grid_params = Vector4(minor_spacing,
                                    major_spacing,
                                    k_scene_grid_minor_fade_distance,
                                    k_scene_grid_major_fade_distance);

    void* mapped_data = nullptr;
    if (!m_Rhi->MapMemory(m_SceneGridConstantBufferMemory,
                          0,
                          sizeof(DX12SceneGridConstants),
                          0,
                          &mapped_data) ||
        mapped_data == nullptr)
    {
        return;
    }
    std::memcpy(mapped_data, &constants, sizeof(DX12SceneGridConstants));
    m_Rhi->UnmapMemory(m_SceneGridConstantBufferMemory);

    RHICommandBuffer* command_buffer = m_Rhi->GetCurrentCommandBuffer();
    const RHIRect2D& scene_scissor =
        swapchain_info.scissor[static_cast<uint32_t>(ViewportType::scene)];

    dx12_rhi->BeginSwapchainOverlayDraw();
    dx12_rhi->SetBindlessDescriptorHeaps();

    m_Rhi->CmdSetViewportPFN(command_buffer, 0, 1, scene_viewport);
    m_Rhi->CmdSetScissorPFN(command_buffer, 0, 1, &scene_scissor);

    auto* dx12_cmd = dx12_rhi->getCurrentCommandList();
    dx12_cmd->SetPipelineState(m_SceneGridPso.Get());
    dx12_cmd->SetGraphicsRootSignature(m_BindlessRootSignature.Get());

    // Root param 0: bindless index (unused by scene_grid shader, but
    // pushed for root-signature consistency — slot 0 = white placeholder).
    dx12_cmd->SetGraphicsRoot32BitConstant(0, 0, 0);

    // Root param 1: UBO via root CBV.
    auto* dx12_grid_ubo = static_cast<DX12Buffer*>(m_SceneGridConstantBuffer);
    dx12_cmd->SetGraphicsRootConstantBufferView(1, dx12_grid_ubo->getResource()->GetGPUVirtualAddress());

    dx12_cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    dx12_cmd->DrawInstanced(3, 1, 0, 0);
}

// ---------------------------------------------------------------------------------------------------
// VT overlay: the first IN-FRAME consumer of a virtual texture.
//
// What it proves that the offscreen blitter could not: the page table and the physical pool are
// reached from a shader that runs inside the real frame, through the bindless SRV heap
// (ResourceDescriptorHeap[slot]) -- the way a material would reach them -- and the result lands on the
// swapchain the user is looking at. It is deliberately NOT a material: wiring VT into the mesh
// material system (descriptor layout, per-draw VT handle, UV derivatives for mip selection) is a
// bigger change that touches every material permutation.
// ---------------------------------------------------------------------------------------------------
namespace
{
    // Must match cbuffer VtOverlayParams in shader/hlsl/vt/vt_overlay.frag.hlsl (b1).
    struct DX12VtOverlayConstants
    {
        uint32_t lookup[4];   // page_size, pool_width_pages, pages_x, pages_y
        uint32_t extent[4];   // virtual width, virtual height, page-table slot, pool slot
        float border[4];      // rgb frame colour
        float rect[4];        // overlay size in pixels
        uint32_t v2[4];       // page_border, mip_count, filter (0 point / 1 mip select + bilinear), unused
    };
    static_assert(sizeof(DX12VtOverlayConstants) == 80, "VtOverlayParams layout");
    constexpr uint32_t kVtOverlayConstantBufferBytes = 256;  // root CBV needs a 256-byte-aligned size
    static_assert(sizeof(DX12VtOverlayConstants) <= kVtOverlayConstantBufferBytes, "fits the CBV");
}  // namespace

bool DX12MainCameraPass::SetupVirtualTextureOverlayResources()
{
#if defined(Z_HAS_VULKAN)
    auto* dx12_rhi = dynamic_cast<DX12RHI*>(m_Rhi);
    if (!dx12_rhi || !m_BindlessRootSignature)
    {
        return false;
    }

#ifdef ZENGINE_SHADER_ROOT
    const std::string shader_root = std::string(ZENGINE_SHADER_ROOT) + "/hlsl/vt/";
#else
    const std::string shader_root = "e:/Engine/ZEngine/engine/shader/hlsl/vt/";
#endif

    DX12ShaderCompiler vs_compiler;
    DX12ShaderCompileResult vs_result =
        vs_compiler.CompileFromFile(shader_root + "vt_blit.vert.hlsl", ShaderStage::Vertex, {}, {}, "main", "vs_6_0", "2021");
    if (!vs_result.success)
    {
        LOG_ERROR(ZRender, "DX12MainCameraPass: VT overlay VS compile failed: {}", vs_result.error_message);
        return false;
    }

    // ps_6_6: ResourceDescriptorHeap[] needs SM 6.6.
    DX12ShaderCompiler ps_compiler;
    DX12ShaderCompileResult ps_result =
        ps_compiler.CompileFromFile(shader_root + "vt_overlay.frag.hlsl", ShaderStage::Fragment, {}, {}, "main", "ps_6_6", "2021");
    if (!ps_result.success)
    {
        LOG_ERROR(ZRender, "DX12MainCameraPass: VT overlay PS compile failed: {}", ps_result.error_message);
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc = {};
    pso_desc.pRootSignature = m_BindlessRootSignature.Get();
    pso_desc.VS = {vs_result.dxil_code.data(), vs_result.dxil_code.size()};
    pso_desc.PS = {ps_result.dxil_code.data(), ps_result.dxil_code.size()};
    pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso_desc.NumRenderTargets = 1;
    pso_desc.RTVFormats[0] = dx12_rhi->GetSwapchainDXGIFormat();
    pso_desc.SampleDesc.Count = 1;
    pso_desc.SampleMask = UINT_MAX;
    pso_desc.DepthStencilState.DepthEnable = FALSE;
    pso_desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    pso_desc.BlendState.RenderTarget[0].BlendEnable = FALSE;  // opaque window
    pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso_desc.RasterizerState.DepthClipEnable = TRUE;

    HRESULT hr = dx12_rhi->getDevice()->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&m_VtOverlayPso));
    if (!PassCheckDX12(hr, "DX12MainCameraPass: VT overlay CreateGraphicsPipelineState failed"))
    {
        return false;
    }

    m_Rhi->CreateBuffer(kVtOverlayConstantBufferBytes,
                        RHI_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                        RHI_MEMORY_PROPERTY_HOST_VISIBLE_BIT | RHI_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        m_VtOverlayConstantBuffer,
                        m_VtOverlayConstantBufferMemory);
    if (m_VtOverlayConstantBuffer == nullptr || m_VtOverlayConstantBufferMemory == nullptr)
    {
        return false;
    }
    LOG_INFO(ZRender, "DX12MainCameraPass: VT overlay initialized (bindless page table + physical pool)");
    return true;
#else
    return false;
#endif
}

void DX12MainCameraPass::VerifyVirtualTextureOverlaySelfTest(uint64_t handle)
{
#if defined(Z_HAS_VULKAN)
    m_VtSelfTestState = 2;  // one shot, whatever happens below
    auto resource = VirtualTextureManager::Get().GetVirtualTexture(handle);
    if (!resource || !resource->getSourceData() || m_VtSelfTestReadback == nullptr)
    {
        LOG_ERROR(ZRender, "VT overlay self-test: cannot verify (texture or readback buffer is gone)");
        return;
    }
    const TextureData& source = *resource->getSourceData();
    const uint32_t w = m_VtSelfTestRect[0];
    const uint32_t h = m_VtSelfTestRect[1];
    void* mapped = nullptr;
    if (!m_Rhi->MapMemory(m_VtSelfTestReadbackMemory, 0, static_cast<RHIDeviceSize>(m_VtSelfTestRowPitch) * h, 0, &mapped) ||
        mapped == nullptr)
    {
        LOG_ERROR(ZRender, "VT overlay self-test: cannot map the readback buffer");
        return;
    }
    const uint8_t* bytes = static_cast<const uint8_t*>(mapped);

    // CPU reference: the same contract the shader implements (VirtualTextureMath.h). v2 = nearest-mip selection from
    // the uv derivatives (1/w, 1/h per pixel on a full-rect triangle), coarser-mip residency fallback, clamped bilinear.
    // v1 (border 0) = point sample of mip 0.
    const bool v2 = resource->getPageBorder() > 0;
    const uint32_t mip_count = resource->getSampledMipCount();
    std::vector<VTRef::Image> chain;
    if (v2)
    {
        chain = VTRef::BuildMipChain(source.m_Pixels, source.m_Width, source.m_Height, mip_count);
    }
    const auto resident = [&](uint32_t mip, uint32_t px, uint32_t py) {
        const PageTableEntry entry = resource->GetPageTableEntry(mip, px, py);
        return entry.is_valid && entry.physical_slot != UINT32_MAX;
    };
    const float lod = VTRef::Lod(source.m_Width, source.m_Height, 1.0f / static_cast<float>(w), 0.0f, 0.0f, 1.0f / static_cast<float>(h));
    constexpr float kTolerance = 2.0f;  // hardware bilinear weights are ~8-bit fixed point; the reference is fp32
    constexpr uint32_t kBorder = 3;     // 2 px frame + 1 px of slack

    uint32_t checked = 0, within = 0, mismatch = 0, fallback_pixels = 0, colour_runs = 0, exact = 0;
    uint32_t mip_histogram[16] = {};
    float max_error = 0.0f;
    uint32_t first_bad_x = 0, first_bad_y = 0;
    float first_bad_got[3] = {}, first_bad_want[3] = {};
    uint32_t last_color = 0xFFFFFFFFu;
    for (uint32_t y = kBorder; y + kBorder < h; ++y)
    {
        for (uint32_t x = kBorder; x + kBorder < w; ++x)
        {
            const uint8_t* px = bytes + static_cast<size_t>(y) * m_VtSelfTestRowPitch + static_cast<size_t>(x) * 4u;
            const uint32_t got = static_cast<uint32_t>(px[0]) | (static_cast<uint32_t>(px[1]) << 8) | (static_cast<uint32_t>(px[2]) << 16);
            if (got != last_color)
            {
                ++colour_runs;
                last_color = got;
            }
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(w);
            const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(h);
            float want[3];
            bool is_resident = false;
            if (v2)
            {
                const VTRef::SampleResult ref = VTRef::SampleWithFallback(
                    chain, source.m_Width, source.m_Height, resource->getPageSize(), resident, u, v, lod);
                is_resident = ref.resident;
                want[0] = ref.color.r;
                want[1] = ref.color.g;
                want[2] = ref.color.b;
                if (is_resident)
                {
                    ++mip_histogram[std::min(ref.mip, 15u)];
                }
            }
            else
            {
                const uint32_t tx = std::min(static_cast<uint32_t>(std::clamp(u, 0.0f, 1.0f) * static_cast<float>(source.m_Width)), source.m_Width - 1);
                const uint32_t ty = std::min(static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * static_cast<float>(source.m_Height)), source.m_Height - 1);
                is_resident = resident(0, tx / resource->getPageSize(), ty / resource->getPageSize());
                const uint8_t* t = source.m_Pixels + (static_cast<size_t>(ty) * source.m_Width + tx) * 4u;
                want[0] = t[0];
                want[1] = t[1];
                want[2] = t[2];
            }
            if (!is_resident)
            {
                ++fallback_pixels;  // the shader draws a magenta checkerboard there: not comparable with the source
                continue;
            }
            ++checked;
            const float error = std::max({std::fabs(static_cast<float>(px[0]) - want[0]),
                                          std::fabs(static_cast<float>(px[1]) - want[1]),
                                          std::fabs(static_cast<float>(px[2]) - want[2])});
            max_error = std::max(max_error, error);
            if (error < 0.5f)
            {
                ++exact;
            }
            if (error <= kTolerance)
            {
                ++within;
                continue;
            }
            if (mismatch == 0)
            {
                first_bad_x = x;
                first_bad_y = y;
                for (int c = 0; c < 3; ++c)
                {
                    first_bad_got[c] = static_cast<float>(px[c]);
                    first_bad_want[c] = want[c];
                }
            }
            ++mismatch;
        }
    }
    m_Rhi->UnmapMemory(m_VtSelfTestReadbackMemory);
    m_Rhi->FreeMemory(m_VtSelfTestReadbackMemory);
    m_Rhi->DestroyBuffer(m_VtSelfTestReadback);
    m_VtSelfTestReadbackMemory = nullptr;
    m_VtSelfTestReadback = nullptr;

    // "colour runs" is a sanity probe against a degenerate pass (a flat overlay would trivially match a flat source).
    LOG_INFO(ZRender,
             "VT overlay self-test: {} rect {}x{} lod {:.2f}, compared {} resident pixels: within +-{:.0f}={} (exact={}) "
             "MISMATCH={} max-error={:.2f} (non-resident/checker pixels skipped: {}; colour runs seen: {}; mips 0..3: {}/{}/{}/{})",
             v2 ? "v2(mip+bilinear)" : "v1(point)", w, h, lod, checked, kTolerance, within, exact, mismatch, max_error,
             fallback_pixels, colour_runs, mip_histogram[0], mip_histogram[1], mip_histogram[2], mip_histogram[3]);
    if (mismatch != 0)
    {
        LOG_ERROR(ZRender,
                  "VT overlay self-test FAILED: first mismatch at overlay pixel ({},{}): got rgb=({:.0f},{:.0f},{:.0f}) expected "
                  "rgb=({:.1f},{:.1f},{:.1f})",
                  first_bad_x, first_bad_y, first_bad_got[0], first_bad_got[1], first_bad_got[2], first_bad_want[0],
                  first_bad_want[1], first_bad_want[2]);
    }
    else if (checked == 0)
    {
        LOG_ERROR(ZRender, "VT overlay self-test FAILED: nothing was comparable (no resident texel under the overlay)");
    }
    else
    {
        LOG_INFO(ZRender, "VT overlay self-test PASSED");
    }
#else
    (void)handle;
#endif
}

void DX12MainCameraPass::DrawVirtualTextureOverlay()
{
#if defined(Z_HAS_VULKAN)
    if (!VirtualTextureSettings::IsOverlayEnabled() || !VirtualTextureSettings::IsEnabled())
    {
        return;
    }
    auto* render_system = GET_SYSTEM(RenderSystem);
    auto* dx12_rhi = dynamic_cast<DX12RHI*>(m_Rhi);
    if (render_system == nullptr || dx12_rhi == nullptr || !m_BindlessRootSignature)
    {
        return;
    }

    // The producer hands out a handle only while it owns a live texture (see getGpuVisibleHandle).
    const uint64_t handle = render_system->GetVirtualTextureGpuHandle();
    if (handle == 0)
    {
        return;
    }
    VirtualTextureManager& manager = VirtualTextureManager::Get();
    auto resource = manager.GetVirtualTexture(handle);
    auto pool = manager.getPhysicalTexturePool();
    if (!resource || !pool)
    {
        return;
    }
    RHIImageView* page_table_view = resource->getPageTableTextureView();
    RHIImageView* pool_view = pool->getPhysicalTextureView();
    // No page table = nothing was ever resident; pool without content = still UNDEFINED/never uploaded.
    // Either way there is nothing meaningful to sample yet (the overlay would be pure fallback).
    if (page_table_view == nullptr || pool_view == nullptr || !pool->hasContent())
    {
        return;
    }

    if (!m_VtOverlayReady)
    {
        if (m_VtOverlaySetupFailed)
        {
            return;
        }
        m_VtOverlayReady = SetupVirtualTextureOverlayResources();
        if (!m_VtOverlayReady)
        {
            m_VtOverlaySetupFailed = true;
            LOG_ERROR(ZRender, "DX12MainCameraPass: VT overlay unavailable (setup failed); it will not be retried");
            return;
        }
    }

    // Bindless slots are owned by the pool / the page table resource: they were registered when the views
    // were created (see VirtualTextureCreateGenericImage -- registering later would copy a recycled SRV
    // descriptor). Nothing to allocate or refresh here.
    const uint32_t page_table_slot = resource->getPageTableBindlessSlot();
    const uint32_t pool_slot = pool->getBindlessSlot();
    if (page_table_slot == 0xFFFFFFFFu || pool_slot == 0xFFFFFFFFu)
    {
        static bool s_logged = false;
        if (!s_logged)
        {
            s_logged = true;
            LOG_ERROR(ZRender, "DX12MainCameraPass: VT overlay skipped: page table / pool have no bindless slot (page_table={} pool={})",
                      page_table_slot, pool_slot);
        }
        return;
    }

    // ---- placement: lower-left corner of the Scene viewport, aspect of the virtual texture ----
    // Prefer the Scene panel; when it is not the visible tab fall back to the Game panel, so the overlay
    // is visible whichever viewport the editor is showing.
    RHIViewport* scene_viewport = m_Rhi->GetViewport(ViewportType::scene);
    if (!scene_viewport || scene_viewport->width < 80.0f || scene_viewport->height < 80.0f)
    {
        scene_viewport = m_Rhi->GetViewport(ViewportType::game);
    }
    if (!scene_viewport || scene_viewport->width < 80.0f || scene_viewport->height < 80.0f)
    {
        static bool s_logged = false;
        if (!s_logged)
        {
            s_logged = true;
            RHIViewport* sv = m_Rhi->GetViewport(ViewportType::scene);
            RHIViewport* gv = m_Rhi->GetViewport(ViewportType::game);
            LOG_WARNING(ZRender,
                        "DX12MainCameraPass: VT overlay skipped: neither viewport is visible (scene {:.0f}x{:.0f}, game {:.0f}x{:.0f})",
                        sv ? sv->width : -1.0f, sv ? sv->height : -1.0f, gv ? gv->width : -1.0f, gv ? gv->height : -1.0f);
        }
        return;
    }
    const float vt_w = static_cast<float>(std::max(resource->getVirtualWidth(), 1u));
    const float vt_h = static_cast<float>(std::max(resource->getVirtualHeight(), 1u));
    constexpr float kMargin = 10.0f;
    float overlay_w = std::clamp(scene_viewport->width * 0.35f, 64.0f, 640.0f);
    float overlay_h = overlay_w * vt_h / vt_w;
    const float max_h = scene_viewport->height * 0.5f;
    if (overlay_h > max_h)
    {
        overlay_h = max_h;
        overlay_w = overlay_h * vt_w / vt_h;
    }
    overlay_w = std::floor(overlay_w);
    overlay_h = std::floor(std::max(overlay_h, 8.0f));

    RHIViewport overlay_viewport {};
    overlay_viewport.x = std::floor(scene_viewport->x + kMargin);
    overlay_viewport.y = std::floor(scene_viewport->y + scene_viewport->height - kMargin - overlay_h);
    overlay_viewport.width = overlay_w;
    overlay_viewport.height = overlay_h;
    overlay_viewport.minDepth = 0.0f;
    overlay_viewport.maxDepth = 1.0f;
    RHIRect2D overlay_scissor {};
    overlay_scissor.offset.x = static_cast<int32_t>(overlay_viewport.x);
    overlay_scissor.offset.y = static_cast<int32_t>(overlay_viewport.y);
    overlay_scissor.extent.width = static_cast<uint32_t>(overlay_w);
    overlay_scissor.extent.height = static_cast<uint32_t>(overlay_h);

    // ---- constants ----
    VirtualTextureShaderParams shader_params {};
    if (!resource->BuildShaderParams(*pool, shader_params))
    {
        static bool s_logged_layout = false;
        if (!s_logged_layout)
        {
            s_logged_layout = true;
            LOG_ERROR(ZRender, "DX12MainCameraPass: VT overlay skipped: the texture and the physical pool disagree on the page layout");
        }
        return;
    }
    DX12VtOverlayConstants constants {};
    std::memcpy(constants.lookup, shader_params.lookup, sizeof(constants.lookup));
    std::memcpy(constants.extent, shader_params.extent, sizeof(constants.extent));
    std::memcpy(constants.v2, shader_params.v2, sizeof(constants.v2));
    constants.extent[2] = page_table_slot;
    constants.extent[3] = pool_slot;
    // v2 (mip selection from derivatives + bilinear) needs the gutter; a border-0 layout can only point-sample mip 0.
    constants.v2[2] = shader_params.v2[0] > 0 ? 1u : 0u;
    constants.border[0] = 1.0f;
    constants.border[1] = 0.85f;
    constants.border[2] = 0.2f;
    constants.rect[0] = overlay_w;
    constants.rect[1] = overlay_h;

    void* mapped = nullptr;
    if (!m_Rhi->MapMemory(m_VtOverlayConstantBufferMemory, 0, sizeof(constants), 0, &mapped) || mapped == nullptr)
    {
        return;
    }
    std::memcpy(mapped, &constants, sizeof(constants));
    m_Rhi->UnmapMemory(m_VtOverlayConstantBufferMemory);

    if (handle != m_VtOverlayHandle)
    {
        // A different texture (VT toggled off/on, or the source changed): new page table, new bindless slots.
        // Start the draw counter over and re-arm the self-test so the NEW texture gets verified too.
        m_VtOverlayHandle = handle;
        m_VtOverlayDrawCount = 0;
        if (m_VtSelfTestState == 2)
        {
            m_VtSelfTestState = 0;
        }
    }
    ++m_VtOverlayDrawCount;
    if (m_VtSelfTestState == 1 && m_VtOverlayDrawCount >= m_VtSelfTestVerifyAtDraw)
    {
        // The capture was recorded kVerifyDelay draws ago; by now every in-flight frame slot has been waited on.
        VerifyVirtualTextureOverlaySelfTest(handle);
    }

    // ---- draw ----
    RHICommandBuffer* command_buffer = m_Rhi->GetCurrentCommandBuffer();
    float event_color[4] = {1.0f, 0.2f, 0.9f, 1.0f};
    m_Rhi->PushEvent(command_buffer, "VT Overlay", event_color);
    dx12_rhi->BeginSwapchainOverlayDraw();
    dx12_rhi->SetBindlessDescriptorHeaps();

    m_Rhi->CmdSetViewportPFN(command_buffer, 0, 1, &overlay_viewport);
    m_Rhi->CmdSetScissorPFN(command_buffer, 0, 1, &overlay_scissor);

    auto* dx12_cmd = dx12_rhi->getCurrentCommandList();
    dx12_cmd->SetPipelineState(m_VtOverlayPso.Get());
    dx12_cmd->SetGraphicsRootSignature(m_BindlessRootSignature.Get());
    dx12_cmd->SetGraphicsRoot32BitConstant(0, 0, 0);  // b0: unused by this shader, set for signature consistency
    auto* ubo = static_cast<DX12Buffer*>(m_VtOverlayConstantBuffer);
    dx12_cmd->SetGraphicsRootConstantBufferView(1, ubo->getResource()->GetGPUVirtualAddress());
    dx12_cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    dx12_cmd->DrawInstanced(3, 1, 0, 0);
    m_Rhi->PopEvent(command_buffer);

    // Self-test capture: after the overlay has been on screen for a while, copy its pixels out of the back buffer.
    if (VirtualTextureSettings::IsOverlaySelfTestEnabled() && m_VtSelfTestState == 0 && m_VtOverlayDrawCount >= 40)
    {
        const uint32_t w = static_cast<uint32_t>(overlay_w);
        const uint32_t h = static_cast<uint32_t>(overlay_h);
        const uint32_t row_pitch = (w * 4u + 255u) & ~255u;
        m_Rhi->CreateBuffer(static_cast<RHIDeviceSize>(row_pitch) * h,
                            RHI_BUFFER_USAGE_TRANSFER_DST_BIT,
                            RHI_MEMORY_PROPERTY_HOST_VISIBLE_BIT | RHI_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                            m_VtSelfTestReadback,
                            m_VtSelfTestReadbackMemory);
        uint32_t pitch_out = 0;
        if (m_VtSelfTestReadback != nullptr &&
            dx12_rhi->RecordBackBufferRegionReadback(static_cast<uint32_t>(overlay_viewport.x),
                                                     static_cast<uint32_t>(overlay_viewport.y),
                                                     w,
                                                     h,
                                                     m_VtSelfTestReadback,
                                                     pitch_out))
        {
            m_VtSelfTestRowPitch = pitch_out;
            m_VtSelfTestRect[0] = w;
            m_VtSelfTestRect[1] = h;
            m_VtSelfTestVerifyAtDraw = m_VtOverlayDrawCount + 8;
            m_VtSelfTestState = 1;
        }
        else
        {
            m_VtSelfTestState = 2;
            LOG_ERROR(ZRender, "VT overlay self-test: could not record the back-buffer readback");
        }
    }

    if (!m_VtOverlayLoggedFirstDraw)
    {
        m_VtOverlayLoggedFirstDraw = true;
        LOG_INFO(ZRender,
                 "DX12MainCameraPass: VT overlay first draw: rect=({:.0f},{:.0f}) {:.0f}x{:.0f} "
                 "page_table_slot={} pool_slot={}",
                 overlay_viewport.x,
                 overlay_viewport.y,
                 overlay_w,
                 overlay_h,
                 page_table_slot,
                 pool_slot);
    }
#endif
}

void DX12MainCameraPass::TryLateInitializeSkybox()
{
    if (m_SkyboxReady)
    {
        return;
    }
    if (m_GlobalRenderResource == nullptr ||
        m_GlobalRenderResource->m_IblResource.m_SpecularTextureImageView == nullptr)
    {
        LOG_INFO(ZRender, "TryLateInitializeSkybox: deferred (GlobalRenderResource={:016X}, SpecularView={:016X})",
                 (uint64_t)(uintptr_t)m_GlobalRenderResource,
                 m_GlobalRenderResource ? (uint64_t)(uintptr_t)m_GlobalRenderResource->m_IblResource.m_SpecularTextureImageView : 0);
        return;
    }
    LOG_INFO(ZRender, "TryLateInitializeSkybox: attempting SetupSkyboxResources (SpecularView={:016X})",
             (uint64_t)(uintptr_t)m_GlobalRenderResource->m_IblResource.m_SpecularTextureImageView);
    if (SetupSkyboxResources())
    {
        m_SkyboxReady = true;
        m_SkyboxSetupAborted = false;
        LOG_INFO(ZRender, "TryLateInitializeSkybox: SUCCESS - skybox is now ready");
    }
    else
    {
        LOG_WARNING(ZRender, "TryLateInitializeSkybox: SetupSkyboxResources FAILED");
    }
}

void DX12MainCameraPass::OnGlobalRenderResourceUploaded()
{
    TryLateInitializeSkybox();
    RefreshSkyboxCubemapDescriptor();
    if (m_GlobalRenderResource != nullptr &&
        m_GlobalRenderResource->m_IblResource.m_SpecularTextureImageView != nullptr)
    {
        auto* cubemap_view =
            static_cast<DX12ImageView*>(m_GlobalRenderResource->m_IblResource.m_SpecularTextureImageView);
        if (cubemap_view->getGpuHandle().ptr == 0)
        {
            LOG_WARNING(ZRender, "DX12MainCameraPass: specular IBL cubemap SRV has no GPU handle after upload");
        }
    }
    if (m_Rp1Ready)
    {
        m_Rp1Pass.RefreshMeshGlobalIblDescriptors();
    }
    if (m_Rp2Ready)
    {
        m_Rp2Pass.RefreshColorGradingDescriptorBindings();
    }
}

void DX12MainCameraPass::DrawEditorSkyboxOverlays(const std::array<bool, 2>& skybox_visible)
{
    TryLateInitializeSkybox();
    if (!m_SkyboxReady)
    {
        static bool warned = false;
        if (!warned)
        {
            LOG_WARNING(ZRender,
                        "DX12MainCameraPass: skybox draw skipped (ready={} ibl={} aborted={})",
                        m_SkyboxReady,
                        m_GlobalRenderResource != nullptr &&
                            m_GlobalRenderResource->m_IblResource.m_SpecularTextureImageView != nullptr,
                        m_SkyboxSetupAborted);
            warned = true;
        }
        return;
    }

    if (skybox_visible[static_cast<size_t>(ViewportType::game)])
    {
        DrawSkybox(ViewportType::game);
    }
    if (skybox_visible[static_cast<size_t>(ViewportType::scene)])
    {
        DrawSkybox(ViewportType::scene);
    }
    DrawSkyboxPreview();
}

bool DX12MainCameraPass::DrawRp1Chain(const std::array<bool, 2>& skybox_visible)
{
    if (m_Rp1Ready && m_Rp1Pass.DrawRP1WithGraph(skybox_visible))
    {
        // RP1 driven by the render graph (ZENGINE_RG_RP1=1): the same three Draw*Pass()
        // calls, with the graph's barriers in place of the hand-written group below.
        // DrawRP1WithGraph rejects before recording anything, so a false return falls
        // through to the hand-written chain with an untouched command buffer.
        return true;
    }
    else if (m_Rp1Ready)
    {
        // GBufferPass → DeferredLightingPass → ForwardLightingPass.

        // Pass 1: G-Buffer (writes GBufferA/B/C + Depth).
        m_Rp1Pass.DrawGBufferPass(skybox_visible);

        // Image layout transition: G-Buffer → SHADER_READ_ONLY_OPTIMAL, Depth → DEPTH_STENCIL_READ_ONLY_OPTIMAL.
        // This ensures DeferredLightingPass can read G-Buffer as input attachments.
        if (m_Rhi != nullptr && m_FramebufferResourcesReady)
        {
            RHIImageMemoryBarrier barriers[4] = {};

            // GBufferA
            barriers[0].sType = RHI_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barriers[0].srcAccessMask = RHI_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            barriers[0].dstAccessMask = RHI_ACCESS_SHADER_READ_BIT;
            barriers[0].oldLayout = RHI_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            barriers[0].newLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            barriers[0].srcQueueFamilyIndex = RHI_QUEUE_FAMILY_IGNORED;
            barriers[0].dstQueueFamilyIndex = RHI_QUEUE_FAMILY_IGNORED;
            barriers[0].image = m_FramebufferResources.getAttachmentImage(_main_camera_pass_gbuffer_a);
            barriers[0].subresourceRange.aspectMask = RHI_IMAGE_ASPECT_COLOR_BIT;
            barriers[0].subresourceRange.baseMipLevel = 0;
            barriers[0].subresourceRange.levelCount = 1;
            barriers[0].subresourceRange.baseArrayLayer = 0;
            barriers[0].subresourceRange.layerCount = 1;

            // GBufferB
            barriers[1] = barriers[0];
            barriers[1].image = m_FramebufferResources.getAttachmentImage(_main_camera_pass_gbuffer_b);

            // GBufferC
            barriers[2] = barriers[0];
            barriers[2].image = m_FramebufferResources.getAttachmentImage(_main_camera_pass_gbuffer_c);

            // Depth
            barriers[3].sType = RHI_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barriers[3].srcAccessMask = RHI_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            barriers[3].dstAccessMask = RHI_ACCESS_SHADER_READ_BIT | RHI_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
            barriers[3].oldLayout = RHI_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            barriers[3].newLayout = RHI_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
            barriers[3].srcQueueFamilyIndex = RHI_QUEUE_FAMILY_IGNORED;
            barriers[3].dstQueueFamilyIndex = RHI_QUEUE_FAMILY_IGNORED;
            barriers[3].image = m_Rhi->GetDepthImageInfo().depth_image;
            barriers[3].subresourceRange.aspectMask = RHI_IMAGE_ASPECT_DEPTH_BIT;
            barriers[3].subresourceRange.baseMipLevel = 0;
            barriers[3].subresourceRange.levelCount = 1;
            barriers[3].subresourceRange.baseArrayLayer = 0;
            barriers[3].subresourceRange.layerCount = 1;

            m_Rhi->CmdPipelineBarrier(m_Rhi->GetCurrentCommandBuffer(),
                                       RHI_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | RHI_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                                       RHI_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | RHI_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
                                       0, 0, nullptr, 0, nullptr, 4, barriers);
        }

        // Pass 2: Deferred Lighting + Sky (reads G-Buffer, writes BackupOdd).
        m_Rp1Pass.DrawDeferredLightingPass(skybox_visible);

        // Note: no manual barrier needed here. CmdEndRenderPassPFN already
        // transitions BackupOdd to SHADER_READ_ONLY_OPTIMAL (the render pass
        // finalLayout), and ForwardLightingPass::BeginRenderPass will transition
        // it back to RENDER_TARGET via initialLayout handling.

        // Pass 3: Forward Lighting (reads/writes BackupOdd, transparent objects).
        m_Rp1Pass.DrawForwardLightingPass(skybox_visible);
    }
    return false;
}

void DX12MainCameraPass::DrawRp1AbCapture(const std::array<bool, 2>& skybox_visible)
{
    // Same frame, same per-frame data, RP1 drawn twice: A = hand-written, B = graph. A
    // correct graph path makes the two backup_odd readbacks bit-identical.
    RHIImage* backup_odd = m_FramebufferResources.getAttachmentImage(_main_camera_pass_backup_buffer_odd);
    const RHIExtent2D extent = m_Rhi->GetSwapchainInfo().extent;
    if (!m_Rp1Ready || backup_odd == nullptr)
    {
        m_RgAbCapture.RecordStage("rp1", false, false);
        m_RgAbCapture.AddNote("rp1 skipped: RP1 not ready or backup_odd missing");
        DrawRp1Chain(skybox_visible);
        return;
    }

    const bool graph_was_enabled = m_Rp1Pass.IsRenderGraphPathEnabled();
    MegaLights::MegaLightsSystem* megalights = m_Rp1Pass.m_MegaLightsSystem;
    MegaLights::MegaLightsSystem::HistoryBookmark history {};
    if (megalights != nullptr)
    {
        history = megalights->SaveHistoryBookmark();
    }

    // A: hand-written.
    m_Rp1Pass.SetRenderGraphPathEnabled(false);
    DrawRp1Chain(skybox_visible);
    m_RgAbCapture.CopyImage("rp1.handwritten.backup_odd", backup_odd, RHI_FORMAT_R16G16B16A16_SFLOAT,
                            extent.width, extent.height, RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // B: graph, from the same temporal state.
    if (megalights != nullptr)
    {
        megalights->RestoreHistoryBookmark(history);
    }
    const bool graph_available = m_Rp1Pass.SetRenderGraphPathEnabled(true);
    const bool graph_used = graph_available && DrawRp1Chain(skybox_visible);
    if (!graph_available)
    {
        DrawRp1Chain(skybox_visible);  // keep the frame drawn; the manifest says graph_used=false
    }
    m_RgAbCapture.CopyImage("rp1.graph.backup_odd", backup_odd, RHI_FORMAT_R16G16B16A16_SFLOAT,
                            extent.width, extent.height, RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    m_RgAbCapture.RecordStage("rp1", true, graph_used);
    if (megalights != nullptr)
    {
        m_RgAbCapture.AddNote("rp1: MegaLights temporal bookkeeping restored between the A and B runs");
    }

    m_Rp1Pass.SetRenderGraphPathEnabled(graph_was_enabled);
}

void DX12MainCameraPass::DrawRp2AbCapture(uint32_t swapchain_index)
{
    // RP2 drawn twice from the same backup_odd (RP1's B output): A = hand-written,
    // B = graph. Captures the graded HDR (backup_even) and the composited backbuffer
    // BEFORE the editor overlay (ImGui / ZSlate run later, outside this pass).
    RHIImage* backup_even = m_FramebufferResources.getAttachmentImage(_main_camera_pass_backup_buffer_even);
    const RHISwapChainDesc swapchain = m_Rhi->GetSwapchainInfo();
    RHIImage* backbuffer = swapchain_index < swapchain.images.size() ? swapchain.images[swapchain_index] : nullptr;
    const RHIExtent2D extent = swapchain.extent;

    const auto capture = [&](const char* run) {
        const std::string prefix = std::string("rp2.") + run;
        m_RgAbCapture.CopyImage((prefix + ".backup_even").c_str(), backup_even, RHI_FORMAT_R16G16B16A16_SFLOAT,
                                extent.width, extent.height, RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        // The backbuffer goes back to RENDER_TARGET: the editor overlay draws into it next.
        m_RgAbCapture.CopyImage((prefix + ".backbuffer").c_str(), backbuffer, swapchain.image_format,
                                extent.width, extent.height, RHI_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    };

    const bool graph_was_enabled = m_Rp2Pass.IsRenderGraphPathEnabled();

    m_Rp2Pass.SetRenderGraphPathEnabled(false);
    m_Rp2Pass.DrawRP2(swapchain_index, {});
    capture("handwritten");

    m_Rp2Pass.SetRenderGraphPathEnabled(true);
    m_Rp2Pass.DrawRP2(swapchain_index, {});
    const bool graph_used = m_Rp2Pass.LastDrawUsedRenderGraph();
    capture("graph");
    m_RgAbCapture.RecordStage("rp2", true, graph_used);

    m_Rp2Pass.SetRenderGraphPathEnabled(graph_was_enabled);
}

void DX12MainCameraPass::Draw(const std::vector<RenderCallback>& post_ui_callbacks,
                              const std::array<bool, 2>& skybox_visible)
{
    (void)skybox_visible;
    TryLateInitializeSkybox();

    if (auto* dx12_rhi = dynamic_cast<DX12RHI*>(m_Rhi))
    {
        if (dx12_rhi->IsDeviceRemoved(" at start of MainCamera draw"))
        {
            return;
        }
    }

    // In-frame A/B capture (ZENGINE_RG_AB_CAPTURE=<frame>): on that one frame each stage is
    // drawn twice, hand-written then graph, with a readback after each (see
    // DX12RenderGraphAbCapture.h). Every other frame takes the normal path below.
    const bool ab_capture_frame = m_RgAbCapture.BeginFrame();
    if (ab_capture_frame)
    {
        DrawRp1AbCapture(skybox_visible);
    }
    else
    {
        DrawRp1Chain(skybox_visible);
    }

    // =========================================================================
    // RP1→RP2 barrier: backup_odd is already in SHADER_READ_ONLY state after
    // ForwardLightingPass::EndRenderPass (finalLayout=SHADER_READ_ONLY_OPTIMAL).
    // No additional transition is needed on DX12 — the ResourceBarrier issued by
    // CmdEndRenderPassPFN already ensures visibility of RP1 color writes to
    // subsequent shader reads. The explicit CmdPipelineBarrier below is kept as
    // a no-op safety net (oldLayout matches the tracked state, so it will be
    // skipped by TransitionImage).
    // =========================================================================
    if (m_FramebufferResourcesReady)
    {
        RHIImage* backup_odd_img = m_FramebufferResources.getAttachmentImage(_main_camera_pass_backup_buffer_odd);
        if (backup_odd_img != nullptr)
        {
            RHIImageMemoryBarrier barrier {};
            barrier.sType = RHI_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.pNext = nullptr;
            barrier.srcAccessMask = RHI_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            barrier.dstAccessMask = RHI_ACCESS_SHADER_READ_BIT;
            barrier.oldLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;  // matches tracked state
            barrier.newLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;  // no layout change
            barrier.srcQueueFamilyIndex = RHI_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = RHI_QUEUE_FAMILY_IGNORED;
            barrier.image = backup_odd_img;
            RHIImageSubresourceRange range {};
            range.aspectMask = RHI_IMAGE_ASPECT_COLOR_BIT;
            range.baseMipLevel = 0;
            range.levelCount = 1;
            range.baseArrayLayer = 0;
            range.layerCount = 1;
            barrier.subresourceRange = range;

            m_Rhi->CmdPipelineBarrier(m_Rhi->GetCurrentCommandBuffer(),
                                       RHI_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                       RHI_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                       0, 0, nullptr, 0, nullptr, 1, &barrier);
        }
    }

    // UE-style refactor: disable legacy BindlessTonemapPass.
    // MainCameraRp2Pass::DrawRP2() now handles color_grading/fxaa/combine_ui.
    // Keep this block commented for now; remove after confirming RP2 works.
    if (false && m_TonemapReady)
    {
        m_TonemapPass.Draw();
    }

    if (m_Rp2Ready)
    {
        uint32_t swapchain_index = 0;
        if (auto* dx12_rhi = dynamic_cast<DX12RHI*>(m_Rhi))
        {
            swapchain_index = dx12_rhi->getCurrentBackBufferIndex();
        }
        // Editor ImGui must run after RP2 ends (swapchain overlay). Recording it
        // inside the UI subpass breaks the emulated render pass and leaves the
        // swapchain at the PrepareBeforePass clear color.
        if (ab_capture_frame)
        {
            DrawRp2AbCapture(swapchain_index);
        }
        else
        {
            m_Rp2Pass.DrawRP2(swapchain_index, {});
        }
    }

    // Swapchain skybox overlay is drawn from EditorUIPass after the ZSlate batch.

    for (const RenderCallback& callback : post_ui_callbacks)
    {
        if (callback)
        {
            callback();
        }
    }

    // Axis gizmo is drawn from EditorUIPass::Draw() after the native ZSlate batch so
    // it composites above panel chrome/grid overlays.
}
