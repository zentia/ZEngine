#pragma once

#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Core/Math/Vector4.h"
#include "Runtime/Function/Render/MaterialCustomParamSlots.h"

#include <EASTL/string.h>

#include <cstdint>
#include <vector>

/// 一个 sub-mesh 槽位上的**运行时**材质 override。
///
/// 语义是"改哪个字段就只管哪个字段"：`has_*` 为 false 的字段继续跟着材质资产走。
/// 这不是靠在这里存一份资产副本实现的 —— BaseRenderer 每次 SubmitRenderState 都会
/// 重新从资产 populate 一遍 `GameObjectMaterialDesc`，override 只在 populate **之后**
/// 套用，所以"没覆盖的字段 = 资产的当前值"是自动成立的，改资产也能立刻看到。
///
/// 与 Unity 的对应关系：`renderer.materials[i]` 上那份被实例化出来的材质。8 个标
/// 量之外，自定义槽 `custom_params` 承载 `SetFloat("_DissolveAmount")` 这类具名属
/// 性（名字到槽位的映射见 MaterialCustomParamNames.h），keyword 覆盖见下面
/// MaterialKeywordOverride。
///
/// 没出现在这个数组里的 keyword 继续跟随材质资产（Unity 的语义也是这样：
/// `material.EnableKeyword` 只管你显式提过的那些）。
struct MaterialKeywordOverride
{
    eastl::string name;
    bool enabled {false};
};

struct MaterialInstanceOverride
{
    /// 这个槽位**需不需要一份自己的 GpuPBRMaterial**。
    ///
    /// 判据是"要不要改 UBO 里的值" —— 8 个标量 + 自定义槽。keyword **不算**：
    /// keyword 变了会走另一个 MaterialSourceDesc hash（RenderType.h 里
    /// getHashValue 已经把 m_EnabledShaderKeywords 计入），于是拿到另一份
    /// **共享**材质的变体，根本不需要 per-part 实例。把 keyword 算进来会让
    /// 每个开了 keyword 的 part 都白白失去合批。
    bool NeedsMaterialInstance() const
    {
        if (has_base_color || has_emissive || has_metallic || has_roughness || has_normal_scale ||
            has_occlusion || has_blend || has_double_sided)
        {
            return true;
        }
        for (uint8_t mask : custom_component_mask)
        {
            if (mask != 0)
            {
                return true;
            }
        }
        return false;
    }

    /// 有任何覆盖吗（含 keyword）。给 `HasMaterialOverride()` 这种"这个槽位被
    /// 动过吗"的查询用。
    bool IsActive() const { return NeedsMaterialInstance() || !keyword_overrides.empty(); }

    void Clear() { *this = MaterialInstanceOverride {}; }

    /// 自定义槽的覆盖掩码：`custom_component_mask[slot]` 的 bit i 表示
    /// `custom_params[slot]` 的第 i 个分量被覆盖。
    ///
    /// 按**分量**而不是按整个 vec4 记，是为了让"只改溶解阈值"不会顺手把同槽位的
    /// 边缘宽度也 override 成 0 —— 没覆盖的分量继续走资产 / 上次的值。
    uint8_t custom_component_mask[k_MaterialCustomParamCount] {};
    Vector4 custom_params[k_MaterialCustomParamCount] {};

    /// per-renderer 的 keyword 覆盖。空 = 全部跟随资产。
    std::vector<MaterialKeywordOverride> keyword_overrides;

    bool HasCustomComponent(uint32_t slot, uint32_t component) const
    {
        if (slot >= k_MaterialCustomParamCount || component >= 4)
        {
            return false;
        }
        return (custom_component_mask[slot] & static_cast<uint8_t>(1u << component)) != 0;
    }

    void SetCustomComponent(uint32_t slot, uint32_t component, float value)
    {
        if (slot >= k_MaterialCustomParamCount || component >= 4)
        {
            return;
        }
        custom_component_mask[slot] |= static_cast<uint8_t>(1u << component);
        custom_params[slot][component] = value;
    }

    /// 找一条 keyword 覆盖。没有返回 nullptr。
    const MaterialKeywordOverride* FindKeyword(const eastl::string& name) const
    {
        for (const MaterialKeywordOverride& keyword : keyword_overrides)
        {
            if (keyword.name == name)
            {
                return &keyword;
            }
        }
        return nullptr;
    }

    /// 设置（已存在就改，不存在就追加）。
    void SetKeyword(const eastl::string& name, bool enabled)
    {
        for (MaterialKeywordOverride& keyword : keyword_overrides)
        {
            if (keyword.name == name)
            {
                keyword.enabled = enabled;
                return;
            }
        }
        keyword_overrides.push_back(MaterialKeywordOverride {name, enabled});
    }

    bool has_base_color {false};
    Vector4 base_color {1.0f, 1.0f, 1.0f, 1.0f};

    bool has_emissive {false};
    Vector3 emissive {0.0f, 0.0f, 0.0f};

    bool has_metallic {false};
    float metallic {1.0f};

    bool has_roughness {false};
    float roughness {1.0f};

    bool has_normal_scale {false};
    float normal_scale {1.0f};

    bool has_occlusion {false};
    float occlusion {1.0f};

    bool has_blend {false};
    bool blend {false};

    bool has_double_sided {false};
    bool double_sided {false};
};
