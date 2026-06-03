#include "InspectorMaterialPreview.h"

#include "InspectorAssetCommon.h"

#include "Editor/EditorWindow/PreviewWindow/PreviewRaster.h"
#include "Runtime/Core/Math/MathHeaders.h"
#include "Runtime/UI/Render/UiGpuResources.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stb_image.h>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    struct MaterialPreviewImage
    {
        int width {0};
        int height {0};
        std::vector<unsigned char> rgba_pixels;
    };

    struct MaterialPreviewImageCacheEntry
    {
        bool attempted = false;
        std::filesystem::file_time_type write_time = std::filesystem::file_time_type::min();
        std::shared_ptr<MaterialPreviewImage> image;
    };

    struct MaterialPreviewImageSample
    {
        Vector3 rgb {1.0f, 1.0f, 1.0f};
        float alpha {1.0f};
    };

    struct MaterialPreviewTextures
    {
        std::shared_ptr<MaterialPreviewImage> base_color_map;
        std::shared_ptr<MaterialPreviewImage> metallic_roughness_map;
        std::shared_ptr<MaterialPreviewImage> normal_map;
        std::shared_ptr<MaterialPreviewImage> occlusion_map;
        std::shared_ptr<MaterialPreviewImage> emission_map;
    };

    struct MaterialPreviewVertex
    {
        Vector3 position;
        Vector3 normal;
        Vector3 tangent;
        Vector3 bitangent;
        Vector2 uv;
    };

    struct MaterialPreviewSurface
    {
        Vector3 base_color {1.0f, 1.0f, 1.0f};
        Vector3 emission {0.0f, 0.0f, 0.0f};
        Vector3 normal {0.0f, 0.0f, 1.0f};
        float alpha {1.0f};
        float metallic {0.0f};
        float roughness {1.0f};
        float occlusion {1.0f};
    };

    Vector3 ClampPreviewColor(const Vector3& color)
{
    return Vector3(Math::Clamp(color.x, 0.0f, 1.0f), Math::Clamp(color.y, 0.0f, 1.0f), Math::Clamp(color.z, 0.0f, 1.0f));
}

Vector3 NormalizePreviewVector(const Vector3& vector, const Vector3& fallback)
{
    return vector.isZeroLength() ? fallback : vector.normalisedCopy();
}

float SaturatePreview(float value)
{
    return Math::Clamp(value, 0.0f, 1.0f);
}

Vector3 GetMaterialPreviewLightDirection(const MaterialPreviewState& state)
{
    const float sin_yaw = std::sin(state.light_yaw_radians);
    const float cos_yaw = std::cos(state.light_yaw_radians);
    const float sin_pitch = std::sin(state.light_pitch_radians);
    const float cos_pitch = std::cos(state.light_pitch_radians);

    return NormalizePreviewVector(Vector3(sin_yaw, -sin_pitch * cos_yaw, cos_pitch * cos_yaw), Vector3::UNIT_Z);
}

Vector3 SamplePreviewEnvironmentColor(const Vector3& direction, const MaterialPreviewState& state)
{
    const Vector3 normalized_direction = NormalizePreviewVector(direction, Vector3::UNIT_Z);
    const float up_factor = SaturatePreview(normalized_direction.y * 0.5f + 0.5f);
    const float sky_factor = std::pow(up_factor, 0.65f);
    const float ground_factor = std::pow(1.0f - up_factor, 0.75f);

    const Vector3 sky_horizon(0.58f, 0.66f, 0.78f);
    const Vector3 sky_zenith(0.15f, 0.22f, 0.38f);
    const Vector3 ground_horizon(0.30f, 0.27f, 0.24f);
    const Vector3 ground_bottom(0.10f, 0.10f, 0.11f);

    const Vector3 sky_color = sky_horizon * (1.0f - sky_factor) + sky_zenith * sky_factor;
    const Vector3 ground_color = ground_horizon * (1.0f - ground_factor) + ground_bottom * ground_factor;
    Vector3 environment_color = ground_color * (1.0f - up_factor) + sky_color * up_factor;

    const Vector3 light_direction = GetMaterialPreviewLightDirection(state);
    const float sun_factor = std::pow(SaturatePreview(normalized_direction.dotProduct(light_direction)), 48.0f);
    environment_color += Vector3(1.00f, 0.93f, 0.84f) * sun_factor * 0.35f;

    return ClampPreviewColor(environment_color * (0.35f + state.environment_intensity * 0.65f));
}

uint32_t ToPreviewColor(const Vector3& color, float alpha)
{
    const Vector3 clamped_color = ClampPreviewColor(color);
    const float clamped_alpha = Math::Clamp(alpha, 0.0f, 1.0f);
    return PreviewRaster::Pack(static_cast<uint8_t>(clamped_color.x * 255.0f + 0.5f),
                               static_cast<uint8_t>(clamped_color.y * 255.0f + 0.5f),
                               static_cast<uint8_t>(clamped_color.z * 255.0f + 0.5f),
                               static_cast<uint8_t>(clamped_alpha * 255.0f + 0.5f));
}

Vector3 RotatePreviewVector(const Vector3& vector, float yaw_radians, float pitch_radians)
{
    const float cos_yaw = std::cos(yaw_radians);
    const float sin_yaw = std::sin(yaw_radians);

    Vector3 yaw_rotated(cos_yaw * vector.x + sin_yaw * vector.z, vector.y, -sin_yaw * vector.x + cos_yaw * vector.z);

    const float cos_pitch = std::cos(pitch_radians);
    const float sin_pitch = std::sin(pitch_radians);
    return Vector3(yaw_rotated.x,
                   cos_pitch * yaw_rotated.y - sin_pitch * yaw_rotated.z,
                   sin_pitch * yaw_rotated.y + cos_pitch * yaw_rotated.z);
}

MaterialPreviewVertex RotatePreviewVertex(const MaterialPreviewVertex& vertex, float yaw_radians, float pitch_radians)
{
    MaterialPreviewVertex rotated_vertex = vertex;
    rotated_vertex.position = RotatePreviewVector(vertex.position, yaw_radians, pitch_radians);
    rotated_vertex.normal = NormalizePreviewVector(RotatePreviewVector(vertex.normal, yaw_radians, pitch_radians), Vector3::UNIT_Z);
    rotated_vertex.tangent = NormalizePreviewVector(RotatePreviewVector(vertex.tangent, yaw_radians, pitch_radians), Vector3::UNIT_X);
    rotated_vertex.bitangent = NormalizePreviewVector(RotatePreviewVector(vertex.bitangent, yaw_radians, pitch_radians), Vector3::UNIT_Y);
    return rotated_vertex;
}

float WrapPreviewUV(float value)
{
    const float wrapped_value = value - std::floor(value);
    return wrapped_value < 0.0f ? wrapped_value + 1.0f : wrapped_value;
}

std::filesystem::path ResolvePreviewTexturePath(const eastl::string& stored_path)
{
    std::filesystem::path resolved_path = ResolveProjectAssetPath(stored_path);
    if (resolved_path.empty())
    {
        return {};
    }

    if (std::filesystem::exists(resolved_path))
    {
        return resolved_path.lexically_normal();
    }

    static constexpr std::array<const char*, 5> k_extensions = {".png", ".jpg", ".jpeg", ".tga", ".bmp"};
    for (const char* extension : k_extensions)
    {
        std::filesystem::path candidate_path = resolved_path;
        candidate_path.replace_extension(extension);
        if (std::filesystem::exists(candidate_path))
        {
            return candidate_path.lexically_normal();
        }
    }

    return {};
}

std::shared_ptr<MaterialPreviewImage> LoadPreviewTextureImage(const eastl::string& stored_path)
{
    const std::filesystem::path resolved_path = ResolvePreviewTexturePath(stored_path);
    if (resolved_path.empty())
    {
        return nullptr;
    }

    static std::unordered_map<std::string, MaterialPreviewImageCacheEntry> image_cache;
    const std::string cache_key = resolved_path.generic_string();
    MaterialPreviewImageCacheEntry& cache_entry = image_cache[cache_key];
    const std::filesystem::file_time_type current_write_time = GetInspectorFileWriteTime(resolved_path);
    if (cache_entry.attempted &&
        cache_entry.write_time == current_write_time)
    {
        return cache_entry.image;
    }

    cache_entry.attempted = true;
    cache_entry.write_time = current_write_time;
    cache_entry.image.reset();

    int width = 0;
    int height = 0;
    unsigned char* image_data = stbi_load(resolved_path.string().c_str(), &width, &height, nullptr, 4);
    if (image_data == nullptr || width <= 0 || height <= 0)
    {
        if (image_data != nullptr)
        {
            stbi_image_free(image_data);
        }
        return nullptr;
    }

    std::shared_ptr<MaterialPreviewImage> image = std::make_shared<MaterialPreviewImage>();
    image->width = width;
    image->height = height;
    image->rgba_pixels.assign(image_data, image_data + static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    stbi_image_free(image_data);

    cache_entry.image = image;
    return cache_entry.image;
}

MaterialPreviewTextures ResolveMaterialPreviewTextures(const MaterialRes& material)
{
    MaterialPreviewTextures textures;
    textures.base_color_map = LoadPreviewTextureImage(material.m_BaseColourTextureFile);
    textures.metallic_roughness_map = LoadPreviewTextureImage(material.m_MetallicRoughnessTextureFile);
    textures.normal_map = LoadPreviewTextureImage(material.m_NormalTextureFile);
    textures.occlusion_map = LoadPreviewTextureImage(material.m_OcclusionTextureFile);
    textures.emission_map = LoadPreviewTextureImage(material.m_EmissiveTextureFile);
    return textures;
}

bool UsesLitMaterialPreview(const MaterialRes& material)
{
    return material.GetShaderName().empty() || material.GetShaderName() == "StandardLit";
}

MaterialPreviewImageSample SamplePreviewImage(const std::shared_ptr<MaterialPreviewImage>& image,
                                              const Vector2& uv,
                                              const MaterialPreviewImageSample& fallback_sample = MaterialPreviewImageSample {})
{
    if (image == nullptr || image->width <= 0 || image->height <= 0 || image->rgba_pixels.empty())
    {
        return fallback_sample;
    }

    const float wrapped_u = WrapPreviewUV(uv.x);
    const float wrapped_v = 1.0f - WrapPreviewUV(uv.y);
    const int pixel_x = Math::Clamp(static_cast<int>(wrapped_u * static_cast<float>(image->width - 1) + 0.5f), 0, image->width - 1);
    const int pixel_y = Math::Clamp(static_cast<int>(wrapped_v * static_cast<float>(image->height - 1) + 0.5f), 0, image->height - 1);
    const size_t pixel_index = (static_cast<size_t>(pixel_y) * static_cast<size_t>(image->width) + static_cast<size_t>(pixel_x)) * 4;

    MaterialPreviewImageSample sample;
    sample.rgb = Vector3(image->rgba_pixels[pixel_index] / 255.0f,
                         image->rgba_pixels[pixel_index + 1] / 255.0f,
                         image->rgba_pixels[pixel_index + 2] / 255.0f);
    sample.alpha = image->rgba_pixels[pixel_index + 3] / 255.0f;
    return sample;
}

MaterialPreviewSurface EvaluateMaterialPreviewSurface(const MaterialRes& material,
                                                      const MaterialPreviewTextures& textures,
                                                      const MaterialPreviewVertex& vertex)
{
    const MaterialPreviewImageSample base_color_sample =
        SamplePreviewImage(textures.base_color_map, vertex.uv, MaterialPreviewImageSample {});

    MaterialPreviewSurface surface;
    surface.base_color = ClampPreviewColor(material.m_BaseColorFactor * base_color_sample.rgb);
    surface.alpha = material.m_IsBlend ? Math::Clamp(material.m_AlphaFactor * base_color_sample.alpha, 0.0f, 1.0f) : 1.0f;
    surface.metallic = Math::Clamp(material.m_MetallicFactor, 0.0f, 1.0f);
    surface.roughness = Math::Clamp(material.m_RoughnessFactor, 0.04f, 1.0f);
    surface.occlusion = 1.0f;

    const MaterialPreviewImageSample metallic_roughness_sample = SamplePreviewImage(
        textures.metallic_roughness_map, vertex.uv, MaterialPreviewImageSample {Vector3(1.0f, 1.0f, 1.0f), 1.0f});
    surface.metallic = Math::Clamp(surface.metallic * metallic_roughness_sample.rgb.z, 0.0f, 1.0f);
    surface.roughness = Math::Clamp(surface.roughness * metallic_roughness_sample.rgb.y, 0.04f, 1.0f);

    const MaterialPreviewImageSample occlusion_sample =
        SamplePreviewImage(textures.occlusion_map, vertex.uv, MaterialPreviewImageSample {Vector3(1.0f, 1.0f, 1.0f), 1.0f});
    surface.occlusion = Math::Clamp(1.0f - material.m_OcclusionStrength + occlusion_sample.rgb.x * material.m_OcclusionStrength, 0.0f, 1.0f);

    const MaterialPreviewImageSample emission_sample =
        SamplePreviewImage(textures.emission_map, vertex.uv, MaterialPreviewImageSample {Vector3(1.0f, 1.0f, 1.0f), 1.0f});
    surface.emission = ClampPreviewColor(material.m_EmissiveFactor * emission_sample.rgb);

    surface.normal = NormalizePreviewVector(vertex.normal, Vector3::UNIT_Z);
    if (textures.normal_map != nullptr)
    {
        const MaterialPreviewImageSample normal_sample =
            SamplePreviewImage(textures.normal_map, vertex.uv, MaterialPreviewImageSample {Vector3(0.5f, 0.5f, 1.0f), 1.0f});
        Vector3 tangent_space_normal(normal_sample.rgb.x * 2.0f - 1.0f,
                                     normal_sample.rgb.y * 2.0f - 1.0f,
                                     normal_sample.rgb.z * 2.0f - 1.0f);
        tangent_space_normal.x *= material.m_NormalScale;
        tangent_space_normal.y *= material.m_NormalScale;
        tangent_space_normal = NormalizePreviewVector(tangent_space_normal, Vector3::UNIT_Z);

        const Vector3 tangent = NormalizePreviewVector(vertex.tangent, Vector3::UNIT_X);
        const Vector3 bitangent = NormalizePreviewVector(vertex.bitangent, Vector3::UNIT_Y);
        surface.normal = NormalizePreviewVector(tangent * tangent_space_normal.x + bitangent * tangent_space_normal.y + surface.normal * tangent_space_normal.z,
                                                surface.normal);
    }

    return surface;
}

Vector3 EvaluateMaterialPreviewColor(const MaterialPreviewSurface& surface, const MaterialPreviewState& state, bool use_lit_shading)
{
    if (!use_lit_shading)
    {
        return ClampPreviewColor(surface.base_color + surface.emission);
    }

    const Vector3 light_dir = GetMaterialPreviewLightDirection(state);
    const Vector3 view_dir = Vector3::UNIT_Z;

    const float ndotl = Math::Clamp(surface.normal.dotProduct(light_dir), 0.0f, 1.0f);

    Vector3 half_vector = light_dir + view_dir;
    if (half_vector.isZeroLength())
    {
        half_vector = view_dir;
    }
    else
    {
        half_vector.normalise();
    }

    const float ndoth = Math::Clamp(surface.normal.dotProduct(half_vector), 0.0f, 1.0f);
    const float ndotv = Math::Clamp(surface.normal.dotProduct(view_dir), 0.0f, 1.0f);
    const float specular_power = 8.0f + (1.0f - surface.roughness) * 120.0f;
    const float specular_factor = std::pow(ndoth, specular_power) * (0.45f + (1.0f - surface.roughness) * 1.55f);
    const float fresnel = std::pow(1.0f - ndotv, 5.0f);

    const Vector3 specular_color = Vector3(0.04f, 0.04f, 0.04f) * (1.0f - surface.metallic) + surface.base_color * surface.metallic;
    const Vector3 diffuse = surface.base_color * ndotl * (0.82f - surface.metallic * 0.22f);
    const Vector3 specular = specular_color * specular_factor;
    const Vector3 rim = specular_color * fresnel * 0.12f;

    const Vector3 ambient_environment = SamplePreviewEnvironmentColor(surface.normal, state) * surface.base_color * (0.10f + 0.24f * surface.occlusion);
    const Vector3 reflection_direction = NormalizePreviewVector((-view_dir).reflect(surface.normal), surface.normal);
    const Vector3 reflection_environment = SamplePreviewEnvironmentColor(reflection_direction, state) * specular_color *
                                           (0.08f + surface.metallic * 0.92f) * (0.18f + (1.0f - surface.roughness) * 0.82f) *
                                           state.reflection_intensity * (0.45f + fresnel * 0.55f);

    return ClampPreviewColor(ambient_environment + diffuse + specular + rim + reflection_environment + surface.emission);
}

Vector2 ProjectPreviewPoint(const Vector3& point, const Vector2& center, float radius)
{
    const float perspective_scale = 1.0f + point.z * 0.18f;
    return Vector2(center.x + point.x * radius * perspective_scale, center.y - point.y * radius * perspective_scale);
}

void DrawCheckerboardRaster(PreviewRaster& raster, float alpha)
{
    constexpr int checker_size = 16;
    const uint8_t alpha_value = static_cast<uint8_t>(SaturatePreview(alpha) * 255.0f + 0.5f);
    const int width_i = static_cast<int>(raster.Width());
    const int height_i = static_cast<int>(raster.Height());
    for (int y = 0; y < height_i; y += checker_size)
    {
        for (int x = 0; x < width_i; x += checker_size)
        {
            const bool even = (((x / checker_size) + (y / checker_size)) % 2) == 0;
            const uint32_t color = even ? PreviewRaster::Pack(72, 72, 76, alpha_value)
                                        : PreviewRaster::Pack(58, 58, 62, alpha_value);
            raster.FillRect(x, y, std::min(x + checker_size, width_i), std::min(y + checker_size, height_i), color);
        }
    }
}

// Procedural skybox painted into the preview bitmap (bitmap-local pixel space,
// origin top-left). Mirrors the previous ImGui DrawPreviewSkybox draw order.
void DrawSkyboxRaster(PreviewRaster& raster, const MaterialPreviewState& state)
{
    const Vector3 light_dir = GetMaterialPreviewLightDirection(state);
    const uint32_t sky_top = ToPreviewColor(Vector3(0.175f, 0.245f, 0.415f), 1.0f);  // avg of the two top corners
    const uint32_t sky_horizon = ToPreviewColor(Vector3(0.58f, 0.66f, 0.78f), 1.0f);
    const uint32_t ground_horizon = ToPreviewColor(Vector3(0.30f, 0.27f, 0.24f), 1.0f);
    const uint32_t ground_bottom = ToPreviewColor(Vector3(0.10f, 0.10f, 0.11f), 1.0f);

    const float width = static_cast<float>(raster.Width());
    const float height = static_cast<float>(raster.Height());
    const int width_i = static_cast<int>(raster.Width());
    const int height_i = static_cast<int>(raster.Height());
    const float horizon_y = height * (0.56f - light_dir.y * 0.08f);
    const int horizon_i = static_cast<int>(horizon_y);

    raster.FillRectVGradient(0, 0, width_i, horizon_i, sky_top, sky_horizon);
    raster.FillRectVGradient(0, horizon_i, width_i, height_i, ground_horizon, ground_bottom);
    raster.FillRect(0, horizon_i, width_i, horizon_i + 1, PreviewRaster::Pack(255, 255, 255, 28));

    const float sun_x = width * (0.5f + light_dir.x * 0.32f);
    const float sun_y = horizon_y - height * (0.20f + light_dir.y * 0.18f);
    raster.FillEllipse(sun_x, sun_y, width * 0.11f, width * 0.11f, PreviewRaster::Pack(255, 222, 170, 26));
    raster.FillEllipse(sun_x, sun_y, width * 0.065f, width * 0.065f, PreviewRaster::Pack(255, 228, 184, 46));
    raster.FillEllipse(sun_x, sun_y, width * 0.032f, width * 0.032f, PreviewRaster::Pack(255, 241, 220, 110));
}

MaterialPreviewVertex BuildSpherePreviewVertex(float u, float v, float theta, float phi)
{
    const Vector3 position(std::cos(phi) * std::sin(theta), std::sin(phi), std::cos(phi) * std::cos(theta));
    Vector3 tangent(std::cos(theta), 0.0f, -std::sin(theta));
    tangent = NormalizePreviewVector(tangent, Vector3::UNIT_X);
    const Vector3 normal = NormalizePreviewVector(position, Vector3::UNIT_Z);
    const Vector3 bitangent = NormalizePreviewVector(normal.crossProduct(tangent), Vector3::UNIT_Y);
    return MaterialPreviewVertex {position, normal, tangent, bitangent, Vector2(u, v)};
}

MaterialPreviewVertex BuildPlanePreviewVertex(float u, float v)
{
    const Vector3 position(-1.0f + 2.0f * u, -1.0f + 2.0f * v, 0.0f);
    const Vector3 normal = Vector3::UNIT_Z;
    const Vector3 tangent = Vector3::UNIT_X;
    const Vector3 bitangent = Vector3::UNIT_Y;
    return MaterialPreviewVertex {position, normal, tangent, bitangent, Vector2(u, v)};
}

MaterialPreviewVertex BuildCubePreviewVertex(const Vector3& face_normal, const Vector3& face_tangent, float u, float v)
{
    const Vector3 tangent = NormalizePreviewVector(face_tangent, Vector3::UNIT_X);
    const Vector3 normal = NormalizePreviewVector(face_normal, Vector3::UNIT_Z);
    const Vector3 bitangent = NormalizePreviewVector(normal.crossProduct(tangent), Vector3::UNIT_Y);
    const float half_extent = 0.78f;
    const Vector3 position = normal * half_extent + tangent * ((u * 2.0f - 1.0f) * half_extent) + bitangent * ((v * 2.0f - 1.0f) * half_extent);
    return MaterialPreviewVertex {position, normal, tangent, bitangent, Vector2(u, v)};
}

MaterialPreviewVertex AveragePreviewVertex(const MaterialPreviewVertex& v0, const MaterialPreviewVertex& v1, const MaterialPreviewVertex& v2)
{
    MaterialPreviewVertex average_vertex;
    average_vertex.position = (v0.position + v1.position + v2.position) / 3.0f;
    average_vertex.normal = NormalizePreviewVector((v0.normal + v1.normal + v2.normal) / 3.0f, Vector3::UNIT_Z);
    average_vertex.tangent = NormalizePreviewVector((v0.tangent + v1.tangent + v2.tangent) / 3.0f, Vector3::UNIT_X);
    average_vertex.bitangent = NormalizePreviewVector((v0.bitangent + v1.bitangent + v2.bitangent) / 3.0f, Vector3::UNIT_Y);
    average_vertex.uv = Vector2((v0.uv.x + v1.uv.x + v2.uv.x) / 3.0f, (v0.uv.y + v1.uv.y + v2.uv.y) / 3.0f);
    return average_vertex;
}

void AppendMaterialPreviewTriangle(std::vector<MaterialPreviewTriangle>& triangles,
                                   const MaterialRes& material,
                                   const MaterialPreviewTextures& textures,
                                   const MaterialPreviewState& state,
                                   const MaterialPreviewVertex& v0,
                                   const MaterialPreviewVertex& v1,
                                   const MaterialPreviewVertex& v2,
                                   const Vector2& center,
                                   float radius)
{
    const float depth = (v0.position.z + v1.position.z + v2.position.z) / 3.0f;

    Vector3 face_normal = (v1.position - v0.position).crossProduct(v2.position - v0.position);
    if (face_normal.isZeroLength())
    {
        return;
    }
    face_normal.normalise();
    if (!material.m_IsDoubleSided && face_normal.z <= 0.0f)
    {
        return;
    }

    MaterialPreviewSurface surface = EvaluateMaterialPreviewSurface(material, textures, AveragePreviewVertex(v0, v1, v2));
    if (material.m_IsDoubleSided && surface.normal.z < 0.0f)
    {
        surface.normal = -surface.normal;
    }
    if (surface.alpha <= 0.01f)
    {
        return;
    }

    const bool use_lit_shading = UsesLitMaterialPreview(material);
    triangles.push_back(MaterialPreviewTriangle {ProjectPreviewPoint(v0.position, center, radius),
                                                 ProjectPreviewPoint(v1.position, center, radius),
                                                 ProjectPreviewPoint(v2.position, center, radius),
                                                 depth,
                                                 ToPreviewColor(EvaluateMaterialPreviewColor(surface, state, use_lit_shading), surface.alpha)});
}

void AppendSpherePreviewMesh(std::vector<MaterialPreviewTriangle>& triangles,
                             const MaterialRes& material,
                             const MaterialPreviewTextures& textures,
                             const MaterialPreviewState& state,
                             float yaw_radians,
                             float pitch_radians,
                             const Vector2& center,
                             float radius,
                             int latitude_segments,
                             int longitude_segments)
{
    const int k_latitude_segments = latitude_segments;
    const int k_longitude_segments = longitude_segments;
    for (int latitude = 0; latitude < k_latitude_segments; ++latitude)
    {
        const float v0 = static_cast<float>(latitude) / static_cast<float>(k_latitude_segments);
        const float v1 = static_cast<float>(latitude + 1) / static_cast<float>(k_latitude_segments);
        const float phi0 = -Math_HALF_PI + v0 * Math_PI;
        const float phi1 = -Math_HALF_PI + v1 * Math_PI;

        for (int longitude = 0; longitude < k_longitude_segments; ++longitude)
        {
            const float u0 = static_cast<float>(longitude) / static_cast<float>(k_longitude_segments);
            const float u1 = static_cast<float>(longitude + 1) / static_cast<float>(k_longitude_segments);
            const float theta0 = -Math_PI + u0 * Math_TWO_PI;
            const float theta1 = -Math_PI + u1 * Math_TWO_PI;

            const MaterialPreviewVertex p00 = RotatePreviewVertex(BuildSpherePreviewVertex(u0, v0, theta0, phi0), yaw_radians, pitch_radians);
            const MaterialPreviewVertex p10 = RotatePreviewVertex(BuildSpherePreviewVertex(u1, v0, theta1, phi0), yaw_radians, pitch_radians);
            const MaterialPreviewVertex p01 = RotatePreviewVertex(BuildSpherePreviewVertex(u0, v1, theta0, phi1), yaw_radians, pitch_radians);
            const MaterialPreviewVertex p11 = RotatePreviewVertex(BuildSpherePreviewVertex(u1, v1, theta1, phi1), yaw_radians, pitch_radians);

            AppendMaterialPreviewTriangle(triangles, material, textures, state, p00, p10, p11, center, radius);
            AppendMaterialPreviewTriangle(triangles, material, textures, state, p00, p11, p01, center, radius);
        }
    }
}

void AppendPlanePreviewMesh(std::vector<MaterialPreviewTriangle>& triangles,
                            const MaterialRes& material,
                            const MaterialPreviewTextures& textures,
                            const MaterialPreviewState& state,
                            float yaw_radians,
                            float pitch_radians,
                            const Vector2& center,
                            float radius)
{
    constexpr int k_plane_segments = 18;
    for (int y = 0; y < k_plane_segments; ++y)
    {
        const float v0 = static_cast<float>(y) / static_cast<float>(k_plane_segments);
        const float v1 = static_cast<float>(y + 1) / static_cast<float>(k_plane_segments);
        for (int x = 0; x < k_plane_segments; ++x)
        {
            const float u0 = static_cast<float>(x) / static_cast<float>(k_plane_segments);
            const float u1 = static_cast<float>(x + 1) / static_cast<float>(k_plane_segments);

            const MaterialPreviewVertex p00 = RotatePreviewVertex(BuildPlanePreviewVertex(u0, v0), yaw_radians, pitch_radians);
            const MaterialPreviewVertex p10 = RotatePreviewVertex(BuildPlanePreviewVertex(u1, v0), yaw_radians, pitch_radians);
            const MaterialPreviewVertex p01 = RotatePreviewVertex(BuildPlanePreviewVertex(u0, v1), yaw_radians, pitch_radians);
            const MaterialPreviewVertex p11 = RotatePreviewVertex(BuildPlanePreviewVertex(u1, v1), yaw_radians, pitch_radians);

            AppendMaterialPreviewTriangle(triangles, material, textures, state, p00, p10, p11, center, radius);
            AppendMaterialPreviewTriangle(triangles, material, textures, state, p00, p11, p01, center, radius);
        }
    }
}

void AppendCubePreviewMesh(std::vector<MaterialPreviewTriangle>& triangles,
                           const MaterialRes& material,
                           const MaterialPreviewTextures& textures,
                           const MaterialPreviewState& state,
                           float yaw_radians,
                           float pitch_radians,
                           const Vector2& center,
                           float radius)
{
    struct CubeFaceDesc
    {
        Vector3 normal;
        Vector3 tangent;
    };

    static const std::array<CubeFaceDesc, 6> k_cube_faces = {CubeFaceDesc {Vector3::UNIT_Z, Vector3::UNIT_X},
                                                             CubeFaceDesc {Vector3::NEGATIVE_UNIT_Z, Vector3::NEGATIVE_UNIT_X},
                                                             CubeFaceDesc {Vector3::UNIT_X, Vector3::NEGATIVE_UNIT_Z},
                                                             CubeFaceDesc {Vector3::NEGATIVE_UNIT_X, Vector3::UNIT_Z},
                                                             CubeFaceDesc {Vector3::UNIT_Y, Vector3::UNIT_X},
                                                             CubeFaceDesc {Vector3::NEGATIVE_UNIT_Y, Vector3::UNIT_X}};

    constexpr int k_cube_segments = 8;
    for (const CubeFaceDesc& face : k_cube_faces)
    {
        for (int y = 0; y < k_cube_segments; ++y)
        {
            const float v0 = static_cast<float>(y) / static_cast<float>(k_cube_segments);
            const float v1 = static_cast<float>(y + 1) / static_cast<float>(k_cube_segments);
            for (int x = 0; x < k_cube_segments; ++x)
            {
                const float u0 = static_cast<float>(x) / static_cast<float>(k_cube_segments);
                const float u1 = static_cast<float>(x + 1) / static_cast<float>(k_cube_segments);

                const MaterialPreviewVertex p00 = RotatePreviewVertex(BuildCubePreviewVertex(face.normal, face.tangent, u0, v0), yaw_radians, pitch_radians);
                const MaterialPreviewVertex p10 = RotatePreviewVertex(BuildCubePreviewVertex(face.normal, face.tangent, u1, v0), yaw_radians, pitch_radians);
                const MaterialPreviewVertex p01 = RotatePreviewVertex(BuildCubePreviewVertex(face.normal, face.tangent, u0, v1), yaw_radians, pitch_radians);
                const MaterialPreviewVertex p11 = RotatePreviewVertex(BuildCubePreviewVertex(face.normal, face.tangent, u1, v1), yaw_radians, pitch_radians);

                AppendMaterialPreviewTriangle(triangles, material, textures, state, p00, p10, p11, center, radius);
                AppendMaterialPreviewTriangle(triangles, material, textures, state, p00, p11, p01, center, radius);
            }
        }
    }
}

std::string BuildMaterialPreviewTextureSummary(const MaterialRes& material, const MaterialPreviewTextures& textures)
{
    std::string loaded_maps;
    std::string missing_maps;

    auto append_texture_state = [&](const char* label, const eastl::string& stored_path, const std::shared_ptr<MaterialPreviewImage>& image) {
        if (image != nullptr)
        {
            if (!loaded_maps.empty())
            {
                loaded_maps += " / ";
            }
            loaded_maps += label;
        }
        else if (!stored_path.empty())
        {
            if (!missing_maps.empty())
            {
                missing_maps += " / ";
            }
            missing_maps += label;
        }
    };

    append_texture_state("Base", material.m_BaseColourTextureFile, textures.base_color_map);
    append_texture_state("Normal", material.m_NormalTextureFile, textures.normal_map);
    append_texture_state("MetallicRoughness", material.m_MetallicRoughnessTextureFile, textures.metallic_roughness_map);
    append_texture_state("Occlusion", material.m_OcclusionTextureFile, textures.occlusion_map);
    append_texture_state("Emission", material.m_EmissiveTextureFile, textures.emission_map);

    if (loaded_maps.empty() && missing_maps.empty())
    {
        return "Textures: material parameters only";
    }

    std::string summary;
    if (!loaded_maps.empty())
    {
        summary = "Textures: " + loaded_maps;
    }
    if (!missing_maps.empty())
    {
        if (!summary.empty())
        {
            summary += "  |  ";
        }
        summary += "Missing: " + missing_maps;
    }

    return summary;
}

uint64_t ComputeMaterialPreviewSignature(const MaterialRes& material, const MaterialPreviewState& state, const Vector2& center, float radius)
{
    auto hash_combine = [](uint64_t seed, uint64_t value) {
        return seed ^ (value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2));
    };

    auto hash_float = [&](uint64_t seed, float value) {
        const uint32_t bits = *reinterpret_cast<const uint32_t*>(&value);
        return hash_combine(seed, static_cast<uint64_t>(bits));
    };

    uint64_t signature = 0;
    signature = hash_combine(signature, static_cast<uint64_t>(state.mesh_type));
    signature = hash_float(signature, state.yaw_radians);
    signature = hash_float(signature, state.pitch_radians);
    signature = hash_float(signature, state.zoom);
    signature = hash_float(signature, state.pan_offset.x);
    signature = hash_float(signature, state.pan_offset.y);
    signature = hash_float(signature, state.preview_size);
    signature = hash_float(signature, center.x);
    signature = hash_float(signature, center.y);
    signature = hash_float(signature, radius);
    signature = hash_combine(signature, material.m_IsBlend ? 1ULL : 0ULL);
    signature = hash_combine(signature, material.m_IsDoubleSided ? 1ULL : 0ULL);
    signature = hash_float(signature, material.m_BaseColorFactor.x);
    signature = hash_float(signature, material.m_BaseColorFactor.y);
    signature = hash_float(signature, material.m_BaseColorFactor.z);
    signature = hash_float(signature, material.m_AlphaFactor);
    signature = hash_float(signature, material.m_MetallicFactor);
    signature = hash_float(signature, material.m_RoughnessFactor);
    const auto hash_eastl_string = [&](uint64_t seed, const eastl::string& value) {
        return hash_combine(seed, static_cast<uint64_t>(eastl::hash<eastl::string>()(value)));
    };
    signature = hash_eastl_string(signature, material.m_BaseColourTextureFile);
    signature = hash_eastl_string(signature, material.m_MetallicRoughnessTextureFile);
    signature = hash_eastl_string(signature, material.m_NormalTextureFile);
    signature = hash_eastl_string(signature, material.m_OcclusionTextureFile);
    signature = hash_eastl_string(signature, material.m_EmissiveTextureFile);

    if (UsesLitMaterialPreview(material))
    {
        signature = hash_float(signature, state.light_yaw_radians);
        signature = hash_float(signature, state.light_pitch_radians);
        signature = hash_float(signature, state.environment_intensity);
        signature = hash_float(signature, state.reflection_intensity);
        signature = hash_combine(signature, state.show_skybox_background ? 1ULL : 0ULL);
    }

    return signature;
}

// Renders the material's lit sphere (or plane/cube) into the preview bitmap in
// bitmap-local pixel space. Mirrors the old ImGui draw order, minus the
// interactive controls (the native Preview window shows a static view).
void RenderMaterialBitmap(PreviewRaster& raster,
                          const MaterialRes& material,
                          const MaterialPreviewTextures& textures,
                          MaterialPreviewState& state,
                          bool use_lit_shading)
{
    if (state.show_skybox_background)
    {
        DrawSkyboxRaster(raster, state);
        DrawCheckerboardRaster(raster, material.m_IsBlend ? 0.30f : 0.10f);
    }
    else
    {
        DrawCheckerboardRaster(raster, 1.0f);
    }
    raster.FillRect(0, 0, static_cast<int>(raster.Width()), static_cast<int>(raster.Height()),
                    PreviewRaster::Pack(18, 18, 22, state.show_skybox_background ? 24 : 45));

    const float size_f = static_cast<float>(raster.Width());
    const Vector2 center(size_f * 0.5f, size_f * 0.5f);
    const float base_radius = size_f * (state.mesh_type == MaterialPreviewMeshType::Plane ? 0.33f : 0.36f);
    const float radius = base_radius * state.zoom;

    if (use_lit_shading)
    {
        // Contact shadow.
        raster.FillEllipse(center.x, center.y + radius * 0.86f, radius * 0.82f, radius * 0.24f,
                           PreviewRaster::Pack(0, 0, 0, 64));
    }

    const uint64_t preview_signature = ComputeMaterialPreviewSignature(material, state, center, radius);
    if (state.cached_signature != preview_signature || state.cached_triangles.empty())
    {
        state.cached_signature = preview_signature;
        state.cached_triangles.clear();
        state.cached_triangles.reserve(32 * 64 * 2);

        const bool use_lite_sphere = !use_lit_shading;
        const int sphere_lat = use_lite_sphere ? 12 : 24;
        const int sphere_lon = use_lite_sphere ? 24 : 48;

        switch (state.mesh_type)
        {
            case MaterialPreviewMeshType::Plane:
                AppendPlanePreviewMesh(state.cached_triangles, material, textures, state, state.yaw_radians,
                                       state.pitch_radians, center, radius);
                break;
            case MaterialPreviewMeshType::Cube:
                AppendCubePreviewMesh(state.cached_triangles, material, textures, state, state.yaw_radians,
                                      state.pitch_radians, center, radius);
                break;
            case MaterialPreviewMeshType::Sphere:
            default:
                AppendSpherePreviewMesh(state.cached_triangles, material, textures, state, state.yaw_radians,
                                        state.pitch_radians, center, radius, sphere_lat, sphere_lon);
                break;
        }

        std::sort(state.cached_triangles.begin(), state.cached_triangles.end(),
                  [](const MaterialPreviewTriangle& lhs, const MaterialPreviewTriangle& rhs) { return lhs.depth < rhs.depth; });
    }

    for (const MaterialPreviewTriangle& triangle : state.cached_triangles)
    {
        raster.FillTriangle(triangle.p0.x, triangle.p0.y, triangle.p1.x, triangle.p1.y, triangle.p2.x, triangle.p2.y,
                            triangle.color);
    }

    // Outline (sphere ring is dropped; non-sphere gets a framing rect).
    if (state.mesh_type != MaterialPreviewMeshType::Sphere)
    {
        raster.StrokeRect(static_cast<int>(center.x - radius), static_cast<int>(center.y - radius),
                          static_cast<int>(center.x + radius), static_cast<int>(center.y + radius),
                          PreviewRaster::Pack(255, 255, 255, 44));
    }

    raster.StrokeRect(0, 0, static_cast<int>(raster.Width()), static_cast<int>(raster.Height()),
                      PreviewRaster::Pack(110, 110, 118, 255));
}

}  // namespace

MaterialPreviewResult RenderMaterialPreviewToTexture(const MaterialRes& material, uint32_t pixel_size)
{
    MaterialPreviewResult result;
    result.pixel_size = pixel_size;

    if (pixel_size == 0)
    {
        result.error = "Invalid preview size";
        return result;
    }

    UiGpuResources* gpu = UiGpuResources::Get();
    if (gpu == nullptr || !gpu->IsReady())
    {
        result.error = "GPU resources unavailable";
        return result;
    }

    static MaterialPreviewState s_state;
    static void* s_handle = nullptr;
    static PreviewRaster s_raster;

    const MaterialPreviewTextures textures = ResolveMaterialPreviewTextures(material);
    const bool use_lit_shading = UsesLitMaterialPreview(material);

    result.texture_summary = BuildMaterialPreviewTextureSummary(material, textures);

    // Static view: clamp/keep defaults (no interactive controls in the native window).
    s_state.zoom = Math::Clamp(s_state.zoom, 0.55f, 2.40f);

    if (s_raster.Width() != pixel_size || s_raster.Height() != pixel_size)
    {
        s_raster.Resize(pixel_size, pixel_size);
        s_state.cached_signature = 0;  // force re-tessellation at the new size
    }

    RenderMaterialBitmap(s_raster, material, textures, s_state, use_lit_shading);

    s_handle = gpu->UpdateDynamicTexture(s_handle, s_raster.Data(), pixel_size, pixel_size);
    if (s_handle == nullptr)
    {
        result.error = "Material preview upload failed.";
        return result;
    }

    result.texture_handle = s_handle;
    result.ok = true;
    return result;
}