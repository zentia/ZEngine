#include "Runtime/Scripting/ScriptAnimationCurve/ScriptAnimationCurve.h"

#include <algorithm>

namespace Runtime
{

    namespace
    {
        ::Keyframe KeyAtOrZero(const ::AnimationCurve& curve, int index)
        {
            if (index < 0 || index >= curve.GetKeyCount())
            {
                return ::Keyframe();
            }
            return curve.keys[static_cast<std::size_t>(index)];
        }
    }  // namespace

    ::AnimationCurve AnimationCurve::Create()
    {
        return ::AnimationCurve();
    }

    ::AnimationCurve AnimationCurve::Linear(float time_start, float value_start,
                                            float time_end, float value_end)
    {
        ::AnimationCurve curve;

        // ⚠️ 除零：time_end == time_start 时 slope 是 ±inf / NaN，而 NaN 参与后面
        // 的 Hermite 会把整条曲线变成 NaN（NaN 参与比较恒 false，于是"找不到段"
        // 的兜底被静默走到）。Unity 的 C# 实现没有这个判据（它会产生 inf）；这里
        // 退化成"两端同值的一条曲线"，至少求值是有定义的。
        if (time_end == time_start)
        {
            curve.AddKey(::Keyframe(time_start, value_start));
            curve.AddKey(::Keyframe(time_end, value_end));
            return curve;
        }

        const float slope = (value_end - value_start) / (time_end - time_start);
        curve.AddKey(::Keyframe(time_start, value_start, 0.f, slope));
        curve.AddKey(::Keyframe(time_end, value_end, slope, 0.f));
        return curve;
    }

    ::AnimationCurve AnimationCurve::EaseInOut(float time_start, float value_start,
                                               float time_end, float value_end)
    {
        ::AnimationCurve curve;
        curve.AddKey(::Keyframe(time_start, value_start, 0.f, 0.f));
        curve.AddKey(::Keyframe(time_end, value_end, 0.f, 0.f));
        return curve;
    }

    ::AnimationCurve AnimationCurve::Constant(float time_start, float time_end, float value)
    {
        ::AnimationCurve curve;
        curve.AddKey(::Keyframe(time_start, value, 0.f, 0.f));
        curve.AddKey(::Keyframe(time_end, value, 0.f, 0.f));
        return curve;
    }

    ::Keyframe AnimationCurve::CreateKey(float time, float value)
    {
        return ::Keyframe(time, value);
    }

    ::Keyframe AnimationCurve::CreateKeyTangents(float time, float value,
                                                 float in_tangent, float out_tangent)
    {
        return ::Keyframe(time, value, in_tangent, out_tangent);
    }

    float AnimationCurve::Evaluate(const ::AnimationCurve& curve, float time)
    {
        return curve.Evaluate(time);
    }

    int AnimationCurve::GetKeyCount(const ::AnimationCurve& curve)
    {
        return curve.GetKeyCount();
    }

    ::Keyframe AnimationCurve::GetKey(const ::AnimationCurve& curve, int index)
    {
        return KeyAtOrZero(curve, index);
    }

    std::vector<::Keyframe> AnimationCurve::GetKeys(const ::AnimationCurve& curve)
    {
        return curve.keys;
    }

    float AnimationCurve::GetDuration(const ::AnimationCurve& curve)
    {
        return curve.GetDuration();
    }

    ::AnimationCurve AnimationCurve::AddKey(const ::AnimationCurve& curve, float time, float value)
    {
        ::AnimationCurve out = curve;
        out.AddKey(time, value);
        return out;
    }

    ::AnimationCurve AnimationCurve::AddKeyFrame(const ::AnimationCurve& curve, const ::Keyframe& key)
    {
        ::AnimationCurve out = curve;
        out.AddKey(key);
        return out;
    }

    ::AnimationCurve AnimationCurve::MoveKey(const ::AnimationCurve& curve, int index, const ::Keyframe& key)
    {
        ::AnimationCurve out = curve;
        out.MoveKey(index, key);
        return out;
    }

    ::AnimationCurve AnimationCurve::RemoveKey(const ::AnimationCurve& curve, int index)
    {
        ::AnimationCurve out = curve;
        out.RemoveKey(index);
        return out;
    }

    ::AnimationCurve AnimationCurve::ClearKeys(const ::AnimationCurve& curve)
    {
        ::AnimationCurve out = curve;
        out.ClearKeys();
        return out;
    }

    ::AnimationCurve AnimationCurve::SetKeys(const ::AnimationCurve& curve,
                                             const std::vector<::Keyframe>& keys)
    {
        ::AnimationCurve out = curve;
        out.keys = keys;
        // 脚本给的数组未必有序。`Evaluate` 依赖有序，不排的话会得到"能跑但值不
        // 对"的结果 —— 那是最难查的一类，所以这里主动排一次。
        std::stable_sort(out.keys.begin(), out.keys.end(),
                         [](const ::Keyframe& a, const ::Keyframe& b) { return a.time < b.time; });
        return out;
    }

    ::AnimationCurve AnimationCurve::SmoothTangents(const ::AnimationCurve& curve, int index)
    {
        ::AnimationCurve out = curve;
        out.SmoothTangents(index);
        return out;
    }

    ::AnimationCurve AnimationCurve::SetPreWrapMode(const ::AnimationCurve& curve, WrapMode mode)
    {
        ::AnimationCurve out = curve;
        out.preWrapMode = mode;
        return out;
    }

    ::AnimationCurve AnimationCurve::SetPostWrapMode(const ::AnimationCurve& curve, WrapMode mode)
    {
        ::AnimationCurve out = curve;
        out.postWrapMode = mode;
        return out;
    }

    WrapMode AnimationCurve::GetPreWrapMode(const ::AnimationCurve& curve)
    {
        return curve.preWrapMode;
    }

    WrapMode AnimationCurve::GetPostWrapMode(const ::AnimationCurve& curve)
    {
        return curve.postWrapMode;
    }

}  // namespace Runtime
