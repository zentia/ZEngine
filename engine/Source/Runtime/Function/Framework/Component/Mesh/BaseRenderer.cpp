#include "BaseRenderer.h"

#include "Runtime/BaseClasses/GameObject.h"
#include "Runtime/Function/Framework/Component/Transform/TransformComponent.h"
#include "Runtime/Function/Render/RenderSwapContext.h"
#include "Runtime/Function/Render/RenderSystem.h"
#include "Runtime/Resource/Asset/AssetManager.h"
#include "Runtime/Resource/ResType/Data/Material.h"

IMPLEMENT_REGISTER_CLASS(BaseRenderer)
IMPLEMENT_OBJECT_SERIALIZE(BaseRenderer)

template<typename TransferFunction>
void BaseRenderer::Transfer(TransferFunction& transfer)
{
    Super::Transfer(transfer);
    transfer.Transfer(m_SubMeshes, "sub_meshes");
    transfer.Transfer(m_SharedMaterialAssets, "shared_materials");
}
INSTANTIATE_TEMPLATE_TRANSFER_EXPORTED(BaseRenderer)

namespace
{
    bool shouldLogMaterialDebug(const eastl::string& material_asset)
    {
        return material_asset.find("white.material.json") != eastl::string::npos;
    }

    eastl::string resolveAssetPath(const eastl::string& asset_path)
    {
        if (asset_path.empty())
        {
            return "";
        }
        return GET_SYSTEM(AssetManager)->GetFullPath(asset_path).generic_string().c_str();
    }

    void populateMaterialDesc(const eastl::string& material_asset, GameObjectMaterialDesc& material_desc)
    {
        material_desc = {};
        material_desc.m_Shader = "StandardLit";
        material_desc.m_MaterialAsset = material_asset;
        material_desc.m_WithTexture = !material_asset.empty();
        if (!material_desc.m_WithTexture)
        {
            // Imported meshes often need double-sided until we validate winding per asset.
            material_desc.m_IsDoubleSided = true;
            return;
        }

        const std::filesystem::path material_path = GET_SYSTEM(AssetManager)->GetFullPath(material_asset);
        const std::string material_path_string = material_path.generic_string();
        Material* material_res = GET_SYSTEM(AssetManager)->loadAsset<Material>(material_asset);
        if (material_res == nullptr)
        {
            LOG_WARNING(ZRender,
                        "populateMaterialDesc failed to load material asset='{}' full='{}'; fallback to built-in default textures",
                        material_asset.c_str(),
                        material_path_string.c_str());
            material_desc.m_WithTexture = false;
            material_desc.m_IsDoubleSided = true;
            return;
        }

        if (shouldLogMaterialDebug(material_asset))
        {
            LOG_INFO(ZRender,
                     "populateMaterialDesc loaded material asset='{}' full='{}' shader='{}' blend={} double_sided={}",
                     material_asset.c_str(),
                     material_path_string.c_str(),
                     material_res->GetShaderName().c_str(),
                     material_res->m_IsBlend,
                     material_res->m_IsDoubleSided);
        }

        material_desc.m_Shader = material_res->GetShaderName();
        material_desc.m_BaseColorFactor = Vector4(material_res->m_BaseColorFactor, material_res->m_AlphaFactor);
        material_desc.m_MetallicFactor = material_res->m_MetallicFactor;
        material_desc.m_RoughnessFactor = material_res->m_RoughnessFactor;
        material_desc.m_NormalScale = material_res->m_NormalScale;
        material_desc.m_OcclusionStrength = material_res->m_OcclusionStrength;
        material_desc.m_EmissiveFactor = material_res->m_EmissiveFactor;
        material_desc.m_IsBlend = material_res->m_IsBlend;
        material_desc.m_IsDoubleSided = material_res->m_IsDoubleSided;
        material_desc.m_BaseColorTextureFile = resolveAssetPath(material_res->GetBaseColourTextureFile());
        material_desc.m_MetallicRoughnessTextureFile = resolveAssetPath(material_res->GetMetallicRoughnessTextureFile());
        material_desc.m_NormalTextureFile = resolveAssetPath(material_res->GetNormalTextureFile());
        material_desc.m_OcclusionTextureFile = resolveAssetPath(material_res->GetOcclusionTextureFile());
        material_desc.m_EmissiveTextureFile = resolveAssetPath(material_res->GetEmissiveTextureFile());
        material_desc.m_EnabledShaderKeywords.clear();
        material_desc.m_EnabledShaderKeywords.reserve(material_res->m_EnabledShaderKeywords.size());
        for (const eastl::string& keyword : material_res->m_EnabledShaderKeywords)
        {
            material_desc.m_EnabledShaderKeywords.push_back(keyword);
        }
    }
}  // namespace

void BaseRenderer::SetSubMeshes(const std::vector<SubMeshRes>& sub_meshes)
{
    m_SubMeshes = sub_meshes;
    MigrateLegacyMaterialAssets();

    if (m_ParentObject != nullptr)
    {
        SubmitRenderState();
    }
}

void BaseRenderer::SetSharedMaterialAssets(const std::vector<eastl::string>& shared_material_assets)
{
    m_SharedMaterialAssets = shared_material_assets;

    if (m_ParentObject != nullptr)
    {
        SubmitRenderState();
    }
}

eastl::string BaseRenderer::ResolveMaterialAsset(size_t sub_mesh_index) const
{
    if (sub_mesh_index < m_SharedMaterialAssets.size())
    {
        return m_SharedMaterialAssets[sub_mesh_index];
    }

    if (m_SharedMaterialAssets.size() == 1)
    {
        return m_SharedMaterialAssets.front();
    }

    return "";
}

void BaseRenderer::MigrateLegacyMaterialAssets()
{
    if (!m_SharedMaterialAssets.empty())
    {
        return;
    }

    bool has_legacy_material = false;
    std::vector<eastl::string> migrated_material_assets;
    migrated_material_assets.reserve(m_SubMeshes.size());

    for (const SubMeshRes& sub_mesh : m_SubMeshes)
    {
        migrated_material_assets.push_back(sub_mesh.m_LegacyMaterialAsset);
        has_legacy_material = has_legacy_material || !sub_mesh.m_LegacyMaterialAsset.empty();
    }

    if (!has_legacy_material)
    {
        return;
    }

    m_SharedMaterialAssets = migrated_material_assets;
    for (SubMeshRes& sub_mesh : m_SubMeshes)
    {
        sub_mesh.m_LegacyMaterialAsset.clear();
    }
}

void BaseRenderer::PostLoadResource(GameObject* parent_object)
{
    Component::PostLoadResource(parent_object);
    MigrateLegacyMaterialAssets();
    SubmitRenderState();
}

void BaseRenderer::OnSerializedFieldsUpdated()
{
    MigrateLegacyMaterialAssets();

    if (m_ParentObject != nullptr)
    {
        SubmitRenderState();
    }
}

std::vector<GameObjectPartDesc> BaseRenderer::BuildRenderParts(const TransformComponent* transform_component) const
{
    const Matrix4x4 object_transform = transform_component != nullptr ? transform_component->getMatrix() : Matrix4x4::IDENTITY;

    std::vector<GameObjectPartDesc> render_parts;
    render_parts.reserve(m_SubMeshes.size());

    for (size_t sub_mesh_index = 0; sub_mesh_index < m_SubMeshes.size(); ++sub_mesh_index)
    {
        const SubMeshRes& sub_mesh = m_SubMeshes[sub_mesh_index];

        GameObjectPartDesc render_part;
        render_part.m_MeshDesc.m_MeshAsset = sub_mesh.m_MeshAsset;
        populateMaterialDesc(ResolveMaterialAsset(sub_mesh_index), render_part.m_MaterialDesc);
        if (m_ForceDoubleSided)
        {
            render_part.m_MaterialDesc.m_IsDoubleSided = true;
        }
        render_part.m_TransformDesc.m_TransformMatrix = object_transform * sub_mesh.m_Transform.getMatrix();
        render_parts.push_back(render_part);
    }

    return render_parts;
}

GameObjectDesc BaseRenderer::BuildGameObjectDescFromParts(const std::vector<GameObjectPartDesc>& render_parts) const
{
    return GameObjectDesc {m_ParentObject != nullptr ? m_ParentObject->GetID() : k_invalid_gobject_id, render_parts};
}

GameObjectDesc BaseRenderer::BuildGameObjectDesc(const TransformComponent* transform_component) const
{
    return BuildGameObjectDescFromParts(BuildRenderParts(transform_component));
}

void BaseRenderer::SubmitRenderState() const
{
    if (m_ParentObject == nullptr)
    {
        return;
    }

    const TransformComponent* transform_component = m_ParentObject->tryGetComponent(TransformComponent);

    RenderSwapContext& render_swap_context = GET_SYSTEM(RenderSystem)->GetSwapContext();
    render_swap_context.GetLogicSwapData().AddDirtyGameObject(BuildGameObjectDesc(transform_component));
}

void BaseRenderer::Tick(float delta_time)
{
    (void)delta_time;

    if (m_ParentObject == nullptr)
    {
        return;
    }

    TransformComponent* transform_component = m_ParentObject->tryGetComponent(TransformComponent);
    if (transform_component == nullptr)
    {
        return;
    }

    if (!transform_component->IsDirty())
    {
        return;
    }

    SubmitRenderState();
    transform_component->setDirtyFlag(false);
}
