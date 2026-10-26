#pragma once

#include "Runtime/Function/Framework/Component/Component.h"
#include "Runtime/Function/Framework/Component/Mesh/MaterialCustomParamNames.h"
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

    // ---- 运行时属性块 ----------------------------------------------------
    //
    // Unity 的 `renderer.material.SetFloat("_DissolveAmount", v)`。
    //
    // 现在有**两条**名字 -> UBO 偏移的路：
    //   1. ShaderLab 反射表 —— 名字和偏移都来自 .shader 自己声明的
    //      `Properties {}` + `CBUFFER_START(UnityPerMaterial)`，导入期算好存进
    //      ShaderRes，运行时由 populateMaterialDesc 搬到
    //      GameObjectMaterialDesc::m_ShaderProperties。**项目加新属性名不用改引擎**，
    //      改 .shader 就行。
    //   2. MaterialCustomParamNames.h 那张写死的表 —— 内置 shader 的老路子，名字
    //      加不了，只在第 1 条没命中时回落。
    //
    // 先查 1 再查 2，于是自定义 shader 的属性名不会撞上内置那几个写死的槽位。
    //
    // setter 返回 bool：**名字认不认识**。打错名字静默失败最难查，返回 bool 让
    // 脚本能立刻发现（Unity 的 SetFloat 返回 void，这里故意不一样）。
    //
    // 名字不认识 / slot 越界都是静默忽略，不崩不抛。
    //
    // 一个时序前提：第 1 条路要读 m_LastMaterialDescs（BuildRenderParts 刷的那份
    // 缓存），所以第一次 SubmitRenderState 之前查不到 shader 属性名，会暂时回落
    // 到第 2 条。PostLoadResource / SetSubMeshes 都会 SubmitRenderState，脚本能
    // 拿到 renderer 时缓存已经在。

    ZFUNCTION()
    bool SetFloat(int slot, const eastl::string& name, float value);
    /// 名字不认识返回 0。
    ZFUNCTION()
    float GetFloat(int slot, const eastl::string& name) const;

    ZFUNCTION()
    bool SetVector(int slot, const eastl::string& name, const Vector4& value);
    /// 名字不认识返回 (0,0,0,0)。
    ZFUNCTION()
    Vector4 GetVector(int slot, const eastl::string& name) const;

    /// 与 SetVector 是同一个东西（颜色在这里就是一个 vec4），留着是为了对齐
    /// Unity 的 SetColor 习惯。
    ZFUNCTION()
    bool SetColor(int slot, const eastl::string& name, const Vector4& value);
    ZFUNCTION()
    Vector4 GetColor(int slot, const eastl::string& name) const;

    /// 撤掉这个槽位上全部自定义槽位的值（8 个标量不受影响）。
    ZFUNCTION()
    void ClearCustomParams(int slot);

    // ---- shader keyword（per-renderer）-----------------------------------
    //
    // 溶解 / 描边这类效果光有标量是不够的 —— shader 得先编译进那个分支。开
    // keyword 会换 PSO（不是改个 UBO 那么便宜），所以别在每帧调用。
    //
    // 没被显式提过的 keyword 继续跟随材质资产，这也是 Unity 的语义。
    //
    // **当前生效范围（重要）**：CPU 侧链路是通的 —— keyword 并进
    // MaterialSourceDesc 的 hash，会拿到另一份共享材质变体。但"换 PSO"只在材质
    // 走 ShaderLab 资产时发生（MainCameraPassShaderCommon.cpp:CanUseRuntimePrimary
    // ShaderPass 要求 vertex/fragment_shader_file 非空）。内置 StandardLit 那条路
    // 恒返回内置 default_pipeline，keyword 改不到实际编译的 shader。
    // 也就是说：今天要让效果真的出现在内置 shader 上，用上面的自定义槽位（动态
    // 分支读 customParams）而不是 keyword。等 ShaderLab 运行时链路通了，keyword
    // 这套不用改就能生效。

    ZFUNCTION()
    void EnableKeyword(int slot, const eastl::string& keyword);
    ZFUNCTION()
    void DisableKeyword(int slot, const eastl::string& keyword);
    /// 这个 keyword 在这个槽位上**生效**吗（override 优先，否则看资产）。
    ZFUNCTION()
    bool HasKeyword(int slot, const eastl::string& keyword) const;
    /// 撤掉全部 keyword 覆盖，回到"完全跟随资产"。
    ZFUNCTION()
    void ClearKeywords(int slot);

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

    /// EnableKeyword / DisableKeyword 的公共实现。
    void SetKeywordEnabled(int slot, const eastl::string& keyword, bool enabled);

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
