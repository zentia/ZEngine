#include "MegaLightsSystem.h"

#include "MegaLightsSettings.h"
#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Core/Math/Math.h"
#include "Runtime/Function/Console/ConsoleManager.h"
#include "Runtime/Function/Render/RenderCamera.h"
#include "Runtime/Function/Render/RenderResource.h"
#include "Runtime/Function/Render/RenderScene.h"
#include "Runtime/Function/Render/RenderSystem.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace MegaLights
{
namespace
{
    void DestroyBuffer(std::shared_ptr<RHI> rhi, RHIBuffer*& buffer, RHIDeviceMemory*& memory)
    {
        if (rhi == nullptr)
        {
            return;
        }
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
    }

    void DestroyImage(std::shared_ptr<RHI> rhi, RHIImage*& image, RHIDeviceMemory*& memory, RHIImageView*& view)
    {
        if (rhi == nullptr)
        {
            return;
        }
        if (view != nullptr)
        {
            rhi->DestroyImageView(view);
            view = nullptr;
        }
        if (image != nullptr)
        {
            rhi->DestroyImage(image);
            image = nullptr;
        }
        if (memory != nullptr)
        {
            rhi->FreeMemory(memory);
            memory = nullptr;
        }
    }

    size_t ViewportHistoryIndex(ViewportType viewport_type)
    {
        return static_cast<size_t>(viewport_type);
    }

    bool SphereIntersectsTileFrustum(const Vector3& center,
                                     float radius,
                                     const Matrix4x4& view_matrix,
                                     const Matrix4x4& proj_matrix,
                                     float tile_ndc_min_x,
                                     float tile_ndc_max_x,
                                     float tile_ndc_min_y,
                                     float tile_ndc_max_y)
    {
        const Vector4 view_center = view_matrix * Vector4(center, 1.0f);
        if (view_center.z >= -radius)
        {
            return true;
        }

        const Vector4 clip_center = proj_matrix * view_center;
        if (clip_center.w <= 0.0f)
        {
            return false;
        }

        const float inv_w   = 1.0f / clip_center.w;
        const float ndc_x   = clip_center.x * inv_w;
        const float ndc_y   = clip_center.y * inv_w;
        const float margin  = radius / std::max(0.01f, -view_center.z);
        const float extent_x = margin * proj_matrix[0][0];
        const float extent_y = margin * proj_matrix[1][1];

        return (ndc_x + extent_x >= tile_ndc_min_x) && (ndc_x - extent_x <= tile_ndc_max_x) &&
               (ndc_y + extent_y >= tile_ndc_min_y) && (ndc_y - extent_y <= tile_ndc_max_y);
    }
}  // namespace

namespace
{
    MegaLightsSystem* GetActiveMegaLightsSystem()
    {
        auto render_system = GET_SYSTEM(RenderSystem);
        if (render_system == nullptr)
        {
            return nullptr;
        }
        auto render_resource =
            std::dynamic_pointer_cast<RenderResource>(render_system->getRenderResource());
        if (render_resource == nullptr)
        {
            return nullptr;
        }
        return &render_resource->GetMegaLightsSystem();
    }

    bool ConsoleSpawnTestLights(const std::vector<std::string>& args)
    {
        MegaLightsSystem* system = GetActiveMegaLightsSystem();
        if (system == nullptr)
        {
            return false;
        }
        uint32_t count = 16;
        if (args.size() > 1)
        {
            count = static_cast<uint32_t>(std::max(0, std::atoi(args[1].c_str())));
        }
        system->SpawnDebugLights(count);
        SetEnabled(true);
        return true;
    }

    bool ConsoleClearTestLights(const std::vector<std::string>& args)
    {
        (void)args;
        if (MegaLightsSystem* system = GetActiveMegaLightsSystem())
        {
            system->ClearDebugLights();
        }
        return true;
    }
    void RegisterConsoleCommands()
    {
        if (auto console = GET_SYSTEM(ConsoleManager))
        {
            console->RegisterCommand("r.MegaLights.SpawnTestLights",
                                     "Spawn N debug point lights in a ring (default 16) and enable MegaLights.",
                                     ConsoleSpawnTestLights);
            console->RegisterCommand("r.MegaLights.ClearTestLights",
                                     "Remove debug point lights spawned by SpawnTestLights.",
                                     ConsoleClearTestLights);
        }
    }
}  // namespace

void MegaLightsSystem::Initialize(std::shared_ptr<RHI> rhi)
{
    m_Rhi = std::move(rhi);
    RegisterConsoleVariables();
    RegisterConsoleCommands();

    if (m_Rhi != nullptr && m_HistorySampler == nullptr)
    {
        RHISamplerCreateInfo sampler_info {};
        sampler_info.sType = RHI_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sampler_info.magFilter = RHI_FILTER_LINEAR;
        sampler_info.minFilter = RHI_FILTER_LINEAR;
        sampler_info.mipmapMode = RHI_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler_info.addressModeU = RHI_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_info.addressModeV = RHI_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_info.addressModeW = RHI_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_info.maxAnisotropy = 1.0f;
        m_Rhi->CreateSampler(&sampler_info, m_HistorySampler);
    }
}

void MegaLightsSystem::Shutdown()
{
    DestroyBuffer(m_Rhi, m_LightsBuffer, m_LightsMemory);
    DestroyBuffer(m_Rhi, m_TileIndicesBuffer, m_TileIndicesMemory);
    DestroyBuffer(m_Rhi, m_TileRangesBuffer, m_TileRangesMemory);
    DestroyHistoryResources();
    if (m_Rhi != nullptr && m_HistorySampler != nullptr)
    {
        m_Rhi->DestroySampler(m_HistorySampler);
        m_HistorySampler = nullptr;
    }
    m_CpuLights.clear();
    m_DebugLights.clear();
    m_TileIndices.clear();
    m_TileRanges.clear();
    m_LightCount = 0;
}

MegaLightsSystem::ViewportHistory& MegaLightsSystem::GetViewportHistory(ViewportType viewport_type)
{
    return m_History[ViewportHistoryIndex(viewport_type)];
}

const MegaLightsSystem::ViewportHistory& MegaLightsSystem::GetViewportHistory(ViewportType viewport_type) const
{
    return m_History[ViewportHistoryIndex(viewport_type)];
}

void MegaLightsSystem::DestroyHistoryResources()
{
    for (ViewportHistory& history : m_History)
    {
        for (uint32_t image_index = 0; image_index < 2; ++image_index)
        {
            DestroyImage(m_Rhi,
                         history.images[image_index],
                         history.memories[image_index],
                         history.sample_views[image_index]);
            if (history.storage_views[image_index] != nullptr &&
                history.storage_views[image_index] != history.sample_views[image_index])
            {
                m_Rhi->DestroyImageView(history.storage_views[image_index]);
            }
            history.storage_views[image_index] = nullptr;
            history.layouts[image_index] = RHI_IMAGE_LAYOUT_UNDEFINED;
        }
        history.width = 0;
        history.height = 0;
        history.history_valid = 0;
        history.ping_pong = 0;
    }
}

void MegaLightsSystem::EnsureHistoryResources(uint32_t width, uint32_t height)
{
    if (m_Rhi == nullptr || width == 0 || height == 0)
    {
        return;
    }

    for (ViewportHistory& history : m_History)
    {
        if (history.width == width && history.height == height && history.images[0] != nullptr)
        {
            continue;
        }

        for (uint32_t image_index = 0; image_index < 2; ++image_index)
        {
            DestroyImage(m_Rhi,
                         history.images[image_index],
                         history.memories[image_index],
                         history.sample_views[image_index]);
            if (history.storage_views[image_index] != nullptr &&
                history.storage_views[image_index] != history.sample_views[image_index])
            {
                m_Rhi->DestroyImageView(history.storage_views[image_index]);
            }
            history.storage_views[image_index] = nullptr;
            history.layouts[image_index] = RHI_IMAGE_LAYOUT_UNDEFINED;
        }

        history.width = width;
        history.height = height;
        history.history_valid = 0;
        history.ping_pong = 0;

        const RHIImageUsageFlags usage = static_cast<RHIImageUsageFlags>(
            RHI_IMAGE_USAGE_SAMPLED_BIT | RHI_IMAGE_USAGE_STORAGE_BIT);

        for (uint32_t image_index = 0; image_index < 2; ++image_index)
        {
            m_Rhi->CreateImage(width,
                               height,
                               RHI_FORMAT_R16G16B16A16_SFLOAT,
                               RHI_IMAGE_TILING_OPTIMAL,
                               usage,
                               RHI_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                               history.images[image_index],
                               history.memories[image_index],
                               0,
                               1,
                               1);

            m_Rhi->CreateImageView(history.images[image_index],
                                   RHI_FORMAT_R16G16B16A16_SFLOAT,
                                   RHI_IMAGE_ASPECT_COLOR_BIT,
                                   RHI_IMAGE_VIEW_TYPE_2D,
                                   1,
                                   1,
                                   history.sample_views[image_index]);

            m_Rhi->CreateImageView(history.images[image_index],
                                   RHI_FORMAT_R16G16B16A16_SFLOAT,
                                   RHI_IMAGE_ASPECT_COLOR_BIT,
                                   RHI_IMAGE_VIEW_TYPE_2D,
                                   1,
                                   1,
                                   history.storage_views[image_index]);
        }
    }
}

void MegaLightsSystem::TransitionHistoryImage(RHICommandBuffer* command_buffer,
                                              ViewportHistory& history,
                                              uint32_t image_index,
                                              RHIImageLayout new_layout,
                                              RHIAccessFlags dst_access)
{
    if (m_Rhi == nullptr || command_buffer == nullptr || history.images[image_index] == nullptr)
    {
        return;
    }

    const RHIImageLayout old_layout = history.layouts[image_index];
    if (old_layout == new_layout)
    {
        return;
    }

    RHIAccessFlags src_access = 0;
    if (old_layout == RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
    {
        src_access = RHI_ACCESS_SHADER_READ_BIT;
    }
    else if (old_layout == RHI_IMAGE_LAYOUT_GENERAL)
    {
        src_access = RHI_ACCESS_SHADER_READ_BIT | RHI_ACCESS_SHADER_WRITE_BIT;
    }

    RHIImageMemoryBarrier barrier {};
    barrier.sType = RHI_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = src_access;
    barrier.dstAccessMask = dst_access;
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = m_Rhi->GetQueueFamilyIndices().graphics_family.value();
    barrier.dstQueueFamilyIndex = m_Rhi->GetQueueFamilyIndices().graphics_family.value();
    barrier.image = history.images[image_index];
    barrier.subresourceRange = {RHI_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    m_Rhi->CmdPipelineBarrier(command_buffer,
                              RHI_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                              RHI_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                              0,
                              0,
                              nullptr,
                              0,
                              nullptr,
                              1,
                              &barrier);
    history.layouts[image_index] = new_layout;
}

void MegaLightsSystem::PrepareDeferredHistory(ViewportType viewport_type, RHICommandBuffer* command_buffer)
{
    if (!IsEnabled() || !IsTemporalDenoiseEnabled() || m_Rhi == nullptr)
    {
        return;
    }

    ViewportHistory& history = GetViewportHistory(viewport_type);
    if (history.images[0] == nullptr)
    {
        return;
    }

    const uint32_t read_index = history.ping_pong;
    const uint32_t write_index = 1u - history.ping_pong;

    if (history.layouts[read_index] == RHI_IMAGE_LAYOUT_UNDEFINED)
    {
        TransitionHistoryImage(command_buffer,
                               history,
                               read_index,
                               RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                               RHI_ACCESS_SHADER_READ_BIT);
    }
    else
    {
        TransitionHistoryImage(command_buffer,
                               history,
                               read_index,
                               RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                               RHI_ACCESS_SHADER_READ_BIT);
    }

    TransitionHistoryImage(command_buffer,
                           history,
                           write_index,
                           RHI_IMAGE_LAYOUT_GENERAL,
                           RHI_ACCESS_SHADER_WRITE_BIT);
}

void MegaLightsSystem::FillDenoiseHeaderFields(MegaLightsHeaderGpu& header, ViewportType viewport_type) const
{
    header.temporal_enable = IsTemporalDenoiseEnabled() ? 1u : 0u;
    header.spatial_enable = IsSpatialDenoiseEnabled() ? 1u : 0u;
    header.spatial_radius = static_cast<uint32_t>(std::clamp(GetSpatialRadius(), 1, 2));

    const ViewportHistory& history = GetViewportHistory(viewport_type);
    header.history_valid = history.history_valid;
    header.temporal_blend = GetTemporalBlend();
    header.disocclusion_threshold = GetDisocclusionThreshold();
    header.spatial_depth_sigma = GetSpatialDepthSigma();
    header.spatial_normal_power = GetSpatialNormalPower();
    CopyMatrixToHeader(header.prev_proj_view, history.prev_proj_view);
}

void MegaLightsSystem::PrepareSpatialReadBarrier(ViewportType viewport_type, RHICommandBuffer* command_buffer)
{
    if (!IsSpatialDenoiseEnabled() || m_Rhi == nullptr || command_buffer == nullptr)
    {
        return;
    }

    ViewportHistory& history = GetViewportHistory(viewport_type);
    const uint32_t write_index = 1u - history.ping_pong;
    if (history.images[write_index] == nullptr)
    {
        return;
    }

    TransitionHistoryImage(command_buffer,
                           history,
                           write_index,
                           RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                           RHI_ACCESS_SHADER_READ_BIT);
}

void MegaLightsSystem::RefreshTemporalHeader(ViewportType viewport_type)
{
    if (m_Rhi == nullptr || m_LightsBuffer == nullptr || m_LightsMemory == nullptr)
    {
        return;
    }

    void* mapped = nullptr;
    const uint32_t header_bytes = static_cast<uint32_t>(sizeof(MegaLightsHeaderGpu));
    m_Rhi->MapMemory(m_LightsMemory, 0, header_bytes, 0, &mapped);
    if (mapped == nullptr)
    {
        return;
    }

    MegaLightsHeaderGpu header {};
    std::memcpy(&header, mapped, sizeof(header));
    FillDenoiseHeaderFields(header, viewport_type);

    std::memcpy(mapped, &header, sizeof(header));
    m_Rhi->UnmapMemory(m_LightsMemory);
}

void MegaLightsSystem::EndDeferredPass(ViewportType viewport_type, const Matrix4x4& current_proj_view)
{
    if (!IsEnabled())
    {
        return;
    }

    ViewportHistory& history = GetViewportHistory(viewport_type);
    history.prev_proj_view = current_proj_view;
    history.history_valid = 1;

    const uint32_t written_index = 1u - history.ping_pong;
    history.layouts[written_index] = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    history.ping_pong = 1u - history.ping_pong;
}

void MegaLightsSystem::InvalidateHistory()
{
    for (ViewportHistory& history : m_History)
    {
        history.history_valid = 0;
        history.ping_pong = 0;
        history.layouts[0] = RHI_IMAGE_LAYOUT_UNDEFINED;
        history.layouts[1] = RHI_IMAGE_LAYOUT_UNDEFINED;
    }
}

void MegaLightsSystem::SyncLightsFromScene(const RenderScene& scene)
{
    m_CpuLights.clear();
    const int max_lights = std::min(GetMaxLights(), static_cast<int>(kMaxLocalLights));
    m_CpuLights.reserve(static_cast<size_t>(max_lights));

    for (const PointLight& point_light : scene.m_PointLightList.m_Lights)
    {
        if (static_cast<int>(m_CpuLights.size()) >= max_lights)
        {
            break;
        }
        MegaLightGpu gpu_light {};
        gpu_light.position_radius[0] = point_light.m_Position.x;
        gpu_light.position_radius[1] = point_light.m_Position.y;
        gpu_light.position_radius[2] = point_light.m_Position.z;
        gpu_light.position_radius[3] = point_light.calculateRadius();

        const Vector3 intensity = point_light.m_Flux / (4.0f * Math_PI);
        gpu_light.intensity[0]  = intensity.x;
        gpu_light.intensity[1]  = intensity.y;
        gpu_light.intensity[2]  = intensity.z;
        gpu_light.intensity[3]  = 0.0f;
        m_CpuLights.push_back(gpu_light);
    }

    for (const MegaLightGpu& debug_light : m_DebugLights)
    {
        if (static_cast<int>(m_CpuLights.size()) >= max_lights)
        {
            break;
        }
        m_CpuLights.push_back(debug_light);
    }

    m_LightCount = static_cast<uint32_t>(m_CpuLights.size());
}

void MegaLightsSystem::SpawnDebugLights(uint32_t count)
{
    m_DebugLights.clear();
    count = std::min(count, kMaxLocalLights);
    m_DebugLights.reserve(count);

    const float ring_radius = 6.0f;
    for (uint32_t i = 0; i < count; ++i)
    {
        const float angle = (static_cast<float>(i) / static_cast<float>(count)) * 2.0f * Math_PI;
        MegaLightGpu gpu_light {};
        gpu_light.position_radius[0] = std::cos(angle) * ring_radius;
        gpu_light.position_radius[1] = 2.0f;
        gpu_light.position_radius[2] = std::sin(angle) * ring_radius;
        gpu_light.position_radius[3] = 8.0f;

        const Vector3 flux(400.0f, 380.0f, 340.0f);
        const Vector3 intensity = flux / (4.0f * Math_PI);
        gpu_light.intensity[0]  = intensity.x;
        gpu_light.intensity[1]  = intensity.y;
        gpu_light.intensity[2]  = intensity.z;
        gpu_light.intensity[3]  = 0.0f;
        m_DebugLights.push_back(gpu_light);
    }
}

void MegaLightsSystem::ClearDebugLights()
{
    m_DebugLights.clear();
}

void MegaLightsSystem::RebuildTileLists(std::shared_ptr<RenderCamera> camera)
{
    m_TileIndices.clear();
    m_TileRanges.clear();

    if (m_LightCount == 0 || camera == nullptr || m_Rhi == nullptr)
    {
        m_TileCountX = 0;
        m_TileCountY = 0;
        return;
    }

    const uint32_t width  = std::max(1u, m_Rhi->GetSwapchainInfo().extent.width);
    const uint32_t height = std::max(1u, m_Rhi->GetSwapchainInfo().extent.height);
    m_ViewportWidth       = width;
    m_ViewportHeight      = height;
    m_TileCountX          = (width + kTileSize - 1) / kTileSize;
    m_TileCountY          = (height + kTileSize - 1) / kTileSize;
    const uint32_t tile_count = m_TileCountX * m_TileCountY;
    m_TileRanges.resize(tile_count * 2, 0);

    const Matrix4x4 view_matrix = camera->GetViewMatrix();
    const Matrix4x4 proj_matrix = camera->GetPersProjMatrix();

    for (uint32_t tile_y = 0; tile_y < m_TileCountY; ++tile_y)
    {
        for (uint32_t tile_x = 0; tile_x < m_TileCountX; ++tile_x)
        {
            const uint32_t tile_index = tile_y * m_TileCountX + tile_x;
            const float tile_ndc_min_x =
                (static_cast<float>(tile_x * kTileSize) / static_cast<float>(width)) * 2.0f - 1.0f;
            const float tile_ndc_max_x =
                (static_cast<float>((tile_x + 1) * kTileSize) / static_cast<float>(width)) * 2.0f - 1.0f;
            const float tile_ndc_min_y =
                (static_cast<float>(tile_y * kTileSize) / static_cast<float>(height)) * 2.0f - 1.0f;
            const float tile_ndc_max_y =
                (static_cast<float>((tile_y + 1) * kTileSize) / static_cast<float>(height)) * 2.0f - 1.0f;

            const uint32_t offset = static_cast<uint32_t>(m_TileIndices.size());
            uint32_t local_count  = 0;

            for (uint32_t light_index = 0; light_index < m_LightCount; ++light_index)
            {
                if (local_count >= kMaxLightsPerTile)
                {
                    break;
                }
                const MegaLightGpu& light = m_CpuLights[light_index];
                const Vector3 center(light.position_radius[0],
                                     light.position_radius[1],
                                     light.position_radius[2]);
                const float radius = light.position_radius[3];
                if (SphereIntersectsTileFrustum(center,
                                                radius,
                                                view_matrix,
                                                proj_matrix,
                                                tile_ndc_min_x,
                                                tile_ndc_max_x,
                                                tile_ndc_min_y,
                                                tile_ndc_max_y))
                {
                    m_TileIndices.push_back(light_index);
                    ++local_count;
                }
            }

            m_TileRanges[tile_index * 2 + 0] = offset;
            m_TileRanges[tile_index * 2 + 1] = local_count;
        }
    }
}

void MegaLightsSystem::Upload(std::shared_ptr<RHI> rhi, ViewportType viewport_type)
{
    if (rhi == nullptr)
    {
        return;
    }

    const uint32_t header_bytes = static_cast<uint32_t>(sizeof(MegaLightsHeaderGpu));
    const uint32_t lights_bytes =
        header_bytes + static_cast<uint32_t>(m_CpuLights.size() * sizeof(MegaLightGpu));
    const uint32_t indices_bytes = static_cast<uint32_t>(m_TileIndices.size() * sizeof(uint32_t));
    const uint32_t ranges_bytes  = static_cast<uint32_t>(m_TileRanges.size() * sizeof(uint32_t));

    if (lights_bytes > m_LightsBufferBytes)
    {
        DestroyBuffer(rhi, m_LightsBuffer, m_LightsMemory);
        m_LightsBufferBytes =
            std::max<uint32_t>(lights_bytes, header_bytes + static_cast<uint32_t>(sizeof(MegaLightGpu) * 16));
        rhi->CreateBufferAndInitialize(RHI_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                       RHI_MEMORY_PROPERTY_HOST_VISIBLE_BIT | RHI_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                       m_LightsBuffer,
                                       m_LightsMemory,
                                       m_LightsBufferBytes,
                                       nullptr,
                                       0);
    }

    if (indices_bytes > m_TileIndicesBufferBytes)
    {
        DestroyBuffer(rhi, m_TileIndicesBuffer, m_TileIndicesMemory);
        m_TileIndicesBufferBytes = std::max<uint32_t>(indices_bytes, sizeof(uint32_t) * 256);
        rhi->CreateBufferAndInitialize(RHI_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                       RHI_MEMORY_PROPERTY_HOST_VISIBLE_BIT | RHI_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                       m_TileIndicesBuffer,
                                       m_TileIndicesMemory,
                                       m_TileIndicesBufferBytes,
                                       nullptr,
                                       0);
    }

    if (ranges_bytes > m_TileRangesBufferBytes)
    {
        DestroyBuffer(rhi, m_TileRangesBuffer, m_TileRangesMemory);
        m_TileRangesBufferBytes = std::max<uint32_t>(ranges_bytes, sizeof(uint32_t) * 64);
        rhi->CreateBufferAndInitialize(RHI_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                       RHI_MEMORY_PROPERTY_HOST_VISIBLE_BIT | RHI_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                       m_TileRangesBuffer,
                                       m_TileRangesMemory,
                                       m_TileRangesBufferBytes,
                                       nullptr,
                                       0);
    }

    if (m_LightsBuffer == nullptr || m_TileIndicesBuffer == nullptr || m_TileRangesBuffer == nullptr)
    {
        return;
    }

    void* mapped = nullptr;
    rhi->MapMemory(m_LightsMemory, 0, lights_bytes, 0, &mapped);
    if (mapped != nullptr)
    {
        MegaLightsHeaderGpu header {};
        header.light_count    = m_LightCount;
        header.tile_count_x   = m_TileCountX;
        header.tile_count_y   = m_TileCountY;
        header.num_samples    = static_cast<uint32_t>(std::max(1, GetNumSamplesPerPixel()));
        header.frame_index     = m_FrameIndex;
        header.ss_steps        = static_cast<uint32_t>(std::max(1, GetScreenSpaceShadowSteps()));
        header.viewport_width  = m_ViewportWidth;
        header.viewport_height = m_ViewportHeight;
        FillDenoiseHeaderFields(header, viewport_type);
        std::memcpy(mapped, &header, sizeof(header));
        if (!m_CpuLights.empty())
        {
            std::memcpy(static_cast<char*>(mapped) + header_bytes,
                        m_CpuLights.data(),
                        m_CpuLights.size() * sizeof(MegaLightGpu));
        }
        rhi->UnmapMemory(m_LightsMemory);
    }

    if (!m_TileIndices.empty())
    {
        rhi->MapMemory(m_TileIndicesMemory, 0, indices_bytes, 0, &mapped);
        if (mapped != nullptr)
        {
            std::memcpy(mapped, m_TileIndices.data(), indices_bytes);
            rhi->UnmapMemory(m_TileIndicesMemory);
        }
    }

    if (!m_TileRanges.empty())
    {
        rhi->MapMemory(m_TileRangesMemory, 0, ranges_bytes, 0, &mapped);
        if (mapped != nullptr)
        {
            std::memcpy(mapped, m_TileRanges.data(), ranges_bytes);
            rhi->UnmapMemory(m_TileRangesMemory);
        }
    }
}

void MegaLightsSystem::Update(const RenderScene& scene,
                              std::shared_ptr<RenderCamera> camera,
                              ViewportType viewport_type)
{
    if (!IsEnabled() || m_Rhi == nullptr)
    {
        return;
    }

    m_ActiveViewportType = viewport_type;
    SyncLightsFromScene(scene);
    ++m_FrameIndex;
    RebuildTileLists(camera);

    const uint32_t width  = std::max(1u, m_Rhi->GetSwapchainInfo().extent.width);
    const uint32_t height = std::max(1u, m_Rhi->GetSwapchainInfo().extent.height);
    EnsureHistoryResources(width, height);
    Upload(m_Rhi, viewport_type);
}

RHIDescriptorBufferInfo MegaLightsSystem::GetLightsBufferInfo() const
{
    RHIDescriptorBufferInfo info {};
    info.buffer = m_LightsBuffer;
    info.offset = 0;
    info.range  = m_LightsBufferBytes;
    return info;
}

RHIDescriptorBufferInfo MegaLightsSystem::GetTileIndicesBufferInfo() const
{
    RHIDescriptorBufferInfo info {};
    info.buffer = m_TileIndicesBuffer;
    info.offset = 0;
    info.range  = m_TileIndicesBufferBytes;
    return info;
}

RHIDescriptorBufferInfo MegaLightsSystem::GetTileRangesBufferInfo() const
{
    RHIDescriptorBufferInfo info {};
    info.buffer = m_TileRangesBuffer;
    info.offset = 0;
    info.range  = m_TileRangesBufferBytes;
    return info;
}

RHIDescriptorImageInfo MegaLightsSystem::GetHistoryReadImageInfo(ViewportType viewport_type) const
{
    RHIDescriptorImageInfo info {};
    const ViewportHistory& history = GetViewportHistory(viewport_type);
    const uint32_t read_index = history.ping_pong;
    info.sampler = m_HistorySampler;
    info.imageView = history.sample_views[read_index];
    info.imageLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return info;
}

RHIDescriptorImageInfo MegaLightsSystem::GetHistoryWriteImageInfo(ViewportType viewport_type) const
{
    RHIDescriptorImageInfo info {};
    const ViewportHistory& history = GetViewportHistory(viewport_type);
    const uint32_t write_index = 1u - history.ping_pong;
    info.sampler = nullptr;
    info.imageView = history.storage_views[write_index];
    info.imageLayout = RHI_IMAGE_LAYOUT_GENERAL;
    return info;
}

RHIDescriptorImageInfo MegaLightsSystem::GetSpatialDirectSampleImageInfo(ViewportType viewport_type) const
{
    RHIDescriptorImageInfo info {};
    const ViewportHistory& history = GetViewportHistory(viewport_type);
    const uint32_t write_index = 1u - history.ping_pong;
    info.sampler = m_HistorySampler;
    info.imageView = history.sample_views[write_index];
    info.imageLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return info;
}

}  // namespace MegaLights
