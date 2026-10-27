#pragma once

// ============================================================================
// UMGAnchor — 锚点/自适应布局的纯换算核心（header-only，无引擎/ZSlate 依赖）
// ----------------------------------------------------------------------------
// P1 末缺口 6：UMG 走 ZSlate flow layout，没有 Unity 的 Anchor。这里补一个"够用"
// 的锚点模型，把"widget 相对父矩形怎么摆"抽成不依赖任何 widget/渲染类型的纯函数，
// 供 UWidget::ResolveAbsoluteTopLeft 和设计器布局共用，可 headless 单测
// （UMGAnchorSmokeTest）。
//
// 模型（Unity RectTransform 的最小可用子集）：
//   - anchor_min / anchor_max：父矩形内的归一化锚点 [0,1]。min==max 是点锚（widget
//     保持自身尺寸、随锚点定位）；min!=max 是拉伸锚（widget 沿该轴填满锚点跨距）。
//   - pivot：widget 自身的参考点 [0,1]（0=左/上，1=右/下，0.5=中心）。决定"widget 的
//     哪个点对齐到锚点+偏移"。
//   - offset：像素偏移，加在锚点像素位置之后。
//
// 两条核心换算：
//   1. AnchorSpanPx：拉伸锚下，锚点跨距在父矩形里的像素矩形（给 Fill 用）。
//   2. ResolveTopLeft：点锚下，widget 左上角的绝对像素位置。
//      anchor_px = parent_pos + anchor * parent_size
//      top_left  = anchor_px + offset - pivot * size
//      这条公式统一处理四角/中心锚：anchor=(0,0),pivot=(0,0) → 贴父左上；
//      anchor=(1,1),pivot=(1,1) → 贴父右下；anchor=pivot=(0.5,0.5) → 居中。
//
// 真正把算出的绝对位置喂给 ZSlate 的 FGeometry / layout（MakeChild）属渲染绑定，
// 留真机清单，不进本文件。这里只保证"给定父矩形与锚点参数，算出的像素位置正确"。
// ============================================================================

#include <cmath>

namespace ZUMG
{
namespace layout_core
{
    struct Vec2
    {
        float x {0.0f};
        float y {0.0f};
    };

    inline float Clamp01(float v)
    {
        if (v < 0.0f) return 0.0f;
        if (v > 1.0f) return 1.0f;
        return v;
    }

    inline Vec2 Clamp01(Vec2 v) { return Vec2{Clamp01(v.x), Clamp01(v.y)}; }

    // 拉伸锚：anchor_min..anchor_max 在父尺寸里对应的像素跨距（左上角在
    // parent_pos + anchor_min*parent_size，尺寸为 (max-min)*parent_size）。
    // 返回的 Vec2 是"相对父左上角的偏移"，调用方加 parent_pos 得绝对位置。
    inline Vec2 AnchorSpanOffset(Vec2 parent_size, Vec2 anchor_min, Vec2 anchor_max)
    {
        anchor_min = Clamp01(anchor_min);
        anchor_max = Clamp01(anchor_max);
        return Vec2{
            anchor_min.x * parent_size.x,
            anchor_min.y * parent_size.y,
        };
    }

    inline Vec2 AnchorSpanSize(Vec2 parent_size, Vec2 anchor_min, Vec2 anchor_max)
    {
        anchor_min = Clamp01(anchor_min);
        anchor_max = Clamp01(anchor_max);
        return Vec2{
            (anchor_max.x - anchor_min.x) * parent_size.x,
            (anchor_max.y - anchor_min.y) * parent_size.y,
        };
    }

    // 点锚：算 widget 左上角的绝对像素位置。anchor/pivot 越界会被夹到 [0,1]。
    // size 是 widget 自身像素尺寸；offset 是像素偏移。
    inline Vec2 ResolveTopLeft(Vec2 parent_pos, Vec2 parent_size,
                               Vec2 anchor, Vec2 pivot,
                               Vec2 offset, Vec2 size)
    {
        anchor = Clamp01(anchor);
        pivot = Clamp01(pivot);
        const float anchor_px_x = parent_pos.x + anchor.x * parent_size.x;
        const float anchor_px_y = parent_pos.y + anchor.y * parent_size.y;
        return Vec2{
            anchor_px_x + offset.x - pivot.x * size.x,
            anchor_px_y + offset.y - pivot.y * size.y,
        };
    }
}  // namespace layout_core
}  // namespace ZUMG
