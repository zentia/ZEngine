#include "Color.h"

#include <algorithm>

namespace
{
    /// Unity 的 `Mathf.Clamp01`。
    float Clamp01(float v)
    {
        return std::max(0.0f, std::min(1.0f, v));
    }

    /// Unity 的 `(byte)(Clamp01(v) * 255f)`：C# 的 `(byte)float` 是**截断**，
    /// 不是四舍五入。写成 `std::lround` 会和 Unity 工程搬过来的取色值差 1。
    uint8_t ToByte(float channel_0_1)
    {
        return static_cast<uint8_t>(Clamp01(channel_0_1) * 255.0f);
    }

    /// 把一条通道的插值结果折回 0..255。
    ///
    /// ⚠️ 这里必须显式夹：`static_cast<uint8_t>(510.0f)` 是**未定义行为**（浮点
    /// 转无符号整型超出目标范围），MSVC 会给 254，clang 给 255，谁都不算错。
    /// 夹住之后 "Unclamped" 指的只是 `t` 不夹 —— 字节值域永远夹，否则同一个
    /// `LerpUnclamped(a, b, 2)` 在不同编译器下得到不同的颜色。
    uint8_t LerpChannel(float from, float to, float t)
    {
        return ToByte((from + (to - from) * t) / 255.0f);
    }

    /// 逐通道插值后截断。`t` 由调用方决定是否先夹。
    Color32 LerpChannels(const Color32& from, const Color32& to, float t)
    {
        return Color32(LerpChannel(from.r, to.r, t), LerpChannel(from.g, to.g, t), LerpChannel(from.b, to.b, t),
                       LerpChannel(from.a, to.a, t));
    }
}  // namespace

Color32::Color32(const Color& color)
    : r(ToByte(color.r)), g(ToByte(color.g)), b(ToByte(color.b)), a(ToByte(color.a))
{}

Color32::operator Color() const
{
    Color out;
    out.r = static_cast<float>(r) / 255.0f;
    out.g = static_cast<float>(g) / 255.0f;
    out.b = static_cast<float>(b) / 255.0f;
    out.a = static_cast<float>(a) / 255.0f;
    return out;
}

Color32 Color32::Lerp(const Color32& from, const Color32& to, float t)
{
    return LerpChannels(from, to, Clamp01(t));
}

Color32 Color32::LerpUnclamped(const Color32& from, const Color32& to, float t)
{
    return LerpChannels(from, to, t);
}
