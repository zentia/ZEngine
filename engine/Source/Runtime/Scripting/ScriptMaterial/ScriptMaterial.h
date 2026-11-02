#pragma once

#include "Runtime/BaseClasses/Object.h"
#include "Runtime/BaseClasses/PPtr.h"
#include "Runtime/Core/Color/Color.h"
#include "Runtime/Core/Math/Vector4.h"
#include "Runtime/Function/Framework/Component/Mesh/BaseRenderer.h"

#include <EASTL/string.h>

#include "ScriptMaterial.generated.h"

// =============================================================================
// `MaterialInstance` —— Unity 的 `UnityEngine.Material`（`renderer.material` 拿到的那份）

// 脚本里它叫 `Material`
// --------------------
// C++ 侧**不能**叫 `Material`：全局的材质**资产**类已经占了这个名字
// （`Resource/ResType/Data/Material.h`），而 ZHeaderTool 要求反射类名全局唯一
// （生成的 `.generated.h` 落在一个扁平目录里，同名的第二份会把第一份覆盖掉）。
// 资产侧是序列化类型，名字写进了磁盘上的 .zasset，改不动。
//
// 于是 C++ 侧叫 `MaterialInstance`（"实例化出来的那份材质"，与 Unity 的语义对得
// 上），脚本侧靠 d.ts 里一行 `declare type Material = MaterialInstance;` 拿回
// Unity 的拼写 —— 类型标注、`renderer.material` 的返回值类型都能写 `Material`。
// =============================================================================
//
// 为什么是一个"句柄"而不是材质资产本身
// ------------------------------------
// 引擎里已经有一个全局 `class Material`（`Resource/ResType/Data/Material.h`），
// 那是**资产**：shader 引用 + 导入期保存下来的属性。它不能当 Unity 的
// `renderer.material` 用，原因写在那个文件的注释里（脚本拿不到它的实例，而且
// 资产是共享的 —— 改一个等于改所有用它的 renderer）。
//
// 引擎真正有的、与 Unity `renderer.material` 对位的，是 `BaseRenderer` 上那份
// **per-sub-mesh 的运行时材质实例**（`MaterialInstanceOverride`）：改基础色、
// SetFloat、换贴图、开关 keyword 都是真的生效、真的只影响这一个 renderer。
// 缺的只是"脚本世界里有一个对象代表它"。
//
// 于是这里的 `Material` 是一个**句柄**：`{renderer, slot}` 两个字段，全部方法
// 转发到 `BaseRenderer` 的对应槽位。它派生 `Object` 只是为了能被 ZCLASS 发布并
// 跨脚本边界传递 —— 它自己不持有任何材质数据。
//
// 状态在 renderer 上 ⇒ **不用缓存也能对**：`renderer.material` 每次 new 一个句柄，
// 读到的值仍然是对的（因为值本来就不存在句柄里）。`BaseRenderer::GetMaterial`
// 还是按槽位缓存了一份，免得每帧 `renderer.material.color = x` 都在造对象。
//
// 悬空安全
// --------
// 用 `PPtr<BaseRenderer>`：renderer 被销毁之后它解析成 nullptr，所有 setter 变成
// 空操作、所有 getter 返回默认值。不崩、不抛 —— 一个被 Destroy 掉的 renderer 上
// 留下的材质句柄不该把整局游戏带崩。脚本可以用 `IsValid()` 自己判。
//
// 刻意没做的 Unity 成员
// ---------------------
//   * `mainTexture` / `mainTextureOffset` / `mainTextureScale`：Unity 那三个是
//     `Texture` 对象 + UV 变换。这里的贴图槽存的是**资产路径**（换贴图换的是
//     材质描述符集里那张图的来源），没有 Texture 对象身份；UV 变换引擎里不存在
//     （GameObjectMaterialDesc 没有 tiling/offset 字段）。发一个恒为 (0,0)/(1,1)
//     的假属性比不发更糟 —— 脚本读了以为生效了。
//   * `shader` / `renderQueue` / `enableInstancing` / `globalIlluminationFlags` /
//     `passCount` / `SetPass` / `CopyPropertiesFromMaterial`：都没有后端。
//   * `SetInt` / `GetInt` / `SetMatrix` / `GetMatrix`：材质属性块里没有整型和矩阵
//     的落点（ShaderPropertySlot::FloatCount 直接拒掉矩阵，见那里的注释）。
//     用 SetFloat 冒充 SetInt 会在回读时给出 "1.0" 这种数字，比干脆没有更误导。
//   * `shaderKeywords`（string[]）：今天只知道**这个槽位显式覆盖过**的 keyword，
//     资产自己开的那份没有 per-renderer 的回读路径。发一个"只有你手动开过的"列表
//     会让脚本以为那是全集。要列就列全，列不全就不发。
// =============================================================================

ZCLASS()
class MaterialInstance : public Object
{
    ZGENERATED_BODY(MaterialInstance)

public:
    MaterialInstance() = default;

    // ---- 绑定（刻意不导出）------------------------------------------------
    //
    // 句柄由 `BaseRenderer::GetMaterial(slot)` 造并缓存，脚本自己 new 出来的
    // `Material` 没有绑定目标，所有方法走安全默认（见文件头"悬空安全"）。

    void Bind(BaseRenderer* renderer, int slot)
    {
        m_Renderer = renderer;
        m_Slot = slot;
    }

    /// 这个句柄还指向一个活着的 renderer 吗。
    ///
    /// 不是 Unity 的成员，留着是因为"SetFloat 一直返回 false"最难查的一种原因
    /// 就是 renderer 已经被 Destroy 了 —— 给脚本一条自查的路。
    ZFUNCTION()
    bool IsValid() const { return m_Renderer != nullptr; }

    // ---- 主色（Unity 的 `material.color`）----------------------------------
    //
    // 与 `BaseRenderer` 的 base color 是同一个量（w = alpha）。Unity 的
    // `material.color` 就是它，所以这里不再加一层名字。

    ZFUNCTION()
    void SetMainColor(const Color& color)
    {
        if (BaseRenderer* renderer = m_Renderer)
        {
            renderer->SetBaseColor(m_Slot, Vector4(color.r, color.g, color.b, color.a));
        }
    }

    ZFUNCTION()
    Color GetMainColor() const
    {
        const Vector4 base = (m_Renderer != nullptr) ? m_Renderer->GetBaseColor(m_Slot)
                                                     : Vector4(1.0f, 1.0f, 1.0f, 1.0f);
        Color result;
        result.r = base.x;
        result.g = base.y;
        result.b = base.z;
        result.a = base.w;
        return result;
    }
    ZSCRIPTPROPERTY(Name = "color", Getter = "GetMainColor", Setter = "SetMainColor")

    // ---- 具名属性（Unity 的 `material.SetFloat("_DissolveAmount", v)`）------
    //
    // 名字先查 .shader 自己声明的 `Properties{}`（ShaderLab 反射表），没命中再回落
    // 内置那张写死的名字表。细节见 BaseRenderer 里同一段注释。
    //
    // setter 返回 bool = **名字认不认识**（Unity 返回 void，这里故意不一样：打错
    // 名字静默失败是最难查的一类 bug）。

    ZFUNCTION()
    bool SetFloat(const eastl::string& name, float value)
    {
        return (m_Renderer != nullptr) && m_Renderer->SetFloat(m_Slot, name, value);
    }

    /// 名字不认识返回 0。
    ZFUNCTION()
    float GetFloat(const eastl::string& name) const
    {
        return (m_Renderer != nullptr) ? m_Renderer->GetFloat(m_Slot, name) : 0.0f;
    }

    /// 与 `SetVector` 是同一个东西（颜色在这里就是一个 vec4），留着是为了对齐
    /// Unity 的 `SetColor` 习惯。
    ZFUNCTION()
    bool SetColor(const eastl::string& name, const Color& value)
    {
        return (m_Renderer != nullptr) &&
               m_Renderer->SetColor(m_Slot, name, Vector4(value.r, value.g, value.b, value.a));
    }

    /// 名字不认识 / 句柄没绑定时返回不透明白（1,1,1,1）。
    ///
    /// ⚠️ 与 `GetFloat` 的 0 不同是有意的：读成 (0,0,0,0) 会让物体当场消失，而
    /// "这条属性不存在"不是一个该让物体消失的错误。
    ///
    /// ⚠️ 判据用的是 `HasMaterialVector` 而不是"读出来是不是 0"：一条**存在且真的
    /// 是黑**的属性必须能读出黑。拿返回值当哨兵会把"黑色"和"不存在"混成一件事，
    /// 于是"把溶解边缘调成黑色"这个完全合法的操作会被当成没设过。
    ZFUNCTION()
    Color GetColor(const eastl::string& name) const
    {
        const bool have = (m_Renderer != nullptr) && m_Renderer->HasMaterialVector(m_Slot, name);
        const Vector4 value = have ? m_Renderer->GetColor(m_Slot, name)
                                   : Vector4(1.0f, 1.0f, 1.0f, 1.0f);
        Color result;
        result.r = value.x;
        result.g = value.y;
        result.b = value.z;
        result.a = value.w;
        return result;
    }

    ZFUNCTION()
    bool SetVector(const eastl::string& name, const Vector4& value)
    {
        return (m_Renderer != nullptr) && m_Renderer->SetVector(m_Slot, name, value);
    }

    /// 名字不认识返回 (0,0,0,0)。
    ZFUNCTION()
    Vector4 GetVector(const eastl::string& name) const
    {
        return (m_Renderer != nullptr) ? m_Renderer->GetVector(m_Slot, name)
                                       : Vector4(0.0f, 0.0f, 0.0f, 0.0f);
    }

    /// `texture_asset` 用项目相对路径（与 `shared_materials` 同一套约定）。
    /// 名字不认识返回 false；传空串表示"这个槽不要自定义贴图"（回落 PBR）。
    ZFUNCTION()
    bool SetTexture(const eastl::string& name, const eastl::string& texture_asset)
    {
        return (m_Renderer != nullptr) && m_Renderer->SetTexture(m_Slot, name, texture_asset);
    }

    /// 当前生效的贴图路径。名字不认识返回空串。
    ZFUNCTION()
    eastl::string GetTexture(const eastl::string& name) const
    {
        return (m_Renderer != nullptr) ? m_Renderer->GetTexture(m_Slot, name) : eastl::string();
    }

    // ---- 属性是否存在 ------------------------------------------------------
    //
    // Unity 的 `material.HasProperty(name)` / `HasFloat` / `HasColor` / …
    //
    // 这一组是**为了可读性而不是为了性能**：写之前先问一遍比写完发现 SetFloat
    // 返回 false 好查。判据与 setter 用的是同一张表，所以"HasFloat 说有"和
    // "SetFloat 说认得"永远一致 —— 两套判据会在某次改表之后悄悄分叉。

    ZFUNCTION()
    bool HasProperty(const eastl::string& name) const
    {
        return (m_Renderer != nullptr) && m_Renderer->HasMaterialProperty(m_Slot, name);
    }

    ZFUNCTION()
    bool HasFloat(const eastl::string& name) const
    {
        return (m_Renderer != nullptr) && m_Renderer->HasMaterialFloat(m_Slot, name);
    }

    ZFUNCTION()
    bool HasVector(const eastl::string& name) const
    {
        return (m_Renderer != nullptr) && m_Renderer->HasMaterialVector(m_Slot, name);
    }

    /// 颜色在这个引擎里就是一个 vec4 属性，所以与 `HasVector` 同判据（Unity 也是：
    /// `HasColor("_Color")` 与 `HasVector("_Color")` 在 URP 里是一个东西）。
    ZFUNCTION()
    bool HasColor(const eastl::string& name) const
    {
        return (m_Renderer != nullptr) && m_Renderer->HasMaterialVector(m_Slot, name);
    }

    ZFUNCTION()
    bool HasTexture(const eastl::string& name) const
    {
        return (m_Renderer != nullptr) && m_Renderer->HasMaterialTexture(m_Slot, name);
    }

    // ---- shader keyword ----------------------------------------------------
    //
    // ⚠️ 生效范围写在 `BaseRenderer` 的 keyword 注释里，这里复述一次因为太容易踩：
    // CPU 侧链路是通的（keyword 并进材质 hash），但"换 PSO"只在材质走 ShaderLab
    // 资产时发生 —— 内置 StandardLit 那条路恒返回内置 default_pipeline。今天要让
    // 效果真的出现在内置 shader 上，用上面的具名属性而不是 keyword。

    ZFUNCTION()
    void EnableKeyword(const eastl::string& keyword)
    {
        if (m_Renderer != nullptr)
        {
            m_Renderer->EnableKeyword(m_Slot, keyword);
        }
    }

    ZFUNCTION()
    void DisableKeyword(const eastl::string& keyword)
    {
        if (m_Renderer != nullptr)
        {
            m_Renderer->DisableKeyword(m_Slot, keyword);
        }
    }

    /// 这个 keyword 在这个槽位上**生效**吗（override 优先，否则看资产）。
    ZFUNCTION()
    bool IsKeywordEnabled(const eastl::string& keyword) const
    {
        return (m_Renderer != nullptr) && m_Renderer->HasKeyword(m_Slot, keyword);
    }

private:
    PPtr<BaseRenderer> m_Renderer;
    int m_Slot {0};
};
