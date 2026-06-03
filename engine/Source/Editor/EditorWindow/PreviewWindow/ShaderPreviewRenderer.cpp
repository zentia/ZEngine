#include "ShaderPreviewRenderer.h"

#include "Runtime/Function/Render/Interface/DX12/DX12RHI.h"
#include "Runtime/Function/Render/Interface/DX12/DX12ShaderCompiler.h"
#include "Runtime/Function/Render/RenderSystem.h"
#include "Runtime/Function/Render/RenderType.h"
#include "Runtime/Core/Math/Vector2.h"  // UV float2 in preview vertices (was ImVec2)
#include "Runtime/Function/Render/ShaderRegistry.h"
#include "Runtime/Function/ShaderLab/ShaderLabParser.h"
#include "Runtime/Project/ProjectInfo.h"
#include "Runtime/Resource/ResType/Data/Material.h"
#include "Runtime/UI/Render/UiGpuResources.h"  // AdoptExternalImageView (native ZSlate preview handle)
#include "core/Log/LogSystem.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_set>
#include <vector>

#if defined(_WIN32)
    #include <cctype>
#endif

#ifdef _WIN32
    #include "Runtime/Function/Render/Interface/DX12/DX12RHIResource.h"  // DX12Image / DX12ImageView (RTT adopt)
    #include <d3d12.h>
    #include <d3dcompiler.h>
    #include <wrl/client.h>
    #pragma comment(lib, "d3dcompiler.lib")
using Microsoft::WRL::ComPtr;
#endif

namespace
{
    std::filesystem::file_time_type GetFileWriteTime(const std::filesystem::path& path)
    {
        std::error_code error_code;
        const auto write_time = std::filesystem::last_write_time(path, error_code);
        return error_code ? std::filesystem::file_time_type::min() : write_time;
    }

    bool ReadTextFile(const std::filesystem::path& path, std::string& output)
    {
        std::ifstream file(path, std::ios::in | std::ios::binary);
        if (!file.is_open())
        {
            output.clear();
            return false;
        }

        std::ostringstream buffer;
        buffer << file.rdbuf();
        output = buffer.str();
        return true;
    }

    void LogLegacyAssetsShaderWarningOnce(const std::string& shader_path)
    {
        static std::unordered_set<std::string> warned_paths;
        if (warned_paths.insert(shader_path).second)
        {
            LOG_WARNING(ZShader,
                        ".shader file under Assets/ is deprecated; move {} to <project>/Shaders/",
                        shader_path);
        }
    }

    std::string NormaliseShaderLookupKey(const eastl::string& shader_name)
    {
        std::string key = shader_name.c_str();
#if defined(_WIN32)
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
#endif
        return key;
    }

    std::filesystem::path GetProjectContentPath()
    {
        const std::shared_ptr<ProjectInfo> project_info = GET_SYSTEM(ProjectInfo);
        if (project_info == nullptr)
        {
            return {};
        }

        const std::filesystem::path content_path = project_info->GetProjectContent();
        return content_path.empty() ? std::filesystem::path {} : std::filesystem::absolute(content_path).lexically_normal();
    }

    std::filesystem::path FindShaderByName(const eastl::string& shader_name)
    {
        if (shader_name.empty() || shader_name == "StandardLit")
        {
            return {};
        }

        const std::shared_ptr<ProjectInfo> project_info = GET_SYSTEM(ProjectInfo);
        if (project_info == nullptr || project_info->GetProjectRoot().empty())
        {
            return {};
        }

        static std::string s_CachedProjectRoot;
        static std::unordered_map<std::string, std::filesystem::path> s_ResolvedByName;

        const std::string project_root_key = project_info->GetProjectRoot().generic_string();
        if (project_root_key != s_CachedProjectRoot)
        {
            s_CachedProjectRoot = project_root_key;
            s_ResolvedByName.clear();
        }

        const std::string lookup_key = shader_name.c_str();
        const auto cached_it = s_ResolvedByName.find(lookup_key);
        if (cached_it != s_ResolvedByName.end())
        {
            return cached_it->second;
        }

        std::filesystem::path resolved;
        if (ShaderRegistry* registry = GET_SYSTEM(ShaderRegistry).get())
        {
            if (const ShaderRegistryEntry* entry = registry->FindByName(shader_name))
            {
                if (!entry->m_SourceRelPath.empty())
                {
                    std::error_code source_ec;
                    resolved = project_info->GetProjectRoot() / std::filesystem::path(entry->m_SourceRelPath.c_str());
                    if (!std::filesystem::exists(resolved, source_ec) || source_ec)
                    {
                        resolved.clear();
                    }
                    else
                    {
                        resolved = resolved.lexically_normal();
                    }
                }
            }
        }

        s_ResolvedByName[lookup_key] = resolved;
        return resolved;
    }

    struct PreviewSource
    {
        std::filesystem::path shader_path;
        std::string source_code;
        std::string vertex_entry {"main"};
        std::string fragment_entry {"main"};
        std::vector<uint8_t> material_constant_data;
    };

    std::string NormalizePreviewPropertyKey(const std::string& value)
    {
        std::string normalized;
        normalized.reserve(value.size());
        for (const unsigned char ch : value)
        {
            if (std::isalnum(ch) != 0)
            {
                normalized.push_back(static_cast<char>(std::tolower(ch)));
            }
        }
        return normalized;
    }

    bool IsPreviewBaseColorProperty(const std::string& normalized_key)
    {
        return normalized_key == "basecolor" || normalized_key == "color" || normalized_key == "albedo" || normalized_key == "maincolor";
    }

    bool IsPreviewEmissionColorProperty(const std::string& normalized_key)
    {
        return normalized_key == "emission" || normalized_key == "emissive" || normalized_key == "emissioncolor" || normalized_key == "emissivecolor";
    }

    bool IsPreviewMetallicProperty(const std::string& normalized_key)
    {
        return normalized_key == "metallic" || normalized_key == "metalness";
    }

    bool IsPreviewRoughnessProperty(const std::string& normalized_key)
    {
        return normalized_key == "roughness" || normalized_key == "smoothness";
    }

    bool IsPreviewNormalScaleProperty(const std::string& normalized_key)
    {
        return normalized_key == "normalscale";
    }

    bool IsPreviewOcclusionProperty(const std::string& normalized_key)
    {
        return normalized_key == "occlusion" || normalized_key == "ao" || normalized_key == "occlusionstrength";
    }

    const MaterialFloatProperty* FindPreviewMaterialFloatProperty(const Material* material, const std::string& property_name)
    {
        if (material == nullptr)
        {
            return nullptr;
        }

        for (const MaterialFloatProperty& property : material->m_FloatProperties)
        {
            if (property_name == property.m_Name.c_str())
            {
                return &property;
            }
        }
        return nullptr;
    }

    const MaterialColorProperty* FindPreviewMaterialColorProperty(const Material* material, const std::string& property_name)
    {
        if (material == nullptr)
        {
            return nullptr;
        }

        for (const MaterialColorProperty& property : material->m_ColorProperties)
        {
            if (property_name == property.m_Name.c_str())
            {
                return &property;
            }
        }
        return nullptr;
    }

    const MaterialToggleProperty* FindPreviewMaterialToggleProperty(const Material* material, const std::string& property_name)
    {
        if (material == nullptr)
        {
            return nullptr;
        }

        for (const MaterialToggleProperty& property : material->m_ToggleProperties)
        {
            if (property_name == property.m_Name.c_str())
            {
                return &property;
            }
        }
        return nullptr;
    }

    std::array<float, 4> ResolvePreviewShaderPropertyValue(const ZEngine::ShaderLab::ShaderProperty& property, const Material* preview_material)
    {
        using namespace ZEngine::ShaderLab;

        std::array<float, 4> value {0.0f, 0.0f, 0.0f, 0.0f};
        switch (property.type)
        {
            case PropertyType::Color:
                value = {property.default_value.color[0], property.default_value.color[1], property.default_value.color[2], property.default_value.color[3]};
                break;
            case PropertyType::Vector:
                value = {property.default_value.vector[0], property.default_value.vector[1], property.default_value.vector[2], property.default_value.vector[3]};
                break;
            case PropertyType::Float:
            case PropertyType::Int:
            case PropertyType::Range:
                value[0] = property.default_value.float_value;
                break;
            default:
                break;
        }

        if (preview_material == nullptr)
        {
            return value;
        }

        if (const MaterialColorProperty* color_property = FindPreviewMaterialColorProperty(preview_material, property.name))
        {
            return {color_property->m_Color.x, color_property->m_Color.y, color_property->m_Color.z, color_property->m_Alpha};
        }
        if (const MaterialFloatProperty* float_property = FindPreviewMaterialFloatProperty(preview_material, property.name))
        {
            value[0] = float_property->m_Value;
            return value;
        }
        if (const MaterialToggleProperty* toggle_property = FindPreviewMaterialToggleProperty(preview_material, property.name))
        {
            value[0] = toggle_property->m_Value ? 1.0f : 0.0f;
            return value;
        }

        const std::string normalized_key = NormalizePreviewPropertyKey(property.name);
        if (property.type == PropertyType::Color || property.type == PropertyType::Vector)
        {
            if (IsPreviewBaseColorProperty(normalized_key))
            {
                return {preview_material->m_BaseColorFactor.x,
                        preview_material->m_BaseColorFactor.y,
                        preview_material->m_BaseColorFactor.z,
                        preview_material->m_AlphaFactor};
            }
            if (IsPreviewEmissionColorProperty(normalized_key))
            {
                return {preview_material->m_EmissiveFactor.x,
                        preview_material->m_EmissiveFactor.y,
                        preview_material->m_EmissiveFactor.z,
                        1.0f};
            }
            return value;
        }

        if (property.type == PropertyType::Float || property.type == PropertyType::Int || property.type == PropertyType::Range)
        {
            if (IsPreviewMetallicProperty(normalized_key))
            {
                value[0] = preview_material->m_MetallicFactor;
            }
            else if (IsPreviewRoughnessProperty(normalized_key))
            {
                value[0] = preview_material->m_RoughnessFactor;
            }
            else if (IsPreviewNormalScaleProperty(normalized_key))
            {
                value[0] = preview_material->m_NormalScale;
            }
            else if (IsPreviewOcclusionProperty(normalized_key))
            {
                value[0] = preview_material->m_OcclusionStrength;
            }
        }

        return value;
    }

    void AlignPreviewConstantBufferData(std::vector<uint8_t>& data, const size_t alignment)
    {
        const size_t remainder = data.size() % alignment;
        if (remainder != 0)
        {
            data.resize(data.size() + alignment - remainder, 0);
        }
    }

    void AppendPreviewShaderPropertyData(std::vector<uint8_t>& data,
                                         const ZEngine::ShaderLab::ShaderProperty& property,
                                         const std::array<float, 4>& value)
    {
        using namespace ZEngine::ShaderLab;

        if (property.type == PropertyType::Color || property.type == PropertyType::Vector)
        {
            AlignPreviewConstantBufferData(data, 16);
            const size_t offset = data.size();
            data.resize(offset + sizeof(float) * 4, 0);
            std::memcpy(data.data() + offset, value.data(), sizeof(float) * 4);
            return;
        }

        if (property.type == PropertyType::Float || property.type == PropertyType::Int || property.type == PropertyType::Range)
        {
            if ((data.size() % 16) + sizeof(float) > 16)
            {
                AlignPreviewConstantBufferData(data, 16);
            }
            const size_t offset = data.size();
            data.resize(offset + sizeof(float), 0);
            std::memcpy(data.data() + offset, value.data(), sizeof(float));
        }
    }

    void BuildPreviewMaterialConstantData(const std::shared_ptr<ZEngine::ShaderLab::ShaderLabAsset>& shader_asset,
                                          const Material* preview_material,
                                          std::vector<uint8_t>& out_data)
    {
        out_data.clear();
        if (shader_asset == nullptr)
        {
            return;
        }

        for (const ZEngine::ShaderLab::ShaderProperty& property : shader_asset->properties)
        {
            AppendPreviewShaderPropertyData(out_data, property, ResolvePreviewShaderPropertyValue(property, preview_material));
        }
    }

    std::string TrimPreviewText(const std::string& value)
    {
        size_t start = 0;
        while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])) != 0)
        {
            ++start;
        }

        size_t end = value.size();
        while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0)
        {
            --end;
        }

        return value.substr(start, end - start);
    }

    std::string ToLowerPreviewText(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        return value;
    }

    bool StartsWithPreviewInsensitive(const std::string& value, const char* prefix)
    {
        const size_t prefix_length = std::strlen(prefix);
        if (value.size() < prefix_length)
        {
            return false;
        }

        for (size_t index = 0; index < prefix_length; ++index)
        {
            if (std::tolower(static_cast<unsigned char>(value[index])) != std::tolower(static_cast<unsigned char>(prefix[index])))
            {
                return false;
            }
        }
        return true;
    }

    void ExtractPreviewEntryPoints(const std::string& program_source, std::string& vertex_entry, std::string& fragment_entry)
    {
        std::istringstream stream(program_source);
        std::string line;
        while (std::getline(stream, line))
        {
            const std::string trimmed = TrimPreviewText(line);
            if (!StartsWithPreviewInsensitive(trimmed, "#pragma"))
            {
                continue;
            }

            std::istringstream line_stream(trimmed);
            std::string pragma_token;
            std::string stage_token;
            std::string entry_token;
            line_stream >> pragma_token >> stage_token >> entry_token;
            stage_token = ToLowerPreviewText(stage_token);
            if (stage_token == "vertex" && !entry_token.empty())
            {
                vertex_entry = entry_token;
            }
            else if ((stage_token == "fragment" || stage_token == "pixel") && !entry_token.empty())
            {
                fragment_entry = entry_token;
            }
        }
    }

    std::string BuildPreviewCompileSource(const std::string& program_source)
    {
        std::istringstream stream(program_source);
        std::ostringstream sanitized;
        std::string line;
        bool first_line = true;
        while (std::getline(stream, line))
        {
            const std::string trimmed = TrimPreviewText(line);
            if (StartsWithPreviewInsensitive(trimmed, "#pragma"))
            {
                continue;
            }

            if (!first_line)
            {
                sanitized << '\n';
            }
            sanitized << line;
            first_line = false;
        }

        return TrimPreviewText(sanitized.str());
    }

    bool FillPreviewSourceFromProgram(const std::filesystem::path& shader_path,
                                      const std::string& program_source,
                                      const std::string& default_vertex_entry,
                                      const std::string& default_fragment_entry,
                                      PreviewSource& out_source)
    {
        out_source.shader_path = shader_path.lexically_normal();
        out_source.source_code = BuildPreviewCompileSource(program_source);
        out_source.vertex_entry = default_vertex_entry.empty() ? "vert" : default_vertex_entry;
        out_source.fragment_entry = default_fragment_entry.empty() ? "frag" : default_fragment_entry;
        ExtractPreviewEntryPoints(program_source, out_source.vertex_entry, out_source.fragment_entry);
        return !out_source.source_code.empty();
    }

    bool ExtractPreviewSourceFromShaderText(const std::filesystem::path& shader_path,
                                            const std::string& shader_source,
                                            PreviewSource& out_source)
    {
        static constexpr std::array<std::pair<const char*, const char*>, 2> k_program_markers = {{{"HLSLPROGRAM", "ENDHLSL"}, {"CGPROGRAM", "ENDCG"}}};

        for (const auto& markers : k_program_markers)
        {
            const size_t begin = shader_source.find(markers.first);
            if (begin == std::string::npos)
            {
                continue;
            }

            const size_t content_begin = begin + std::strlen(markers.first);
            const size_t end = shader_source.find(markers.second, content_begin);
            if (end == std::string::npos)
            {
                continue;
            }

            const std::string program_source = shader_source.substr(content_begin, end - content_begin);
            if (FillPreviewSourceFromProgram(shader_path, program_source, "vert", "frag", out_source))
            {
                return true;
            }
        }

        return false;
    }

    bool ResolvePreviewSource(const std::filesystem::path& selected_asset_path,

                              const std::string& resolved_asset_type,
                              const Material* preview_material,
                              PreviewSource& out_source,
                              std::string& out_error)
    {
        out_source = PreviewSource {};
        out_error.clear();

        std::filesystem::path shader_path;
        if (resolved_asset_type == "shader")
        {
            shader_path = selected_asset_path;
        }
        else if (preview_material != nullptr)
        {
            const eastl::string preview_shader_name = !preview_material->m_Shader.empty()
                                                          ? preview_material->m_Shader
                                                          : preview_material->GetShaderName();
            shader_path = FindShaderByName(preview_shader_name);
        }

        if (shader_path.empty() || shader_path.extension() != ".shader")
        {
            out_error = "The selected asset does not have a runnable .shader source file.";
            return false;
        }

        std::string source;
        if (!ReadTextFile(shader_path, source))
        {
            out_error = "Failed to read the shader source file.";
            return false;
        }

        ZEngine::ShaderLab::ShaderLabParser parser(source);
        const bool parsed_successfully = parser.Parse();
        const auto shader_asset = parsed_successfully ? parser.GetAsset() : nullptr;

        if (shader_asset != nullptr)
        {
            BuildPreviewMaterialConstantData(shader_asset, preview_material, out_source.material_constant_data);
            for (const auto& subshader : shader_asset->subshaders)
            {
                for (const auto& pass : subshader.passes)
                {
                    if (pass.programs.empty())
                    {
                        continue;
                    }

                    const auto& program = pass.programs.front();
                    if (FillPreviewSourceFromProgram(shader_path,
                                                     program.source_code,
                                                     program.vertex_entry,
                                                     program.fragment_entry,
                                                     out_source))
                    {
                        return true;
                    }
                }
            }
        }

        if (ExtractPreviewSourceFromShaderText(shader_path, source, out_source))
        {
            return true;
        }

        if (!parsed_successfully)
        {
            out_error = parser.GetError().empty() ? "ShaderLab parsing failed." : parser.GetError();
            return false;
        }

        if (shader_asset == nullptr)
        {
            out_error = "ShaderLab asset is null.";
            return false;
        }

        out_error = "The current shader does not provide an HLSL program for preview.";
        return false;
    }

    bool ResolvePreviewSourceCached(const std::filesystem::path& selected_asset_path,
                                    const std::string& resolved_asset_type,
                                    const Material* preview_material,
                                    PreviewSource& out_source,
                                    std::string& out_error)
    {
        static std::filesystem::path s_CacheKeyPath;
        static std::filesystem::file_time_type s_CacheKeyMtime = std::filesystem::file_time_type::min();
        static PreviewSource s_CachedSource;
        static std::string s_CachedError;
        static bool s_CacheValid = false;

        std::filesystem::path cache_path = selected_asset_path;
        if (resolved_asset_type == "material" && preview_material != nullptr)
        {
            const eastl::string shader_name = !preview_material->m_Shader.empty() ? preview_material->m_Shader
                                                                                  : preview_material->GetShaderName();
            cache_path = FindShaderByName(shader_name);
        }

        const std::filesystem::file_time_type cache_mtime = GetFileWriteTime(cache_path);
        if (s_CacheValid && cache_path == s_CacheKeyPath && cache_mtime == s_CacheKeyMtime)
        {
            out_source = s_CachedSource;
            out_error = s_CachedError;
            return out_error.empty();
        }

        s_CacheValid = ResolvePreviewSource(selected_asset_path, resolved_asset_type, preview_material, out_source, out_error);
        if (s_CacheValid)
        {
            s_CacheKeyPath = cache_path;
            s_CacheKeyMtime = cache_mtime;
            s_CachedSource = out_source;
            s_CachedError.clear();
        }
        else
        {
            s_CacheKeyPath.clear();
            s_CacheKeyMtime = std::filesystem::file_time_type::min();
            s_CachedSource = PreviewSource {};
            s_CachedError = out_error;
        }
        return s_CacheValid;
    }

    struct LocalVertex
    {
        Vector3 position {0.0f, 0.0f, 0.0f};
        Vector3 normal {0.0f, 0.0f, 1.0f};
        Vector3 tangent {1.0f, 0.0f, 0.0f};
        Vector2 uv {0.0f, 0.0f};
    };

    struct PreviewVertex
    {
        float position[4] {0.0f, 0.0f, 0.0f, 1.0f};
        float normal[3] {0.0f, 0.0f, 1.0f};
        float tangent[4] {1.0f, 0.0f, 0.0f, 1.0f};
        float uv[2] {0.0f, 0.0f};
    };

    struct PreviewTriangle
    {
        PreviewVertex vertices[3];
        float depth {0.0f};
    };

    enum class PreviewMeshType
    {
        Sphere = 0,
        Plane,
        Cube
    };

    struct PreviewState
    {
        float yaw_radians {0.6f};
        float pitch_radians {-0.35f};
        float zoom {1.0f};
        float preview_size {256.0f};
        PreviewMeshType mesh_type {PreviewMeshType::Sphere};
    };

    Vector3 NormalizeVector(const Vector3& value, const Vector3& fallback)
    {
        return value.squaredLength() <= 0.000001f ? fallback : value.normalisedCopy();
    }

    Vector3 RotateVector(const Vector3& value, const float yaw_radians, const float pitch_radians)
    {
        const float cos_yaw = std::cos(yaw_radians);
        const float sin_yaw = std::sin(yaw_radians);
        const float cos_pitch = std::cos(pitch_radians);
        const float sin_pitch = std::sin(pitch_radians);

        const Vector3 yaw_rotated(cos_yaw * value.x + sin_yaw * value.z, value.y, -sin_yaw * value.x + cos_yaw * value.z);
        return Vector3(yaw_rotated.x,
                       cos_pitch * yaw_rotated.y - sin_pitch * yaw_rotated.z,
                       sin_pitch * yaw_rotated.y + cos_pitch * yaw_rotated.z);
    }

    PreviewVertex TransformVertex(const LocalVertex& vertex, const PreviewState& state)
    {
        const Vector3 rotated_position = RotateVector(vertex.position, state.yaw_radians, state.pitch_radians);
        const Vector3 rotated_normal = NormalizeVector(RotateVector(vertex.normal, state.yaw_radians, state.pitch_radians), Vector3::UNIT_Z);
        const Vector3 rotated_tangent = NormalizeVector(RotateVector(vertex.tangent, state.yaw_radians, state.pitch_radians), Vector3::UNIT_X);

        const float camera_distance = 3.2f / std::max(state.zoom, 0.15f);
        const float view_z = rotated_position.z + camera_distance;
        const float inv_z = 1.0f / std::max(view_z, 0.15f);

        PreviewVertex output {};
        output.position[0] = rotated_position.x * 1.45f * inv_z;
        output.position[1] = rotated_position.y * 1.45f * inv_z;
        output.position[2] = std::clamp((view_z - 0.1f) / (camera_distance + 4.0f), 0.0f, 1.0f);
        output.position[3] = 1.0f;

        output.normal[0] = rotated_normal.x;
        output.normal[1] = rotated_normal.y;
        output.normal[2] = rotated_normal.z;

        output.tangent[0] = rotated_tangent.x;
        output.tangent[1] = rotated_tangent.y;
        output.tangent[2] = rotated_tangent.z;
        output.tangent[3] = 1.0f;

        output.uv[0] = vertex.uv.x;
        output.uv[1] = vertex.uv.y;
        return output;
    }

    void AppendTriangle(std::vector<PreviewTriangle>& triangles,
                        const LocalVertex& v0,
                        const LocalVertex& v1,
                        const LocalVertex& v2,
                        const PreviewState& state)
    {
        PreviewTriangle triangle {};
        triangle.vertices[0] = TransformVertex(v0, state);
        triangle.vertices[1] = TransformVertex(v1, state);
        triangle.vertices[2] = TransformVertex(v2, state);
        triangle.depth = (triangle.vertices[0].position[2] + triangle.vertices[1].position[2] + triangle.vertices[2].position[2]) / 3.0f;
        triangles.push_back(triangle);
    }

    LocalVertex BuildSphereVertex(const float u, const float v, const float theta, const float phi)
    {
        const Vector3 position(std::cos(phi) * std::sin(theta), std::sin(phi), std::cos(phi) * std::cos(theta));
        Vector3 tangent(std::cos(theta), 0.0f, -std::sin(theta));
        tangent = NormalizeVector(tangent, Vector3::UNIT_X);
        return LocalVertex {position, NormalizeVector(position, Vector3::UNIT_Z), tangent, Vector2(u, v)};
    }

    LocalVertex BuildPlaneVertex(const float u, const float v)
    {
        return LocalVertex {Vector3(-1.0f + 2.0f * u, -1.0f + 2.0f * v, 0.0f), Vector3::UNIT_Z, Vector3::UNIT_X, Vector2(u, v)};
    }

    LocalVertex BuildCubeVertex(const Vector3& face_normal, const Vector3& face_tangent, const float u, const float v)
    {
        const Vector3 tangent = NormalizeVector(face_tangent, Vector3::UNIT_X);
        const Vector3 normal = NormalizeVector(face_normal, Vector3::UNIT_Z);
        const Vector3 bitangent = NormalizeVector(normal.crossProduct(tangent), Vector3::UNIT_Y);
        const float half_extent = 0.78f;
        const Vector3 position = normal * half_extent + tangent * ((u * 2.0f - 1.0f) * half_extent) + bitangent * ((v * 2.0f - 1.0f) * half_extent);
        return LocalVertex {position, normal, tangent, Vector2(u, v)};
    }

    std::vector<PreviewVertex> BuildPreviewVertices(const PreviewState& state)
    {
        std::vector<PreviewTriangle> triangles;

        switch (state.mesh_type)
        {
            case PreviewMeshType::Plane:
            {
                constexpr int k_segments = 18;
                triangles.reserve(static_cast<size_t>(k_segments) * static_cast<size_t>(k_segments) * 2U);
                for (int y = 0; y < k_segments; ++y)
                {
                    const float v0 = static_cast<float>(y) / static_cast<float>(k_segments);
                    const float v1 = static_cast<float>(y + 1) / static_cast<float>(k_segments);
                    for (int x = 0; x < k_segments; ++x)
                    {
                        const float u0 = static_cast<float>(x) / static_cast<float>(k_segments);
                        const float u1 = static_cast<float>(x + 1) / static_cast<float>(k_segments);
                        const LocalVertex p00 = BuildPlaneVertex(u0, v0);
                        const LocalVertex p10 = BuildPlaneVertex(u1, v0);
                        const LocalVertex p01 = BuildPlaneVertex(u0, v1);
                        const LocalVertex p11 = BuildPlaneVertex(u1, v1);
                        AppendTriangle(triangles, p00, p10, p11, state);
                        AppendTriangle(triangles, p00, p11, p01, state);
                    }
                }
                break;
            }
            case PreviewMeshType::Cube:
            {
                struct FaceDesc
                {
                    Vector3 normal;
                    Vector3 tangent;
                };
                static const std::array<FaceDesc, 6> faces = {FaceDesc {Vector3::UNIT_Z, Vector3::UNIT_X},
                                                              FaceDesc {Vector3::NEGATIVE_UNIT_Z, Vector3::NEGATIVE_UNIT_X},
                                                              FaceDesc {Vector3::UNIT_X, Vector3::NEGATIVE_UNIT_Z},
                                                              FaceDesc {Vector3::NEGATIVE_UNIT_X, Vector3::UNIT_Z},
                                                              FaceDesc {Vector3::UNIT_Y, Vector3::UNIT_X},
                                                              FaceDesc {Vector3::NEGATIVE_UNIT_Y, Vector3::UNIT_X}};
                constexpr int k_segments = 8;
                triangles.reserve(faces.size() * static_cast<size_t>(k_segments) * static_cast<size_t>(k_segments) * 2U);
                for (const FaceDesc& face : faces)
                {
                    for (int y = 0; y < k_segments; ++y)
                    {
                        const float v0 = static_cast<float>(y) / static_cast<float>(k_segments);
                        const float v1 = static_cast<float>(y + 1) / static_cast<float>(k_segments);
                        for (int x = 0; x < k_segments; ++x)
                        {
                            const float u0 = static_cast<float>(x) / static_cast<float>(k_segments);
                            const float u1 = static_cast<float>(x + 1) / static_cast<float>(k_segments);
                            const LocalVertex p00 = BuildCubeVertex(face.normal, face.tangent, u0, v0);
                            const LocalVertex p10 = BuildCubeVertex(face.normal, face.tangent, u1, v0);
                            const LocalVertex p01 = BuildCubeVertex(face.normal, face.tangent, u0, v1);
                            const LocalVertex p11 = BuildCubeVertex(face.normal, face.tangent, u1, v1);
                            AppendTriangle(triangles, p00, p10, p11, state);
                            AppendTriangle(triangles, p00, p11, p01, state);
                        }
                    }
                }
                break;
            }
            case PreviewMeshType::Sphere:
            default:
            {
                constexpr int k_latitude_segments = 24;
                constexpr int k_longitude_segments = 48;
                triangles.reserve(static_cast<size_t>(k_latitude_segments) * static_cast<size_t>(k_longitude_segments) * 2U);
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
                        const LocalVertex p00 = BuildSphereVertex(u0, v0, theta0, phi0);
                        const LocalVertex p10 = BuildSphereVertex(u1, v0, theta1, phi0);
                        const LocalVertex p01 = BuildSphereVertex(u0, v1, theta0, phi1);
                        const LocalVertex p11 = BuildSphereVertex(u1, v1, theta1, phi1);
                        AppendTriangle(triangles, p00, p10, p11, state);
                        AppendTriangle(triangles, p00, p11, p01, state);
                    }
                }
                break;
            }
        }

        std::sort(triangles.begin(), triangles.end(), [](const PreviewTriangle& lhs, const PreviewTriangle& rhs) {
            return lhs.depth < rhs.depth;
        });

        std::vector<PreviewVertex> vertices;
        vertices.reserve(triangles.size() * 3U);
        for (const PreviewTriangle& triangle : triangles)
        {
            vertices.push_back(triangle.vertices[0]);
            vertices.push_back(triangle.vertices[1]);
            vertices.push_back(triangle.vertices[2]);
        }
        return vertices;
    }

#ifdef _WIN32
    constexpr DXGI_FORMAT k_preview_format = DXGI_FORMAT_R8G8B8A8_UNORM;
    constexpr uint32_t k_preview_render_texture_size = 420U;

    D3D12_RESOURCE_BARRIER MakeBarrier(ID3D12Resource* resource,

                                       const D3D12_RESOURCE_STATES before,
                                       const D3D12_RESOURCE_STATES after)
    {
        D3D12_RESOURCE_BARRIER barrier {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        return barrier;
    }

    class DX12PreviewRenderer
    {
    public:
        // Native ZSlate path: render the RTT (default view, no ImGui interaction
        // widgets) and return a UiGpuResources handle pointing at the color target.
        // No ImGui drawing. Re-renders only when the selected asset or the shader
        // source mtime changes; the handle is stable across frames once it appears.
        //
        // NOTE (P11g reconstruction): the original working-tree DrawNative body was
        // lost to an accidental `git checkout`; this is a best-effort rebuild from the
        // surviving header + free function + the documented design (sync GPU via
        // RunSynchronizedGpuReadback + ExecuteDedicatedUploadCommands, then adopt via
        // UiGpuResources::AdoptExternalImageView). Verify against intent if behaviour
        // differs from the pre-loss version.
        ShaderPreviewTextureResult DrawNative(const std::filesystem::path& selected_asset_path,
                                              const std::string& resolved_asset_type,
                                              const Material* preview_material)
        {
            ShaderPreviewTextureResult result;
            result.handled = true;

            const std::shared_ptr<RHI> rhi = GET_SYSTEM(RenderSystem)->GetRHI();
            if (rhi == nullptr || rhi->getGraphicsAPI() != GraphicsAPI::DirectX12)
            {
                result.handled = false;
                return result;
            }

            const std::shared_ptr<DX12RHI> dx12_rhi = std::static_pointer_cast<DX12RHI>(rhi);
            if (dx12_rhi == nullptr || dx12_rhi->IsDeviceRemoved(" before native shader preview"))
            {
                result.message = "DX12 device was removed; restart the editor.";
                return result;
            }

            PreviewSource preview_source;
            if (!ResolvePreviewSourceCached(selected_asset_path, resolved_asset_type, preview_material, preview_source, result.message))
            {
                if (resolved_asset_type == "shader" || (preview_material != nullptr && preview_material->GetShaderName() != "StandardLit"))
                {
                    // Keep handled = true so the caller surfaces result.message.
                    result.texture_handle = m_NativeHandle;
                    return result;
                }
                result.handled = false;
                result.message.clear();
                return result;
            }

            // Re-render gate: a fresh draw is only needed when the selection changed
            // or the shader source was edited on disk. Otherwise the persistent RTT
            // (and its adopted handle) already hold the latest content.
            const std::filesystem::file_time_type shader_write_time = GetFileWriteTime(preview_source.shader_path);
            if (m_NativeLastAssetPath != selected_asset_path)
            {
                m_NativeLastAssetPath = selected_asset_path;
                m_State = PreviewState {};
                m_NativeNeedsRender = true;
            }
            if (shader_write_time != m_NativeRenderedWriteTime)
            {
                m_NativeNeedsRender = true;
            }

            if (m_NativeNeedsRender)
            {
                const std::vector<PreviewVertex> vertices = BuildPreviewVertices(m_State);
                std::string render_error;
                bool rendered = false;
                // The dedicated upload command list is independent of the frame render
                // pass, so this is safe even while RP2's UI subpass is open.
                // RunSynchronizedGpuReadback flushes in-flight frames + runs on the RHI
                // thread, satisfying ExecuteDedicatedUploadCommands' threading contract.
                GET_SYSTEM(RenderSystem)->RunSynchronizedGpuReadback([&]() {
                    rendered = RenderNative(dx12_rhi, preview_source, vertices, k_preview_render_texture_size, render_error);
                });

                if (!rendered)
                {
                    // Leave m_NativeNeedsRender true so a later frame retries.
                    result.message = render_error;
                    result.texture_handle = m_NativeHandle;
                    return result;
                }

                result.rendered = true;
                m_NativeNeedsRender = false;
                m_NativeRenderedWriteTime = shader_write_time;

                // Adopt the RTT color target into UiGpuResources. EnsureShaderVisibleImageView
                // (invoked inside AdoptExternalImageView) creates the bindless SRV from the
                // wrapped DX12 resource, so we only need to point the view at m_RenderTarget.
                m_NativeImage.setResource(m_RenderTarget, k_preview_format, m_TextureSize, m_TextureSize, 1, 1, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                m_NativeView.setResource(&m_NativeImage, k_preview_format, {}, {}, RHI_IMAGE_VIEW_TYPE_2D, 1, 1);
                if (UiGpuResources* gpu = UiGpuResources::Get())
                {
                    m_NativeHandle = gpu->AdoptExternalImageView(m_NativeHandle, &m_NativeView, nullptr);
                }
            }

            result.texture_handle = m_NativeHandle;
            return result;
        }

    private:
        // Records the offscreen preview draw onto the dedicated (render-pass-independent)
        // upload command list. Device-side resources are created up front (they only need
        // the device, not a command list), then the draw is recorded inside the one-shot
        // list provided by ExecuteDedicatedUploadCommands.
        bool RenderNative(const std::shared_ptr<DX12RHI>& dx12_rhi,
                          const PreviewSource& preview_source,
                          const std::vector<PreviewVertex>& vertices,
                          const uint32_t texture_size,
                          std::string& out_error)
        {
            out_error.clear();
            if (vertices.empty())
            {
                out_error = "Preview mesh is empty.";
                return false;
            }

            ID3D12Device* device = dx12_rhi != nullptr ? dx12_rhi->getDevice() : nullptr;
            if (device == nullptr)
            {
                out_error = "DX12 device is unavailable.";
                return false;
            }

            if (m_Device != device)
            {
                reset();
                m_Device = device;
            }

            if (!ensureRootSignature(out_error) ||
                !ensureRenderTarget(dx12_rhi, texture_size, out_error) ||
                !ensurePipeline(preview_source, out_error) ||
                !ensureVertexBuffer(vertices, out_error) ||
                !ensureMaterialConstantBuffer(preview_source, out_error))
            {
                return false;
            }

            const bool ok = dx12_rhi->ExecuteDedicatedUploadCommands([&](ID3D12GraphicsCommandList* command_list) {
                if (m_RenderTargetState != D3D12_RESOURCE_STATE_RENDER_TARGET)
                {
                    const D3D12_RESOURCE_BARRIER barrier = MakeBarrier(m_RenderTarget.Get(), m_RenderTargetState, D3D12_RESOURCE_STATE_RENDER_TARGET);
                    command_list->ResourceBarrier(1, &barrier);
                    m_RenderTargetState = D3D12_RESOURCE_STATE_RENDER_TARGET;
                }

                D3D12_VIEWPORT viewport {};
                viewport.Width = static_cast<float>(texture_size);
                viewport.Height = static_cast<float>(texture_size);
                viewport.MinDepth = 0.0f;
                viewport.MaxDepth = 1.0f;
                D3D12_RECT scissor_rect {0, 0, static_cast<LONG>(texture_size), static_cast<LONG>(texture_size)};

                command_list->OMSetRenderTargets(1, &m_RtvHandle, FALSE, nullptr);
                command_list->RSSetViewports(1, &viewport);
                command_list->RSSetScissorRects(1, &scissor_rect);
                const float clear_color[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                command_list->ClearRenderTargetView(m_RtvHandle, clear_color, 0, nullptr);
                command_list->SetGraphicsRootSignature(m_RootSignature.Get());
                command_list->SetGraphicsRootConstantBufferView(0, m_MaterialConstantBuffer->GetGPUVirtualAddress());
                command_list->SetPipelineState(m_PipelineState.Get());
                command_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                command_list->IASetVertexBuffers(0, 1, &m_VertexBufferView);
                command_list->DrawInstanced(static_cast<UINT>(vertices.size()), 1, 0, 0);

                const D3D12_RESOURCE_BARRIER barrier = MakeBarrier(m_RenderTarget.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                command_list->ResourceBarrier(1, &barrier);
                m_RenderTargetState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            });

            if (!ok)
            {
                out_error = "Dedicated upload command execution failed.";
                return false;
            }
            return true;
        }

        bool ensureRootSignature(std::string& out_error)
        {
            if (m_RootSignature)
            {
                return true;
            }

            D3D12_ROOT_PARAMETER root_parameter {};
            root_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            root_parameter.Descriptor.ShaderRegister = 0;
            root_parameter.Descriptor.RegisterSpace = 0;
            root_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

            D3D12_ROOT_SIGNATURE_DESC root_signature_desc {};
            root_signature_desc.NumParameters = 1;
            root_signature_desc.pParameters = &root_parameter;
            root_signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

            ComPtr<ID3DBlob> signature_blob;
            ComPtr<ID3DBlob> error_blob;
            const HRESULT serialize_result = D3D12SerializeRootSignature(&root_signature_desc,
                                                                         D3D_ROOT_SIGNATURE_VERSION_1,
                                                                         &signature_blob,
                                                                         &error_blob);
            if (FAILED(serialize_result) || !signature_blob)
            {
                out_error = error_blob ? std::string(static_cast<const char*>(error_blob->GetBufferPointer()), error_blob->GetBufferSize())
                                       : "Failed to serialize the root signature.";
                return false;
            }

            if (FAILED(m_Device->CreateRootSignature(0,
                                                     signature_blob->GetBufferPointer(),
                                                     signature_blob->GetBufferSize(),
                                                     IID_PPV_ARGS(&m_RootSignature))) ||
                !m_RootSignature)
            {
                out_error = "Failed to create the root signature.";
                return false;
            }
            return true;
        }

        bool ensureRenderTarget(const std::shared_ptr<DX12RHI>& dx12_rhi, const uint32_t texture_size, std::string& out_error)
        {
            if (m_RenderTarget && m_TextureSize == texture_size)
            {
                return true;
            }

            m_RenderTarget.Reset();
            m_RtvHeap.Reset();
            m_TextureSize = 0;

            D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc {};
            rtv_heap_desc.NumDescriptors = 1;
            rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            if (FAILED(m_Device->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(&m_RtvHeap))) || !m_RtvHeap)
            {
                out_error = "Failed to create the RTT RTV heap.";
                return false;
            }

            D3D12_HEAP_PROPERTIES heap_properties {};
            heap_properties.Type = D3D12_HEAP_TYPE_DEFAULT;

            D3D12_RESOURCE_DESC resource_desc {};
            resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            resource_desc.Width = texture_size;
            resource_desc.Height = texture_size;
            resource_desc.DepthOrArraySize = 1;
            resource_desc.MipLevels = 1;
            resource_desc.Format = k_preview_format;
            resource_desc.SampleDesc.Count = 1;
            resource_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            resource_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

            D3D12_CLEAR_VALUE clear_value {};
            clear_value.Format = k_preview_format;
            if (FAILED(m_Device->CreateCommittedResource(&heap_properties,
                                                         D3D12_HEAP_FLAG_NONE,
                                                         &resource_desc,
                                                         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                         &clear_value,
                                                         IID_PPV_ARGS(&m_RenderTarget))) ||
                !m_RenderTarget)
            {
                out_error = "Failed to create the RTT color texture.";
                return false;
            }

            m_RtvHandle = m_RtvHeap->GetCPUDescriptorHandleForHeapStart();
            m_Device->CreateRenderTargetView(m_RenderTarget.Get(), nullptr, m_RtvHandle);

            // The sampleable SRV is created by UiGpuResources::AdoptExternalImageView
            // (-> EnsureShaderVisibleImageView) in the bindless heap; no SRV is built here.
            (void)dx12_rhi;

            m_TextureSize = texture_size;
            m_RenderTargetState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            return true;
        }

        bool ensurePipeline(const PreviewSource& preview_source, std::string& out_error)
        {
            const std::filesystem::file_time_type shader_write_time = GetFileWriteTime(preview_source.shader_path);
            const std::string cache_key = preview_source.shader_path.generic_string() + "|" + preview_source.vertex_entry + "|" + preview_source.fragment_entry;
            if (m_PipelineState && cache_key == m_PipelineCacheKey && shader_write_time == m_PipelineWriteTime)
            {
                return true;
            }

            const std::filesystem::path project_content = GetProjectContentPath();
            std::vector<std::string> include_paths;
            include_paths.push_back(preview_source.shader_path.parent_path().generic_string());
            if (!project_content.empty())
            {
                include_paths.push_back(project_content.generic_string());
            }

            const DX12ShaderCompileResult vs_result = m_Compiler.CompileFromSource(preview_source.source_code,
                                                                                   ShaderStage::Vertex,

                                                                                   preview_source.shader_path.generic_string(),
                                                                                   include_paths,
                                                                                   {},
                                                                                   preview_source.vertex_entry);
            if (!vs_result.success)
            {
                out_error = vs_result.error_message.empty() ? "Vertex shader compilation failed." : vs_result.error_message;
                return false;
            }

            const DX12ShaderCompileResult ps_result = m_Compiler.CompileFromSource(preview_source.source_code,
                                                                                   ShaderStage::Fragment,

                                                                                   preview_source.shader_path.generic_string(),
                                                                                   include_paths,
                                                                                   {},
                                                                                   preview_source.fragment_entry);
            if (!ps_result.success)
            {
                out_error = ps_result.error_message.empty() ? "Pixel shader compilation failed." : ps_result.error_message;
                return false;
            }

            const std::array<D3D12_INPUT_ELEMENT_DESC, 4> input_layout = {
                D3D12_INPUT_ELEMENT_DESC {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                D3D12_INPUT_ELEMENT_DESC {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                D3D12_INPUT_ELEMENT_DESC {"TANGENT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 28, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                D3D12_INPUT_ELEMENT_DESC {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 44, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};

            D3D12_BLEND_DESC blend_desc {};
            const D3D12_RENDER_TARGET_BLEND_DESC rt_blend = {TRUE,
                                                             FALSE,
                                                             D3D12_BLEND_SRC_ALPHA,
                                                             D3D12_BLEND_INV_SRC_ALPHA,
                                                             D3D12_BLEND_OP_ADD,
                                                             D3D12_BLEND_ONE,
                                                             D3D12_BLEND_INV_SRC_ALPHA,
                                                             D3D12_BLEND_OP_ADD,
                                                             D3D12_LOGIC_OP_NOOP,
                                                             D3D12_COLOR_WRITE_ENABLE_ALL};
            blend_desc.RenderTarget[0] = rt_blend;

            D3D12_RASTERIZER_DESC rasterizer_desc {};
            rasterizer_desc.FillMode = D3D12_FILL_MODE_SOLID;
            rasterizer_desc.CullMode = D3D12_CULL_MODE_NONE;
            rasterizer_desc.DepthClipEnable = TRUE;
            rasterizer_desc.FrontCounterClockwise = FALSE;

            D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline_desc {};
            pipeline_desc.pRootSignature = m_RootSignature.Get();
            pipeline_desc.VS.pShaderBytecode = vs_result.dxil_code.data();
            pipeline_desc.VS.BytecodeLength = vs_result.dxil_code.size();
            pipeline_desc.PS.pShaderBytecode = ps_result.dxil_code.data();
            pipeline_desc.PS.BytecodeLength = ps_result.dxil_code.size();
            pipeline_desc.BlendState = blend_desc;
            pipeline_desc.SampleMask = UINT_MAX;
            pipeline_desc.RasterizerState = rasterizer_desc;
            pipeline_desc.DepthStencilState.DepthEnable = FALSE;
            pipeline_desc.DepthStencilState.StencilEnable = FALSE;
            pipeline_desc.InputLayout.pInputElementDescs = input_layout.data();
            pipeline_desc.InputLayout.NumElements = static_cast<UINT>(input_layout.size());
            pipeline_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            pipeline_desc.NumRenderTargets = 1;
            pipeline_desc.RTVFormats[0] = k_preview_format;
            pipeline_desc.SampleDesc.Count = 1;

            if (FAILED(m_Device->CreateGraphicsPipelineState(&pipeline_desc, IID_PPV_ARGS(&m_PipelineState))) || !m_PipelineState)
            {
                out_error = "Failed to create the RTT pipeline state; the shader likely depends on material constants, textures, or extra root bindings.";

                return false;
            }

            m_PipelineCacheKey = cache_key;
            m_PipelineWriteTime = shader_write_time;
            return true;
        }

        bool ensureVertexBuffer(const std::vector<PreviewVertex>& vertices, std::string& out_error)
        {
            const size_t required_bytes = vertices.size() * sizeof(PreviewVertex);
            if (!m_VertexBuffer || m_VertexBufferCapacity < required_bytes)
            {
                m_VertexBuffer.Reset();
                m_VertexBufferCapacity = std::max(required_bytes, static_cast<size_t>(64 * 1024));

                D3D12_HEAP_PROPERTIES heap_properties {};
                heap_properties.Type = D3D12_HEAP_TYPE_UPLOAD;

                D3D12_RESOURCE_DESC resource_desc {};
                resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                resource_desc.Width = m_VertexBufferCapacity;
                resource_desc.Height = 1;
                resource_desc.DepthOrArraySize = 1;
                resource_desc.MipLevels = 1;
                resource_desc.SampleDesc.Count = 1;
                resource_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

                if (FAILED(m_Device->CreateCommittedResource(&heap_properties,
                                                             D3D12_HEAP_FLAG_NONE,
                                                             &resource_desc,
                                                             D3D12_RESOURCE_STATE_GENERIC_READ,
                                                             nullptr,
                                                             IID_PPV_ARGS(&m_VertexBuffer))) ||
                    !m_VertexBuffer)
                {
                    out_error = "Failed to create the vertex buffer.";
                    return false;
                }
            }

            void* mapped_data = nullptr;
            if (FAILED(m_VertexBuffer->Map(0, nullptr, &mapped_data)) || mapped_data == nullptr)
            {
                out_error = "Failed to map the vertex buffer.";
                return false;
            }

            std::memcpy(mapped_data, vertices.data(), required_bytes);
            m_VertexBuffer->Unmap(0, nullptr);

            m_VertexBufferView.BufferLocation = m_VertexBuffer->GetGPUVirtualAddress();
            m_VertexBufferView.SizeInBytes = static_cast<UINT>(required_bytes);
            m_VertexBufferView.StrideInBytes = sizeof(PreviewVertex);
            return true;
        }

        bool ensureMaterialConstantBuffer(const PreviewSource& preview_source, std::string& out_error)
        {
            const size_t source_bytes = std::max(preview_source.material_constant_data.size(), static_cast<size_t>(16));
            const size_t required_bytes = (source_bytes + 255U) & ~static_cast<size_t>(255U);
            if (!m_MaterialConstantBuffer || m_MaterialConstantBufferCapacity < required_bytes)
            {
                m_MaterialConstantBuffer.Reset();
                m_MaterialConstantBufferCapacity = required_bytes;

                D3D12_HEAP_PROPERTIES heap_properties {};
                heap_properties.Type = D3D12_HEAP_TYPE_UPLOAD;

                D3D12_RESOURCE_DESC resource_desc {};
                resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                resource_desc.Width = m_MaterialConstantBufferCapacity;
                resource_desc.Height = 1;
                resource_desc.DepthOrArraySize = 1;
                resource_desc.MipLevels = 1;
                resource_desc.SampleDesc.Count = 1;
                resource_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

                if (FAILED(m_Device->CreateCommittedResource(&heap_properties,
                                                             D3D12_HEAP_FLAG_NONE,
                                                             &resource_desc,
                                                             D3D12_RESOURCE_STATE_GENERIC_READ,
                                                             nullptr,
                                                             IID_PPV_ARGS(&m_MaterialConstantBuffer))) ||
                    !m_MaterialConstantBuffer)
                {
                    out_error = "Failed to create the preview material constant buffer.";
                    return false;
                }
            }

            void* mapped_data = nullptr;
            if (FAILED(m_MaterialConstantBuffer->Map(0, nullptr, &mapped_data)) || mapped_data == nullptr)
            {
                out_error = "Failed to map the preview material constant buffer.";
                return false;
            }

            std::memset(mapped_data, 0, m_MaterialConstantBufferCapacity);
            if (!preview_source.material_constant_data.empty())
            {
                std::memcpy(mapped_data, preview_source.material_constant_data.data(), preview_source.material_constant_data.size());
            }
            m_MaterialConstantBuffer->Unmap(0, nullptr);
            return true;
        }

        void reset()
        {
            m_RootSignature.Reset();
            m_PipelineState.Reset();
            m_RenderTarget.Reset();
            m_RtvHeap.Reset();
            m_VertexBuffer.Reset();
            m_MaterialConstantBuffer.Reset();
            m_TextureSize = 0;
            m_RtvHandle = {};
            m_VertexBufferView = {};
            m_VertexBufferCapacity = 0;
            m_MaterialConstantBufferCapacity = 0;
            m_PipelineCacheKey.clear();
            m_PipelineWriteTime = std::filesystem::file_time_type::min();
            m_RenderTargetState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            // Native path: m_RenderTarget was just released, so force a fresh draw +
            // re-adopt on the next DrawNative (handle reset -> new UiGpuResources entry).
            m_NativeNeedsRender = true;
            m_NativeRenderedWriteTime = std::filesystem::file_time_type::min();
            m_NativeHandle = nullptr;
        }

    private:
        PreviewState m_State;
        DX12ShaderCompiler m_Compiler;
        ID3D12Device* m_Device {nullptr};
        ComPtr<ID3D12RootSignature> m_RootSignature;
        ComPtr<ID3D12PipelineState> m_PipelineState;
        ComPtr<ID3D12DescriptorHeap> m_RtvHeap;
        ComPtr<ID3D12Resource> m_RenderTarget;
        ComPtr<ID3D12Resource> m_VertexBuffer;
        ComPtr<ID3D12Resource> m_MaterialConstantBuffer;
        D3D12_CPU_DESCRIPTOR_HANDLE m_RtvHandle {};
        D3D12_VERTEX_BUFFER_VIEW m_VertexBufferView {};
        D3D12_RESOURCE_STATES m_RenderTargetState {D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE};
        std::string m_PipelineCacheKey;
        std::filesystem::file_time_type m_PipelineWriteTime {std::filesystem::file_time_type::min()};
        size_t m_VertexBufferCapacity {0};
        size_t m_MaterialConstantBufferCapacity {0};
        uint32_t m_TextureSize {0};

        // P11g reconstruction: native ZSlate preview path. The RTT color target
        // (m_RenderTarget) is wrapped into these RHI shims and adopted into
        // UiGpuResources so an SImage can sample it; the handle is stable across
        // frames. Re-render is gated on selection / shader-source-mtime change.
        std::filesystem::path m_NativeLastAssetPath;
        bool m_NativeNeedsRender {true};
        std::filesystem::file_time_type m_NativeRenderedWriteTime {std::filesystem::file_time_type::min()};
        DX12Image m_NativeImage;
        DX12ImageView m_NativeView;
        void* m_NativeHandle {nullptr};
    };

#endif
}  // namespace

ShaderPreviewTextureResult RenderShaderPreviewToNativeTexture(const std::filesystem::path& selected_asset_path,
                                                              const std::string& resolved_asset_type,
                                                              const Material* preview_material)
{
#ifdef _WIN32
    // Distinct renderer instance from the ImGui widget so the two paths don't
    // fight over the shared RTT / pipeline state.
    static DX12PreviewRenderer renderer;
    return renderer.DrawNative(selected_asset_path, resolved_asset_type, preview_material);
#else
    (void)selected_asset_path;
    (void)resolved_asset_type;
    (void)preview_material;
    return {};
#endif
}
