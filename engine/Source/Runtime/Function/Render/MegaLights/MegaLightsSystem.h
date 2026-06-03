#pragma once

#include "MegaLightsDefinitions.h"
#include "Runtime/Function/Render/Interface/RHI.h"
#include "Runtime/Function/Render/Light.h"
#include "Runtime/Function/Render/RenderType.h"

#include <memory>
#include <vector>

class Level;
class RenderCamera;
class RenderScene;

namespace MegaLights
{

class MegaLightsSystem
{
public:
    void Initialize(RHI* rhi);
    void Shutdown();

    void Update(const RenderScene& scene, std::shared_ptr<RenderCamera> camera, ViewportType viewport_type);

    void PrepareDeferredHistory(ViewportType viewport_type, RHICommandBuffer* command_buffer);
    void PrepareSpatialReadBarrier(ViewportType viewport_type, RHICommandBuffer* command_buffer);
    void EndDeferredPass(ViewportType viewport_type, const Matrix4x4& current_proj_view);
    void RefreshTemporalHeader(ViewportType viewport_type);

    bool HasGpuData() const { return m_LightCount > 0; }
    uint32_t GetLightCount() const { return m_LightCount; }

    RHIDescriptorBufferInfo GetLightsBufferInfo() const;
    RHIDescriptorBufferInfo GetTileIndicesBufferInfo() const;
    RHIDescriptorBufferInfo GetTileRangesBufferInfo() const;
    RHIDescriptorImageInfo GetHistoryReadImageInfo(ViewportType viewport_type) const;
    RHIDescriptorImageInfo GetHistoryWriteImageInfo(ViewportType viewport_type) const;
    RHIDescriptorImageInfo GetSpatialDirectSampleImageInfo(ViewportType viewport_type) const;

    void SyncLightsFromScene(const RenderScene& scene);
    void SpawnDebugLights(uint32_t count);
    void ClearDebugLights();
    void InvalidateHistory();

private:
    struct ViewportHistory
    {
        RHIImage* images[2] {nullptr, nullptr};
        RHIDeviceMemory* memories[2] {nullptr, nullptr};
        RHIImageView* sample_views[2] {nullptr, nullptr};
        RHIImageView* storage_views[2] {nullptr, nullptr};
        RHIImageLayout layouts[2] {RHI_IMAGE_LAYOUT_UNDEFINED, RHI_IMAGE_LAYOUT_UNDEFINED};
        Matrix4x4 prev_proj_view {};
        uint32_t history_valid {0};
        uint32_t ping_pong {0};
        uint32_t width {0};
        uint32_t height {0};
    };

    void RebuildTileLists(std::shared_ptr<RenderCamera> camera);
    void Upload(RHI* rhi, ViewportType viewport_type);
    void EnsureHistoryResources(uint32_t width, uint32_t height);
    void DestroyHistoryResources();
    void TransitionHistoryImage(RHICommandBuffer* command_buffer,
                                ViewportHistory& history,
                                uint32_t image_index,
                                RHIImageLayout new_layout,
                                RHIAccessFlags dst_access);
    void FillDenoiseHeaderFields(MegaLightsHeaderGpu& header, ViewportType viewport_type) const;

    ViewportHistory& GetViewportHistory(ViewportType viewport_type);
    const ViewportHistory& GetViewportHistory(ViewportType viewport_type) const;

    RHI* m_Rhi;
    RHISampler* m_HistorySampler {nullptr};
    std::vector<MegaLightGpu> m_CpuLights;
    std::vector<MegaLightGpu> m_DebugLights;
    std::vector<uint32_t> m_TileIndices;
    std::vector<uint32_t> m_TileRanges;  // pairs: offset, count
    ViewportHistory m_History[kViewportHistoryCount] {};

    RHIBuffer* m_LightsBuffer {nullptr};
    RHIDeviceMemory* m_LightsMemory {nullptr};
    RHIBuffer* m_TileIndicesBuffer {nullptr};
    RHIDeviceMemory* m_TileIndicesMemory {nullptr};
    RHIBuffer* m_TileRangesBuffer {nullptr};
    RHIDeviceMemory* m_TileRangesMemory {nullptr};

    uint32_t m_LightCount {0};
    uint32_t m_TileCountX {0};
    uint32_t m_TileCountY {0};
    uint32_t m_ViewportWidth {0};
    uint32_t m_ViewportHeight {0};
    uint32_t m_FrameIndex {0};
    uint32_t m_LightsBufferBytes {0};
    uint32_t m_TileIndicesBufferBytes {0};
    uint32_t m_TileRangesBufferBytes {0};
    ViewportType m_ActiveViewportType {ViewportType::game};
};

}  // namespace MegaLights
