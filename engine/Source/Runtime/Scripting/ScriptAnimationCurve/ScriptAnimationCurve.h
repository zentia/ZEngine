#pragma once

#include "Runtime/BaseClasses/Object.h"
#include "Runtime/Core/Math/AnimationCurve.h"

#include "ScriptAnimationCurve.generated.h"

// =============================================================================
// `AnimationCurve` —— Unity 的 `UnityEngine.AnimationCurve` 在脚本侧的那一半
// =============================================================================
//
// 与 `Bounds` / `Color32` 同一套分工（详见 ScriptBounds.h / ScriptColor.h）：
//
//   * 值类型 `::AnimationCurve` / `::Keyframe`（`Runtime/Core/Math/
//     AnimationCurve.h`）作为**值**跨边界 —— `ZScript::Converter` 把它们当普通
//     JS 对象搬运，.d.ts 里是 `declare interface AnimationCurve`。
//   * 这个 `Runtime::AnimationCurve` 是**门面**：Unity 那批 `curve.Evaluate(t)`
//     得挂在某个绑定类上。名字必须叫 `AnimationCurve`，于是只能放进命名空间。
//
// 为什么编辑类方法返回**新**曲线
// ------------------------------
// Unity 的 `AnimationCurve` 是 class，`AddKey` 就地改自己。这里过边界时是值（JS
// 端就是 `{keys, preWrapMode, postWrapMode}` 字面量），没有地方挂实例方法 ——
// 更根本的是 ZHeaderTool 绑不了构造函数，脚本写不出 `new AnimationCurve()`。
// 所以是 `AnimationCurve.AddKey(c, t, v)` 返回新曲线，与
// `Bounds.Encapsulate(b, p)` 同一个约定。
//
// ⚠️ 这条差异对脚本是**可见**的：`c = c.AddKey(...)` 不能写成 `c.AddKey(...)`。
// 忘了接返回值的话曲线一直是空的，而空曲线求值是 0 —— 不报错、不崩溃。
//
// 两个重载不同名
// --------------
// 绑定表按脚本名索引（pesapi `set_method_info`），两个 `AddKey` 后者会覆盖前者。
// 所以是 `AddKey(time, value)` + `AddKeyFrame(key)` —— 与 `Bounds` 的
// `Encapsulate` / `EncapsulateBounds` 同一套命名。
// =============================================================================

namespace Runtime
{
    /// Unity 的 `AnimationCurve` 静态面。
    ///
    /// 它不该被实例化 —— 它只是脚本侧的命名空间（同 `Bounds` / `Color` /
    /// `Mathf`）。之所以还得是个 `Object` 派生类，是因为 ZCLASS 要求。
    ZCLASS()
    class AnimationCurve : public Object
    {
        ZGENERATED_BODY(AnimationCurve)

    public:
        // ---- 工厂 --------------------------------------------------------

        /// Unity 的 `new AnimationCurve()`：一条空曲线（`Evaluate` 恒为 0）。
        ZFUNCTION()
        static ::AnimationCurve Create();

        /// Unity 的 `AnimationCurve.Linear(timeStart, valueStart, timeEnd, valueEnd)`。
        ///
        /// 实现照抄 Unity（两条切线 = 整段的斜率，两个端点外侧的切线为 0）：
        ///
        ///     slope = (valueEnd - valueStart) / (timeEnd - timeStart)
        ///     keys  = { (timeStart, valueStart, 0,     slope),
        ///               (timeEnd,   valueEnd,   slope, 0) }
        ///
        /// 端点那两个 0 不是"随便填的"：代进 Hermite 基之后它们正好把三次项消
        /// 掉。填成 slope 会得到一条过冲的 S 形，而**端点处仍然经过给定的两个
        /// 值** —— 只看端点的夹具是绿的。
        ZFUNCTION()
        static ::AnimationCurve Linear(float time_start, float value_start,
                                       float time_end, float value_end);

        /// Unity 的 `AnimationCurve.EaseInOut(...)`：两条切线都是 0，于是 Hermite
        /// 退化成 smoothstep。
        ZFUNCTION()
        static ::AnimationCurve EaseInOut(float time_start, float value_start,
                                          float time_end, float value_end);

        /// Unity 的 `AnimationCurve.Constant(timeStart, timeEnd, value)`：两端
        /// 同值、切线为 0 —— 求值恒等于 `value`（与时间无关）。
        ///
        /// ⚠️ 参数顺序是 (timeStart, timeEnd, value)，**value 在最后**，与
        /// `Linear` / `EaseInOut` 的 (time, value, time, value) 不同。这是
        /// Unity 的签名（它只有"一个"值要恒定）。
        ZFUNCTION()
        static ::AnimationCurve Constant(float time_start, float time_end, float value);

        /// Unity 的 `new Keyframe(time, value)`（切线默认 0）。
        ZFUNCTION()
        static ::Keyframe CreateKey(float time, float value);

        /// Unity 的 `new Keyframe(time, value, inTangent, outTangent)`。
        /// 两个重载不能同名（见文件头），所以叫 `CreateKeyTangents`。
        ZFUNCTION()
        static ::Keyframe CreateKeyTangents(float time, float value,
                                            float in_tangent, float out_tangent);

        // ---- 查询 --------------------------------------------------------

        /// Unity 的 `curve.Evaluate(time)`。
        ZFUNCTION()
        static float Evaluate(const ::AnimationCurve& curve, float time);

        /// Unity 的 `curve.length`。
        ZFUNCTION()
        static int GetKeyCount(const ::AnimationCurve& curve);

        /// Unity 的 `curve[index]`。越界返回一个全 0 的帧（不是崩）。
        ZFUNCTION()
        static ::Keyframe GetKey(const ::AnimationCurve& curve, int index);

        /// Unity 的 `curve.keys`。返回的是**快照**：改它不影响曲线（见
        /// `Converter<std::vector<T>>` 的说明）。
        ZFUNCTION()
        static std::vector<::Keyframe> GetKeys(const ::AnimationCurve& curve);

        /// 末帧时间 - 首帧时间；不足两帧是 0。`Loop` / `PingPong` 的周期就是它。
        ZFUNCTION()
        static float GetDuration(const ::AnimationCurve& curve);

        // ---- 编辑（全部返回**新**曲线）------------------------------------

        ZFUNCTION()
        static ::AnimationCurve AddKey(const ::AnimationCurve& curve, float time, float value);

        ZFUNCTION()
        static ::AnimationCurve AddKeyFrame(const ::AnimationCurve& curve, const ::Keyframe& key);

        /// Unity 的 `curve.MoveKey(index, key)`。
        ZFUNCTION()
        static ::AnimationCurve MoveKey(const ::AnimationCurve& curve, int index, const ::Keyframe& key);

        /// Unity 的 `curve.RemoveKey(index)`。
        ZFUNCTION()
        static ::AnimationCurve RemoveKey(const ::AnimationCurve& curve, int index);

        /// Unity 的 `curve.ClearKeys()`（wrap 模式保留）。
        ZFUNCTION()
        static ::AnimationCurve ClearKeys(const ::AnimationCurve& curve);

        /// Unity 的 `curve.SetKeys(keys)`：整批换掉帧列表。
        ZFUNCTION()
        static ::AnimationCurve SetKeys(const ::AnimationCurve& curve,
                                        const std::vector<::Keyframe>& keys);

        /// Unity 的 `curve.SmoothTangents(index)`。
        ZFUNCTION()
        static ::AnimationCurve SmoothTangents(const ::AnimationCurve& curve, int index);

        // ---- 环绕模式 ----------------------------------------------------

        ZFUNCTION()
        static ::AnimationCurve SetPreWrapMode(const ::AnimationCurve& curve, WrapMode mode);

        ZFUNCTION()
        static ::AnimationCurve SetPostWrapMode(const ::AnimationCurve& curve, WrapMode mode);

        ZFUNCTION()
        static WrapMode GetPreWrapMode(const ::AnimationCurve& curve);

        ZFUNCTION()
        static WrapMode GetPostWrapMode(const ::AnimationCurve& curve);
    };
}  // namespace Runtime
