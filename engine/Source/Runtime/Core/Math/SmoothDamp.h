#pragma once

#include "Runtime/Core/Math/Vector3.h"

#include <algorithm>
#include <cmath>

// =============================================================================
// SmoothDamp —— Unity 的 `Mathf.SmoothDamp` / `Vector3.SmoothDamp`
// =============================================================================
//
// 一个"弹簧-阻尼"式的渐进：不需要自己记时间，也不需要手动积分，给
// `smoothTime`（大致"到达目标所需的时间"）就能得到不抖的跟随。
//
// ⚠️ 为什么要有**结果结构**而不是直接返回值
// ------------------------------------------
// Unity 的签名是 `SmoothDamp(current, target, ref currentVelocity, smoothTime)`：
// 速度是**跨帧状态**，用 `ref` 传进传出。TS 既没有 `ref` 也没有 `out`，而且
// ZEngine 的值类型跨边界时是**快照**（改一个传进来的 `{value, velocity}` 不会
// 写回调用方）。所以只能把"新值 + 新速度"**一起**返回：
//
//     const r = Mathf.SmoothDamp(pos, target, vel, 0.3, 0, dt);
//     pos = r.value;
//     vel = r.velocity;      // ⚠️ 忘了这行 = 每帧从静止重新开始 = 永远到不了
//
// 这与 `AnimationCurve.AddKey` 那个坑同源：**不接住返回值是静默的**。
// 这里多一层危险：只接 `value` 不接 `velocity` 看着"在动"，只是永远慢一拍。
//
// ⚠️ `exp` 是 Unity 的**多项式近似**，不是 `std::exp`
// ----------------------------------------------------
// Unity 的 SmoothDamp 用的是
//
//     exp = 1 / (1 + x + 0.48x² + 0.235x³)      （x = omega * deltaTime）
//
// 而不是 `std::exp(-x)`。两者在 x 小的时候非常接近，但**不相等**，而
// `smoothTime` 很小（x 很大）时差得明显。照抄 Unity 的近似，否则同一份
// `smoothTime` 在 ZEngine 里与在 Unity 里不是同一条曲线 —— 而它仍然是"平滑
// 的"，看不出错了。
//
// ⚠️ 过冲守卫
// ------------
// 最后那段"如果已经越过了目标就钉在目标上"不是可有可无的收尾：少了它，
// `deltaTime` 大 / `smoothTime` 小时输出会**绕着目标来回抖**，而它仍然是平滑
// 的、仍然收敛 —— 只有逐帧比对才看得出来。
// =============================================================================

/// `Mathf.SmoothDamp` 的返回值：新值与**新速度**（速度是跨帧状态，必须接住）。
struct SmoothDampResult
{
    float value {0.f};
    float velocity {0.f};
};

/// `Vector3.SmoothDamp` 的返回值，三个分量各一套。
struct Vector3SmoothDampResult
{
    Vector3 value;
    Vector3 velocity;
};

// =============================================================================
// 实现（Mathf 与 Vector3 两个门面**共用**，避免两份实现漂移）
// =============================================================================
namespace SmoothDampDetail
{
    /// Unity 允许 `smoothTime` 为 0（内部取 `Mathf.Max(0.0001f, smoothTime)`）；
    /// 直接拿它当除数会得到 inf / NaN，而 NaN 一旦进到位置里就再也洗不掉。
    inline float ClampSmoothTime(float smooth_time)
    {
        return (smooth_time > 0.0001f) ? smooth_time : 0.0001f;
    }

    /// 一个分量的 SmoothDamp。`out_velocity` 是**新速度**（跨帧状态的另一半）。
    inline float Step(float current, float target, float current_velocity, float smooth_time,
                      float max_speed, float delta_time, float& out_velocity)
    {
        smooth_time = ClampSmoothTime(smooth_time);

        // ⚠️ deltaTime <= 0 时公式的分母为 0。Unity 在这种帧上（Time.deltaTime
        // 为 0，比如暂停的第一帧）会得到 NaN。这里选择"这一帧不动、速度原样
        // 带过去" —— NaN 会静默污染后面的每一帧，而"不动"是可恢复的。
        if (!(delta_time > 0.f))
        {
            out_velocity = current_velocity;
            return current;
        }

        const float omega = 2.f / smooth_time;
        const float x = omega * delta_time;
        // ⚠️ Unity 的多项式近似，不是 std::exp(-x)。见文件头。
        const float exp = 1.f / (1.f + x + 0.48f * x * x + 0.235f * x * x * x);

        float change = current - target;
        const float original_to = target;

        // maxSpeed：把"这一步要走多远"夹住。⚠️ 夹的是 `change`（当前到目标的
        // 距离），上限是 `maxSpeed * smoothTime` —— 不是 `maxSpeed * deltaTime`。
        // 写成后者的话限速会随帧率变，而它看起来仍然"在限速"。
        if (max_speed > 0.f)
        {
            const float max_change = max_speed * smooth_time;
            change = std::min(std::max(change, -max_change), max_change);
        }

        target = current - change;
        const float temp = (current_velocity + omega * change) * delta_time;
        out_velocity = (current_velocity - omega * temp) * exp;
        float output = target + (change + temp) * exp;

        // ⚠️ 过冲守卫：从 current 往 original_to 走，若已经**越过**了就钉住。
        if ((original_to - current > 0.f) == (output > original_to))
        {
            output = original_to;
            out_velocity = (output - original_to) / delta_time;
        }
        return output;
    }
}  // namespace SmoothDampDetail
