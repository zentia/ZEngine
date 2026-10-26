#pragma once

#include <cstdint>

// =========================================================================
// 运行时属性块（Unity MaterialPropertyBlock 那一层）的槽位约定。
//
// 单独一个 header 是因为三个地方都要知道"有几个槽、哪个槽是干什么的"，而它们
// 不方便互相 include：
//   - RenderCommon.h（MeshMaterialUniform，UBO 本体）
//   - RenderObject.h（GameObjectMaterialDesc，跨渲染边界的描述）
//   - RenderEntity.h（RenderEntity，场景侧）
// RenderCommon.h 很重（拉进 RHI + RenderType），拖进 RenderObject.h 有循环
// include 的风险。所以这里只放常量，不 include 任何东西。
// =========================================================================

/// 材质 UBO 里自定义属性槽的个数（`MeshMaterialUniform::customParams`）。
///
/// 4 个 vec4 = 16 个标量。这是"够用"和"UBO 别太大"之间的一个折中：UBO 是
/// per-part 传的，加到 8 个 vec4 就是给每个带 override 的 part 多传 64 字节。
/// 要更多槽位改这一个常量即可（三套 shader 侧的数组长度同步改）。
constexpr uint32_t k_MaterialCustomParamCount = 4;

/// 内置 shader 消费 `customParams` 的约定。脚本不直接碰下标，走名字 —— 名字到
/// (槽位, 分量) 的翻译表在 Function/Framework/Component/Mesh/MaterialCustomParamNames.h。
///
/// 槽 0：溶解。`_DISSOLVE_ON` 打开时 [0].x = 阈值、[0].y = 边缘宽度。
/// 槽 1：溶解边缘颜色（`_DISSOLVE_ON`）。
/// 槽 2 / 3：通用槽，内置 shader **不消费**，留给项目自己的 shader / 等 ShaderLab
///           运行时通了之后接真正的具名属性。脚本可以用 `_CustomParam0/1` 读写，
///           但当前不会有任何视觉变化。
constexpr uint32_t k_MaterialCustomParamSlot_Dissolve = 0;
constexpr uint32_t k_MaterialCustomParamSlot_DissolveEdgeColor = 1;
constexpr uint32_t k_MaterialCustomParamSlot_Custom0 = 2;
constexpr uint32_t k_MaterialCustomParamSlot_Custom1 = 3;

/// 一个 vec4 槽里的分量数。四个约定的标量名各占一个分量。
constexpr uint32_t k_MaterialCustomParamComponents = 4;
