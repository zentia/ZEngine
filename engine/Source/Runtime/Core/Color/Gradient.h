#pragma once

// =============================================================================
// Gradient —— Unity 的 `UnityEngine.Gradient`
// =============================================================================
//
// "颜色随时间变化"的通用表达：粒子的一生、拖尾、伤害数字的上色、天空盒的昼夜
// 过渡都用它。Unity 的形状是**两条独立的键序列**：
//
//   * `colorKeys` —— 颜色键（颜色 + 时间）
//   * `alphaKeys` —— 透明度键（alpha + 时间）
//
// ⚠️ 为什么是两条而不是一条带 alpha 的颜色键：Unity 允许"颜色在 0.3 处变红、
// 透明度在 0.7 处淡出"—— 两个维度的时间点**不需要对齐**。合并成一条会强制它们
// 同步，而这在美术手里是个硬限制。
//
// 值类型是 POD 风格的 struct（与 `Color` / `Color32` / `Keyframe` 同一套），
// 跨脚本边界时是**快照**：所有"改"都返回新值，不原地改。
//
// 键的顺序
// --------
// `Evaluate` **自己排序**，不要求调用方预先排好 —— 未排序的输入是最常见的
// 来源（编辑器里拖手柄会打乱顺序），而一个"颜色突然跳变"的 bug 极难回溯到
// "键数组没排序"。排序是 O(n log n)，n 通常 <= 8。
// =============================================================================

#include "Runtime/Core/Color/Color.h"

#include <algorithm>
#include <vector>

/// Unity 的 `GradientMode`：
///
/// | 值 | 含义 |
/// |----|------|
/// | `Blend` (0) | 键之间**插值**（默认，也是绝大多数用途要的） |
/// | `Fixed` (1) | 键之间**不插值**，取左侧那个键的值（阶梯状） |
///
/// `Fixed` 是 Unity 2018+ 加的，用来做"分段上色"（血条的颜色分段、等级颜色
/// 分段）。它在两端之外仍然按端点值处理 —— 只影响键**之间**。
enum class GradientMode : int
{
    Blend = 0,
    Fixed = 1,
};

/// Unity 的 `GradientColorKey`：一个颜色 + 它所在的时间点（0..1）。
struct GradientColorKey
{
    Color color{1.0f, 1.0f, 1.0f, 1.0f};
    float time{0.0f};
};

/// Unity 的 `GradientAlphaKey`：一个 alpha + 它所在的时间点（0..1）。
///
/// ⚠️ 只有 alpha，没有颜色 —— 见文件头"两条键序列"的说明。
struct GradientAlphaKey
{
    float alpha{1.0f};
    float time{0.0f};
};

/// Unity 的 `UnityEngine.Gradient`。
struct Gradient
{
    std::vector<GradientColorKey> color_keys;
    std::vector<GradientAlphaKey> alpha_keys;
    GradientMode mode{GradientMode::Blend};

    Gradient() = default;

    /// Unity 的 `new Gradient()`：**两端都是不透明白**，不是空渐变。
    ///
    /// 空渐变（没有任何键）的 `Evaluate` 是`未定义`的 —— Unity 返回黑色，而黑色
    /// 在绝大多数用途里看起来就是"这个东西没上色"，和"键设错了"分不开。给一个
    /// 白到白的默认渐变，则"我还没设键"表现为白色，一眼可辨。
    static Gradient CreateDefault()
    {
        Gradient g;
        GradientColorKey c0;
        c0.color.r = 1.0f;
        c0.color.g = 1.0f;
        c0.color.b = 1.0f;
        c0.color.a = 1.0f;
        c0.time = 0.0f;
        GradientColorKey c1 = c0;
        c1.time = 1.0f;
        g.color_keys.push_back(c0);
        g.color_keys.push_back(c1);
        GradientAlphaKey a0;
        a0.alpha = 1.0f;
        a0.time = 0.0f;
        GradientAlphaKey a1 = a0;
        a1.time = 1.0f;
        g.alpha_keys.push_back(a0);
        g.alpha_keys.push_back(a1);
        return g;
    }

    /// 按 `time`（0..1）求值。
    ///
    /// 三条约定（都与 Unity 一致）：
    ///   * **两端之外按端点值**：`Evaluate(-5)` 与 `Evaluate(0)` 同，
    ///     `Evaluate(9)` 与 `Evaluate(1)` 同。循环要用 `Mathf.Repeat` 自己把
    ///     time 折回来。
    ///   * **键未排序也能用** —— 内部先排序（见文件头）。
    ///   * **没有键**时：颜色取白、alpha 取 1。返回黑色会让"忘了设键"看起来像
    ///     "设了个黑色"。
    Color Evaluate(float time) const;
};

inline Color Gradient::Evaluate(float time) const
{
    // ---- alpha ----
    std::vector<GradientAlphaKey> alphas = alpha_keys;
    std::sort(alphas.begin(), alphas.end(),
              [](const GradientAlphaKey& a, const GradientAlphaKey& b) { return a.time < b.time; });

    float alpha = 1.0f;
    if (!alphas.empty())
    {
        if (time <= alphas.front().time)
        {
            alpha = alphas.front().alpha;
        }
        else if (time >= alphas.back().time)
        {
            alpha = alphas.back().alpha;
        }
        else
        {
            for (std::size_t i = 0; i + 1 < alphas.size(); ++i)
            {
                const GradientAlphaKey& lo = alphas[i];
                const GradientAlphaKey& hi = alphas[i + 1];
                if (time >= lo.time && time <= hi.time)
                {
                    const float span = hi.time - lo.time;
                    const float t = (span > 0.0f) ? (time - lo.time) / span : 0.0f;
                    // ⚠️ alpha 键**永远插值**，不受 `mode` 影响 —— Unity 的
                    // `GradientMode.Fixed` 只作用于颜色键。改这条会让"分段上色"
                    // 的渐变透明度也跟着跳。
                    alpha = lo.alpha + (hi.alpha - lo.alpha) * t;
                    break;
                }
            }
        }
    }

    // ---- 颜色 ----
    std::vector<GradientColorKey> colors = color_keys;
    std::sort(colors.begin(), colors.end(),
              [](const GradientColorKey& a, const GradientColorKey& b) { return a.time < b.time; });

    Color rgb;
    rgb.r = 1.0f;
    rgb.g = 1.0f;
    rgb.b = 1.0f;
    rgb.a = 1.0f;
    if (!colors.empty())
    {
        if (time <= colors.front().time)
        {
            rgb = colors.front().color;
        }
        else if (time >= colors.back().time)
        {
            rgb = colors.back().color;
        }
        else
        {
            for (std::size_t i = 0; i + 1 < colors.size(); ++i)
            {
                const GradientColorKey& lo = colors[i];
                const GradientColorKey& hi = colors[i + 1];
                if (time >= lo.time && time <= hi.time)
                {
                    const float span = hi.time - lo.time;
                    float t = (span > 0.0f) ? (time - lo.time) / span : 0.0f;
                    if (mode == GradientMode::Fixed)
                    {
                        // 阶梯：取左侧那个键的值。用 `round` 而不是 `floor` 的
                        // 话，"刚好在两个键中间"会跳到右侧 —— 那不是 Fixed 的
                        // 意思（Fixed = "保持到下一个键为止"）。
                        t = 0.0f;
                    }
                    rgb.r = lo.color.r + (hi.color.r - lo.color.r) * t;
                    rgb.g = lo.color.g + (hi.color.g - lo.color.g) * t;
                    rgb.b = lo.color.b + (hi.color.b - lo.color.b) * t;
                    rgb.a = lo.color.a + (hi.color.a - lo.color.a) * t;
                    break;
                }
            }
        }
    }

    // alpha 键覆盖颜色键里自带的 alpha：这是"两条独立键序列"的必然结论 ——
    // alpha 由 alphaKeys 单独决定，颜色键里的 a 分量不参与。
    rgb.a = alpha;
    return rgb;
}
