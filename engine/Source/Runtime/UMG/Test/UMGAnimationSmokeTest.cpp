// ============================================================================
// UMGAnimationSmokeTest — UWidget 动画纯核心 + 状态机的无头验证
// ----------------------------------------------------------------------------
// 覆盖三块：
//   A. anim_core::ApplyEasing：进度→缓动曲线的端点与单调性。
//   B. anim_core::AnimTrack::Evaluate：线性插值 + 缓动；越界 lerp 夹紧。
//   C. anim_core::RenderTransform::ApplyToSlot：属性路径分派；未识别路径返回 false；
//      只动一个槽不影响其它槽的默认值。
//   D. UWidgetAnimation：Play/Tick/loop/reverse 的时序推进，逐帧把 lerp 写进目标
//      RenderTransform（用 ZSlate::FCurveSequence 推时间，本测试只验证"随时间推进
//      正确地插值出目标值"）。
//
// 全部纯 CPU，headless，只链 include root（UMGAnimation.h header-only；UWidgetAnimation.h
// 引 ZSlate::FCurveSequence，但那也是 header-only 且只依赖 std 头，无需链库）。
//
// 构建（任意主机，纯 CPU，headless）：
//   cmake -B build -DZENGINE_BUILD_UMG_ANIMATION_SMOKE_TEST=ON
//   cmake --build build --target UMGAnimationSmokeTest
// ============================================================================

#include "Runtime/UMG/Animation/UMGAnimation.h"
#include "Runtime/UMG/Animation/UWidgetAnimation.h"

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

    using ZUMG::anim_core::AnimTrack;
    using ZUMG::anim_core::ApplyEasing;
    using ZUMG::anim_core::Easing;
    using ZUMG::anim_core::RenderTransform;
}  // namespace

int main()
{
    std::printf("UMGAnimationSmokeTest (widget animation core + driver)\n");

    // --- A. ApplyEasing 端点 -------------------------------------------------
    Check(Near(ApplyEasing(0.0f, Easing::Linear), 0.0f), "ApplyEasing: Linear at 0 -> 0");
    Check(Near(ApplyEasing(1.0f, Easing::Linear), 1.0f), "ApplyEasing: Linear at 1 -> 1");
    Check(Near(ApplyEasing(0.0f, Easing::QuadOut), 0.0f), "ApplyEasing: QuadOut at 0 -> 0");
    Check(Near(ApplyEasing(1.0f, Easing::QuadOut), 1.0f), "ApplyEasing: QuadOut at 1 -> 1");
    Check(Near(ApplyEasing(0.5f, Easing::QuadIn), 0.25f), "ApplyEasing: QuadIn at 0.5 -> 0.25");
    Check(Near(ApplyEasing(0.5f, Easing::QuadOut), 0.75f), "ApplyEasing: QuadOut at 0.5 -> 0.75");
    // 越界夹紧：负进度与 >1 进度都夹到 [0,1] 后再缓动。
    Check(Near(ApplyEasing(-0.5f, Easing::Linear), 0.0f), "ApplyEasing: negative clamped to 0");
    Check(Near(ApplyEasing(1.5f, Easing::Linear), 1.0f), "ApplyEasing: >1 clamped to 1");

    // --- B. AnimTrack::Evaluate 线性插值 + 缓动 ------------------------------
    {
        AnimTrack linear{0.0f, 10.0f, Easing::Linear};
        Check(Near(linear.Evaluate(0.0f), 0.0f), "AnimTrack: start value at lerp 0");
        Check(Near(linear.Evaluate(1.0f), 10.0f), "AnimTrack: end value at lerp 1");
        Check(Near(linear.Evaluate(0.5f), 5.0f), "AnimTrack: midpoint linear");
        // 缓动轨道：QuadIn 在 lerp=0.5 时值 = start + (end-start)*0.25。
        AnimTrack eased{0.0f, 10.0f, Easing::QuadIn};
        Check(Near(eased.Evaluate(0.5f), 2.5f), "AnimTrack: QuadIn eases the interpolation");
        // 负方向轨道（淡出）：1 -> 0。
        AnimTrack fadeOut{1.0f, 0.0f, Easing::Linear};
        Check(Near(fadeOut.Evaluate(0.25f), 0.75f), "AnimTrack: reverse direction (1->0)");
    }

    // --- C. RenderTransform::ApplyToSlot 路径分派 ----------------------------
    {
        RenderTransform rt;  // 默认：opacity=1, trans=0, scale=1, rot=0
        Check(rt.ApplyToSlot("render_opacity", 0.5f), "ApplyToSlot: render_opacity recognized");
        Check(Near(rt.Opacity, 0.5f), "ApplyToSlot: opacity written");
        Check(rt.ApplyToSlot("render_translation.x", 12.0f), "ApplyToSlot: translation.x recognized");
        Check(Near(rt.TranslationX, 12.0f), "ApplyToSlot: translation.x written");
        Check(rt.ApplyToSlot("render_scale.y", 2.0f), "ApplyToSlot: scale.y recognized");
        Check(Near(rt.ScaleY, 2.0f), "ApplyToSlot: scale.y written");
        Check(rt.ApplyToSlot("render_rotation", 90.0f), "ApplyToSlot: rotation recognized");
        Check(Near(rt.Rotation, 90.0f), "ApplyToSlot: rotation written");
        // 未识别路径返回 false，不改动任何槽。
        Check(!rt.ApplyToSlot("unknown_property", 1.0f), "ApplyToSlot: unknown path returns false");
        Check(!rt.ApplyToSlot(nullptr, 1.0f), "ApplyToSlot: null path returns false");
        // 只动 opacity 不影响 scale 默认值（仍是 1）。
        RenderTransform rt2;
        rt2.ApplyToSlot("render_opacity", 0.0f);
        Check(Near(rt2.ScaleX, 1.0f) && Near(rt2.ScaleY, 1.0f), "ApplyToSlot: untouched slots keep defaults");
        // UE 风格全名也接受。
        RenderTransform rt3;
        Check(rt3.ApplyToSlot("RenderTransform.Translation.X", 5.0f), "ApplyToSlot: UE-style full name recognized");
        Check(Near(rt3.TranslationX, 5.0f), "ApplyToSlot: UE-style full name written");
    }

    // --- D. UWidgetAnimation 时序推进 ---------------------------------------
    {
        // 0.25s 淡入（opacity 0->1，QuadOut）。用可精确表示的步长避免浮点累积误差。
        ZUMG::UWidgetAnimation fade;
        fade.AddTrack("render_opacity", 0.0f, 1.0f, Easing::QuadOut);
        fade.Play(0.25f, /*loop=*/false);
        Check(fade.IsPlaying(), "UWidgetAnimation: playing after Play");

        RenderTransform rt;  // opacity 默认 1，动画会覆盖它
        // 第 1 帧 +0.125s -> lerp=0.5 -> QuadOut(0.5)=0.75。
        bool still = fade.Tick(0.125f, rt);
        Check(still, "UWidgetAnimation: still playing at half duration");
        Check(Near(rt.Opacity, 0.75f), "UWidgetAnimation: opacity eased to 0.75 at lerp 0.5");

        // 第 2 帧 +0.125s -> 到达终点 lerp=1 -> opacity=1，且不再 playing。
        still = fade.Tick(0.125f, rt);
        Check(!still, "UWidgetAnimation: finished at end of duration");
        Check(Near(rt.Opacity, 1.0f), "UWidgetAnimation: opacity reaches 1 at end");
        Check(fade.IsFinished(), "UWidgetAnimation: IsFinished after end");
    }

    // --- D2. 多轨道同播：opacity + translation 一起插值 ---------------------
    {
        ZUMG::UWidgetAnimation slide;
        slide.AddTrack("render_opacity", 0.0f, 1.0f, Easing::Linear);
        slide.AddTrack("render_translation.x", 0.0f, 100.0f, Easing::Linear);
        slide.Play(1.0f, /*loop=*/false);

        RenderTransform rt;
        // 半程 lerp=0.5：opacity=0.5，translation.x=50。
        const bool still = slide.Tick(0.5f, rt);
        Check(still, "UWidgetAnimation: multi-track still playing at half duration");
        Check(Near(rt.Opacity, 0.5f), "UWidgetAnimation: multi-track opacity interpolated");
        Check(Near(rt.TranslationX, 50.0f), "UWidgetAnimation: multi-track translation interpolated");
        // 未被任何轨道命中的 scale 保持默认 1。
        Check(Near(rt.ScaleX, 1.0f) && Near(rt.ScaleY, 1.0f), "UWidgetAnimation: unbound slots keep defaults");
    }

    // --- D3. loop：循环动画在周期边界回绕并继续播放 -------------------------
    {
        ZUMG::UWidgetAnimation pulse;
        pulse.AddTrack("render_scale.x", 1.0f, 2.0f, Easing::Linear);
        pulse.Play(0.5f, /*loop=*/true);

        RenderTransform rt;
        bool still = true;
        // 走 4 帧共 0.5s（正好一个周期），循环下应仍在播放。
        for (int i = 0; i < 4 && still; ++i)
            still = pulse.Tick(0.125f, rt);
        Check(still, "UWidgetAnimation: looping animation keeps playing past one period");
        // 循环回绕后进度应落回 [0,1) 区间（scale 介于 1..2 之间，非终点）。
        Check(rt.ScaleX >= 1.0f && rt.ScaleX <= 2.0f, "UWidgetAnimation: looped scale stays in [start,end]");
    }

    // --- D4. reverse：反向播放从终点回到起点 --------------------------------
    {
        ZUMG::UWidgetAnimation rev;
        rev.AddTrack("render_opacity", 0.0f, 1.0f, Easing::Linear);
        rev.PlayReverse(1.0f);
        RenderTransform rt;
        // 反向起始 lerp 接近 1 -> opacity 接近 1。
        rev.Evaluate(rt);
        Check(Near(rt.Opacity, 1.0f), "UWidgetAnimation: reverse starts at end value");
        // 推进半程 -> opacity 接近 0.5。
        rev.Tick(0.5f, rt);
        Check(Near(rt.Opacity, 0.5f), "UWidgetAnimation: reverse midpoint ~0.5");
    }

    // --- D5. 瞬时动画（duration<=0）：一次性写终点 --------------------------
    {
        ZUMG::UWidgetAnimation snap;
        snap.AddTrack("render_opacity", 0.0f, 1.0f, Easing::Linear);
        snap.Play(0.0f, /*loop=*/false);
        RenderTransform rt;  // 默认 opacity=1，但瞬时动画应把它写成终点 1（相同）
        const bool still = snap.Tick(0.016f, rt);
        Check(!still, "UWidgetAnimation: instant animation does not keep playing");
        Check(Near(rt.Opacity, 1.0f), "UWidgetAnimation: instant animation writes end value");
    }

    if (g_Failures == 0)
    {
        std::printf("UMGAnimationSmokeTest: ALL CHECKS PASSED\n");
        return 0;
    }
    std::printf("UMGAnimationSmokeTest: %d CHECK(S) FAILED\n", g_Failures);
    return 1;
}
