#pragma once

#include <cstdint>
#include <filesystem>
#include <string> for imported MeshData .zasset products.
// Software-lit orbit view; runs in the native ZSlate Preview window.
//
// The preview is software-rasterized into an off-screen RGBA8 bitmap which is
// uploaded to a ZSlate dynamic texture (UiGpuResources::UpdateDynamicTexture).
// The Preview window displays the returned handle through an SImage. This
// replaces the old ImGui draw-list path now that the Preview window paints
// through the retained ZSlate widget tree.
namespace MeshDataPreview
{
    bool IsSupportedAssetType(const std::string& resolved_asset_type);

    void InvalidatePreview(const std::filesystem::path& asset_path);
    void InvalidateAll();

    // Interaction applied to the orbit camera this frame. The caller gates these
    // to the preview region being hovered/active; deltas are in pixels, wheel in
    // notches.
    struct PreviewInput
    {
        float drag_dx {0.0f};
        float drag_dy {0.0f};
        float wheel {0.0f};
        bool orbit {false};  // left-drag rotate
        bool pan {false};    // shift+left / middle-drag pan
        bool reset {false};  // reset view to defaults
    };

    struct PreviewFrame
    {
        void* texture_handle {nullptr};  // ZSlate dynamic-texture handle (stable across frames)
        uint32_t pixel_size {0};
        size_t vertex_count {0};
        size_t index_count {0};
        size_t triangle_count {0};
        bool ok {false};
        std::string error;
    };

    // Renders the mesh at `asset_path` into an internal dynamic texture sized
    // `pixel_size` x `pixel_size` and returns its handle. Cheap when nothing
    // changed (no re-raster / no GPU re-upload); re-rasterizes only when the
    // camera, asset, or size changes. Requires the native ZSlate backend (the
    // handle is a UiGpuResources handle, not an ImGui texture id).
    PreviewFrame RenderToTexture(const std::filesystem::path& asset_path, uint32_t pixel_size, const PreviewInput& input);

    // Fixed-camera thumbnail for Content Browser tiles. Each asset keeps its own
    // GPU texture handle (separate from the interactive Preview window singleton).
    PreviewFrame RenderThumbnailToTexture(const std::filesystem::path& asset_path, uint32_t pixel_size);

    // Content Browser async path: worker-thread rasterization + main-thread GPU upload.
    // Call TickPendingThumbnails once per frame from the Content Browser panel.
    void RequestThumbnail(const std::filesystem::path& asset_path, uint32_t pixel_size);
    void* TryGetThumbnailHandle(const std::filesystem::path& asset_path, uint32_t pixel_size);
    bool IsThumbnailPending(const std::filesystem::path& asset_path, uint32_t pixel_size);
    // max_per_frame = GPU uploads drained per tick; raster jobs are submitted separately.
    bool TickPendingThumbnails(int max_per_frame);
}  // namespace MeshDataPreview
