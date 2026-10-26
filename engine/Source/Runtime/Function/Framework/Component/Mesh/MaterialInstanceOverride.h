#pragma once

#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Core/Math/Vector4.h"

/// 一个 sub-mesh 槽位上的**运行时**材质 override。
///
/// 语义是"改哪个字段就只管哪个字段"：`has_*` 为 false 的字段继续跟着材质资产走。
/// 这不是靠在这里存一份资产副本实现的 —— BaseRenderer 每次 SubmitRenderState 都会
/// 重新从资产 populate 一遍 `GameObjectMaterialDesc`，override 只在 populate **之后**
/// 套用，所以"没覆盖的字段 = 资产的当前值"是自动成立的，改资产也能立刻看到。
///
/// 与 Unity 的对应关系：`renderer.materials[i]` 上那份被实例化出来的材质。区别是
/// ZEngine 这里只 override `MeshMaterialUniform` 里已有的 8 个标量 —— 没有具名
/// shader 属性槽（Material 资产侧那 4 个 MaterialXXXProperty vector 从来没进过
/// uniform），所以 `SetFloat("_DissolveAmount")` 这类需求不在这一层。
struct MaterialInstanceOverride
{
    /// 一个字段都没覆盖 = 这个槽位不需要自己的 GpuPBRMaterial，继续用共享材质。
    bool IsActive() const
    {
        return has_base_color || has_emissive || has_metallic || has_roughness || has_normal_scale ||
               has_occlusion || has_blend || has_double_sided;
    }

    void Clear() { *this = MaterialInstanceOverride {}; }

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
