#include "BaseRenderer.h"

#include "Runtime/Core/Math/LargeWorldCoordinates.h"
#include "Runtime/BaseClasses/GameObject.h"
#include "Runtime/Function/Framework/Component/Transform/Transform.h"
#include "Runtime/Function/Framework/Component/Transform/TransformChangeDispatch.h"
#include "Runtime/Function/Render/RenderSwapContext.h"
#include "Runtime/Function/Render/RenderSystem.h"
#include "Runtime/Resource/Asset/AssetManager.h"
#include "Runtime/Resource/ResType/Data/Material.h"
#include "Runtime/Resource/ResType/Data/Shader.h"

#include <algorithm>



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

    eastl::string resolveAssetPath(const eastl::string& asset_path)
    {
        if (asset_path.empty())
        {
            return "";
        }
        return GET_SYSTEM(AssetManager)->GetFullPath(asset_path).generic_string().c_str();
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

        // 自定义槽按**分量**套用：只改被覆盖的那一个 float，同槽位的其它分量
        // 继续跟着资产走。
        for (uint32_t slot = 0; slot < k_MaterialCustomParamCount; ++slot)
        {
            for (uint32_t component = 0; component < 4; ++component)
            {
                if (override_data.HasCustomComponent(slot, component))
                {
                    material_desc.m_CustomParams[slot][component] = override_data.custom_params[slot][component];
                }
            }
        }

        // ShaderLab 具名属性：per-renderer 的 override 按 float 掩码盖在资产值上。
        // 没被覆盖的分量（掩码为 0）继续跟着材质资产走 —— 与上面 custom_params
        // 按分量覆盖是同一个思路，只是粒度细到单个 float。
        ZEngine::ShaderLab::ApplyOverride(override_data.property_blob, material_desc.m_PropertyBlob);

        // keyword 覆盖。没被提到的 keyword 继续跟随资产。
        //
        // 最后那个 sort 不是顺手加的：MaterialSourceDesc::getHashValue() 是按顺序
        // hash_combine keyword 的，同一个集合顺序不同就会得到两个不同的 material
        // asset id —— 也就是两份内容完全一样的共享材质（各带一份纹理上传）。
        // 排序让"同集合"必然得到"同 hash"。只在真的有 keyword 覆盖时才排，
        // 纯资产路径的 keyword 顺序保持不动。
        if (!override_data.keyword_overrides.empty())
        {
            auto& keywords = material_desc.m_EnabledShaderKeywords;
            for (const MaterialKeywordOverride& keyword : override_data.keyword_overrides)
            {
                keywords.erase(std::remove_if(keywords.begin(),
                                              keywords.end(),
                                              [&](const eastl::string& existing) { return existing == keyword.name; }),
                               keywords.end());
                if (keyword.enabled)
                {
                    keywords.push_back(keyword.name);
                }
            }
            std::sort(keywords.begin(), keywords.end());
        }

        // 贴图覆盖。走的是 MaterialSourceDesc 的 hash（不是 per-part 实例）：换
        // 一张贴图 = 另一份共享材质变体。没被覆盖的槽继续跟随材质资产。
        for (uint32_t slot = 0; slot < ZEngine::ShaderLab::k_MaterialTextureSlotCount; ++slot)
        {
            if (override_data.HasTextureSlot(slot))
            {
                material_desc.m_TextureSlotFiles[slot] = resolveAssetPath(override_data.texture_slot_files[slot]);
            }
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

    /// 把 shader 声明的属性 + 材质资产保存的属性值合成"落点表 + 资产值字节"。
    ///
    /// 与 RenderType.h 里 BuildShaderPropertySlots 的分工：那边只认渲染侧的
    /// RenderShaderPropertyDesc，这边负责把 ShaderRes / Material 两套资源数据翻过去，
    /// 并让**材质资产保存的值盖掉 shader 的默认值** —— Unity 的语义就是"材质改过
    /// 就是改过"，shader 里的默认值只是初始值。
    ///
    /// 落点（UBO 字节偏移）在导入 .shader 时就算好了（ShaderCbufferReflection），
    /// 这里只是搬过来，不做任何解析 —— populateMaterialDesc 每帧都会跑。
    void BuildMaterialShaderProperties(const ShaderRes& shader,
                                       const Material& material,
                                       std::vector<ZEngine::ShaderLab::ShaderPropertySlot>& out_slots,
                                       ZEngine::ShaderLab::ShaderPropertyBlob& out_values)
    {
        std::vector<RenderShaderPropertyDesc> properties;
        properties.reserve(shader.m_Properties.size());

        for (const ShaderPropertyDesc& source : shader.m_Properties)
        {
            RenderShaderPropertyDesc property;
            property.m_Name = source.m_Name;
            property.m_DisplayName = source.m_DisplayName;
            property.m_Type = source.m_Type;
            property.m_DefaultFloat = source.m_DefaultFloat;
            property.m_RangeMin = source.m_RangeMin;
            property.m_RangeMax = source.m_RangeMax;
            property.m_DefaultColor[0] = source.m_DefaultColor.x;
            property.m_DefaultColor[1] = source.m_DefaultColor.y;
            property.m_DefaultColor[2] = source.m_DefaultColor.z;
            property.m_DefaultColor[3] = source.m_DefaultAlpha;
            property.m_DefaultTexture = source.m_DefaultTexture;
            property.m_DefaultToggle = source.m_DefaultToggle;
            property.m_CbufferOffset = source.m_CbufferOffset;
            property.m_CbufferSize = source.m_CbufferSize;
            property.m_CbufferComponents = source.m_CbufferComponents;
            property.m_CbufferName = source.m_CbufferName;
            property.m_TextureSlot = source.m_TextureSlot;

            for (const MaterialFloatProperty& saved : material.m_FloatProperties)
            {
                if (saved.m_Name == source.m_Name)
                {
                    property.m_DefaultFloat = saved.m_Value;
                    break;
                }
            }
            for (const MaterialColorProperty& saved : material.m_ColorProperties)
            {
                if (saved.m_Name == source.m_Name)
                {
                    property.m_DefaultColor[0] = saved.m_Color.x;
                    property.m_DefaultColor[1] = saved.m_Color.y;
                    property.m_DefaultColor[2] = saved.m_Color.z;
                    property.m_DefaultColor[3] = saved.m_Alpha;
                    break;
                }
            }
            for (const MaterialToggleProperty& saved : material.m_ToggleProperties)
            {
                if (saved.m_Name == source.m_Name)
                {
                    property.m_DefaultToggle = saved.m_Value;
                    break;
                }
            }

            properties.push_back(std::move(property));
        }

        BuildShaderPropertySlots(properties, out_slots, out_values);
    }

    /// 2D 属性 -> 逐槽贴图路径 + "名字 -> 槽位"表。
    ///
    /// 槽位是导入期按 Properties{} 里 2D 属性的**声明顺序**分配的（ShaderTextureSlot.h），
    /// 这里只负责把每个槽填上实际的贴图路径。取值优先级：
    ///
    ///   1. 材质资产保存的同名贴图（Material::m_TextureProperties 的 PPtr）——
    ///      Unity 语义是"材质改过就是改过"，和 Float/Color 那一套一致；
    ///   2. shader 声明里的默认贴图（ShaderPropertyDesc::m_DefaultTexture）；
    ///   3. 都没有 -> 留空，GPU 侧按槽回落到那张 PBR 贴图。
    ///
    /// 第 2 条只认**像路径**的字符串（含 '/' 或 '.'）。Unity 的内置名 "white" /
    /// "black" / "bump" 在这里没有对应物（引擎没有那几张 1x1 内置贴图），硬猜一个
    /// 等价文件只会把画面搞错，不如留空走回落。
    void BuildMaterialTextureSlots(const ShaderRes& shader,
                                   const Material& material,
                                   eastl::string* out_slot_files,
                                   uint32_t slot_count,
                                   std::vector<ZEngine::ShaderLab::ShaderTextureSlotBinding>& out_bindings)
    {
        out_bindings.clear();
        for (uint32_t i = 0; i < slot_count; ++i)
        {
            out_slot_files[i].clear();
        }

        std::vector<RenderShaderPropertyDesc> properties;
        properties.reserve(shader.m_Properties.size());
        for (const ShaderPropertyDesc& source : shader.m_Properties)
        {
            if (source.m_TextureSlot < 0 ||
                source.m_TextureSlot >= static_cast<int32_t>(slot_count))
            {
                continue;
            }
            RenderShaderPropertyDesc property;
            property.m_Name = source.m_Name;
            property.m_Type = source.m_Type;
            property.m_DefaultTexture = source.m_DefaultTexture;
            property.m_TextureSlot = source.m_TextureSlot;
            properties.push_back(std::move(property));
        }
        BuildShaderTextureSlots(properties, out_bindings);

        for (const ZEngine::ShaderLab::ShaderTextureSlotBinding& binding : out_bindings)
        {
            const uint32_t slot = static_cast<uint32_t>(binding.slot);

            eastl::string resolved;
            for (const MaterialTextureProperty& saved : material.m_TextureProperties)
            {
                if (saved.m_Name == binding.name.c_str())
                {
                    resolved = Material::ResolveTextureAssetPath(saved.m_Texture);
                    break;
                }
            }

            if (resolved.empty())
            {
                for (const ShaderPropertyDesc& source : shader.m_Properties)
                {
                    if (source.m_Name != binding.name.c_str())
                    {
                        continue;
                    }
                    const eastl::string& declared = source.m_DefaultTexture;
                    if (!declared.empty() &&
                        (declared.find('/') != eastl::string::npos || declared.find('.') != eastl::string::npos))
                    {
                        resolved = declared;
                    }
                    break;
                }
            }

            if (resolved.empty())
            {
                continue;
            }
            out_slot_files[slot] = resolveAssetPath(resolved);
        }
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

        // ShaderLab 具名属性。只有自定义 shader 资产才带属性表；内置 StandardLit
        // 的 m_Properties 为空，m_ShaderProperties 也跟着为空，SetFloat 于是继续
        // 走 MaterialCustomParamNames 那张写死的表（和改动前一模一样）。
        //
        // 解析不在这里做：UBO 偏移是导入 .shader 时算好存进 ShaderRes 的，这里只搬。
        if (ShaderRes* shader_res = material_res->m_ShaderPptr)
        {
            BuildMaterialShaderProperties(*shader_res,
                                          *material_res,
                                          material_desc.m_ShaderProperties,
                                          material_desc.m_PropertyBlob);

            // 2D 属性 -> 贴图槽。槽位来自导入期（shader 里 2D 属性的声明顺序），
            // 贴图来自材质资产。没有 2D 属性的 shader 五个槽全空，GPU 侧逐槽回落
            // PBR 贴图 —— 内置 StandardLit 完全走不到这段。
            BuildMaterialTextureSlots(*shader_res,
                                      *material_res,
                                      material_desc.m_TextureSlotFiles,
                                      ZEngine::ShaderLab::k_MaterialTextureSlotCount,
                                      material_desc.m_TextureProperties);
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
            // 是 NeedsMaterialInstance 而不是 IsActive：只开 keyword 的话走另一份
            // 共享材质变体就够了，不需要 per-part 实例（那样会白白丢掉合批）。
            render_part.m_MaterialDesc.m_UseMaterialInstance = slot_override.NeedsMaterialInstance();
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
    // 层级激活（Unity activeInHierarchy）：父级被停用，子对象也得藏。
    //
    // 这里以前是 `transform_component == nullptr || transform_component->isEnabled()`，
    // 只认自己那一个开关，于是"父停用、子仍画"。改成问 GameObject，和
    // GameObject::Tick 用的是同一个判定。
    const bool object_active = m_ParentObject->IsActiveInHierarchy();
    if (!isEnabled() || !object_active)
    {
        // Empty parts remove stale render entities for this GameObject.
        render_swap_context.GetLogicSwapData().AddDirtyGameObject(
            GameObjectDesc {m_ParentObject->GetID(), {}});
        return;
    }

    render_swap_context.GetLogicSwapData().AddDirtyGameObject(BuildGameObjectDesc(transform_component));
}

void BaseRenderer::OnOwnerActiveChanged(bool active)
{
    (void)active;
    // 值本身不用读：SubmitRenderState 会重新问一次 IsActiveInHierarchy()，
    // 传进来的这个只是给"不关心方向的组件"用的。
    SubmitRenderState();
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

// ---- 运行时属性块 --------------------------------------------------------

bool BaseRenderer::SetFloat(int slot, const eastl::string& name, float value)
{
    // 名字认不认识要先于"槽位在不在"判断 —— 打错名字静默失败最难查，返回 false
    // 让脚本能立刻发现（Unity 的 SetFloat 返回 void，这里故意不一样）。
    //
    // 两张名字表：
    //   1. ShaderLab 反射表（GameObjectMaterialDesc::m_ShaderProperties）—— 名字和
    //      UBO 偏移都来自 .shader 自己声明的 Properties / CBUFFER，项目加新属性名
    //      不用改引擎；
    //   2. MaterialCustomParamNames 那张写死的表 —— 内置 shader 的老路子。
    // 先查 1，命中就按偏移写；没命中再回落 2。
    const GameObjectMaterialDesc* desc = cachedMaterialDescAt(m_LastMaterialDescs, slot);
    const bool known_by_shader =
        desc != nullptr && ZEngine::ShaderLab::FindSlot(desc->m_ShaderProperties, name.c_str()) != nullptr;

    const MaterialCustomParamLocation* location = MaterialCustomParams::FindFloat(name);
    if (location == nullptr && !known_by_shader)
    {
        return false;
    }

    MaterialInstanceOverride* slot_override = MutableOverrideAt(slot);
    if (slot_override == nullptr)
    {
        return false;
    }

    if (known_by_shader)
    {
        ZEngine::ShaderLab::SetNamedFloat(desc->m_ShaderProperties, slot_override->property_blob, name.c_str(), value);
    }
    else
    {
        slot_override->SetCustomComponent(location->slot, location->component, value);
    }
    SubmitRenderState();
    return true;
}

float BaseRenderer::GetFloat(int slot, const eastl::string& name) const
{
    // m_LastMaterialDescs 是 BuildRenderParts 里 override 套完之后的那份，所以直接
    // 读它就是"当前生效值"，不用再回头查 override。
    const GameObjectMaterialDesc* desc = cachedMaterialDescAt(m_LastMaterialDescs, slot);
    if (desc != nullptr)
    {
        float value = 0.0f;
        if (ZEngine::ShaderLab::GetNamedFloat(desc->m_ShaderProperties, desc->m_PropertyBlob, name.c_str(), value))
        {
            return value;
        }
    }

    const MaterialCustomParamLocation* location = MaterialCustomParams::FindFloat(name);
    if (location == nullptr)
    {
        return 0.0f;
    }

    const MaterialInstanceOverride* slot_override = OverrideAt(slot);
    if (slot_override != nullptr && slot_override->HasCustomComponent(location->slot, location->component))
    {
        return slot_override->custom_params[location->slot][location->component];
    }

    return desc != nullptr ? desc->m_CustomParams[location->slot][location->component] : 0.0f;
}

bool BaseRenderer::SetVector(int slot, const eastl::string& name, const Vector4& value)
{
    const GameObjectMaterialDesc* desc = cachedMaterialDescAt(m_LastMaterialDescs, slot);
    const bool known_by_shader =
        desc != nullptr && ZEngine::ShaderLab::FindSlot(desc->m_ShaderProperties, name.c_str()) != nullptr;

    const MaterialCustomParamLocation* location = MaterialCustomParams::FindVector(name);
    if (location == nullptr && !known_by_shader)
    {
        return false;
    }

    MaterialInstanceOverride* slot_override = MutableOverrideAt(slot);
    if (slot_override == nullptr)
    {
        return false;
    }

    if (known_by_shader)
    {
        const float values[4] = {value.x, value.y, value.z, value.w};
        ZEngine::ShaderLab::SetNamedVector(desc->m_ShaderProperties, slot_override->property_blob, name.c_str(), values);
    }
    else
    {
        // 整个 vec4 一起覆盖：四个分量全打上标记。
        for (uint32_t component = 0; component < 4; ++component)
        {
            slot_override->SetCustomComponent(location->slot, component, value[component]);
        }
    }
    SubmitRenderState();
    return true;
}

Vector4 BaseRenderer::GetVector(int slot, const eastl::string& name) const
{
    const GameObjectMaterialDesc* desc = cachedMaterialDescAt(m_LastMaterialDescs, slot);
    if (desc != nullptr)
    {
        float values[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        if (ZEngine::ShaderLab::GetNamedVector(desc->m_ShaderProperties, desc->m_PropertyBlob, name.c_str(), values))
        {
            return Vector4(values[0], values[1], values[2], values[3]);
        }
    }

    const MaterialCustomParamLocation* location = MaterialCustomParams::FindVector(name);
    if (location == nullptr)
    {
        return Vector4(0.0f, 0.0f, 0.0f, 0.0f);
    }

    const MaterialInstanceOverride* slot_override = OverrideAt(slot);
    if (slot_override != nullptr)
    {
        // 四个分量全被覆盖才算 override 了这个向量；只覆盖了一部分的话逐个分量
        // 回退到资产值，别把没覆盖的分量说成 0。
        Vector4 result = desc != nullptr ? desc->m_CustomParams[location->slot]
                                         : Vector4(0.0f, 0.0f, 0.0f, 0.0f);
        for (uint32_t component = 0; component < 4; ++component)
        {
            if (slot_override->HasCustomComponent(location->slot, component))
            {
                result[component] = slot_override->custom_params[location->slot][component];
            }
        }
        return result;
    }

    return desc != nullptr ? desc->m_CustomParams[location->slot] : Vector4(0.0f, 0.0f, 0.0f, 0.0f);
}

bool BaseRenderer::SetColor(int slot, const eastl::string& name, const Vector4& value)
{
    return SetVector(slot, name, value);
}

Vector4 BaseRenderer::GetColor(int slot, const eastl::string& name) const
{
    return GetVector(slot, name);
}

void BaseRenderer::ClearCustomParams(int slot)
{
    MaterialInstanceOverride* slot_override = MutableOverrideAt(slot);
    if (slot_override == nullptr)
    {
        return;
    }

    bool changed = false;
    for (uint32_t param_slot = 0; param_slot < k_MaterialCustomParamCount; ++param_slot)
    {
        if (slot_override->custom_component_mask[param_slot] != 0)
        {
            slot_override->custom_component_mask[param_slot] = 0;
            slot_override->custom_params[param_slot] = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
            changed = true;
        }
    }

    // 具名属性一并清掉：名字是 shader 给的，槽位是写死的，两条路都得撤干净，
    // 否则 ClearCustomParams 之后这个 part 还带着一份 override 材质实例。
    if (slot_override->property_blob.AnyOverride())
    {
        slot_override->property_blob.Clear();
        changed = true;
    }

    if (slot_override->HasTextureOverride())
    {
        slot_override->ClearTextureSlots();
        changed = true;
    }

    if (changed)
    {
        SubmitRenderState();
    }
}

// ---- 贴图属性 ----------------------------------------------------------

bool BaseRenderer::SetTexture(int slot, const eastl::string& name, const eastl::string& texture_asset)
{
    // 名字 -> 槽位。两张表，与 SetFloat 同一套思路：
    //   1. shader 自己声明的 2D 属性（导入期按声明顺序分配的槽位）；
    //   2. 内置 StandardLit 的贴图别名（_MainTex / _BumpMap / ...）。
    // 两张表都不认识就返回 false —— 往一个猜出来的槽里塞贴图，画面错了还没处查。
    const GameObjectMaterialDesc* desc = cachedMaterialDescAt(m_LastMaterialDescs, slot);
    const ZEngine::ShaderLab::ShaderTextureSlotBinding* binding =
        desc != nullptr ? ZEngine::ShaderLab::FindTextureSlotBinding(desc->m_TextureProperties, name.c_str()) : nullptr;

    const int32_t texture_slot = binding != nullptr ? binding->slot
                                                   : ZEngine::ShaderLab::FindBuiltinTextureSlot(name.c_str());
    if (texture_slot < 0)
    {
        return false;
    }

    MaterialInstanceOverride* slot_override = MutableOverrideAt(slot);
    if (slot_override == nullptr)
    {
        return false;
    }

    slot_override->SetTextureSlot(static_cast<uint32_t>(texture_slot), texture_asset);
    SubmitRenderState();
    return true;
}

eastl::string BaseRenderer::GetTexture(int slot, const eastl::string& name) const
{
    const GameObjectMaterialDesc* desc = cachedMaterialDescAt(m_LastMaterialDescs, slot);
    const ZEngine::ShaderLab::ShaderTextureSlotBinding* binding =
        desc != nullptr ? ZEngine::ShaderLab::FindTextureSlotBinding(desc->m_TextureProperties, name.c_str()) : nullptr;

    const int32_t texture_slot = binding != nullptr ? binding->slot
                                                   : ZEngine::ShaderLab::FindBuiltinTextureSlot(name.c_str());
    if (texture_slot < 0)
    {
        return {};
    }

    // override 存的是脚本传进来的原值（项目相对），资产侧存的是解析后的绝对路径。
    // getter 各自原样返回 —— 想拿绝对路径就用 AssetManager 解析一次，这里不做
    // 隐式转换（转换错了比不转换更难查）。
    const MaterialInstanceOverride* slot_override = OverrideAt(slot);
    if (slot_override != nullptr && slot_override->HasTextureSlot(static_cast<uint32_t>(texture_slot)))
    {
        return slot_override->texture_slot_files[texture_slot];
    }
    return desc != nullptr ? desc->m_TextureSlotFiles[texture_slot] : eastl::string();
}

// ---- shader keyword -----------------------------------------------------

void BaseRenderer::SetKeywordEnabled(int slot, const eastl::string& keyword, bool enabled)
{
    if (keyword.empty())
    {
        return;
    }

    MaterialInstanceOverride* slot_override = MutableOverrideAt(slot);
    if (slot_override == nullptr)
    {
        return;
    }

    // 已经是这个值就别触发重新 submit —— 换 PSO 比改 UBO 贵得多，而脚本很可能
    // 在 Update 里每帧调一次 EnableKeyword。
    if (const MaterialKeywordOverride* existing = slot_override->FindKeyword(keyword))
    {
        if (existing->enabled == enabled)
        {
            return;
        }
    }

    slot_override->SetKeyword(keyword, enabled);
    SubmitRenderState();
}

void BaseRenderer::EnableKeyword(int slot, const eastl::string& keyword)
{
    SetKeywordEnabled(slot, keyword, true);
}

void BaseRenderer::DisableKeyword(int slot, const eastl::string& keyword)
{
    SetKeywordEnabled(slot, keyword, false);
}

bool BaseRenderer::HasKeyword(int slot, const eastl::string& keyword) const
{
    if (keyword.empty())
    {
        return false;
    }

    // override 优先。
    const MaterialInstanceOverride* slot_override = OverrideAt(slot);
    if (slot_override != nullptr)
    {
        if (const MaterialKeywordOverride* existing = slot_override->FindKeyword(keyword))
        {
            return existing->enabled;
        }
    }

    // 没被覆盖就跟随资产：看上一次 BuildRenderParts 算出来的生效 keyword 列表。
    const GameObjectMaterialDesc* desc = cachedMaterialDescAt(m_LastMaterialDescs, slot);
    if (desc != nullptr)
    {
        for (const eastl::string& enabled_keyword : desc->m_EnabledShaderKeywords)
        {
            if (enabled_keyword == keyword)
            {
                return true;
            }
        }
    }
    return false;
}

void BaseRenderer::ClearKeywords(int slot)
{
    MaterialInstanceOverride* slot_override = MutableOverrideAt(slot);
    if (slot_override == nullptr || slot_override->keyword_overrides.empty())
    {
        return;
    }

    slot_override->keyword_overrides.clear();
    SubmitRenderState();
}
