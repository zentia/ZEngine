#include "InspectorAssetCommon.h"

#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Project/ProjectInfo.h"
#include "Runtime/Resource/Asset/AssetManager.h"
#include "Runtime/Resource/ResType/Data/Material.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <memory>
#include <system_error>

namespace
{
    std::filesystem::path GetProjectAssetsPath()
    {
        const std::shared_ptr<ProjectInfo> project_info = GET_SYSTEM(ProjectInfo);
        if (project_info == nullptr)
        {
            return {};
        }
        return project_info->GetProjectContent();
    }

    std::string NormalizeInspectorAssetType(std::string asset_type)
    {
        std::transform(asset_type.begin(),
                       asset_type.end(),
                       asset_type.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (asset_type.size() > 3 && asset_type.compare(asset_type.size() - 3, 3, "res") == 0)
        {
            asset_type.erase(asset_type.size() - 3);
        }
        return asset_type;
    }

    void CopyMaterialAssetForInspector(MaterialRes& destination, const MaterialRes& source)
    {
        destination.m_Shader = source.m_Shader;
        destination.m_ShaderGuid = source.m_ShaderGuid;
        destination.m_ShaderPptr = source.m_ShaderPptr;
        destination.m_FloatProperties = source.m_FloatProperties;
        destination.m_ColorProperties = source.m_ColorProperties;
        destination.m_TextureProperties = source.m_TextureProperties;
        destination.m_ToggleProperties = source.m_ToggleProperties;
        destination.m_BaseColorFactor = source.m_BaseColorFactor;
        destination.m_AlphaFactor = source.m_AlphaFactor;
        destination.m_MetallicFactor = source.m_MetallicFactor;
        destination.m_RoughnessFactor = source.m_RoughnessFactor;
        destination.m_NormalScale = source.m_NormalScale;
        destination.m_OcclusionStrength = source.m_OcclusionStrength;
        destination.m_EmissiveFactor = source.m_EmissiveFactor;
        destination.m_IsBlend = source.m_IsBlend;
        destination.m_IsDoubleSided = source.m_IsDoubleSided;
        destination.m_BaseColourTextureFile = source.m_BaseColourTextureFile;
        destination.m_MetallicRoughnessTextureFile = source.m_MetallicRoughnessTextureFile;
        destination.m_NormalTextureFile = source.m_NormalTextureFile;
        destination.m_OcclusionTextureFile = source.m_OcclusionTextureFile;
        destination.m_EmissiveTextureFile = source.m_EmissiveTextureFile;
        destination.m_EnabledShaderKeywords = source.m_EnabledShaderKeywords;
    }
}  // namespace

std::string ResolveInspectorAssetType(const std::filesystem::path& asset_path, const std::string& selected_asset_type)
{
    std::string resolved_type = NormalizeInspectorAssetType(selected_asset_type);

    std::string extension = asset_path.extension().string();
    std::transform(extension.begin(),
                   extension.end(),
                   extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (extension == ".shader")
    {
        return "shader";
    }
    if (extension == ".zasset")
    {
        const std::string runtime_asset_type =
            NormalizeInspectorAssetType(GET_SYSTEM(AssetManager)->GetAssetTypeName(asset_path));
        if (!runtime_asset_type.empty())
        {
            resolved_type = runtime_asset_type;
        }
    }

    return resolved_type;
}

bool IsTexture2DInspectorAssetType(const std::string& resolved_asset_type)
{
    // resolved_asset_type is already lowercased + "Res"-stripped by ResolveInspectorAssetType.
    // Texture2D has no "Res" suffix, so the token is a flat "texture2d".
    return resolved_asset_type == "texture2d";
}

bool IsGenericInspectorZAssetType(const std::filesystem::path& asset_path, const std::string& resolved_asset_type)
{
    std::string extension = asset_path.extension().string();
    std::transform(extension.begin(),
                   extension.end(),
                   extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension == ".zasset" &&
           (resolved_asset_type.empty() || resolved_asset_type == "zasset" || resolved_asset_type == "asset");
}

bool LoadMaterialDefinitionForInspector(MaterialRes& out_material, const std::filesystem::path& asset_path)
{
    if (asset_path.empty() || !std::filesystem::exists(asset_path))
    {
        return false;
    }

    std::filesystem::path read_path = asset_path;
    MaterialRes* loaded = GET_SYSTEM(AssetManager)->ReadObject<MaterialRes>(read_path);
    if (loaded == nullptr)
    {
        return false;
    }

    CopyMaterialAssetForInspector(out_material, *loaded);
    out_material.InitializeRuntimeTypeInfo();
    return true;
}

bool MaterialUsesCustomProjectShader(const MaterialRes& material)
{
    const eastl::string shader_name = !material.m_Shader.empty() ? material.m_Shader : material.GetShaderName();
    return !shader_name.empty() && shader_name != "StandardLit";
}

std::filesystem::path ResolveProjectAssetPath(const eastl::string& stored_path)
{
    if (stored_path.empty())
    {
        return {};
    }

    std::filesystem::path path(stored_path.c_str());
    if (path.is_absolute())
    {
        return path.lexically_normal();
    }
    return (GetProjectAssetsPath() / path).lexically_normal();
}

std::filesystem::file_time_type GetInspectorFileWriteTime(const std::filesystem::path& path)
{
    if (path.empty())
    {
        return std::filesystem::file_time_type::min();
    }

    std::error_code error_code;
    if (!std::filesystem::exists(path, error_code) || error_code)
    {
        return std::filesystem::file_time_type::min();
    }

    const std::filesystem::file_time_type write_time = std::filesystem::last_write_time(path, error_code);
    return error_code ? std::filesystem::file_time_type::min() : write_time;
}
