#pragma once
#include "Runtime/Core/Math/Vector3.h"

#include <cstdint>

class Color
{
public:
    DECLARE_SERIALIZE(Color);
    float r;
    float g;
    float b;
    float a;
    Vector3 toVector3() { return Vector3(r, g, b); }
};

template<typename TransferFunction>
void Color::Transfer(TransferFunction& transfer)
{
    transfer.Transfer(r, "r");
    transfer.Transfer(g, "g");
    transfer.Transfer(b, "b");
    transfer.Transfer(a, "a");
}

// =============================================================================
// Color32 —— Unity 的 `UnityEngine.Color32`
// =============================================================================
//
// 四个 0..255 的字节。与 `Color` 的互转是**隐式**的（Unity 也是），但两个方向
// 的取整规则不对称，而且这里照抄 Unity 的**截断**而不是四舍五入：
//
//   * `Color -> Color32`：`(byte)(Clamp01(v) * 255)`。C# 的 `(byte)float` 是截
//     断，所以 0.5 变成 127 而不是 128。
//   * `Color32 -> Color`：`v / 255`（精确）。
//
// 于是来回一趟不保等：`Color(0.5f) -> 127 -> 0.498f`。这是 Unity 的行为，不是
// bug —— 要保等就别在两者之间反复转。
//
// 默认值是**全 0**，与 Unity 的 `default(Color32)` 一致（透明黑）。注意这与脚
// 本边界上的规则相反：TS 字面量 `{r, g, b}` 省略 `a` 时按**不透明**处理
// （a=255，即 Unity 的 `new Color32(r, g, b)`）—— 那条规则在
// `ZScript::Converter<Color32>` 里，不在这里。
struct Color32
{
    uint8_t r{0};
    uint8_t g{0};
    uint8_t b{0};
    uint8_t a{0};

    Color32() = default;
    Color32(uint8_t in_r, uint8_t in_g, uint8_t in_b, uint8_t in_a) : r(in_r), g(in_g), b(in_b), a(in_a) {}

    /// Unity 的 `implicit operator Color32(Color)`。
    Color32(const Color& color);

    /// Unity 的 `implicit operator Color(Color32)`。
    operator Color() const;

    /// Unity 的 `Color32.Lerp`：`t` **被夹到 [0,1]**，逐通道线性插值后截断。
    static Color32 Lerp(const Color32& from, const Color32& to, float t);

    /// 同上但 `t` 不夹（Unity 2018.1+ 的 `Color32.LerpUnclamped`）。
    static Color32 LerpUnclamped(const Color32& from, const Color32& to, float t);

    bool operator==(const Color32& other) const
    {
        return r == other.r && g == other.g && b == other.b && a == other.a;
    }
    bool operator!=(const Color32& other) const { return !(*this == other); }
};

// =============================================================================
// ColorParseResult —— `ColorUtility.TryParseHtmlString` 的结果
// =============================================================================
//
// Unity 的签名是 `bool TryParseHtmlString(string htmlString, out Color color)`。
// TS 没有 out 参数，而 `Color` 又是值类型（跨边界是快照），所以"成功与否"和
// "颜色"只能一起回来 —— 与 `RaycastHit.valid` / `SmoothDampResult` 同一套路。
//
// ⚠️ 失败时 `color` 是全 0（含 a = 0，即透明黑），**不是**保留调用方原来的值。
// 这是有意的：调用方拿到一个"看着像旧值"的结果会当成解析成功继续用，而全 0
// 至少在屏幕上什么都没有。要不要用得看 `valid`。
struct ColorParseResult
{
    bool valid{false};
    Color color{};
};