#pragma once

#include "Runtime/Core/Math/Vector4.h"
#include "Runtime/Function/Render/MaterialCustomParamSlots.h"

#include <EASTL/string.h>

#include <cstdint>
#include <vector>

/// 一个具名属性在 `MeshMaterialUniform::customParams` 里的落点。
///
/// 这是"内置 shader 固定效果槽"方案（区别于 Unity 那种任意 shader 的任意具名
/// 属性）的代价所在：**属性名是引擎写死的**，项目加不了新名字。原因不是偷懒，
/// 而是名字到 offset 的绑定本来该由 shader 的反射数据给出，而 ZEngine 现在没有
/// 这条数据 —— ShaderLab 编译产物只有裸 SPIR-V 字节，运行时也不解析
/// `CBUFFER_START(UnityPerMaterial)`（详见脚本绑定覆盖度报告）。等那条链路通了，
/// 这张表就该被真正的属性元数据取代。
struct MaterialCustomParamLocation
{
    /// `customParams` 的下标（vec4 槽位）。
    uint32_t slot {0};
    /// 分量下标 0..3。只有标量名用它，向量名忽略。
    uint32_t component {0};
};

/// 内置效果名表。改这张表**不会**自动让 shader 认新属性 —— shader 侧得有对应的
/// 消费代码（现在只有溶解那一组，见 shader/include/material_custom_params.h 与
/// 它的 HLSL 镜像 hlsl/common/material_custom_params.hlsli）。
namespace MaterialCustomParams
{
    /// ---- 标量名（SetFloat / GetFloat）----

    /// 溶解阈值 [0,1]。0 = 完整（也是默认值，此时 shader 里那条分支整个跳过），
    /// 1 = 全溶掉。噪声是世界空间的块状哈希，不是 UV —— 角色会"从身上烧过去"。
    constexpr const char* k_DissolveAmount = "_DissolveAmount";
    /// 溶解边缘的宽度（在阈值附近多宽的一段被染成边缘色，单位是噪声空间）。
    constexpr const char* k_DissolveEdgeWidth = "_DissolveEdgeWidth";

    /// ---- 向量 / 颜色名（SetVector / SetColor，整个 vec4）----

    /// 溶解边缘的颜色。rgb 是颜色，**a 是强度**：shader 里算的是 rgb * a，所以想
    /// 要纯色就传 (r, g, b, 1)，只传 rgb 会得到 a=0 也就是没有边缘。
    constexpr const char* k_DissolveEdgeColor = "_DissolveEdgeColor";

    /// 通用槽。内置 shader **不消费**这两个，写进去不会有视觉变化 —— 留给项目
    /// 自己的 shader，或者等 ShaderLab 运行时通了之后接真正的具名属性。
    constexpr const char* k_CustomParam0 = "_CustomParam0";
    constexpr const char* k_CustomParam1 = "_CustomParam1";

    /// 按名字查标量属性。查不到返回 nullptr（调用方静默忽略，不报错 —— 脚本
    /// 打错名字不该崩游戏）。
    const MaterialCustomParamLocation* FindFloat(const eastl::string& name);

    /// 按名字查向量 / 颜色属性。查不到返回 nullptr。
    const MaterialCustomParamLocation* FindVector(const eastl::string& name);

    /// 全部已注册的标量名，供编辑器下拉和报错信息用。
    const std::vector<const char*>& FloatNames();
    /// 全部已注册的向量 / 颜色名。
    const std::vector<const char*>& VectorNames();
}  // namespace MaterialCustomParams
