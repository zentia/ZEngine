#pragma once

#include "Runtime/Function/Framework/Component/Component.h"
#include "Runtime/Function/Framework/Component/Mesh/MaterialInstanceOverride.h"
#include "Runtime/Function/Render/RenderObject.h"
#include "Runtime/Resource/ResType/Components/Mesh.h"

#include <vector>
#include "BaseRenderer.generated.h"

class Transform;

ZCLASS()
class BaseRenderer : public Component
{
    ZGENERATED_BODY(BaseRenderer)

public:
    BaseRenderer() = default;

    void SetSubMeshes(const std::vector<SubMeshRes>& sub_meshes);
    void SetSharedMaterialAssets(const std::vector<eastl::string>& shared_material_assets);

    // Editor mesh placement / foliage: force no back-face cull even when the bound
    // Material has is_double_sided=false. Not serialized.
    void SetForceDoubleSided(bool force) { m_ForceDoubleSided = force; }
    bool GetForceDoubleSided() const { return m_ForceDoubleSided; }

    const std::vector<SubMeshRes>& getSubMeshes() const { return m_SubMeshes; }
    const std::vector<eastl::string>& getSharedMaterialAssets() const { return m_SharedMaterialAssets; }

    // ---- 运行时材质实例 ----------------------------------------------------
    //
    // Unity 里脚本写 `renderer.material.color = red`，引擎立刻给你拷一份材质出来，
    // 之后这个 renderer 就用自己的那份。ZEngine 之前没有这条链路：材质只有"资产"
    // 一种形态，GpuPBRMaterial 按资产缓存，所有用同一个 .zasset 的 part 共用一份
    // MeshMaterialUniform —— 改一个的标量等于改全部。
    //
    // 这里的做法是：每个 sub-mesh 槽位存一份 override，SubmitRenderState 时套在
    // GameObjectMaterialDesc 上并置 `m_UseMaterialInstance`，RenderScene 于是给这个
    // part 一份**自己的** GpuPBRMaterial（克隆共享材质：纹理 / 采样器 / shader 共享，
    // 只有 uniform buffer 和描述符集是自己的）。
    //
    // 槽位粒度而不是组件粒度：一个网格有多个 sub-mesh 时各自可能是不同材质
    // （Unity 的 renderer.materials[]），按组件粒度会把它们一起改掉。
    //
    // **`slot` 是 sub-mesh 下标**，用 `GetMaterialSlotCount()` 拿上界；越界静默忽略。

    /// sub-mesh 数 = 材质槽位数。
    ZFUNCTION()
    int GetMaterialSlotCount() const { return static_cast<int>(m_SubMeshes.size()); }

    /// 覆盖基础色（w = alpha）。受击闪白 / 选中高亮 / 队伍染色都靠它。
    ZFUNCTION()
    void SetBaseColor(int slot, const Vector4& color);
    /// 当前**生效**的基础色 —— 有 override 就是 override，没有就是资产值。
    ZFUNCTION()
    Vector4 GetBaseColor(int slot) const;

    ZFUNCTION()
    void SetEmissive(int slot, const Vector3& emissive);
    ZFUNCTION()
    Vector3 GetEmissive(int slot) const;

    ZFUNCTION()
    void SetMetallic(int slot, float metallic);
    ZFUNCTION()
    float GetMetallic(int slot) const;

    ZFUNCTION()
    void SetRoughness(int slot, float roughness);
    ZFUNCTION()
    float GetRoughness(int slot) const;

    ZFUNCTION()
    void SetNormalScale(int slot, float normal_scale);
    ZFUNCTION()
    float GetNormalScale(int slot) const;

    ZFUNCTION()
    void SetOcclusionStrength(int slot, float occlusion_strength);
    ZFUNCTION()
    float GetOcclusionStrength(int slot) const;

    /// 混合开关。alpha 要生效通常得连它一起开（shader 是否吃 alpha 取决于材质）。
    ZFUNCTION()
    void SetBlend(int slot, bool blend);
    ZFUNCTION()
    bool GetBlend(int slot) const;

    ZFUNCTION()
    void SetDoubleSided(int slot, bool double_sided);
    ZFUNCTION()
    bool GetDoubleSided(int slot) const;

    /// 撤掉这个槽位上全部 override，回到共享材质（这个 part 又能和同资产的其它
    /// part 合批了）。闪白结束之后要调它 —— 否则这份实例材质会一直留着。
    ZFUNCTION()
    void ClearMaterialOverride(int slot);
    /// 这个槽位当前是否有 override。
    ZFUNCTION()
    bool HasMaterialOverride(int slot) const;

    void PostLoadResource(GameObject* parent_object) override;
    void OnSerializedFieldsUpdated() override;
    void Tick(float delta_time) override;

    void RefreshRenderStateFromTransformDispatch() const;

protected:
    std::vector<GameObjectPartDesc> BuildRenderParts(const Transform* transform_component) const;
    GameObjectDesc BuildGameObjectDescFromParts(const std::vector<GameObjectPartDesc>& render_parts) const;
    void SubmitRenderState() const;
    eastl::string ResolveMaterialAsset(size_t sub_mesh_index) const;
    void MigrateLegacyMaterialAssets();

protected:
    virtual GameObjectDesc BuildGameObjectDesc(const Transform* transform_component) const;

    /// 按 m_SubMeshes 的数量对齐 override 槽位（保留已有内容）。sub-mesh 列表变化
    /// （SetSubMeshes / 反序列化 / 遗留材质迁移）之后必须调。
    void SyncMaterialOverrideCount();

private:
    /// 越界返回 nullptr。const 版本给 getter 用，非 const 版本给 setter 用。
    MaterialInstanceOverride* MutableOverrideAt(int slot);
    const MaterialInstanceOverride* OverrideAt(int slot) const;

    ZPROPERTY()
    std::vector<SubMeshRes> m_SubMeshes;
    ZPROPERTY(Name="shared_materials")
    std::vector<eastl::string> m_SharedMaterialAssets;
    bool m_ForceDoubleSided {false};

    /// 每个 sub-mesh 槽位一份，与 m_SubMeshes 同下标。
    std::vector<MaterialInstanceOverride> m_MaterialOverrides;

    /// 上一次 BuildRenderParts 算出来的**生效**材质描述。getter 要返回"当前真正
    /// 在画的值"（override 优先，否则资产值），而资产值只在 BuildRenderParts 里
    /// 现算，getter 再算一遍等于每次读都去 loadAsset。所以这里缓存一份。
    mutable std::vector<GameObjectMaterialDesc> m_LastMaterialDescs;
};
