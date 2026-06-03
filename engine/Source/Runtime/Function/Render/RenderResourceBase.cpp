#include "Runtime/Function/Render/RenderResourceBase.h"

#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Profiler/Profiler.h"
#include "Runtime/Project/ProjectInfo.h"
#include "Runtime/Resource/Asset/AssetManager.h"
#include "Runtime/Resource/Config/ConfigManager.h"
#include "Runtime/Resource/ResType/Data/MeshData.h"
#include "Runtime/Resource/ResType/Data/Shader.h"

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#define TINYOBJLOADER_DISABLE_FAST_FLOAT
#define TINYOBJLOADER_IMPLEMENTATION
#include "tiny_obj_loader.h"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <vector>

namespace
{
    std::filesystem::path GetProjectAssetRoot()
    {
        const std::shared_ptr<ProjectInfo> project_info = GET_SYSTEM(ProjectInfo);
        if (project_info != nullptr)
        {
            const std::filesystem::path project_content = project_info->GetProjectContent();
            if (!project_content.empty())
            {
                return std::filesystem::absolute(project_content).lexically_normal();
            }
        }
        return {};
    }

    std::filesystem::path ResolveProjectAssetPath(const eastl::string& asset_path)
    {
        if (asset_path.empty())
        {
            return {};
        }

        std::filesystem::path path(asset_path.c_str());
        if (path.is_absolute())
        {
            return path.lexically_normal();
        }

        const std::filesystem::path asset_root = GetProjectAssetRoot();
        if (!asset_root.empty())
        {
            return (asset_root / path).lexically_normal();
        }
        return std::filesystem::absolute(path).lexically_normal();
    }

    std::filesystem::path ResolveRuntimeAssetPath(const eastl::string& asset_path)
    {
        if (asset_path.empty())
        {
            return {};
        }

        std::filesystem::path path(asset_path.c_str());
        if (path.is_absolute())
        {
            return path.lexically_normal();
        }

        return GET_SYSTEM(AssetManager)->GetFullPath(asset_path).lexically_normal();
    }

    bool ShouldLogMeshLoadDebug(const eastl::string& mesh_asset)
    {
        return mesh_asset.find("cube.mesh.json") != eastl::string::npos;
    }

    std::string ToUpperCopy(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
            return static_cast<char>(std::toupper(character));
        });
        return value;
    }

    bool EqualsIgnoreCase(const eastl::string& lhs, const char* rhs)
    {
        return ToUpperCopy(lhs.c_str()) == ToUpperCopy(rhs != nullptr ? rhs : "");
    }

    RenderShaderPassData BuildRuntimeShaderPass(const ShaderPassDesc& shader_pass, const ShaderRes& shader_asset)
    {
        RenderShaderPassData runtime_pass;
        runtime_pass.m_Name = shader_pass.m_Name.empty() ? shader_pass.m_LightMode : shader_pass.m_Name;
        runtime_pass.m_LightMode = shader_pass.m_LightMode.empty() ? "GBuffer" : shader_pass.m_LightMode;
        runtime_pass.m_RenderPipeline =
            shader_pass.m_RenderPipeline.empty() ? shader_asset.m_RenderPipeline : shader_pass.m_RenderPipeline;
        runtime_pass.m_VertexEntry = shader_pass.m_VertexEntry.empty() ? shader_asset.m_VertexEntry : shader_pass.m_VertexEntry;
        runtime_pass.m_FragmentEntry =
            shader_pass.m_FragmentEntry.empty() ? shader_asset.m_FragmentEntry : shader_pass.m_FragmentEntry;
        runtime_pass.m_Cull = shader_pass.m_Cull.empty() ? "Back" : shader_pass.m_Cull;
        runtime_pass.m_Ztest = shader_pass.m_Ztest.empty() ? "LEqual" : shader_pass.m_Ztest;
        runtime_pass.m_Blend = shader_pass.m_Blend.empty() ? "Off" : shader_pass.m_Blend;
        runtime_pass.m_Zwrite = shader_pass.m_Zwrite;

        const eastl::string& vertex_shader_file =
            shader_pass.m_VertexShaderFile.empty() ? shader_asset.m_VertexShaderFile : shader_pass.m_VertexShaderFile;
        const eastl::string& fragment_shader_file = shader_pass.m_FragmentShaderFile.empty()
                                                        ? shader_asset.m_FragmentShaderFile
                                                        : shader_pass.m_FragmentShaderFile;

        const std::filesystem::path vertex_shader_path = ResolveProjectAssetPath(vertex_shader_file);
        const std::filesystem::path fragment_shader_path = ResolveProjectAssetPath(fragment_shader_file);
        runtime_pass.m_VertexShaderFile =
            !vertex_shader_path.empty() ? vertex_shader_path.generic_string().c_str() : vertex_shader_file;
        runtime_pass.m_FragmentShaderFile =
            !fragment_shader_path.empty() ? fragment_shader_path.generic_string().c_str() : fragment_shader_file;
        return runtime_pass;
    }

    RenderShaderPassData BuildLegacyRuntimeShaderPass(const ShaderRes& shader_asset)
    {
        ShaderPassDesc legacy_pass;
        legacy_pass.m_Name = "GBuffer";
        legacy_pass.m_LightMode = "GBuffer";
        legacy_pass.m_VertexShaderFile = shader_asset.m_VertexShaderFile;
        legacy_pass.m_FragmentShaderFile = shader_asset.m_FragmentShaderFile;
        legacy_pass.m_RenderPipeline = shader_asset.m_RenderPipeline;
        legacy_pass.m_VertexEntry = shader_asset.m_VertexEntry;
        legacy_pass.m_FragmentEntry = shader_asset.m_FragmentEntry;
        legacy_pass.m_Cull = "Back";
        legacy_pass.m_Ztest = "LEqual";
        legacy_pass.m_Blend = "Off";
        legacy_pass.m_Zwrite = true;
        return BuildRuntimeShaderPass(legacy_pass, shader_asset);
    }

    const RenderShaderPassData* FindShaderPassByLightMode(const std::vector<RenderShaderPassData>& shader_passes,
                                                          const char* desired_light_mode)
    {
        for (const RenderShaderPassData& shader_pass : shader_passes)
        {
            if (EqualsIgnoreCase(shader_pass.m_LightMode, desired_light_mode))
            {
                return &shader_pass;
            }
        }
        return nullptr;
    }

    bool TryResolveShaderAsset(const eastl::string& shader_name, ShaderRes& shader_asset, std::filesystem::path& shader_asset_path)

    {
        if (shader_name.empty() || shader_name == "StandardLit")
        {
            return false;
        }

        const std::filesystem::path asset_root = GetProjectAssetRoot();
        std::error_code error_code;
        if (asset_root.empty() || !std::filesystem::exists(asset_root, error_code))
        {
            return false;
        }

        // Phase B: ask the AssetManager directly for ShaderRes .zassets under
        // the project's asset root. The editor override consults the
        // AssetRegistry's by-type reverse index (O(matches)), and the runtime
        // base falls back to a directory walk -- which is the same cost as
        // the previous hand-rolled loop here.
        std::shared_ptr<AssetManager> asset_manager = GET_SYSTEM(AssetManager);
        if (asset_manager == nullptr)
        {
            return false;
        }

        for (const std::filesystem::path& current_path : asset_manager->GetAssetsByType("ShaderRes", asset_root))
        {
            std::filesystem::path read_path = current_path;
            std::unique_ptr<ShaderRes> shader(asset_manager->ReadObject<ShaderRes>(read_path));
            if (shader == nullptr)
            {
                continue;
            }

            const eastl::string candidate_name = shader->m_ShaderName.empty() ? current_path.stem().generic_string().c_str() : shader->m_ShaderName;
            if (candidate_name == shader_name || current_path.stem().generic_string() == shader_name.c_str())
            {
                shader_asset.m_ShaderName = shader->m_ShaderName;
                shader_asset.m_Properties = shader->m_Properties;
                shader_asset.m_Passes = shader->m_Passes;
                shader_asset.m_VertexShaderFile = shader->m_VertexShaderFile;
                shader_asset.m_FragmentShaderFile = shader->m_FragmentShaderFile;
                shader_asset.m_RenderPipeline = shader->m_RenderPipeline;
                shader_asset.m_SourceLanguage = shader->m_SourceLanguage;
                shader_asset.m_VertexEntry = shader->m_VertexEntry;
                shader_asset.m_FragmentEntry = shader->m_FragmentEntry;
                shader_asset.m_IncludeDirectory = shader->m_IncludeDirectory;
                shader_asset.m_EnableDx12 = shader->m_EnableDx12;
                shader_asset.m_EnableVulkan = shader->m_EnableVulkan;
                shader_asset.m_EnableMetal = shader->m_EnableMetal;
                shader_asset_path = current_path;
                return true;
            }
        }

        return false;
    }
}  // namespace

std::shared_ptr<TextureData> RenderResourceBase::LoadTextureHDR(eastl::string file, int desired_channels)

{
    Z_PROFILE_SCOPE("RenderResourceBase::loadTextureHDR");

    std::shared_ptr<TextureData> texture = std::make_shared<TextureData>();

    int iw, ih, n;
    auto&& path = GET_SYSTEM(AssetManager)->GetFullPath(file).string();
    LOG_INFO(ZRender, path);
    float* float_pixels = stbi_loadf(path.c_str(), &iw, &ih, &n, desired_channels);
    texture->m_Pixels = reinterpret_cast<uint8_t*>(float_pixels);

    if (!texture->m_Pixels)
        return nullptr;

    texture->m_Width = iw;
    texture->m_Height = ih;
    switch (desired_channels)
    {
        case 2:
            texture->m_Format = RHIFormat::RHI_FORMAT_R32G32_SFLOAT;
            break;
        case 4:
            texture->m_Format = RHIFormat::RHI_FORMAT_R32G32B32A32_SFLOAT;
            break;
        default:
            // three component format is not supported in some vulkan driver implementations
            throw std::runtime_error("unsupported channels number");
            break;
    }
    texture->m_Depth = 1;
    texture->m_ArrayLayers = 1;
    texture->m_MipLevels = 1;
    texture->m_Type = ZENGINE_IMAGE_TYPE::ZENGINE_IMAGE_TYPE_2D;

    return texture;
}

std::shared_ptr<TextureData> RenderResourceBase::LoadTexture(eastl::string file, bool is_srgb)
{
    std::shared_ptr<TextureData> texture = std::make_shared<TextureData>();

    int iw, ih, n;
    unsigned char* image_data =
        stbi_load(GET_SYSTEM(AssetManager)->GetFullPath(file).generic_string().c_str(), &iw, &ih, &n, 4);
    texture->m_Pixels = reinterpret_cast<uint8_t*>(image_data);

    if (!texture->m_Pixels)
        return nullptr;

    texture->m_Width = iw;
    texture->m_Height = ih;
    texture->m_Format = (is_srgb) ? RHIFormat::RHI_FORMAT_R8G8B8A8_SRGB : RHIFormat::RHI_FORMAT_R8G8B8A8_UNORM;
    texture->m_Depth = 1;
    texture->m_ArrayLayers = 1;
    texture->m_MipLevels = 1;
    texture->m_Type = ZENGINE_IMAGE_TYPE::ZENGINE_IMAGE_TYPE_2D;

    return texture;
}

RenderMeshData RenderResourceBase::LoadMeshData(const MeshSourceDesc& source, AxisAlignedBox& bounding_box)
{
    RenderMeshData ret;

    const std::filesystem::path mesh_asset_path = ResolveRuntimeAssetPath(source.m_MeshAsset);
    const std::string mesh_asset_path_string = mesh_asset_path.generic_string();
    if (mesh_asset_path.extension() == ".obj")
    {
        // 兼容旧资源：仍允许直接从 obj 源文件构建运行时网格。
        ret.m_StaticMeshData = LoadStaticMesh(mesh_asset_path.generic_string().c_str(), bounding_box);
    }
    else
    {
        MeshData* mesh_asset = GET_SYSTEM(AssetManager)->loadAsset<MeshData>(source.m_MeshAsset);
        if (mesh_asset == nullptr && !mesh_asset_path.empty())
        {
            std::filesystem::path read_path = mesh_asset_path;
            mesh_asset = GET_SYSTEM(AssetManager)->ReadObject<MeshData>(read_path);
        }
        if (mesh_asset == nullptr)
        {
            LOG_ERROR(ZRender,
                      "loadMeshData failed to load mesh asset='{}' full='{}'",
                      source.m_MeshAsset.c_str(),
                      mesh_asset_path_string.c_str());
            return ret;
        }

        // vertex buffer
        size_t vertex_size = mesh_asset->vertex_buffer.size() * sizeof(MeshVertexDataDefinition);
        ret.m_StaticMeshData.m_VertexBuffer = std::make_shared<BufferData>(vertex_size);
        MeshVertexDataDefinition* vertex = (MeshVertexDataDefinition*)ret.m_StaticMeshData.m_VertexBuffer->m_Data;
        for (size_t i = 0; i < mesh_asset->vertex_buffer.size(); i++)
        {
            vertex[i].x = mesh_asset->vertex_buffer[i].px;
            vertex[i].y = mesh_asset->vertex_buffer[i].py;
            vertex[i].z = mesh_asset->vertex_buffer[i].pz;
            vertex[i].nx = mesh_asset->vertex_buffer[i].nx;
            vertex[i].ny = mesh_asset->vertex_buffer[i].ny;
            vertex[i].nz = mesh_asset->vertex_buffer[i].nz;
            vertex[i].tx = mesh_asset->vertex_buffer[i].tx;
            vertex[i].ty = mesh_asset->vertex_buffer[i].ty;
            vertex[i].tz = mesh_asset->vertex_buffer[i].tz;
            vertex[i].u = mesh_asset->vertex_buffer[i].u;
            vertex[i].v = mesh_asset->vertex_buffer[i].v;

            bounding_box.Merge(Vector3(vertex[i].x, vertex[i].y, vertex[i].z));
        }

        // index buffer
        size_t index_size = mesh_asset->index_buffer.size() * sizeof(uint16_t);
        ret.m_StaticMeshData.m_IndexBuffer = std::make_shared<BufferData>(index_size);
        uint16_t* index = (uint16_t*)ret.m_StaticMeshData.m_IndexBuffer->m_Data;
        for (size_t i = 0; i < mesh_asset->index_buffer.size(); i++)
        {
            index[i] = static_cast<uint16_t>(mesh_asset->index_buffer[i]);
        }

        if (mesh_asset->vertex_buffer.empty() || mesh_asset->index_buffer.empty())
        {
            LOG_WARNING(ZRender,
                        "loadMeshData asset='{}' has empty geometry: vertices={} indices={} full='{}'",
                        source.m_MeshAsset.c_str(),
                        mesh_asset->vertex_buffer.size(),
                        mesh_asset->index_buffer.size(),
                        mesh_asset_path_string.c_str());
        }
        else if (ShouldLogMeshLoadDebug(source.m_MeshAsset))
        {
            const Vector3 bbox_min = bounding_box.getMinCorner();
            const Vector3 bbox_max = bounding_box.getMaxCorner();
            LOG_INFO(ZRender,
                     "loadMeshData asset='{}' vertices={} indices={} bbox_min=({}, {}, {}) bbox_max=({}, {}, {}) full='{}'",
                     source.m_MeshAsset.c_str(),
                     mesh_asset->vertex_buffer.size(),
                     mesh_asset->index_buffer.size(),
                     bbox_min.x,
                     bbox_min.y,
                     bbox_min.z,
                     bbox_max.x,
                     bbox_max.y,
                     bbox_max.z,
                     mesh_asset_path_string.c_str());
        }

        // skeleton binding buffer
        if (!mesh_asset->bind.empty())
        {
            if (mesh_asset->bind.size() != mesh_asset->vertex_buffer.size())
            {
                LOG_WARNING(ZRender,
                            "Mesh asset {} has {} binding entries but {} vertices; skip vertex blending.",
                            source.m_MeshAsset.c_str(),
                            mesh_asset->bind.size(),
                            mesh_asset->vertex_buffer.size());
            }
            else
            {
                size_t data_size = mesh_asset->bind.size() * sizeof(MeshVertexBindingDataDefinition);
                ret.m_SkeletonBindingBuffer = std::make_shared<BufferData>(data_size);
                MeshVertexBindingDataDefinition* binding_data =
                    reinterpret_cast<MeshVertexBindingDataDefinition*>(ret.m_SkeletonBindingBuffer->m_Data);
                for (size_t i = 0; i < mesh_asset->bind.size(); i++)
                {
                    binding_data[i].m_Index0 = mesh_asset->bind[i].index0;
                    binding_data[i].m_Index1 = mesh_asset->bind[i].index1;
                    binding_data[i].m_Index2 = mesh_asset->bind[i].index2;
                    binding_data[i].m_Index3 = mesh_asset->bind[i].index3;
                    binding_data[i].m_Weight0 = mesh_asset->bind[i].weight0;
                    binding_data[i].m_Weight1 = mesh_asset->bind[i].weight1;
                    binding_data[i].m_Weight2 = mesh_asset->bind[i].weight2;
                    binding_data[i].m_Weight3 = mesh_asset->bind[i].weight3;
                }
            }
        }
    }

    if (bounding_box.IsValid())
    {
        m_BoundingBoxCacheMap.insert(std::make_pair(source, bounding_box));
    }

    return ret;
}

RenderMaterialData RenderResourceBase::LoadMaterialData(const MaterialSourceDesc& source)
{
    Z_PROFILE_SCOPE("RenderResourceBase::loadMaterialData");
    RenderMaterialData ret;
    ret.m_Shader = source.m_Shader;
    ret.m_BaseColorTexture = LoadTexture(source.m_BaseColorFile, true);
    ret.m_MetallicRoughnessTexture = LoadTexture(source.m_MetallicRoughnessFile);
    ret.m_NormalTexture = LoadTexture(source.m_NormalFile);
    ret.m_OcclusionTexture = LoadTexture(source.m_OcclusionFile);
    ret.m_EmissiveTexture = LoadTexture(source.m_EmissiveFile);
    ret.m_EnabledShaderKeywords = source.m_EnabledShaderKeywords;

    ShaderRes shader_asset;
    std::filesystem::path shader_asset_path;
    if (TryResolveShaderAsset(source.m_Shader, shader_asset, shader_asset_path))
    {
        ret.m_Shader = shader_asset.m_ShaderName.empty() ? source.m_Shader : shader_asset.m_ShaderName;
        ret.m_ShaderAssetFile = shader_asset_path.generic_string().c_str();
        ret.m_SourceLanguage = shader_asset.m_SourceLanguage;
        ret.m_EnableDx12 = shader_asset.m_EnableDx12;
        ret.m_EnableVulkan = shader_asset.m_EnableVulkan;
        ret.m_EnableMetal = shader_asset.m_EnableMetal;

        const std::filesystem::path include_directory = ResolveProjectAssetPath(shader_asset.m_IncludeDirectory);
        if (!include_directory.empty())
        {
            ret.m_IncludeDirectory = include_directory.generic_string().c_str();
        }
        else
        {
            ret.m_IncludeDirectory = shader_asset.m_IncludeDirectory;
        }

        ret.m_ShaderPasses.clear();
        if (!shader_asset.m_Passes.empty())
        {
            ret.m_ShaderPasses.reserve(shader_asset.m_Passes.size());
            for (const ShaderPassDesc& shader_pass : shader_asset.m_Passes)
            {
                ret.m_ShaderPasses.emplace_back(BuildRuntimeShaderPass(shader_pass, shader_asset));
            }
        }
        else if (!shader_asset.m_VertexShaderFile.empty() || !shader_asset.m_FragmentShaderFile.empty())
        {
            ret.m_ShaderPasses.emplace_back(BuildLegacyRuntimeShaderPass(shader_asset));
        }

        const RenderShaderPassData* primary_pass = FindShaderPassByLightMode(ret.m_ShaderPasses, "GBuffer");
        if (primary_pass == nullptr && !ret.m_ShaderPasses.empty())
        {
            primary_pass = &ret.m_ShaderPasses.front();
        }

        RenderShaderPassData fallback_pass;
        if (primary_pass == nullptr)
        {
            fallback_pass = BuildLegacyRuntimeShaderPass(shader_asset);
            primary_pass = &fallback_pass;
        }

        ret.m_VertexShaderFile = primary_pass->m_VertexShaderFile;
        ret.m_FragmentShaderFile = primary_pass->m_FragmentShaderFile;
        ret.m_RenderPipeline = primary_pass->m_RenderPipeline;
        ret.m_LightMode = primary_pass->m_LightMode;
        ret.m_VertexEntry = primary_pass->m_VertexEntry;
        ret.m_FragmentEntry = primary_pass->m_FragmentEntry;
        ret.m_Cull = primary_pass->m_Cull;
        ret.m_Ztest = primary_pass->m_Ztest;
        ret.m_Blend = primary_pass->m_Blend;
        ret.m_Zwrite = primary_pass->m_Zwrite;
    }

    return ret;
}

AxisAlignedBox RenderResourceBase::GetCachedBoudingBox(const MeshSourceDesc& source) const
{
    auto find_it = m_BoundingBoxCacheMap.find(source);
    if (find_it != m_BoundingBoxCacheMap.end())
    {
        return find_it->second;
    }
    return AxisAlignedBox();
}

StaticMeshData RenderResourceBase::LoadStaticMesh(eastl::string filename, AxisAlignedBox& bounding_box)
{
    StaticMeshData mesh_data;

    tinyobj::ObjReader reader;
    tinyobj::ObjReaderConfig reader_config;
    reader_config.vertex_color = false;
    reader_config.triangulate = true;

    if (!reader.ParseFromFile(filename.c_str(), reader_config))
    {
        if (!reader.Error().empty())
        {
            LOG_ERROR(ZRender, "loadMesh {} failed, error: {}", filename.c_str(), reader.Error());
        }
        assert(0);
    }

    if (!reader.Warning().empty())
    {
        LOG_WARNING(ZRender, "loadMesh {} warning, warning: {}", filename.c_str(), reader.Warning());
    }

    auto& attrib = reader.GetAttrib();
    auto& shapes = reader.GetShapes();

    std::vector<MeshVertexDataDefinition> mesh_vertices;

    for (size_t s = 0; s < shapes.size(); s++)
    {
        size_t index_offset = 0;
        for (size_t f = 0; f < shapes[s].mesh.num_face_vertices.size(); f++)
        {
            const size_t fv = size_t(shapes[s].mesh.num_face_vertices[f]);

            bool with_normal = true;
            bool with_texcoord = true;

            std::vector<Vector3> vertices(fv);
            std::vector<Vector3> normals(fv);
            std::vector<Vector2> uvs(fv);

            // expanding vertex data is not efficient and is for testing purposes only
            for (size_t v = 0; v < fv; v++)
            {
                auto idx = shapes[s].mesh.indices[index_offset + v];
                auto vx = attrib.vertices[3 * size_t(idx.vertex_index) + 0];
                auto vy = attrib.vertices[3 * size_t(idx.vertex_index) + 1];
                auto vz = attrib.vertices[3 * size_t(idx.vertex_index) + 2];

                vertices[v].x = static_cast<float>(vx);
                vertices[v].y = static_cast<float>(vy);
                vertices[v].z = static_cast<float>(vz);

                bounding_box.Merge(Vector3(vertices[v].x, vertices[v].y, vertices[v].z));

                if (idx.normal_index >= 0)
                {
                    auto nx = attrib.normals[3 * size_t(idx.normal_index) + 0];
                    auto ny = attrib.normals[3 * size_t(idx.normal_index) + 1];
                    auto nz = attrib.normals[3 * size_t(idx.normal_index) + 2];

                    normals[v].x = static_cast<float>(nx);
                    normals[v].y = static_cast<float>(ny);
                    normals[v].z = static_cast<float>(nz);
                }
                else
                {
                    with_normal = false;
                }

                if (idx.texcoord_index >= 0)
                {
                    auto tx = attrib.texcoords[2 * size_t(idx.texcoord_index) + 0];
                    auto ty = attrib.texcoords[2 * size_t(idx.texcoord_index) + 1];

                    uvs[v].x = static_cast<float>(tx);
                    uvs[v].y = static_cast<float>(ty);
                }
                else
                {
                    with_texcoord = false;
                }
            }
            index_offset += fv;

            if (fv < 3)
            {
                continue;
            }

            for (size_t triangle_vertex_index = 1; triangle_vertex_index + 1 < fv; ++triangle_vertex_index)
            {
                const size_t triangle_indices[3] = {0, triangle_vertex_index, triangle_vertex_index + 1};

                Vector3 triangle_normal[3];
                Vector2 triangle_uv[3];
                for (size_t triangle_corner_index = 0; triangle_corner_index < 3; ++triangle_corner_index)
                {
                    const size_t source_index = triangle_indices[triangle_corner_index];
                    triangle_normal[triangle_corner_index] = normals[source_index];
                    triangle_uv[triangle_corner_index] = uvs[source_index];
                }

                if (!with_normal)
                {
                    Vector3 edge1 = vertices[triangle_indices[1]] - vertices[triangle_indices[0]];
                    Vector3 edge2 = vertices[triangle_indices[2]] - vertices[triangle_indices[0]];
                    triangle_normal[0] = edge1.crossProduct(edge2).normalisedCopy();
                    triangle_normal[1] = triangle_normal[0];
                    triangle_normal[2] = triangle_normal[0];
                }

                if (!with_texcoord)
                {
                    triangle_uv[0] = Vector2(0.5f, 0.5f);
                    triangle_uv[1] = Vector2(0.5f, 0.5f);
                    triangle_uv[2] = Vector2(0.5f, 0.5f);
                }

                Vector3 tangent {1, 0, 0};
                {
                    Vector3 edge1 = vertices[triangle_indices[1]] - vertices[triangle_indices[0]];
                    Vector3 edge2 = vertices[triangle_indices[2]] - vertices[triangle_indices[0]];
                    Vector2 deltaUV1 = triangle_uv[1] - triangle_uv[0];
                    Vector2 deltaUV2 = triangle_uv[2] - triangle_uv[0];

                    auto divide = deltaUV1.x * deltaUV2.y - deltaUV2.x * deltaUV1.y;
                    if (divide >= 0.0f && divide < 0.000001f)
                        divide = 0.000001f;
                    else if (divide < 0.0f && divide > -0.000001f)
                        divide = -0.000001f;

                    float df = 1.0f / divide;
                    tangent.x = df * (deltaUV2.y * edge1.x - deltaUV1.y * edge2.x);
                    tangent.y = df * (deltaUV2.y * edge1.y - deltaUV1.y * edge2.y);
                    tangent.z = df * (deltaUV2.y * edge1.z - deltaUV1.y * edge2.z);
                    tangent = tangent.normalisedCopy();
                }

                for (size_t triangle_corner_index = 0; triangle_corner_index < 3; ++triangle_corner_index)
                {
                    const Vector3& vertex = vertices[triangle_indices[triangle_corner_index]];

                    MeshVertexDataDefinition mesh_vert {};
                    mesh_vert.x = vertex.x;
                    mesh_vert.y = vertex.y;
                    mesh_vert.z = vertex.z;

                    mesh_vert.nx = triangle_normal[triangle_corner_index].x;
                    mesh_vert.ny = triangle_normal[triangle_corner_index].y;
                    mesh_vert.nz = triangle_normal[triangle_corner_index].z;

                    mesh_vert.u = triangle_uv[triangle_corner_index].x;
                    mesh_vert.v = triangle_uv[triangle_corner_index].y;

                    mesh_vert.tx = tangent.x;
                    mesh_vert.ty = tangent.y;
                    mesh_vert.tz = tangent.z;

                    mesh_vertices.push_back(mesh_vert);
                }
            }
        }
    }

    uint32_t stride = sizeof(MeshVertexDataDefinition);
    mesh_data.m_VertexBuffer = std::make_shared<BufferData>(mesh_vertices.size() * stride);
    mesh_data.m_IndexBuffer = std::make_shared<BufferData>(mesh_vertices.size() * sizeof(uint16_t));

    assert(mesh_vertices.size() <= std::numeric_limits<uint16_t>::max());  // take care of the index range, should be
                                                                           // consistent with the index range used by
                                                                           // vulkan

    uint16_t* indices = (uint16_t*)mesh_data.m_IndexBuffer->m_Data;
    for (size_t i = 0; i < mesh_vertices.size(); i++)
    {
        ((MeshVertexDataDefinition*)(mesh_data.m_VertexBuffer->m_Data))[i] = mesh_vertices[i];
        indices[i] = static_cast<uint16_t>(i);
    }

    return mesh_data;
}