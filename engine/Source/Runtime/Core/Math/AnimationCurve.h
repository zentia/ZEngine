#pragma once

#include "Runtime/BaseClasses/ObjectMacros.h"

#include <cmath>
#include <cstdint>
#include <vector>

// =============================================================================
// AnimationCurve / Keyframe / WrapMode —— Unity 的 `UnityEngine.AnimationCurve`
// =============================================================================
//
// 三个都是**值类型**：与 `Bounds` / `Color32` 同一套分工，`ZScript::Converter`
// 把它们当普通 JS 对象搬运（.d.ts 里是 `declare interface`），脚本侧那批静态
// 方法挂在 `Runtime::AnimationCurve` 这个门面类上（见 ScriptAnimationCurve.h）。
//
// 与 Unity 的形状差异（一个，被工具链逼出来的）
// --------------------------------------------
// Unity 的 `AnimationCurve` 是 **class**（引用类型，`AddKey` 就地改自己）；这里
// 过边界时是值（JS 端就是个 `{keys, preWrapMode, postWrapMode}` 字面量），所以
// 门面上的 `AddKey` **返回新曲线**。与 `Bounds.Encapsulate` 同一个约定。
// 根本原因还是 ZHeaderTool 绑不了构造函数：脚本拿不到"一个带方法的曲线实例"。
//
// 插值：Unity 的三次 Hermite
// --------------------------
// 相邻两帧 k1(t1,v1) / k2(t2,v2) 之间：
//
//     dt = t2 - t1;        u = (t - t1) / dt
//     m0 = k1.outTangent * dt;   m1 = k2.inTangent * dt
//     a  =  2u³ - 3u² + 1        b = u³ - 2u² + u
//     c  =  u³ - u²              d = -2u³ + 3u²
//     v  =  a*v1 + b*m0 + c*m1 + d*v2
//
// ⚠️ 切线是**斜率**（dValue/dTime），不是角度也不是位移，乘 dt 之后才进 Hermite
// 基。少了那个 `* dt`，同一组切线在时间间距不同的两段上会给出不同形状 —— 而它
// 在等距的两段上又碰巧是对的，所以只用均匀采样的夹具看不出来。
//
// ⚠️ `Linear` 造出来的切线就是斜率：
//
//     slope = (vEnd - vStart) / (tEnd - tStart)
//     keys  = { (tStart, vStart, in=0,    out=slope),
//               (tEnd,   vEnd,   in=slope, out=0) }
//
// 这不是"看起来像直线"，代进去 a..d 恰好把 Hermite 退化成线性插值 —— 端点那
// 两个 0 是关键，写成 slope 会变成过冲的 S 形。
// ---------------------------------------------------------------------------

/// Unity 的 `UnityEngine.WrapMode`。数值与 Unity 对齐（脚本里 `WrapMode.Loop`
/// 就是 2），这样从 Unity 拷过来的常量不用换算。
///
/// ⚠️ 枚举值**不带 `k` 前缀**：ZHeaderTool 把 C++ 拼写直接 Pascal 化当 TS 成员名
/// （`parser.py:_pascal_case`），`kOnce` 会变成 `KOnce` 而不是 Unity 的 `Once`。
///
/// | 值 | 语义 |
/// |----|------|
/// | `Default` (0) | 按 `Once` 处理 |
/// | `Once` (1) | 到端点之后**保持**端点的值（Unity 编辑器里的 "Clamp"） |
/// | `Loop` (2) | 首尾相接，循环 |
/// | `PingPong` (4) | 往复（每隔一个周期镜像一次） |
/// | `ClampForever` (8) | 按端点那一条**切线继续外延**（Unity 编辑器里的 "Continue"） |
///
/// 默认是 `Once`：Unity 的文档没写新建曲线的 pre/post wrap 是什么，而"到端点后
/// 保持端点值"是绝大多数脚本的预期（`ClampForever` 会让缓动曲线在结束之后继续
/// 冲出去）。脚本随时可以显式设。
ZENUM()
enum class WrapMode : int
{
    Default      = 0,
    Once         = 1,
    Loop         = 2,
    PingPong     = 4,
    ClampForever = 8,
};

/// Unity 的 `UnityEngine.Keyframe`。
///
/// 四个字段全是 float：`inTangent` / `outTangent` 是**斜率**（见文件头）。
/// Unity 还有 `weightedMode` / `inWeight` / `outWeight`（2018 之后的加权切线），
/// 这里没有 —— 加权的那三个量只影响编辑器里手柄的长度，运行时求值不用它们。
struct Keyframe
{
    float time {0.f};
    float value {0.f};
    /// 左侧（入）切线，斜率 dValue/dTime。
    float inTangent {0.f};
    /// 右侧（出）切线，同样是斜率。
    float outTangent {0.f};

    Keyframe() = default;
    Keyframe(float in_time, float in_value)
        : time(in_time), value(in_value) {}
    Keyframe(float in_time, float in_value, float in_tangent, float out_tangent)
        : time(in_time), value(in_value), inTangent(in_tangent), outTangent(out_tangent) {}
};

class AnimationCurve
{
public:
    /// 关键帧，**按 time 升序**。`AddKey` / `MoveKey` 维护这个顺序；`Evaluate`
    /// 依赖它（二分/线性找段都建立在有序上，乱序会得到"看起来能跑但值不对"的
    /// 结果 —— 那是最难查的一类）。
    std::vector<Keyframe> keys;

    WrapMode preWrapMode {WrapMode::Once};
    WrapMode postWrapMode {WrapMode::Once};

    // ---- 尺寸 ----------------------------------------------------------

    int GetKeyCount() const { return static_cast<int>(keys.size()); }

    /// 第一帧与最后一帧的时间跨度；不足两帧时是 0（不是 NaN：除零会静默产生
    /// NaN，`Loop` / `PingPong` 的取模于是把整条曲线变成 NaN）。
    float GetDuration() const
    {
        if (keys.size() < 2)
        {
            return 0.f;
        }
        return keys.back().time - keys.front().time;
    }

    // ---- 编辑 ----------------------------------------------------------

    /// 加一帧，加完仍然有序。**同一时刻已有一帧时覆盖它**（不是并列插一帧：
    /// 两条同时间的帧会让"这一段"的 dt 为 0，求值只能靠约定挑一条，而 Unity
    /// 的曲线编辑器也不允许同一个 time 上有两个点）。
    void AddKey(float time, float value) { AddKey(Keyframe(time, value)); }

    void AddKey(const Keyframe& key)
    {
        for (Keyframe& existing : keys)
        {
            if (existing.time == key.time)
            {
                existing = key;
                return;
            }
        }
        keys.insert(LowerBound(key.time), key);
    }

    /// Unity 的 `MoveKey(index, key)`：把第 index 帧整个换成 key（时间、值、
    /// 切线都换），然后重新排序。越界静默忽略。
    void MoveKey(int index, const Keyframe& key)
    {
        if (index < 0 || index >= static_cast<int>(keys.size()))
        {
            return;
        }
        keys[static_cast<std::size_t>(index)] = key;
        SortKeys();
    }

    /// Unity 的 `RemoveKey(index)`。越界静默忽略。
    void RemoveKey(int index)
    {
        if (index < 0 || index >= static_cast<int>(keys.size()))
        {
            return;
        }
        keys.erase(keys.begin() + index);
    }

    /// Unity 的 `ClearKeys()`。wrap 模式**保持**（它属于曲线，不属于帧列表）。
    void ClearKeys() { keys.clear(); }

    /// Unity 的 `SmoothTangents(index)`：把第 index 帧的两条切线设成左右两段
    /// 斜率的加权平均，权重取**对面**那一段的时间长度 ——
    ///
    ///     t = (s1 * dt2 + s2 * dt1) / (dt1 + dt2)
    ///
    /// 这样窄的那一段对切线影响更小，曲线不会在密集采样处抖。取算术平均
    /// （`(s1 + s2) / 2`）也能过"左右对称"的夹具，只有**时间间距不等**的样本
    /// 能把两者分开。
    ///
    /// 首尾帧只有一侧有段，就直接用那一侧的斜率。
    void SmoothTangents(int index)
    {
        const int count = static_cast<int>(keys.size());
        if (count < 2 || index < 0 || index >= count)
        {
            return;
        }

        if (index == 0 || index == count - 1)
        {
            const int other = (index == 0) ? 1 : count - 2;
            const float slope = SlopeBetween(index, other);
            keys[static_cast<std::size_t>(index)].inTangent = slope;
            keys[static_cast<std::size_t>(index)].outTangent = slope;
            return;
        }

        const float dt1 = keys[static_cast<std::size_t>(index)].time - keys[static_cast<std::size_t>(index - 1)].time;
        const float dt2 = keys[static_cast<std::size_t>(index + 1)].time - keys[static_cast<std::size_t>(index)].time;
        if (dt1 <= 0.f || dt2 <= 0.f)
        {
            return;
        }
        const float s1 = SlopeBetween(index - 1, index);
        const float s2 = SlopeBetween(index, index + 1);
        const float tangent = (s1 * dt2 + s2 * dt1) / (dt1 + dt2);
        keys[static_cast<std::size_t>(index)].inTangent = tangent;
        keys[static_cast<std::size_t>(index)].outTangent = tangent;
    }

    // ---- 求值 ----------------------------------------------------------

    /// Unity 的 `Evaluate(time)`。
    ///
    /// * 空曲线 → 0。
    /// * 单帧曲线 → 那一帧的值（与时间无关）。
    /// * 时间落在范围内 → 三次 Hermite（见文件头）。
    /// * 时间落在范围外 → 按 `preWrapMode` / `postWrapMode` 处理。
    ///
    /// ⚠️ 非有限输入（NaN / ±inf）直接返回 0。NaN 参与 `<=` / `>=` 比较恒为
    /// false，不判掉的话它会一路走到"没找到段"的兜底分支 —— 那和"真的没有
    /// 段"是分不开的。
    float Evaluate(float time) const
    {
        if (!std::isfinite(time))
        {
            return 0.f;
        }
        if (keys.empty())
        {
            return 0.f;
        }
        if (keys.size() == 1)
        {
            return keys.front().value;
        }

        if (time < keys.front().time)
        {
            return EvaluateOutside(time, preWrapMode, /*before=*/true);
        }
        if (time > keys.back().time)
        {
            return EvaluateOutside(time, postWrapMode, /*before=*/false);
        }

        // 帧上正好：直接用那一帧的值（Hermite 在端点也给出同一个数，走那条路
        // 会先算一遍 dt，而 dt 为 0 时它除零）。
        for (const Keyframe& key : keys)
        {
            if (key.time == time)
            {
                return key.value;
            }
        }

        const std::size_t segment = FindSegment(time);
        return Hermite(keys[segment], keys[segment + 1], time);
    }

private:
    std::vector<Keyframe>::iterator LowerBound(float time)
    {
        auto it = keys.begin();
        while (it != keys.end() && it->time < time)
        {
            ++it;
        }
        return it;
    }

    static bool ByTime(const Keyframe& a, const Keyframe& b) { return a.time < b.time; }

    void SortKeys()
    {
        // 稳定排序：同时间的两帧保持插入顺序（`MoveKey` 之后不该顺手换掉
        // 另一帧的位置）。
        std::stable_sort(keys.begin(), keys.end(), ByTime);
    }

    float SlopeBetween(int from, int to) const
    {
        const Keyframe& a = keys[static_cast<std::size_t>(from)];
        const Keyframe& b = keys[static_cast<std::size_t>(to)];
        const float dt = b.time - a.time;
        if (dt == 0.f)
        {
            return 0.f;
        }
        return (b.value - a.value) / dt;
    }

    /// 求 `keys[i].time <= time < keys[i+1].time` 的那个 i。调用方已保证 time
    /// 落在 [first, last] 闭区间内且不落在任何一帧上。
    std::size_t FindSegment(float time) const
    {
        std::size_t index = 0;
        for (std::size_t i = 0; i + 1 < keys.size(); ++i)
        {
            if (keys[i + 1].time > time)
            {
                index = i;
                break;
            }
            index = i;
        }
        return index;
    }

    static float Hermite(const Keyframe& k1, const Keyframe& k2, float time)
    {
        const float dt = k2.time - k1.time;
        if (dt <= 0.f)
        {
            return k2.value;
        }
        const float u = (time - k1.time) / dt;
        const float u2 = u * u;
        const float u3 = u2 * u;

        const float m0 = k1.outTangent * dt;
        const float m1 = k2.inTangent * dt;

        const float a = 2.f * u3 - 3.f * u2 + 1.f;
        const float b = u3 - 2.f * u2 + u;
        const float c = u3 - u2;
        const float d = -2.f * u3 + 3.f * u2;

        return a * k1.value + b * m0 + c * m1 + d * k2.value;
    }

    float EvaluateOutside(float time, WrapMode mode, bool before) const
    {
        switch (mode)
        {
            case WrapMode::ClampForever:
            {
                // 沿端点那一条切线继续走。
                //
                // ⚠️ 哪一条切线是**反直觉**的：范围**之前**用的是首帧的 **out**
                // 切线（曲线在首帧右侧的斜率，把它往左延长），范围**之后**用的
                // 是末帧的 **in** 切线（曲线在末帧左侧的斜率，往右延长）。
                // 用反了的话 `Linear(0,0,1,1)` 之前/之后都不再是 y = t —— 而
                // 对称的夹具（首末斜率相同）看不出这个区别，只有**首末斜率不同**
                // 的曲线能把它抓出来。
                const Keyframe& edge = before ? keys.front() : keys.back();
                const float tangent = before ? edge.outTangent : edge.inTangent;
                return edge.value + (time - edge.time) * tangent;
            }
            case WrapMode::Loop:
            case WrapMode::PingPong:
            {
                const float duration = GetDuration();
                if (duration <= 0.f)
                {
                    return keys.front().value;
                }
                const float start = keys.front().time;
                float offset = time - start;
                const float period = (mode == WrapMode::Loop) ? duration : (2.f * duration);
                offset = offset - std::floor(offset / period) * period;
                if (mode == WrapMode::PingPong && offset > duration)
                {
                    offset = period - offset;
                }
                return EvaluateInRange(start + offset);
            }
            case WrapMode::Default:
            case WrapMode::Once:
            default:
                return before ? keys.front().value : keys.back().value;
        }
    }

    /// 已经 wrap 进 [first, last] 之后重新走一次"找段 → Hermite"。
    float EvaluateInRange(float time) const
    {
        if (time <= keys.front().time)
        {
            return keys.front().value;
        }
        if (time >= keys.back().time)
        {
            return keys.back().value;
        }
        const std::size_t segment = FindSegment(time);
        return Hermite(keys[segment], keys[segment + 1], time);
    }
};
