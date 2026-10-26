#include "BaseRenderer.h"

#include "Runtime/Core/Math/LargeWorldCoordinates.h"
#include "Runtime/BaseClasses/GameObject.h"
#include "Runtime/Function/Framework/Component/Transform/Transform.h"
#include "Runtime/Function/Framework/Component/Transform/TransformChangeDispatch.h"
#include "Runtime/Function/Render/RenderSwapContext.h"
#include "Runtime/Function/Render/RenderSystem.h"
#include "Runtime/Resource/Asset/AssetManager.h"
#include "Runtime/Resource/ResType/Data/Material.h"



namespace
{
    void EnsureRendererTransformDispatchRegistered()
    {
        static bool registered = false;
        if (registered)
        {
            return;
        }
        registered = true;

        GetTransformChangeDispatch().RegisterBatchCallback(
            Transform::GetRendererChangeSystem(),
            [](const TransformChange* changes, size_t count)
            {
                for (size_t i = 0; i < count; ++i)
                {
                    Transform* transform_component = changes[i].access.hierarchy->transform_pointers[changes[i].access.index];
                    if (transform_component == nullptr || transform_component->GetParentObject() == nullptr)
                    {
                        continue;
                    }

                    BaseRenderer* renderer = transform_component->GetParentObject()->tryGetComponent(BaseRenderer);
                    if (renderer != nullptr)
                    {
                        renderer->RefreshRenderStateFromTransformDispatch();
                        transform_component->setDirtyFlag(false);
                    }
                }
            });
    }

    /// 把 override 套到已经从资产 populate 过的 desc 上。
    ///
    /// 只动 `has_*` 为 true 的字段 —— 其余保持资产值。这也是为什么 override 结构体
    /// 里不必存资产的副本：populate 每次都重跑，资产改了立刻生效。
    void applyMaterialInstanceOverride(const MaterialInstanceOverride& override_data,
                                       GameObjectMaterialDesc& material_desc)
    {
        if (override_data.has_base_color)
        {
            material_desc.m_BaseColorFactor = override_data.base_color;
        }
        if (override_data.has_emissive)
        {
            material_desc.m_EmissiveFactor = override_data.emissive;
        }
        if (override_data.has_metallic)
        {
            material_desc.m_MetallicFactor = override_data.metallic;
        }
        if (override_data.has_roughness)
        {
            material_desc.m_RoughnessFactor = override_data.roughness;
        }
        if (override_data.has_normal_scale)
        {
            material_desc.m_NormalScale = override_data.normal_scale;
        }
        if (override_data.has_occlusion)
        {
            material_desc.m_OcclusionStrength = override_data.occlusion;
        }
        if (override_data.has_blend)
        {
            material_desc.m_IsBlend = override_data.blend;
        }
        if (override_data.has_double_sided)
        {
            material_desc.m_IsDoubleSided = override_data.double_sided;
        }
    }

    /// m_LastMaterialDescs 的安全下标。越界返回 nullptr（getter 走默认值分支）。
    const GameObjectMaterialDesc* cachedMaterialDescAt(const std::vector<GameObjectMaterialDesc>& descs, int slot)
    {
        if (slot < 0 || static_cast<size_t>(slot) >= descs.size())
        {
            return nullptr;
        }
        return &descs[static_cast<size_t>(slot)];
    }

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
    SyncMaterialOverrideCount();

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

void BaseRenderer::RefreshRenderStateFromTransformDispatch() const
{
    SubmitRenderState();
}

void BaseRenderer::PostLoadResource(GameObject* parent_object)
{
    Component::PostLoadResource(parent_object);
    MigrateLegacyMaterialAssets();
    SyncMaterialOverrideCount();

    EnsureRendererTransformDispatchRegistered();
    if (Transform* transform_component = parent_object != nullptr ? parent_object->tryGetComponent(Transform) : nullptr)
    {
        transform_component->EnsureTransformHierarchyExists();
        transform_component->SetDispatchInterested(Transform::GetRendererChangeSystem(), true);
    }

    SubmitRenderState();
}

void BaseRenderer::OnSerializedFieldsUpdated()
{
    MigrateLegacyMaterialAssets();
    SyncMaterialOverrideCount();

    if (m_ParentObject != nullptr)
    {
        SubmitRenderState();
    }
}

std::vector<GameObjectPartDesc> BaseRenderer::BuildRenderParts(const Transform* transform_component) const
{
    Matrix4x4 object_transform =
        transform_component != nullptr ? transform_component->GetLocalToWorldMatrix() : Matrix4x4::IDENTITY;
    object_transform = LargeWorldCoordinates::ApplyRenderOriginToModelMatrix(object_transform);

    std::vector<GameObjectPartDesc> render_parts;
    render_parts.reserve(m_SubMeshes.size());

    // getter 用的一份缓存：BuildRenderParts 是 const 的，但"当前在画的值"只有这里
    // 才算得出来（资产值要 loadAsset），所以缓存必须在这里刷。
    m_LastMaterialDescs.clear();
    m_LastMaterialDescs.reserve(m_SubMeshes.size());

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

        // override 套在 populate **之后**：没被覆盖的字段保持资产值，改资产立刻生效。
        if (sub_mesh_index < m_MaterialOverrides.size())
        {
            const MaterialInstanceOverride& slot_override = m_MaterialOverrides[sub_mesh_index];
            applyMaterialInstanceOverride(slot_override, render_part.m_MaterialDesc);
            render_part.m_MaterialDesc.m_UseMaterialInstance = slot_override.IsActive();
        }

        render_part.m_TransformDesc.m_TransformMatrix = object_transform * sub_mesh.m_Transform.getMatrix();
        m_LastMaterialDescs.push_back(render_part.m_MaterialDesc);
        render_parts.push_back(render_part);
    }

    return render_parts;
}

GameObjectDesc BaseRenderer::BuildGameObjectDescFromParts(const std::vector<GameObjectPartDesc>& render_parts) const
{
    return GameObjectDesc {m_ParentObject != nullptr ? m_ParentObject->GetID() : k_invalid_gobject_id, render_parts};
}

GameObjectDesc BaseRenderer::BuildGameObjectDesc(const Transform* transform_component) const
{
    return BuildGameObjectDescFromParts(BuildRenderParts(transform_component));
}

void BaseRenderer::SubmitRenderState() const
{
    if (m_ParentObject == nullptr)
    {
        return;
    }

    const Transform* transform_component = m_ParentObject->tryGetComponent(Transform);

    RenderSwapContext& render_swap_context = GET_SYSTEM(RenderSystem)->GetSwapContext();
    const bool object_active = transform_component == nullptr || transform_component->isEnabled();
    if (!isEnabled() || !object_active)
    {
        // Empty parts remove stale render entities for this GameObject.
        render_swap_context.GetLogicSwapData().AddDirtyGameObject(
            GameObjectDesc {m_ParentObject->GetID(), {}});
        return;
    }

    render_swap_context.GetLogicSwapData().AddDirtyGameObject(BuildGameObjectDesc(transform_component));
}

void BaseRenderer::Tick(float delta_time)
{
    (void)delta_time;

    if (m_ParentObject == nullptr)
    {
        return;
    }

    Transform* transform_component = m_ParentObject->tryGetComponent(Transform);
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

// ---- 运行时材质实例 --------------------------------------------------------

void BaseRenderer::SyncMaterialOverrideCount()
{
    // resize 而不是 assign：缩减 sub-mesh 再扩回来时，中间那几帧不该把 override 丢掉，
    // 而超出新长度的槽位自然被裁掉。
    m_MaterialOverrides.resize(m_SubMeshes.size());
}

MaterialInstanceOverride* BaseRenderer::MutableOverrideAt(int slot)
{
    if (slot < 0 || static_cast<size_t>(slot) >= m_MaterialOverrides.size())
    {
        return nullptr;
    }
    return &m_MaterialOverrides[static_cast<size_t>(slot)];
}

const MaterialInstanceOverride* BaseRenderer::OverrideAt(int slot) const
{
    if (slot < 0 || static_cast<size_t>(slot) >= m_MaterialOverrides.size())
    {
        return nullptr;
    }
    return &m_MaterialOverrides[static_cast<size_t>(slot)];
}

void BaseRenderer::SetBaseColor(int slot, const Vector4& color)
{
    MaterialInstanceOverride* slot_override = MutableOverrideAt(slot);
    if (slot_override == nullptr)
    {
        return;
    }
    slot_override->has_base_color = true;
    slot_override->base_color = color;
    SubmitRenderState();
}

Vector4 BaseRenderer::GetBaseColor(int slot) const
{
    const MaterialInstanceOverride* slot_override = OverrideAt(slot);
    if (slot_override != nullptr && slot_override->has_base_color)
    {
        return slot_override->base_color;
    }
    const GameObjectMaterialDesc* desc = cachedMaterialDescAt(m_LastMaterialDescs, slot);
    return desc != nullptr ? desc->m_BaseColorFactor : Vector4(1.f, 1.f, 1.f, 1.f);
}

void BaseRenderer::SetEmissive(int slot, const Vector3& emissive)
{
    MaterialInstanceOverride* slot_override = MutableOverrideAt(slot);
    if (slot_override == nullptr)
    {
        return;
    }
    slot_override->has_emissive = true;
    slot_override->emissive = emissive;
    SubmitRenderState();
}

Vector3 BaseRenderer::GetEmissive(int slot) const
{
    const MaterialInstanceOverride* slot_override = OverrideAt(slot);
    if (slot_override != nullptr && slot_override->has_emissive)
    {
        return slot_override->emissive;
    }
    const GameObjectMaterialDesc* desc = cachedMaterialDescAt(m_LastMaterialDescs, slot);
    return desc != nullptr ? desc->m_EmissiveFactor : Vector3(0.f, 0.f, 0.f);
}

void BaseRenderer::SetMetallic(int slot, float metallic)
{
    MaterialInstanceOverride* slot_override = MutableOverrideAt(slot);
    if (slot_override == nullptr)
    {
        return;
    }
    slot_override->has_metallic = true;
    slot_override->metallic = metallic;
    SubmitRenderState();
}

float BaseRenderer::GetMetallic(int slot) const
{
    const MaterialInstanceOverride* slot_override = OverrideAt(slot);
    if (slot_override != nullptr && slot_override->has_metallic)
    {
        return slot_override->metallic;
    }
    const GameObjectMaterialDesc* desc = cachedMaterialDescAt(m_LastMaterialDescs, slot);
    return desc != nullptr ? desc->m_MetallicFactor : 1.f;
}

void BaseRenderer::SetRoughness(int slot, float roughness)
{
    MaterialInstanceOverride* slot_override = MutableOverrideAt(slot);
    if (slot_override == nullptr)
    {
        return;
    }
    slot_override->has_roughness = true;
    slot_override->roughness = roughness;
    SubmitRenderState();
}

float BaseRenderer::GetRoughness(int slot) const
{
    const MaterialInstanceOverride* slot_override = OverrideAt(slot);
    if (slot_override != nullptr && slot_override->has_roughness)
    {
        return slot_override->roughness;
    }
    const GameObjectMaterialDesc* desc = cachedMaterialDescAt(m_LastMaterialDescs, slot);
    return desc != nullptr ? desc->m_RoughnessFactor : 1.f;
}

void BaseRenderer::SetNormalScale(int slot, float normal_scale)
{
    MaterialInstanceOverride* slot_override = MutableOverrideAt(slot);
    if (slot_override == nullptr)
    {
        return;
    }
    slot_override->has_normal_scale = true;
    slot_override->normal_scale = normal_scale;
    SubmitRenderState();
}

float BaseRenderer::GetNormalScale(int slot) const
{
    const MaterialInstanceOverride* slot_override = OverrideAt(slot);
    if (slot_override != nullptr && slot_override->has_normal_scale)
    {
        return slot_override->normal_scale;
    }
    const GameObjectMaterialDesc* desc = cachedMaterialDescAt(m_LastMaterialDescs, slot);
    return desc != nullptr ? desc->m_NormalScale : 1.f;
}

void BaseRenderer::SetOcclusionStrength(int slot, float occlusion_strength)
{
    MaterialInstanceOverride* slot_override = MutableOverrideAt(slot);
    if (slot_override == nullptr)
    {
        return;
    }
    slot_override->has_occlusion = true;
    slot_override->occlusion = occlusion_strength;
    SubmitRenderState();
}

float BaseRenderer::GetOcclusionStrength(int slot) const
{
    const MaterialInstanceOverride* slot_override = OverrideAt(slot);
    if (slot_override != nullptr && slot_override->has_occlusion)
    {
        return slot_override->occlusion;
    }
    const GameObjectMaterialDesc* desc = cachedMaterialDescAt(m_LastMaterialDescs, slot);
    return desc != nullptr ? desc->m_OcclusionStrength : 1.f;
}

void BaseRenderer::SetBlend(int slot, bool blend)
{
    MaterialInstanceOverride* slot_override = MutableOverrideAt(slot);
    if (slot_override == nullptr)
    {
        return;
    }
    slot_override->has_blend = true;
    slot_override->blend = blend;
    SubmitRenderState();
}

bool BaseRenderer::GetBlend(int slot) const
{
    const MaterialInstanceOverride* slot_override = OverrideAt(slot);
    if (slot_override != nullptr && slot_override->has_blend)
    {
        return slot_override->blend;
    }
    const GameObjectMaterialDesc* desc = cachedMaterialDescAt(m_LastMaterialDescs, slot);
    return desc != nullptr ? desc->m_IsBlend : false;
}

void BaseRenderer::SetDoubleSided(int slot, bool double_sided)
{
    MaterialInstanceOverride* slot_override = MutableOverrideAt(slot);
    if (slot_override == nullptr)
    {
        return;
    }
    slot_override->has_double_sided = true;
    slot_override->double_sided = double_sided;
    SubmitRenderState();
}

bool BaseRenderer::GetDoubleSided(int slot) const
{
    const MaterialInstanceOverride* slot_override = OverrideAt(slot);
    if (slot_override != nullptr && slot_override->has_double_sided)
    {
        return slot_override->double_sided;
    }
    const GameObjectMaterialDesc* desc = cachedMaterialDescAt(m_LastMaterialDescs, slot);
    return desc != nullptr ? desc->m_IsDoubleSided : false;
}

void BaseRenderer::ClearMaterialOverride(int slot)
{
    MaterialInstanceOverride* slot_override = MutableOverrideAt(slot);
    if (slot_override == nullptr || !slot_override->IsActive())
    {
        return;
    }
    slot_override->Clear();
    SubmitRenderState();
}

bool BaseRenderer::HasMaterialOverride(int slot) const
{
    const MaterialInstanceOverride* slot_override = OverrideAt(slot);
    return slot_override != nullptr && slot_override->IsActive();
}
