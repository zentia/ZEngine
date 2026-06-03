#include "MeshDataPreview.h"

#include "Editor/EditorWindow/PreviewWindow/PreviewRaster.h"
#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Core/Math/Math.h"
#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Resource/Asset/AssetManager.h"
#include "Runtime/Resource/ResType/Data/MeshData.h"
#include "Runtime/UI/Render/UiGpuResources.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    constexpr const char* k_supported_asset_type = "meshdata";
    constexpr size_t k_max_preview_triangles = 80000U;

    struct Vec2f
    {
        float x {0.0f};
        float y {0.0f};
    };

    struct MeshPreviewTriangle
    {
        Vec2f p0;
        Vec2f p1;
        Vec2f p2;
        float depth {0.0f};
        uint32_t color {0};
    };

    struct MeshPreviewState
    {
        float yaw_radians = 0.6f;
        float pitch_radians = -0.35f;
        float zoom = 1.0f;
        Vec2f pan_offset;
        std::vector<MeshPreviewTriangle> cached_triangles;
        uint64_t cached_signature = 0;
        size_t vertex_count = 0;
        size_t index_count = 0;
        size_t triangle_count = 0;
        void* texture_handle = nullptr;
        PreviewRaster raster;
    };

    struct MeshPreviewVertex
    {
        Vector3 position;
        Vector3 normal;
    };

    // Deep copy of mesh geometry. ReadObject<MeshData> returns an ObjectManager-owned
    // instance -- wrapping it in shared_ptr with default delete corrupts the heap.
    struct MeshPreviewGeometry
    {
        std::vector<Vertex> vertex_buffer;
        std::vector<int> index_buffer;
    };

    struct MeshPreviewCacheEntry
    {
        std::filesystem::file_time_type write_time = std::filesystem::file_time_type::min();
        std::shared_ptr<MeshPreviewGeometry> geometry;
    };

    MeshPreviewState& previewState()
    {
        static MeshPreviewState state;
        return state;
    }

    std::unordered_map<std::string, MeshPreviewCacheEntry>& meshCache()
    {
        static std::unordered_map<std::string, MeshPreviewCacheEntry> cache;
        return cache;
    }

    std::filesystem::file_time_type fileWriteTime(const std::filesystem::path& path)
    {
        if (path.empty())
        {
            return std::filesystem::file_time_type::min();
        }
        std::error_code ec;
        if (!std::filesystem::exists(path, ec) || ec)
        {
            return std::filesystem::file_time_type::min();
        }
        return std::filesystem::last_write_time(path, ec);
    }

    Vector3 normalizeVector(const Vector3& value, const Vector3& fallback)
    {
        return value.isZeroLength() ? fallback : value.normalisedCopy();
    }

    Vector3 rotateVector(const Vector3& value, float yaw_radians, float pitch_radians)
    {
        const float cos_yaw = std::cos(yaw_radians);
        const float sin_yaw = std::sin(yaw_radians);
        const float cos_pitch = std::cos(pitch_radians);
        const float sin_pitch = std::sin(pitch_radians);

        const Vector3 yaw_rotated(cos_yaw * value.x + sin_yaw * value.z,
                                  value.y,
                                  -sin_yaw * value.x + cos_yaw * value.z);
        return Vector3(yaw_rotated.x,
                       cos_pitch * yaw_rotated.y - sin_pitch * yaw_rotated.z,
                       sin_pitch * yaw_rotated.y + cos_pitch * yaw_rotated.z);
    }

    MeshPreviewVertex rotateVertex(const MeshPreviewVertex& vertex, float yaw_radians, float pitch_radians)
    {
        MeshPreviewVertex out = vertex;
        out.position = rotateVector(vertex.position, yaw_radians, pitch_radians);
        out.normal = normalizeVector(rotateVector(vertex.normal, yaw_radians, pitch_radians), Vector3::UNIT_Z);
        return out;
    }

    Vec2f projectPoint(const Vector3& point, const Vec2f& center, float radius)
    {
        const float perspective_scale = 1.0f + point.z * 0.18f;
        return Vec2f {center.x + point.x * radius * perspective_scale, center.y - point.y * radius * perspective_scale};
    }

    uint32_t toColor(const Vector3& rgb, float alpha)
    {
        const Vector3 clamped(Math::Clamp(rgb.x, 0.0f, 1.0f), Math::Clamp(rgb.y, 0.0f, 1.0f), Math::Clamp(rgb.z, 0.0f, 1.0f));
        const float a = Math::Clamp(alpha, 0.0f, 1.0f);
        return PreviewRaster::Pack(static_cast<uint8_t>(clamped.x * 255.0f + 0.5f),
                                   static_cast<uint8_t>(clamped.y * 255.0f + 0.5f),
                                   static_cast<uint8_t>(clamped.z * 255.0f + 0.5f),
                                   static_cast<uint8_t>(a * 255.0f + 0.5f));
    }

    Vector3 evaluateLitColor(const Vector3& base_color, const Vector3& normal)
    {
        const Vector3 light_dir(0.35f, -0.55f, 0.75f);
        const Vector3 view_dir = Vector3::UNIT_Z;
        const float ndotl = Math::Clamp(normal.dotProduct(normalizeVector(light_dir, Vector3::UNIT_Z)), 0.0f, 1.0f);

        Vector3 half_vector = light_dir + view_dir;
        if (!half_vector.isZeroLength())
        {
            half_vector.normalise();
        }
        const float ndoth = Math::Clamp(normal.dotProduct(half_vector), 0.0f, 1.0f);
        const float specular_power = 48.0f;
        const float specular = std::pow(ndoth, specular_power) * 0.35f;

        const Vector3 ambient = base_color * 0.22f;
        const Vector3 diffuse = base_color * ndotl * 0.78f;
        const Vector3 specular_color = Vector3(0.85f, 0.88f, 0.92f) * specular;
        return Vector3(Math::Clamp(ambient.x + diffuse.x + specular_color.x, 0.0f, 1.0f),
                       Math::Clamp(ambient.y + diffuse.y + specular_color.y, 0.0f, 1.0f),
                       Math::Clamp(ambient.z + diffuse.z + specular_color.z, 0.0f, 1.0f));
    }

    void drawBackground(PreviewRaster& raster)
    {
        const int size_x = static_cast<int>(raster.Width());
        const int size_y = static_cast<int>(raster.Height());

        // Sky gradient, then an opaque checkerboard, then a faint dark overlay --
        // mirrors the previous ImGui draw order.
        raster.FillRectVGradient(0, 0, size_x, size_y, PreviewRaster::Pack(38, 52, 78, 255),
                                 PreviewRaster::Pack(18, 20, 26, 255));

        constexpr int checker_size = 16;
        for (int y = 0; y < size_y; y += checker_size)
        {
            for (int x = 0; x < size_x; x += checker_size)
            {
                const bool even = (((x / checker_size) + (y / checker_size)) % 2) == 0;
                const uint32_t color = even ? PreviewRaster::Pack(72, 72, 76, 255) : PreviewRaster::Pack(58, 58, 62, 255);
                raster.FillRect(x, y, std::min(x + checker_size, size_x), std::min(y + checker_size, size_y), color);
            }
        }

        raster.FillRect(0, 0, size_x, size_y, PreviewRaster::Pack(18, 18, 22, 24));
    }

    bool isFiniteVertex(const Vertex& vertex)
    {
        return std::isfinite(vertex.px) && std::isfinite(vertex.py) && std::isfinite(vertex.pz);
    }

    bool loadMeshData(const std::filesystem::path& asset_path, std::shared_ptr<MeshPreviewGeometry>& out_mesh, std::string& out_error)
    {
        out_mesh.reset();
        out_error.clear();

        auto asset_manager = GET_SYSTEM(AssetManager);
        if (asset_manager == nullptr)
        {
            out_error = "AssetManager unavailable";
            return false;
        }

        std::filesystem::path read_path = asset_path;
        MeshData* loaded_mesh = asset_manager->ReadObject<MeshData>(read_path);
        if (loaded_mesh == nullptr)
        {
            out_error = "ReadObject<MeshData> returned null";
            return false;
        }

        if (loaded_mesh->vertex_buffer.empty() || loaded_mesh->index_buffer.empty())
        {
            out_error = "MeshData has no geometry";
            return false;
        }

        auto geometry_copy = std::make_shared<MeshPreviewGeometry>();
        geometry_copy->vertex_buffer = loaded_mesh->vertex_buffer;
        geometry_copy->index_buffer = loaded_mesh->index_buffer;
        out_mesh = std::move(geometry_copy);
        return true;
    }

    bool computeBounds(const MeshPreviewGeometry& mesh, Vector3& out_center, float& out_radius)
    {
        if (mesh.vertex_buffer.empty())
        {
            return false;
        }

        bool found_any = false;
        Vector3 min_corner {};
        Vector3 max_corner {};

        for (const Vertex& vertex : mesh.vertex_buffer)
        {
            if (!isFiniteVertex(vertex))
            {
                continue;
            }

            const Vector3 position(vertex.px, vertex.py, vertex.pz);
            if (!found_any)
            {
                min_corner = position;
                max_corner = position;
                found_any = true;
                continue;
            }

            min_corner.x = std::min(min_corner.x, position.x);
            min_corner.y = std::min(min_corner.y, position.y);
            min_corner.z = std::min(min_corner.z, position.z);
            max_corner.x = std::max(max_corner.x, position.x);
            max_corner.y = std::max(max_corner.y, position.y);
            max_corner.z = std::max(max_corner.z, position.z);
        }

        if (!found_any)
        {
            return false;
        }

        out_center = (min_corner + max_corner) * 0.5f;
        const Vector3 extent = max_corner - min_corner;
        out_radius = std::max(extent.x, std::max(extent.y, extent.z)) * 0.5f;
        if (!std::isfinite(out_radius) || out_radius < 1e-5f)
        {
            out_radius = 1.0f;
        }
        return true;
    }

    MeshPreviewVertex makeLocalVertex(const Vertex& v, const Vector3& center, float radius)
    {
        MeshPreviewVertex out {};
        out.position = Vector3((v.px - center.x) / radius, (v.py - center.y) / radius, (v.pz - center.z) / radius);
        out.normal = normalizeVector(Vector3(v.nx, v.ny, v.nz), Vector3::UNIT_Y);
        return out;
    }

    void appendTriangle(std::vector<MeshPreviewTriangle>& triangles,
                        const MeshPreviewVertex& v0,
                        const MeshPreviewVertex& v1,
                        const MeshPreviewVertex& v2,
                        const Vec2f& center,
                        float radius)
    {
        Vector3 face_normal = (v1.position - v0.position).crossProduct(v2.position - v0.position);
        if (face_normal.isZeroLength())
        {
            return;
        }
        face_normal.normalise();
        if (face_normal.z <= 0.0f)
        {
            return;
        }

        const Vector3 shaded =
            evaluateLitColor(Vector3(0.72f, 0.74f, 0.78f), normalizeVector(v0.normal, Vector3::UNIT_Y));
        const float depth = (v0.position.z + v1.position.z + v2.position.z) / 3.0f;

        triangles.push_back(MeshPreviewTriangle {projectPoint(v0.position, center, radius),
                                                 projectPoint(v1.position, center, radius),
                                                 projectPoint(v2.position, center, radius),
                                                 depth,
                                                 toColor(shaded, 1.0f)});
    }

    uint64_t computeSignature(const std::filesystem::path& asset_path,
                              const MeshPreviewState& state,
                              uint32_t pixel_size,
                              float radius,
                              size_t vertex_count,
                              size_t index_count)
    {
        const auto write_time = fileWriteTime(asset_path);
        uint64_t signature = static_cast<uint64_t>(write_time.time_since_epoch().count());
        signature ^= static_cast<uint64_t>(vertex_count) * 1315423911ULL;
        signature ^= static_cast<uint64_t>(index_count) * 2654435761ULL;
        signature ^= static_cast<uint64_t>(pixel_size) * 40503ULL;
        signature ^= static_cast<uint64_t>(static_cast<int>(state.zoom * 1000.0f));
        signature ^= static_cast<uint64_t>(static_cast<int>(state.yaw_radians * 10000.0f));
        signature ^= static_cast<uint64_t>(static_cast<int>(state.pitch_radians * 10000.0f));
        signature ^= static_cast<uint64_t>(static_cast<int>(state.pan_offset.x * 4.0f));
        signature ^= static_cast<uint64_t>(static_cast<int>(state.pan_offset.y * 4.0f));
        signature ^= static_cast<uint64_t>(static_cast<int>(radius * 100.0f));
        return signature;
    }

    bool buildPreviewTriangles(const MeshPreviewGeometry& mesh,
                               float yaw_radians,
                               float pitch_radians,
                               const Vec2f& center,
                               float radius,
                               std::vector<MeshPreviewTriangle>& out_triangles,
                               size_t& out_triangle_count)
    {
        out_triangles.clear();
        out_triangle_count = 0;

        Vector3 bounds_center {};
        float bounds_radius = 1.0f;
        if (!computeBounds(mesh, bounds_center, bounds_radius))
        {
            return false;
        }

        const size_t index_count = mesh.index_buffer.size();
        if (index_count < 3)
        {
            return false;
        }

        const size_t triangle_count = index_count / 3;
        const size_t stride = triangle_count > k_max_preview_triangles ? (triangle_count / k_max_preview_triangles) + 1 : 1;

        out_triangles.reserve(std::min(triangle_count, k_max_preview_triangles));

        for (size_t tri = 0; tri < triangle_count; tri += stride)
        {
            const int idx0 = mesh.index_buffer[tri * 3 + 0];
            const int idx1 = mesh.index_buffer[tri * 3 + 1];
            const int idx2 = mesh.index_buffer[tri * 3 + 2];
            if (idx0 < 0 || idx1 < 0 || idx2 < 0)
            {
                continue;
            }
            const size_t i0 = static_cast<size_t>(idx0);
            const size_t i1 = static_cast<size_t>(idx1);
            const size_t i2 = static_cast<size_t>(idx2);
            if (i0 >= mesh.vertex_buffer.size() || i1 >= mesh.vertex_buffer.size() || i2 >= mesh.vertex_buffer.size())
            {
                continue;
            }

            const MeshPreviewVertex v0 =
                rotateVertex(makeLocalVertex(mesh.vertex_buffer[i0], bounds_center, bounds_radius), yaw_radians, pitch_radians);
            const MeshPreviewVertex v1 =
                rotateVertex(makeLocalVertex(mesh.vertex_buffer[i1], bounds_center, bounds_radius), yaw_radians, pitch_radians);
            const MeshPreviewVertex v2 =
                rotateVertex(makeLocalVertex(mesh.vertex_buffer[i2], bounds_center, bounds_radius), yaw_radians, pitch_radians);
            appendTriangle(out_triangles, v0, v1, v2, center, radius);
            ++out_triangle_count;
        }

        std::sort(out_triangles.begin(),
                  out_triangles.end(),
                  [](const MeshPreviewTriangle& lhs, const MeshPreviewTriangle& rhs) { return lhs.depth < rhs.depth; });
        return !out_triangles.empty();
    }
}  // namespace

namespace MeshDataPreview
{
    bool IsSupportedAssetType(const std::string& resolved_asset_type)
    {
        return resolved_asset_type == k_supported_asset_type;
    }

    void InvalidatePreview(const std::filesystem::path& asset_path)
    {
        if (asset_path.empty())
        {
            return;
        }
        meshCache().erase(asset_path.lexically_normal().generic_string());
        previewState().cached_signature = 0;
    }

    PreviewFrame RenderToTexture(const std::filesystem::path& asset_path, uint32_t pixel_size, const PreviewInput& input)
    {
        PreviewFrame frame;
        frame.pixel_size = pixel_size;

        if (asset_path.empty() || pixel_size == 0)
        {
            frame.error = "Invalid preview request";
            return frame;
        }

        UiGpuResources* gpu = UiGpuResources::Get();
        if (gpu == nullptr || !gpu->IsReady())
        {
            frame.error = "GPU resources unavailable";
            return frame;
        }

        const std::string cache_key = asset_path.lexically_normal().generic_string();
        const auto write_time = fileWriteTime(asset_path);

        MeshPreviewCacheEntry& cache_entry = meshCache()[cache_key];
        if (cache_entry.write_time != write_time || cache_entry.geometry == nullptr)
        {
            std::string error;
            if (!loadMeshData(asset_path, cache_entry.geometry, error))
            {
                frame.error = "Mesh preview failed: " + error;
                return frame;
            }
            cache_entry.write_time = write_time;
            previewState().cached_signature = 0;
        }

        const std::shared_ptr<MeshPreviewGeometry>& mesh = cache_entry.geometry;
        if (mesh == nullptr)
        {
            frame.error = "Mesh preview unavailable.";
            return frame;
        }

        MeshPreviewState& state = previewState();
        state.vertex_count = mesh->vertex_buffer.size();
        state.index_count = mesh->index_buffer.size();
        state.triangle_count = mesh->index_buffer.size() / 3;

        // Apply interaction.
        if (input.reset)
        {
            state.yaw_radians = 0.6f;
            state.pitch_radians = -0.35f;
            state.zoom = 1.0f;
            state.pan_offset = Vec2f {};
            state.cached_signature = 0;
        }

        const float size_f = static_cast<float>(pixel_size);
        const float pan_limit = size_f * 0.45f;

        if (input.wheel != 0.0f)
        {
            const float zoom_factor = Math::Clamp(1.0f + input.wheel * 0.12f, 0.4f, 1.8f);
            state.zoom = Math::Clamp(state.zoom * zoom_factor, 0.55f, 2.40f);
        }
        if (input.pan)
        {
            state.pan_offset.x = Math::Clamp(state.pan_offset.x + input.drag_dx, -pan_limit, pan_limit);
            state.pan_offset.y = Math::Clamp(state.pan_offset.y + input.drag_dy, -pan_limit, pan_limit);
        }
        else if (input.orbit)
        {
            state.yaw_radians += input.drag_dx * 0.01f;
            state.pitch_radians = Math::Clamp(state.pitch_radians - input.drag_dy * 0.01f, -1.2f, 1.2f);
        }

        state.zoom = Math::Clamp(state.zoom, 0.55f, 2.40f);
        state.pitch_radians = Math::Clamp(state.pitch_radians, -1.2f, 1.2f);

        const Vec2f center {size_f * 0.5f + state.pan_offset.x, size_f * 0.5f + state.pan_offset.y};
        const float radius = size_f * 0.36f * state.zoom;

        const uint64_t signature = computeSignature(asset_path, state, pixel_size, radius, state.vertex_count, state.index_count);
        const bool size_changed = (state.raster.Width() != pixel_size) || (state.raster.Height() != pixel_size);
        if (state.cached_signature != signature || size_changed || state.texture_handle == nullptr)
        {
            state.cached_signature = signature;

            size_t drawn_triangles = 0;
            if (!buildPreviewTriangles(*mesh, state.yaw_radians, state.pitch_radians, center, radius, state.cached_triangles,
                                       drawn_triangles))
            {
                frame.error = "Mesh preview: no drawable triangles.";
                return frame;
            }
            state.triangle_count = drawn_triangles;

            if (size_changed)
            {
                state.raster.Resize(pixel_size, pixel_size);
            }
            drawBackground(state.raster);
            for (const MeshPreviewTriangle& tri : state.cached_triangles)
            {
                state.raster.FillTriangle(tri.p0.x, tri.p0.y, tri.p1.x, tri.p1.y, tri.p2.x, tri.p2.y, tri.color);
            }

            state.texture_handle =
                gpu->UpdateDynamicTexture(state.texture_handle, state.raster.Data(), pixel_size, pixel_size);
            if (state.texture_handle == nullptr)
            {
                frame.error = "Mesh preview upload failed.";
                return frame;
            }
        }

        frame.texture_handle = state.texture_handle;
        frame.vertex_count = state.vertex_count;
        frame.index_count = state.index_count;
        frame.triangle_count = state.triangle_count;
        frame.ok = true;
        return frame;
    }
}  // namespace MeshDataPreview
