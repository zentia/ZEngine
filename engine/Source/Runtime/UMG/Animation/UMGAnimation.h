#pragma once

// ============================================================================
// UMGAnimation — UWidget 动画的纯逻辑核心（header-only，无引擎/ZSlate 依赖）
// ----------------------------------------------------------------------------
// P1 动画里"可本地验证"的那一半：给定一个归一化进度 lerp∈[0,1]，把每条属性轨道
// 插值出目标值，并按属性路径写入一个 RenderTransform。这一层不碰渲染器、不碰
// ZSlate 的 paint 链，所以能 headless 单测（UMGAnimationSmokeTest）。
//
// 分三块，全部纯函数 / 纯状态：
//   1. Easing：进度→缓动曲线（Linear/Quad/Cubic/…），与 ZSlate::ApplyEasing 同义，
//      但这里自带一份，测试无需引入 ZSlate include 链。
//   2. AnimTrack：一条属性轨道 = 起值/终值 + 缓动，Evaluate(lerp) 线性插值出当前值。
//   3. RenderTransform：动画的作用对象（UE UWidget::RenderTransform / RenderOpacity
//      的对应物），带一组命名槽位；ApplyToSlot 按属性路径名把一个值写进对应槽。
//
// 真正把 RenderTransform 画到屏幕（opacity 乘进 tint / transform 变几何）属于
// ZSlate submodule 的 paint 链改造，是 P1 里"不可本地验证"的另一半，落到
// RUNTIME_UI_IMPLEMENTATION_PLAN.md 的真机清单，不进本文件。
//
// 时序状态机（Play/Stop/Tick/loop/reverse）由 UWidgetAnimation（见下）持有，它内部
// 复用 ZSlate::FCurveSequence 的推进逻辑；本文件只负责"拿到 lerp 之后算什么"，
// 这样插值/路径分派与时间推进正交，各自可独立验证。
// ============================================================================

#include <cmath>

namespace ZUMG
{
namespace anim_core
{
    // 缓动类型。与 ZSlate::EEasingType 同名同序，便于上层直通；这里自带实现，
    // 让纯核心与测试都不必拉 ZSlate 头。
    enum class Easing
    {
        Linear,
        QuadIn, QuadOut, QuadInOut,
        CubicIn, CubicOut, CubicInOut,
        SineInOut,
        ExpoOut,
    };

    // 把归一化进度 t∈[0,1] 映射成缓动后的进度。t 越界会被夹紧。
    inline float ApplyEasing(float t, Easing type)
    {
        if (t < 0.0f) t = 0.0f;
        else if (t > 1.0f) t = 1.0f;
        switch (type)
        {
        case Easing::Linear:     return t;
        case Easing::QuadIn:     return t * t;
        case Easing::QuadOut:    return t * (2.0f - t);
        case Easing::QuadInOut:  return t < 0.5f ? 2.0f * t * t : -1.0f + (4.0f - 2.0f * t) * t;
        case Easing::CubicIn:    return t * t * t;
        case Easing::CubicOut:   { const float f = t - 1.0f; return f * f * f + 1.0f; }
        case Easing::CubicInOut: return t < 0.5f ? 4.0f * t * t * t
                                     : (t - 1.0f) * (2.0f * t - 2.0f) * (2.0f * t - 2.0f) + 1.0f;
        case Easing::SineInOut:  return 0.5f * (1.0f - std::cos(3.14159265f * t));
        case Easing::ExpoOut:    return (t >= 1.0f) ? 1.0f : 1.0f - std::pow(2.0f, -10.0f * t);
        default:                 return t;
        }
    }

    // 一条属性轨道：从 StartValue 缓动到 EndValue。Evaluate 接收"未缓动的归一化
    // 进度 lerp∈[0,1]"，内部先缓动再插值，返回该轨道当前的目标值。
    struct AnimTrack
    {
        float StartValue {0.0f};
        float EndValue {1.0f};
        Easing Easing_ {Easing::Linear};

        float Evaluate(float lerp) const
        {
            const float e = ApplyEasing(lerp, Easing_);
            return StartValue + (EndValue - StartValue) * e;
        }
    };

    // 动画的作用对象：UE UWidget 的 RenderOpacity + RenderTransform 的对应物。
    // 所有字段都有"未动画时的默认值"（opacity=1、平移=0、缩放=1、旋转=0），
    // 动画只覆盖被绑定轨道命中的槽，其余槽保持默认——这样一条只动 opacity 的
    // 动画不会把 scale 意外归零。
    struct RenderTransform
    {
        float Opacity {1.0f};
        float TranslationX {0.0f};
        float TranslationY {0.0f};
        float ScaleX {1.0f};
        float ScaleY {1.0f};
        float Rotation {0.0f};  // degrees, clockwise

        // 把 value 写进 property_path 命名的槽位。命中返回 true，未识别的路径返回
        // false（调用方据此可报错或忽略）。属性路径用点号区分子字段，与 UE 的
        // "RenderTransform.Translation.X" 风格一致，但这里收敛成扁平短名以简化
        // 序列化与查表。
        bool ApplyToSlot(const char* property_path, float value)
        {
            switch (SlotIndex(property_path))
            {
            case Slot::Opacity:     Opacity = value; return true;
            case Slot::TranslationX: TranslationX = value; return true;
            case Slot::TranslationY: TranslationY = value; return true;
            case Slot::ScaleX:      ScaleX = value; return true;
            case Slot::ScaleY:      ScaleY = value; return true;
            case Slot::Rotation:    Rotation = value; return true;
            case Slot::Unknown:     default: return false;
            }
        }

        enum class Slot { Opacity, TranslationX, TranslationY, ScaleX, ScaleY, Rotation, Unknown };

        // 属性路径 → 槽位。独立成函数便于测试覆盖路径解析本身。
        static Slot SlotIndex(const char* property_path)
        {
            if (!property_path) return Slot::Unknown;
            // 常用短名 + UE 风格全名都接受。
            struct Entry { const char* name; Slot slot; };
            static constexpr Entry kTable[] = {
                { "render_opacity",              Slot::Opacity },
                { "RenderOpacity",               Slot::Opacity },
                { "render_translation.x",        Slot::TranslationX },
                { "render_translation.y",        Slot::TranslationY },
                { "RenderTransform.Translation.X", Slot::TranslationX },
                { "RenderTransform.Translation.Y", Slot::TranslationY },
                { "render_scale.x",              Slot::ScaleX },
                { "render_scale.y",              Slot::ScaleY },
                { "RenderTransform.Scale.X",     Slot::ScaleX },
                { "RenderTransform.Scale.Y",     Slot::ScaleY },
                { "render_rotation",             Slot::Rotation },
                { "RenderTransform.Angle",       Slot::Rotation },
            };
            for (const Entry& e : kTable)
            {
                const char* a = e.name;
                const char* b = property_path;
                while (*a && *b && *a == *b) { ++a; ++b; }
                if (*a == '\0' && *b == '\0') return e.slot;
            }
            return Slot::Unknown;
        }
    };
}  // namespace anim_core
}  // namespace ZUMG
