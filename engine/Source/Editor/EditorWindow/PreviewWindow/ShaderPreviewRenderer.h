#pragma once

#include <filesystem>
#include <string>

class Material;

// Native ZSlate variant: renders the same DX12 render-to-texture preview but,
// instead of issuing an ImGui::Image, adopts the RTT color target into
// UiGpuResources and returns its stable handle so an SImage can sample it under
// the native backend. Call this every frame while the shader/material is shown:
// the GPU pass defers when an editor render pass is already open, so the handle
// only becomes valid on a frame where the draw lands. The returned handle stays
// stable across frames (the RTT resource is persistent), so the SImage shows the
// latest rendered content without rebinding.
struct ShaderPreviewTextureResult
{
    bool handled {false};         // false => not a DX12 / not previewable asset
    bool rendered {false};        // a fresh GPU draw landed this call
    void* texture_handle {nullptr};  // UiGpuResources handle (nullptr until first draw)
    std::string message;
};

ShaderPreviewTextureResult RenderShaderPreviewToNativeTexture(const std::filesystem::path& selected_asset_path,
                                                              const std::string& resolved_asset_type,
                                                              const Material* preview_material);
