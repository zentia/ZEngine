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

// =========================================================================
// 材质里的 Virtual Texture 槽位（base color 经 VT 采样）。
//
// 不增加 MeshMaterialUniform 的大小（仍是 128 字节，ShaderLab 属性块与它同尺寸）：
//   · `vt_flags`（原 `_padding_0`，offset 52）：位 0 = 启用 VT base color，位 1 = 采样结果按 sRGB 解码；
//   · `customParams[2]` / `customParams[3]` 在 vt_flags 位 0 置位时**被 VT 占用**，按 uint 位模式存放：
//        customParams[2] = { page_size, page_border, mip_count, 0 }
//        customParams[3] = { virtual_width, virtual_height, 0, 0 }
//     所以启用 VT 的材质上，脚本的 `_CustomParam0/1` 不再生效（它们本来就是「项目自留，shader 不解释」）。
// 页池宽度（页数）、页表尺寸不用传：shader 用 GetDimensions 自己取。
//
// 页表和页池是两个图像，走材质描述符集的 binding 6（页表）/ 7（页池，LINEAR+clamp 采样器）——仅 DX12 的
// 网格材质布局有这两个 binding。每帧重写（页池/页表可能重建，DX12 的 SRV 也是按帧循环的）。
// =========================================================================
constexpr uint32_t k_MaterialVtFlag_BaseColor = 1u << 0;
constexpr uint32_t k_MaterialVtFlag_Srgb = 1u << 1;
constexpr uint32_t k_MaterialVtBinding_PageTable = 6;
constexpr uint32_t k_MaterialVtBinding_Pool = 7;
constexpr uint32_t k_MaterialVtCustomParamSlot_Layout = 2;  // customParams[2]
constexpr uint32_t k_MaterialVtCustomParamSlot_Extent = 3;  // customParams[3]

/// 一个材质的 VT 槽位内容。`enabled = false` 时材质的行为与没有这个功能完全一样。
struct MaterialVirtualTextureSlot
{
    bool enabled {false};
    bool srgb {true};
    uint32_t page_size {0};
    uint32_t page_border {0};
    uint32_t mip_count {0};
    uint32_t virtual_width {0};
    uint32_t virtual_height {0};

    bool operator==(const MaterialVirtualTextureSlot& other) const
    {
        return enabled == other.enabled && srgb == other.srgb && page_size == other.page_size &&
               page_border == other.page_border && mip_count == other.mip_count &&
               virtual_width == other.virtual_width && virtual_height == other.virtual_height;
    }
    bool operator!=(const MaterialVirtualTextureSlot& other) const { return !(*this == other); }
};
