#include "Material.h"

#include "Runtime/Core/Memory/MemoryManager.h"
#include "Runtime/Function/Render/ShaderRegistry.h"
#include "Runtime/Function/Render/Texture/Texture2D.h"
#include "Runtime/Function/ShaderLab/ShaderLabVariant.h"
#include "Runtime/Project/ProjectInfo.h"
#include "Runtime/Resource/Asset/AssetManager.h"
#include "Runtime/Resource/ResType/Data/Shader.h"
#include "core/Log/LogSystem.h"

#include <algorithm>

IMPLEMENT_REGISTER_CLASS(MaterialRes)
IMPLEMENT_OBJECT_SERAILIZE(MaterialRes)

namespace
{
    bool TryLoadShaderResFromRegistryEntry(const ShaderRegistryEntry& entry,
                                           PPtr<ShaderRes>& out_pptr,
                                           eastl::string& out_guid)
    {
        const std::shared_ptr<ProjectInfo> project_info = GET_SYSTEM(ProjectInfo);
        const std::shared_ptr<AssetManager> asset_mgr = GET_SYSTEM(AssetManager);
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
}  // namespace

template<typename TransferFunction>
void MaterialRes::Transfer(TransferFunction& transfer)
{
    transfer.Transfer(m_Shader, "shader");
    // PR-SE3a: preferred GUID reference. Stored AFTER m_Shader so that
    // old .zasset files (which only had "shader") still produce the same
    // byte layout up to that point; SafeBinaryRead returns kNotFound for
    // the missing "shader_guid" field and m_ShaderGuid stays default "".
    transfer.Transfer(m_ShaderGuid, "shader_guid");
    // PR-SE3a-migrate: primary PPtr reference. When reading an old
    // .zasset that lacks this field, SafeBinaryRead returns kNotFound and
    // the PPtr stays null; GetShaderName() falls back to m_Shader.
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

    transfer.Transfer(m_BaseColourTextureFile, "base_colour_texture_file");
    transfer.Transfer(m_MetallicRoughnessTextureFile, "metallic_roughness_texture_file");
    transfer.Transfer(m_NormalTextureFile, "normal_texture_file");
    transfer.Transfer(m_OcclusionTextureFile, "occlusion_texture_file");
    transfer.Transfer(m_EmissiveTextureFile, "emissive_texture_file");
    transfer.Transfer(m_EnabledShaderKeywords, "enabled_shader_keywords");

    // Texture cook (Phase 5) PPtr shadow references. Appended last so old
    // .zasset files (no these nodes) read back null via SafeBinaryRead.
    transfer.Transfer(m_BaseColourTexturePptr, "base_colour_texture_pptr");
    transfer.Transfer(m_MetallicRoughnessTexturePptr, "metallic_roughness_texture_pptr");
    transfer.Transfer(m_NormalTexturePptr, "normal_texture_pptr");
    transfer.Transfer(m_OcclusionTexturePptr, "occlusion_texture_pptr");
    transfer.Transfer(m_EmissiveTexturePptr, "emissive_texture_pptr");

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

namespace
{
    // Resolve a texture PPtr to its on-disk .zasset path (project-relative when
    // possible). Returns the fallback string when the PPtr is null or its
    // identity can't be recovered. The renderer hands a ".zasset" path straight
    // to LoadTexture, which loads the cooked Texture2D directly.
    eastl::string ResolveTexturePptrPath(const PPtr<Texture2D>& pptr, const eastl::string& fallback)
    {
        if (pptr.IsNull())
        {
            return fallback;
        }
        const std::shared_ptr<AssetManager> asset_mgr = GET_SYSTEM(AssetManager);
        if (asset_mgr == nullptr)
        {
            return fallback;
        }
        std::filesystem::path out_path;
        int64_t out_lfid = 0;
        if (!asset_mgr->TryGetIdentityForInstance(pptr.GetInstanceID(), out_path, out_lfid) || out_path.empty())
        {
            return fallback;
        }
        return eastl::string(out_path.generic_string().c_str());
    }
}  // namespace

eastl::string MaterialRes::GetBaseColourTextureFile() const
{
    return ResolveTexturePptrPath(m_BaseColourTexturePptr, m_BaseColourTextureFile);
}

eastl::string MaterialRes::GetMetallicRoughnessTextureFile() const
{
    return ResolveTexturePptrPath(m_MetallicRoughnessTexturePptr, m_MetallicRoughnessTextureFile);
}

eastl::string MaterialRes::GetNormalTextureFile() const
{
    return ResolveTexturePptrPath(m_NormalTexturePptr, m_NormalTextureFile);
}

eastl::string MaterialRes::GetOcclusionTextureFile() const
{
    return ResolveTexturePptrPath(m_OcclusionTexturePptr, m_OcclusionTextureFile);
}

eastl::string MaterialRes::GetEmissiveTextureFile() const
{
    return ResolveTexturePptrPath(m_EmissiveTexturePptr, m_EmissiveTextureFile);
}

eastl::string MaterialRes::GetShaderName() const
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

bool MaterialRes::IsShaderKeywordEnabled(const eastl::string& keyword) const
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

void MaterialRes::SetShaderKeywordEnabled(const eastl::string& keyword, bool enabled)
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

void MaterialRes::ClearShaderKeywords()
{
    m_EnabledShaderKeywords.clear();
}

eastl::string MaterialRes::BuildShaderVariantKeyString() const
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

void MaterialRes::SetShaderByName(const eastl::string& name)
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

    const std::shared_ptr<ProjectInfo> project_info = GET_SYSTEM(ProjectInfo);
    const std::shared_ptr<AssetManager> asset_mgr = GET_SYSTEM(AssetManager);
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

INSTANTIATE_TEMPLATE_TRANSFER_EXPORTED(MaterialRes)