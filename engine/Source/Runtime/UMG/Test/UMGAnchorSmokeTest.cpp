// ============================================================================
// UMGAnchorSmokeTest — 锚点/自适应布局纯换算核心的无头验证
// ----------------------------------------------------------------------------
// 覆盖 layout_core（UMGAnchor.h）：
//   - Clamp01：越界夹紧。
//   - AnchorSpanOffset / AnchorSpanSize：拉伸锚下锚点跨距的像素偏移与尺寸。
//   - ResolveTopLeft：点锚下 widget 左上角绝对像素位置——四角锚、中心锚、
//     负偏移、越界 anchor/pivot 夹紧、非原点父矩形。
//
// 全部纯 CPU，headless，只链 include root（UMGAnchor.h header-only，只依赖 <cmath>）。
//
// 构建（任意主机，纯 CPU，headless）：
//   cmake -B build -DZENGINE_BUILD_UMG_ANCHOR_SMOKE_TEST=ON
//   cmake --build build --target UMGAnchorSmokeTest
// ============================================================================

#include "Runtime/UMG/Layout/UMGAnchor.h"

#include <cmath>
#include <cstdio>

namespace
{
    int g_Failures = 0;

    void Check(bool cond, const char* msg)
    {
        if (cond)
        {
            std::printf("  [ ok ] %s\n", msg);
        }
        else
        {
            std::printf("  [FAIL] %s\n", msg);
            ++g_Failures;
        }
    }

    bool Near(float a, float b, float eps = 1e-4f)
    {
        return std::fabs(a - b) <= eps;
    }

    using ZUMG::layout_core::AnchorSpanOffset;
    using ZUMG::layout_core::AnchorSpanSize;
    using ZUMG::layout_core::Clamp01;
    using ZUMG::layout_core::ResolveTopLeft;
    using ZUMG::layout_core::Vec2;
}  // namespace

int main()
{
    std::printf("UMGAnchorSmokeTest (anchor / adaptive layout core)\n");

    // --- Clamp01 -------------------------------------------------------------
    Check(Near(Clamp01(-0.5f), 0.0f), "Clamp01: negative -> 0");
    Check(Near(Clamp01(0.25f), 0.25f), "Clamp01: in-range unchanged");
    Check(Near(Clamp01(1.5f), 1.0f), "Clamp01: >1 -> 1");

    // --- AnchorSpanOffset / AnchorSpanSize（拉伸锚）--------------------------
    {
        Vec2 parent{1000.0f, 600.0f};
        // 全屏拉伸锚 (0,0)-(1,1)：偏移=父左上角，尺寸=父尺寸。
        Vec2 off = AnchorSpanOffset(parent, {0.0f, 0.0f}, {1.0f, 1.0f});
        Vec2 sz = AnchorSpanSize(parent, {0.0f, 0.0f}, {1.0f, 1.0f});
        Check(Near(off.x, 0.0f) && Near(off.y, 0.0f), "AnchorSpan: full-stretch offset at origin");
        Check(Near(sz.x, 1000.0f) && Near(sz.y, 600.0f), "AnchorSpan: full-stretch size = parent size");
        // 顶部条 (0,0)-(1,0.1)：高 = 60px。
        Vec2 barSz = AnchorSpanSize(parent, {0.0f, 0.0f}, {1.0f, 0.1f});
        Check(Near(barSz.x, 1000.0f) && Near(barSz.y, 60.0f), "AnchorSpan: top-bar span (full width, 10% height)");
        // 右下角点锚 (1,1)-(1,1)：跨距尺寸为 0。
        Vec2 ptSz = AnchorSpanSize(parent, {1.0f, 1.0f}, {1.0f, 1.0f});
        Check(Near(ptSz.x, 0.0f) && Near(ptSz.y, 0.0f), "AnchorSpan: point anchor span size = 0");
    }

    // --- ResolveTopLeft：四角锚 ----------------------------------------------
    {
        Vec2 parentPos{0.0f, 0.0f};
        Vec2 parentSize{1000.0f, 600.0f};
        Vec2 size{200.0f, 100.0f};

        // 左上锚 (0,0), pivot (0,0), offset 0 → 贴父左上角。
        Vec2 tl = ResolveTopLeft(parentPos, parentSize, {0.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}, size);
        Check(Near(tl.x, 0.0f) && Near(tl.y, 0.0f), "ResolveTopLeft: top-left anchor pins to parent top-left");

        // 右下锚 (1,1), pivot (1,1), offset 0 → widget 右下角贴父右下角。
        Vec2 br = ResolveTopLeft(parentPos, parentSize, {1.0f, 1.0f}, {1.0f, 1.0f}, {0.0f, 0.0f}, size);
        Check(Near(br.x, 800.0f) && Near(br.y, 500.0f), "ResolveTopLeft: bottom-right anchor pins widget's bottom-right to parent's");

        // 右上锚 (1,0), pivot (1,0) → 贴父右上角。
        Vec2 tr = ResolveTopLeft(parentPos, parentSize, {1.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 0.0f}, size);
        Check(Near(tr.x, 800.0f) && Near(tr.y, 0.0f), "ResolveTopLeft: top-right anchor pins to parent top-right");

        // 左下锚 (0,1), pivot (0,1) → 贴父左下角。
        Vec2 bl = ResolveTopLeft(parentPos, parentSize, {0.0f, 1.0f}, {0.0f, 1.0f}, {0.0f, 0.0f}, size);
        Check(Near(bl.x, 0.0f) && Near(bl.y, 500.0f), "ResolveTopLeft: bottom-left anchor pins to parent bottom-left");
    }

    // --- ResolveTopLeft：中心锚 ----------------------------------------------
    {
        Vec2 parentPos{0.0f, 0.0f};
        Vec2 parentSize{1000.0f, 600.0f};
        Vec2 size{200.0f, 100.0f};
        // anchor (0.5,0.5), pivot (0.5,0.5) → widget 中心对齐父中心。
        // 中心 = (500,300)；左上角 = 中心 - size/2 = (400,250)。
        Vec2 c = ResolveTopLeft(parentPos, parentSize, {0.5f, 0.5f}, {0.5f, 0.5f}, {0.0f, 0.0f}, size);
        Check(Near(c.x, 400.0f) && Near(c.y, 250.0f), "ResolveTopLeft: center anchor centers widget in parent");
    }

    // --- ResolveTopLeft：偏移 ------------------------------------------------
    {
        Vec2 parentPos{0.0f, 0.0f};
        Vec2 parentSize{1000.0f, 600.0f};
        Vec2 size{200.0f, 100.0f};
        // 左上锚 + 偏移 (16, 24) → 左上角 = (16,24)。
        Vec2 p = ResolveTopLeft(parentPos, parentSize, {0.0f, 0.0f}, {0.0f, 0.0f}, {16.0f, 24.0f}, size);
        Check(Near(p.x, 16.0f) && Near(p.y, 24.0f), "ResolveTopLeft: positive offset shifts top-left");
        // 右下锚 + 负偏移 (-16,-24) → 从父右下角往内缩。
        Vec2 q = ResolveTopLeft(parentPos, parentSize, {1.0f, 1.0f}, {1.0f, 1.0f}, {-16.0f, -24.0f}, size);
        Check(Near(q.x, 800.0f - 16.0f) && Near(q.y, 500.0f - 24.0f), "ResolveTopLeft: negative offset insets from parent bottom-right");
    }

    // --- ResolveTopLeft：越界 anchor/pivot 夹紧 ------------------------------
    {
        Vec2 parentPos{0.0f, 0.0f};
        Vec2 parentSize{1000.0f, 600.0f};
        Vec2 size{200.0f, 100.0f};
        // anchor 越界 (1.5, -0.5) 夹到 (1,0)；pivot 越界 (2,2) 夹到 (1,1)。
        // = 右上锚 + pivot 右下：x = 1000 - 200 = 800；y = 0 - 100 = -100。
        Vec2 p = ResolveTopLeft(parentPos, parentSize, {1.5f, -0.5f}, {2.0f, 2.0f}, {0.0f, 0.0f}, size);
        Check(Near(p.x, 800.0f), "ResolveTopLeft: out-of-range anchor.x/pivot clamped");
        Check(Near(p.y, -100.0f), "ResolveTopLeft: out-of-range anchor.y/pivot clamped");
    }

    // --- ResolveTopLeft：非原点父矩形 ----------------------------------------
    {
        // 父矩形不在原点（比如父 widget 已被摆在屏幕 (120,80) 处）。
        Vec2 parentPos{120.0f, 80.0f};
        Vec2 parentSize{1000.0f, 600.0f};
        Vec2 size{200.0f, 100.0f};
        Vec2 c = ResolveTopLeft(parentPos, parentSize, {0.5f, 0.5f}, {0.5f, 0.5f}, {0.0f, 0.0f}, size);
        // 绝对中心 = (120+500, 80+300) = (620,380)；左上角 = 中心 - size/2。
        Check(Near(c.x, 120.0f + 400.0f) && Near(c.y, 80.0f + 250.0f), "ResolveTopLeft: non-origin parent position propagates");
    }

    if (g_Failures == 0)
    {
        std::printf("UMGAnchorSmokeTest: ALL CHECKS PASSED\n");
        return 0;
    }
    std::printf("UMGAnchorSmokeTest: %d CHECK(S) FAILED\n", g_Failures);
    return 1;
}
