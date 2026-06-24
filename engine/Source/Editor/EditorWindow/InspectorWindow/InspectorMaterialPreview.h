#pragma once

#include "Runtime/Core/Math/Vector2.h"
#include "Runtime/Resource/ResType/Data/Material.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

enum class MaterialPreviewMeshType
{
    Sphere = 0,
    Plane,
    Cube
};

struct MaterialPreviewTriangle
{
    Vector2 p0;
    Vector2 p1;
    Vector2 p2;
    float depth {0.0f};
    uint32_t color {0};
};

struct MaterialPreviewState
{
    float yaw_radians = 0.6f;
    float pitch_radians = -0.35f;
    float preview_size = 220.0f;
    float zoom = 1.0f;
    Vector2 pan_offset = Vector2(0.0f, 0.0f);
    float light_yaw_radians = -0.75f;
    float light_pitch_radians = 0.45f;
    float environment_intensity = 0.85f;
    float reflection_intensity = 0.65f;
    bool show_skybox_background = true;
    MaterialPreviewMeshType mesh_type = MaterialPreviewMeshType::Sphere;

    std::vector<MaterialPreviewTriangle> cached_triangles;
    uint64_t cached_signature = 0;
};

struct MaterialPreviewResult
{
    void* texture_handle {nullptr};  // ZSlate dynamic-texture handle (stable across frames)
    uint32_t pixel_size {0};
    std::string texture_summary;
    bool ok {false};
    std::string error;
};

// Software-renders the material onto a lit sphere (over a procedural skybox) into
// an internal dynamic ZSlate texture sized pixel_size x pixel_size and returns its
// handle for display through an SImage. Replaces the old ImGui DrawMaterialPreviewWidget
// now that the Preview window paints through the retained ZSlate widget tree.
// Requires the native ZSlate backend (the handle is a UIGpuResources handle).
MaterialPreviewResult RenderMaterialPreviewToTexture(const Material& material, uint32_t pixel_size);

// Fixed-camera material thumbnail for Content Browser tiles. Each asset path keeps
// its own GPU texture handle (separate from the interactive Preview singleton above).
MaterialPreviewResult RenderMaterialThumbnailFromPath(const std::filesystem::path& asset_path, uint32_t pixel_size);

void InvalidateMaterialPreview(const std::filesystem::path& asset_path);
void InvalidateAllMaterialPreviews();
