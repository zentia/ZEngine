#include "Material.h"

#include "Runtime/BaseClasses/ObjectDefines.h"
#include "Runtime/BaseClasses/TypeManager.h"
#include "Runtime/Core/Memory/MemoryManager.h"
#include "Runtime/Function/Render/ShaderRegistry.h"
#include "Runtime/Function/Render/Texture/Texture2D.h"
#include "Runtime/Function/ShaderLab/ShaderLabVariant.h"
#include "Runtime/Project/ProjectInfo.h"
#include "Runtime/Resource/Asset/AssetManager.h"
#include "Runtime/Resource/ResType/Data/Shader.h"
#include "core/Log/LogSystem.h"

#include <algorithm>

IMPLEMENT_REGISTER_CLASS(Material)
IMPLEMENT_OBJECT_SERAILIZE(Material)

namespace
{
    struct MaterialLegacyTypeAliasRegistrar
    {
        MaterialLegacyTypeAliasRegistrar()
        {
            AutoTypeRegistration::AddRegisterFunction([]() {
                if (const Type* material_type = TypeOf<Material>())
                {
                    TypeManager::GetInstance().RegisterClassNameAlias("MaterialRes", material_type);
                }
            });
        }
    };

    const MaterialLegacyTypeAliasRegistrar g_material_legacy_type_alias;

    bool TryLoadShaderResFromRegistryEntry(const ShaderRegistryEntry& entry,
                                           PPtr<ShaderRes>& out_pptr,
                                           eastl::string& out_guid)
    {
        ProjectInfo* project_info = GET_SYSTEM(ProjectInfo);
        AssetManager* asset_mgr = GET_SYSTEM(AssetManager);
        if (project_info == nullptr || asset_mgr == nullptr || entry.m_ZassetRelPath.empty())
        {
            return false;
        }

        const std::filesystem::path zasset_abs =
            project_info->GetProjectRoot() / std::filesystem::path(entry.m_ZassetRelPath.c_str());
        std::error_code ec;
        if (!std::filesystem::exists(zasset_abs, ec) || ec)
        {
            return false;
        }

        std::filesystem::path read_path = zasset_abs;
        ShaderRes* shader = asset_mgr->ReadObject<ShaderRes>(read_path);
        if (shader == nullptr)
        {
            return false;
        }

        out_pptr = shader;
        out_guid = entry.m_Guid;
        return true;
    }

    void MigrateLegacyTexturePath(PPtr<Texture2D>& out_texture, const eastl::string& legacy_path)
    {
        if (!out_texture.IsNull() || legacy_path.empty())
        {
            return;
        }
        if (!Material::AssignTextureFromAssetPath(out_texture, legacy_path))
        {
            LOG_WARNING(ZRender,
                        "Material: legacy texture path '{}' could not be resolved to a Texture2D asset; "
                        "reassign in the inspector",
                        legacy_path.c_str());
        }
    }
}  // namespace

template<typename TransferFunction>
void MaterialTextureProperty::Transfer(TransferFunction& transfer)
{
    transfer.Transfer(m_Name, "name");

    if (transfer.IsReading())
    {
        eastl::string legacy_texture_file;
        transfer.Transfer(legacy_texture_file, "texture_file");
        transfer.Transfer(m_Texture, "texture_pptr");
        MigrateLegacyTexturePath(m_Texture, legacy_texture_file);
    }
    else
    {
        transfer.Transfer(m_Texture, "texture_pptr");
    }
}

template<typename TransferFunction>
void Material::Transfer(TransferFunction& transfer)
{
    transfer.Transfer(m_Shader, "shader");
    transfer.Transfer(m_ShaderGuid, "shader_guid");
    transfer.Transfer(m_ShaderPptr, "m_shader_pptr");
    transfer.Transfer(m_FloatProperties, "float_properties");
    transfer.Transfer(m_ColorProperties, "color_properties");
    transfer.Transfer(m_TextureProperties, "texture_properties");
    transfer.Transfer(m_ToggleProperties, "toggle_properties");

    transfer.Transfer(m_BaseColorFactor, "base_color_factor");
    transfer.Transfer(m_AlphaFactor, "alpha_factor");
    transfer.Transfer(m_MetallicFactor, "metallic_factor");
    transfer.Transfer(m_RoughnessFactor, "roughness_factor");
    transfer.Transfer(m_NormalScale, "normal_scale");
    transfer.Transfer(m_OcclusionStrength, "occlusion_strength");
    transfer.Transfer(m_EmissiveFactor, "emissive_factor");
    transfer.Transfer(m_IsBlend, "is_blend");
    transfer.Transfer(m_IsDoubleSided, "is_double_sided");

    if (transfer.IsReading())
    {
        eastl::string legacy_base;
        eastl::string legacy_mr;
        eastl::string legacy_normal;
        eastl::string legacy_occlusion;
        eastl::string legacy_emissive;
        transfer.Transfer(legacy_base, "base_colour_texture_file");
        transfer.Transfer(legacy_mr, "metallic_roughness_texture_file");
        transfer.Transfer(legacy_normal, "normal_texture_file");
        transfer.Transfer(legacy_occlusion, "occlusion_texture_file");
        transfer.Transfer(legacy_emissive, "emissive_texture_file");
        transfer.Transfer(m_EnabledShaderKeywords, "enabled_shader_keywords");

        transfer.Transfer(m_BaseColourTexturePptr, "base_colour_texture_pptr");
        transfer.Transfer(m_MetallicRoughnessTexturePptr, "metallic_roughness_texture_pptr");
        transfer.Transfer(m_NormalTexturePptr, "normal_texture_pptr");
        transfer.Transfer(m_OcclusionTexturePptr, "occlusion_texture_pptr");
        transfer.Transfer(m_EmissiveTexturePptr, "emissive_texture_pptr");

        MigrateLegacyTexturePath(m_BaseColourTexturePptr, legacy_base);
        MigrateLegacyTexturePath(m_MetallicRoughnessTexturePptr, legacy_mr);
        MigrateLegacyTexturePath(m_NormalTexturePptr, legacy_normal);
        MigrateLegacyTexturePath(m_OcclusionTexturePptr, legacy_occlusion);
        MigrateLegacyTexturePath(m_EmissiveTexturePptr, legacy_emissive);
    }
    else
    {
        transfer.Transfer(m_EnabledShaderKeywords, "enabled_shader_keywords");
        transfer.Transfer(m_BaseColourTexturePptr, "base_colour_texture_pptr");
        transfer.Transfer(m_MetallicRoughnessTexturePptr, "metallic_roughness_texture_pptr");
        transfer.Transfer(m_NormalTexturePptr, "normal_texture_pptr");
        transfer.Transfer(m_OcclusionTexturePptr, "occlusion_texture_pptr");
        transfer.Transfer(m_EmissiveTexturePptr, "emissive_texture_pptr");
    }

    if (!transfer.IsReading())
    {
        return;
    }

    if (m_ShaderPptr.IsNull() && !m_ShaderGuid.empty())
    {
        if (auto registry = GET_SYSTEM(ShaderRegistry))
        {
            if (const ShaderRegistryEntry* entry = registry->FindByGuid(m_ShaderGuid))
            {
                if (!m_Shader.empty() && !entry->m_ShaderName.empty() && entry->m_ShaderName != m_Shader)
                {
                    m_ShaderGuid.clear();
                }
                else
                {
                    PPtr<ShaderRes> pptr;
                    eastl::string guid;
                    if (TryLoadShaderResFromRegistryEntry(*entry, pptr, guid))
                    {
                        m_ShaderPptr = pptr;
                        if (m_Shader.empty() && !entry->m_ShaderName.empty())
                        {
                            m_Shader = entry->m_ShaderName;
                        }
                    }
                }
            }
            else
            {
                m_ShaderGuid.clear();
            }
        }
    }

    if (!m_ShaderPptr.IsNull() && !m_Shader.empty())
    {
        if (ShaderRes* shader = m_ShaderPptr)
        {
            if (shader != nullptr && !shader->m_ShaderName.empty() && shader->m_ShaderName != m_Shader)
            {
                m_ShaderPptr = PPtr<ShaderRes>();
                m_ShaderGuid.clear();
            }
        }
    }
}

eastl::string Material::ResolveTextureAssetPath(const PPtr<Texture2D>& texture)
{
    if (texture.IsNull())
    {
        return {};
    }
    AssetManager* asset_mgr = GET_SYSTEM(AssetManager);
    if (asset_mgr == nullptr)
    {
        return {};
    }
    std::filesystem::path out_path;
    int64_t out_lfid = 0;
    if (!asset_mgr->TryGetIdentityForInstance(texture.GetInstanceID(), out_path, out_lfid) || out_path.empty())
    {
        return {};
    }
    return eastl::string(out_path.generic_string().c_str());
}

bool Material::AssignTextureFromAssetPath(PPtr<Texture2D>& out_texture, const eastl::string& asset_path)
{
    out_texture = PPtr<Texture2D>();
    if (asset_path.empty())
    {
        return true;
    }

    AssetManager* asset_mgr = GET_SYSTEM(AssetManager);
    if (asset_mgr == nullptr)
    {
        return false;
    }

    std::string path_lower = asset_path.c_str();
    std::transform(path_lower.begin(), path_lower.end(), path_lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (path_lower.size() >= 7 && path_lower.compare(path_lower.size() - 7, 7, ".zasset") == 0)
    {
        if (Texture2D* texture = asset_mgr->loadAsset<Texture2D>(asset_path))
        {
            out_texture = texture;
            return true;
        }
        return false;
    }

    // Legacy migration: source image path -> sibling/imported Texture2D .zasset under Assets/.
    std::filesystem::path source_path(asset_path.c_str());
    std::filesystem::path zasset_candidate = source_path;
    zasset_candidate.replace_extension(".zasset");
    if (Texture2D* texture = asset_mgr->loadAsset<Texture2D>(zasset_candidate.generic_string().c_str()))
    {
        out_texture = texture;
        return true;
    }

    ProjectInfo* project_info = GET_SYSTEM(ProjectInfo);
    if (project_info != nullptr)
    {
        const std::filesystem::path content_root = project_info->GetProjectContent();
        if (!content_root.empty())
        {
            std::error_code ec;
            const std::filesystem::path rel = std::filesystem::relative(source_path, content_root, ec);
            if (!ec && !rel.empty())
            {
                std::filesystem::path under_assets = content_root / rel;
                under_assets.replace_extension(".zasset");
                if (Texture2D* texture =
                        asset_mgr->loadAsset<Texture2D>(under_assets.generic_string().c_str()))
                {
                    out_texture = texture;
                    return true;
                }
            }
        }
    }

    return false;
}

eastl::string Material::GetBaseColourTextureFile() const
{
    return ResolveTextureAssetPath(m_BaseColourTexturePptr);
}

eastl::string Material::GetMetallicRoughnessTextureFile() const
{
    return ResolveTextureAssetPath(m_MetallicRoughnessTexturePptr);
}

eastl::string Material::GetNormalTextureFile() const
{
    return ResolveTextureAssetPath(m_NormalTexturePptr);
}

eastl::string Material::GetOcclusionTextureFile() const
{
    return ResolveTextureAssetPath(m_OcclusionTexturePptr);
}

eastl::string Material::GetEmissiveTextureFile() const
{
    return ResolveTextureAssetPath(m_EmissiveTexturePptr);
}

eastl::string Material::GetShaderName() const
{
    if (!m_ShaderPptr.IsNull())
    {
        ShaderRes* shader = m_ShaderPptr;
        if (shader != nullptr && !shader->m_ShaderName.empty())
        {
            return shader->m_ShaderName;
        }
    }

    if (!m_ShaderGuid.empty())
    {
        if (auto registry = GET_SYSTEM(ShaderRegistry))
        {
            if (const ShaderRegistryEntry* entry = registry->FindByGuid(m_ShaderGuid))
            {
                if (!entry->m_ShaderName.empty())
                {
                    return entry->m_ShaderName;
                }
            }
        }
    }

    return m_Shader;
}

bool Material::IsShaderKeywordEnabled(const eastl::string& keyword) const
{
    if (keyword.empty())
    {
        return false;
    }
    for (const eastl::string& enabled : m_EnabledShaderKeywords)
    {
        if (enabled == keyword)
        {
            return true;
        }
    }
    return false;
}

void Material::SetShaderKeywordEnabled(const eastl::string& keyword, bool enabled)
{
    if (keyword.empty())
    {
        return;
    }

    auto it = std::find(m_EnabledShaderKeywords.begin(), m_EnabledShaderKeywords.end(), keyword);
    if (enabled)
    {
        if (it == m_EnabledShaderKeywords.end())
        {
            m_EnabledShaderKeywords.push_back(keyword);
        }
    }
    else if (it != m_EnabledShaderKeywords.end())
    {
        m_EnabledShaderKeywords.erase(it);
    }
}

void Material::ClearShaderKeywords()
{
    m_EnabledShaderKeywords.clear();
}

eastl::string Material::BuildShaderVariantKeyString() const
{
    if (m_EnabledShaderKeywords.empty())
    {
        return eastl::string {};
    }

    ZEngine::ShaderLab::ShaderVariantKey key;
    for (const eastl::string& keyword : m_EnabledShaderKeywords)
    {
        if (!keyword.empty())
        {
            key[keyword.c_str()] = "1";
        }
    }
    return ZEngine::ShaderLab::StringifyVariantKey(key).c_str();
}

void Material::SetShaderByName(const eastl::string& name)
{
    m_Shader = name;
    m_ShaderGuid.clear();
    m_ShaderPptr = PPtr<ShaderRes>();
    ClearShaderKeywords();

    if (name.empty() || name == "StandardLit")
    {
        return;
    }

    if (auto registry = GET_SYSTEM(ShaderRegistry))
    {
        if (ShaderRegistryEntry* entry = registry->FindByName(name))
        {
            PPtr<ShaderRes> pptr;
            eastl::string guid;
            if (TryLoadShaderResFromRegistryEntry(*entry, pptr, guid))
            {
                m_ShaderPptr = pptr;
                m_ShaderGuid = guid;
                if (!entry->m_ShaderName.empty())
                {
                    m_Shader = entry->m_ShaderName;
                }
                return;
            }
        }
    }

    ProjectInfo* project_info = GET_SYSTEM(ProjectInfo);
    AssetManager* asset_mgr = GET_SYSTEM(AssetManager);
    if (project_info == nullptr || asset_mgr == nullptr)
    {
        return;
    }

    const std::filesystem::path asset_root = project_info->GetProjectContent();
    for (const std::filesystem::path& candidate : asset_mgr->GetAssetsByType("ShaderRes", asset_root))
    {
        const std::string candidate_path = candidate.generic_string();
        if (candidate_path.find("_Generated") != std::string::npos)
        {
            continue;
        }

        std::filesystem::path read_path = candidate;
        ShaderRes* shader = asset_mgr->ReadObject<ShaderRes>(read_path);
        if (shader == nullptr)
        {
            continue;
        }

        const eastl::string candidate_name = shader->m_ShaderName.empty()
                                                 ? candidate.stem().generic_string().c_str()
                                                 : shader->m_ShaderName;
        const bool name_matches = candidate_name == name;
        const bool stem_matches =
            name.find('/') == eastl::string::npos && candidate.stem().generic_string() == name.c_str();
        if (name_matches || stem_matches)
        {
            m_ShaderPptr = shader;
            if (auto registry = GET_SYSTEM(ShaderRegistry))
            {
                if (ShaderRegistryEntry* entry = registry->FindByName(name))
                {
                    m_ShaderGuid = entry->m_Guid;
                }
            }
            return;
        }
    }
}

INSTANTIATE_TEMPLATE_TRANSFER_EXPORTED(Material)
