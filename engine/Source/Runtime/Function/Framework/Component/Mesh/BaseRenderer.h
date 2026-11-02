#pragma once

#include "Runtime/BaseClasses/PPtr.h"
#include "Runtime/Core/Math/Bounds.h"
#include "Runtime/Core/Math/Frustum.h"
#include "Runtime/Function/Framework/Component/Component.h"
#include "Runtime/Function/Framework/Component/Mesh/MaterialCustomParamNames.h"
#include "Runtime/Function/Framework/Component/Mesh/MaterialInstanceOverride.h"
#include "Runtime/Function/Render/RenderObject.h"
#include "Runtime/Resource/ResType/Components/Mesh.h"

#include <vector>
#include "BaseRenderer.generated.h"

class Transform;

// 材质句柄的完整定义在 ScriptMaterial.h 里。这里只前插声明：`GetMaterial` 的返回
// 类型只需要一个不完整类型，而反过来 ScriptMaterial.h 要 include 本文件（它转发
// 到本类的方法），真 include 就成环了。
//
// ⚠️ 名字是 `MaterialInstance` 而不是 `Material`：`Material` 被材质**资产**类占用
// 了，ZHeaderTool 要求反射类名全局唯一。脚本侧靠 d.ts 的类型别名写回 `Material`。
class MaterialInstance;

ZCLASS()
class BaseRenderer : public Component
{
    ZGENERATED_BODY(BaseRenderer)

public:
    /// 登记进 `UpdateVisibilityForAll()` 要扫的那张表（见 .cpp 里的理由）。
    BaseRenderer();
    ~BaseRenderer() override;

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

    /// 撤掉这个槽位上全部自定义槽位的值（8 个标量不受影响，具名属性和贴图覆盖
    /// 一并撤掉）。
    ZFUNCTION()
    void ClearCustomParams(int slot);

    // ---- 属性是否存在的查询 ------------------------------------------------
    //
    // `Material.HasProperty("_DissolveAmount")` 这类查询的落点。
    //
    // ⚠️ 判据与 setter **共用同一张表**（下面的 ResolveShaderProperty 与
    // MaterialCustomParams::Find*），不是另写一份"认识的名字"清单：两套判据会在
    // 某次加属性名之后悄悄分叉，于是 `HasFloat` 说有而 `SetFloat` 说没有 —— 那种
    // bug 只会在某个具体属性上出现，很难复现到。
    //
    // `HasMaterialVector` 同时回答 `HasColor`：颜色在这个引擎里就是一个 vec4 属性，
    // 没有单独的"颜色槽"概念。

    ZFUNCTION()
    bool HasMaterialProperty(int slot, const eastl::string& name) const;
    ZFUNCTION()
    bool HasMaterialFloat(int slot, const eastl::string& name) const;
    ZFUNCTION()
    bool HasMaterialVector(int slot, const eastl::string& name) const;
    ZFUNCTION()
    bool HasMaterialTexture(int slot, const eastl::string& name) const;

    // =========================================================================
    // Unity 的 `renderer.material` / `renderer.materials`
    // =========================================================================

    /// 槽位上那份**运行时**材质的句柄（Unity 的 `renderer.materials[slot]`）。
    ///
    /// 句柄自己不带数据（见 `MaterialInstance` 的文件头），所以按槽位缓存一份就
    /// 够了：脚本每帧 `renderer.material.color = x` 不会每帧造对象，而读到的值仍
    /// 然是"当前生效值"，因为值本来就在 renderer 上。
    ///
    /// 越界返回 nullptr。renderer 被 Destroy 之后句柄自己会退化成"安全默认"
    /// （PPtr 解析成 nullptr），**不会**悬空。
    ///
    /// Unity 的 `renderer.material` 与 `renderer.sharedMaterial` 是两种东西（前者
    /// 是实例化出来的、后者是资产）。这里只发布前者：`sharedMaterial` 在今天没有
    /// 对应的脚本对象（材质资产进不了脚本，见 `Material.h` 的注释），发一个返回
    /// 路径字符串的假 Material 比不发更糟。
    ZFUNCTION()
    MaterialInstance* GetMaterial(int slot);

    /// Unity 的 `renderer.material`：槽位 0 那份。
    ///
    /// 多 sub-mesh 的网格 Unity 也是 `material` 只给第一份，其余走
    /// `materials[i]` —— 这里一样：其余按下标用 `GetMaterial(i)`。
    ZFUNCTION()
    MaterialInstance* GetMainMaterial() { return GetMaterial(0); }
    ZSCRIPTPROPERTY(Name = "material", Getter = "GetMainMaterial")

    /// Unity 的 `renderer.materials.length`（= `GetMaterialSlotCount()`）。
    ///
    /// 单列一个方法而不是让脚本读数组长度：ZFUNCTION 返回不了
    /// `std::vector<Material*>`（没有那个 Converter），`materials` 只能按下标取。
    ZFUNCTION()
    int GetMaterialCount() const { return static_cast<int>(m_SubMeshes.size()); }

    // ---- 贴图属性（per-renderer）----------------------------------------
    //
    // SetTexture("_MainTex", "Textures/rust.asset") —— 名字是 shader 在
    // Properties{} 里声明的 2D 属性名（或内置 shader 的别名，见
    // ShaderLab/ShaderTextureSlot.h 的 FindBuiltinTextureSlot）。
    //
    // 槽位是**复用**材质描述符集 binding 1..5 的：没有为自定义 shader 另开槽，
    // 描述符池也就不用改（VulkanRHI 里 COMBINED_IMAGE_SAMPLER 是按 5 张算的）。
    // 代价是一个 shader 最多 5 张 2D 贴图，且改 .shader 里属性的**声明顺序**会
    // 换槽位（导入期重算，GLSL 侧的 binding 跟着变，不会静默读错）。
    //
    // 换贴图拿到的是另一份**共享**材质变体（MaterialSourceDesc::getHashValue 已
    // 把五槽路径计入），不是 per-part 实例 —— 与 keyword 同一个道理，合批不受
    // 影响。但每次换贴图都要重新上传一次贴图，别在每帧调用。

    /// `texture_asset` 用项目相对路径（与 m_SharedMaterialAssets 同一套约定）。
    /// 名字不认识返回 false；传空串表示"这个槽不要自定义贴图"（回落 PBR）。
    ZFUNCTION()
    bool SetTexture(int slot, const eastl::string& name, const eastl::string& texture_asset);
    /// 当前生效的贴图路径。override 返回脚本设的原值，否则返回资产解析后的路径。
    /// 名字不认识返回空串。
    ZFUNCTION()
    eastl::string GetTexture(int slot, const eastl::string& name) const;

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

    // =========================================================================
    // 包围盒与可见性（Unity 的 `Renderer.bounds` / `localBounds` / `isVisible`）
    // =========================================================================

    /// Unity 的 `renderer.localBounds`：**对象空间**的盒，可读写。
    ///
    /// 默认（没有显式设过）由 sub-mesh 的网格资产算出来，缓存到 sub-mesh 列表
    /// 变化为止。资产读不到（headless / 还没导入）时退回**零盒**—— 中心原点、
    /// 尺寸为 0，与 Unity 的 `default(Bounds)` 同形。
    ///
    /// ⚠️ 退回零盒而不是"单位立方体"：零盒一眼就看得出"没算出来"，而单位立方
    /// 体是一个**完全合理**的包围盒，会让 `bounds` 看起来是对的、剔除却悄悄
    /// 多画或少画。宁可明显地错，不可看似对地错。
    ZFUNCTION()
    Bounds GetLocalBounds() const;
    ZFUNCTION()
    void SetLocalBounds(const Bounds& local_bounds);
    ZSCRIPTPROPERTY(Name="localBounds", Getter="GetLocalBounds", Setter="SetLocalBounds")

    /// 撤掉脚本设过的对象空间包围盒，回到"由网格资产算出来"的默认。
    ///
    /// 只有 `SetLocalBounds` 没有反向操作的话，"设过一次"就是单向的：任何一段
    /// 临时接管包围盒的代码（受击放大一下框、程序化网格算完推进来）结束时都撤
    /// 不干净，于是包围盒一直停在某个旧值上 —— 而"包围盒没跟着变"在画面上表现
    /// 为"该被剔掉的没剔掉"或反过来，是最难往回查的一类问题。
    ZFUNCTION()
    void ClearLocalBoundsOverride();

    /// Unity 的 `renderer.bounds`：**世界空间**的 AABB，只读。
    ///
    /// 做法是把 `localBounds` 的八个角用 `GetLocalToWorldMatrix()` 变换掉再重新
    /// 求 AABB —— 旋转过的盒在世界空间里仍然是轴对齐盒（可能变大），这是 Unity
    /// 的语义，也是"包围盒"该有的行为（只能多包不能漏包）。
    ///
    /// 不用 `Transform::GetLossyScale` 那种"缩放 extents"的近似：非均匀缩放下
    /// 它给出的盒比真的小，斜着放的长条会露出一部分。
    ZFUNCTION()
    Bounds GetBounds() const;
    ZSCRIPTPROPERTY(Name="bounds", Getter="GetBounds")

    /// Unity 的 `renderer.isVisible`：上一帧有没有被**任何一台场景相机**看到。
    ///
    /// ⚠️ 与 Unity 的差别（写在注释里，别以后悄悄改）：Unity 是真的"有没有被渲
    /// 染到"（所以场景视图也算、编辑器里也算）。这里问的是**场景里的
    /// `CameraComponent`**，因为 `RenderCamera` 归 RenderSystem 管，headless 下
    /// 恒为 nullptr —— 用它会得到一个在测试里永远返回 false 的 API，而"永远
    /// false"和"真的看不见"是分不开的。
    ZFUNCTION()
    bool IsVisible() const { return m_VisibleLastFrame; }
    ZSCRIPTPROPERTY(Name="isVisible", Getter="IsVisible")

    /// 重算可见性，并在**变化**那一帧派发 `OnBecameVisible` / `OnBecameInvisible`。
    ///
    /// 由 `UpdateVisibilityForAll()` 每帧调一次；`SetEnabled` 与
    /// `OnOwnerActiveChanged` 变化时也要立刻调一次 —— 否则"关掉渲染器"要等下
    /// 一帧才生效，而下一帧它可能已经不会再被扫到了。
    void UpdateVisibilityAndNotify();

    /// 全场渲染器各刷一次可见性。主循环每帧调一次（与 `MouseEventDispatcher::Tick`
    /// 同一层）。
    ///
    /// ⚠️ 为什么不能挂在 `BaseRenderer::Tick` 上：Tick 在 transform 不脏时直接
    /// return（那是渲染状态提交的优化，不能为了可见性去掉），可见性就只剩
    /// "物体动了才更新" —— 相机转开时它不会变，于是"相机看不到"永远报不出来。
    static void UpdateVisibilityForAll();

    void PostLoadResource(GameObject* parent_object) override;
    void OnSerializedFieldsUpdated() override;
    void Tick(float delta_time) override;

    /// 激活状态一变就重发一次渲染状态。
    ///
    /// 不做这个的话停用是"只停 Tick、画面照样画"：GameObject::Tick 在对象未激活时
    /// 直接 return，本组件的 Tick 不会再被调到，而下面 SubmitRenderState 里那条
    /// "提交空 parts 撤掉渲染实体"的分支只有在 Tick 里被触发 —— 死路一条。
    void OnOwnerActiveChanged(bool active) override;

    /// 启用状态一变就重算一次可见性（理由见 `UpdateVisibilityAndNotify`）。
    void SetEnabled(bool enabled) override;

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

    /// `GetMaterial(slot)` 的句柄缓存，与 m_SubMeshes 同下标。
    ///
    /// 用 PPtr 而不是裸指针：renderer 销毁时句柄可能还被脚本拿着，PPtr 解析成
    /// nullptr 之后写操作自然变成空操作。裸指针会变成悬空，而脚本那头完全看不出
    /// 来自己拿着一个死句柄。
    ///
    /// 不序列化（脚本每次要都能现造，缓存只是省对象分配）。
    std::vector<PPtr<MaterialInstance>> m_MaterialHandles;

    /// 上一次 BuildRenderParts 算出来的**生效**材质描述。getter 要返回"当前真正
    /// 在画的值"（override 优先，否则资产值），而资产值只在 BuildRenderParts 里
    /// 现算，getter 再算一遍等于每次读都去 loadAsset。所以这里缓存一份。
    mutable std::vector<GameObjectMaterialDesc> m_LastMaterialDescs;

    // ---- 包围盒 / 可见性 ----

    /// 脚本显式设过的对象空间包围盒。`SetLocalBounds` 之后一律用它。
    Bounds m_LocalBoundsOverride;
    bool m_HasLocalBoundsOverride {false};

    /// 从网格资产算出来的那份的缓存。算一次要去 loadAsset 每个 sub-mesh，
    /// `bounds` 又是每帧都可能被读的，所以必须有缓存。
    mutable Bounds m_CachedMeshLocalBounds;
    mutable bool m_CachedMeshLocalBoundsValid {false};

    /// 上一帧的可见性。`isVisible` 读它；变化时派发 OnBecameVisible/Invisible。
    bool m_VisibleLastFrame {false};
};
