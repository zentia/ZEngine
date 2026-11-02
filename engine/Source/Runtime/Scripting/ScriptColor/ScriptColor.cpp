#include "Runtime/Scripting/ScriptColor/ScriptColor.h"

// `Mathf::GammaToLinearSpace` / `LinearToGammaSpace` 已经有了，这里复用而不是
// 抄一份公式 —— 两条 sRGB 曲线存在两处是迟早会对不上的经典配置。
#include "Runtime/Scripting/ScriptMath/ScriptMath.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace Runtime
{
    namespace
    {
        ::Color Make(float r, float g, float b, float a)
        {
            ::Color c;
            c.r = r;
            c.g = g;
            c.b = b;
            c.a = a;
            return c;
        }
    }  // namespace

    ::Color Color::GetClear()
    {
        return Make(0.0f, 0.0f, 0.0f, 0.0f);
    }

    ::Color Color::GetWhite()
    {
        return Make(1.0f, 1.0f, 1.0f, 1.0f);
    }

    ::Color Color::GetBlack()
    {
        return Make(0.0f, 0.0f, 0.0f, 1.0f);
    }

    ::Color Color::GetRed()
    {
        return Make(1.0f, 0.0f, 0.0f, 1.0f);
    }

    ::Color Color::GetGreen()
    {
        return Make(0.0f, 1.0f, 0.0f, 1.0f);
    }

    ::Color Color::GetBlue()
    {
        return Make(0.0f, 0.0f, 1.0f, 1.0f);
    }

    // Unity 的 Color.yellow 是 RGB(255, 235, 4)，不是纯 (1,1,0)。
    ::Color Color::GetYellow()
    {
        return Make(1.0f, 235.0f / 255.0f, 4.0f / 255.0f, 1.0f);
    }

    ::Color Color::GetCyan()
    {
        return Make(0.0f, 1.0f, 1.0f, 1.0f);
    }

    ::Color Color::GetMagenta()
    {
        return Make(1.0f, 0.0f, 1.0f, 1.0f);
    }

    ::Color Color::GetGray()
    {
        return Make(0.5f, 0.5f, 0.5f, 1.0f);
    }

    ::Color Color::GetGrey()
    {
        return Make(0.5f, 0.5f, 0.5f, 1.0f);
    }

    ::Color Color::Lerp(const ::Color& a, const ::Color& b, float t)
    {
        float clamped = t;
        if (clamped < 0.0f)
            clamped = 0.0f;
        if (clamped > 1.0f)
            clamped = 1.0f;
        return LerpUnclamped(a, b, clamped);
    }

    ::Color Color::LerpUnclamped(const ::Color& a, const ::Color& b, float t)
    {
        ::Color out;
        out.r = a.r + (b.r - a.r) * t;
        out.g = a.g + (b.g - a.g) * t;
        out.b = a.b + (b.b - a.b) * t;
        out.a = a.a + (b.a - a.a) * t;
        return out;
    }

    ::Color Color::HSVToRGB(float h, float s, float v)
    {
        // 灰：S == 0 时色相没有意义，直接给 (V,V,V)。Unity 同样先判 S 再判 V，
        // 所以 (h, 0, 0) 是黑而不是"无色相的灰"—— V 说了算。
        if (s <= 0.0f)
        {
            return Make(v, v, v, 1.0f);
        }

        // H 环绕：1.25 与 0.25 同色，负数同理（std::fmod 保留符号，所以要 +1）。
        float hue = h - std::floor(h);
        if (hue < 0.0f)
            hue += 1.0f;

        const float sector = hue * 6.0f;
        const int index = static_cast<int>(std::floor(sector));
        const float f = sector - static_cast<float>(index);
        const float p = v * (1.0f - s);
        const float q = v * (1.0f - s * f);
        const float t = v * (1.0f - s * (1.0f - f));

        switch (index % 6)
        {
            case 0:
                return Make(v, t, p, 1.0f);
            case 1:
                return Make(q, v, p, 1.0f);
            case 2:
                return Make(p, v, t, 1.0f);
            case 3:
                return Make(p, q, v, 1.0f);
            case 4:
                return Make(t, p, v, 1.0f);
            default:
                return Make(v, p, q, 1.0f);
        }
    }

    Vector3 Color::RGBToHSV(const ::Color& c)
    {
        // 完全照抄 Unity 的 `RGBToHSV` + `HSVToRGBHelper`：先选主导通道，再按它把
        // 色相旋转到正确的 60° 扇区（offset 0 / 2 / 4 分别对应 R / G / B 主导）。
        //
        // 两个"看着像 bug"的地方其实是 Unity 的原样行为，抄错会让从 Unity 搬过来
        // 的取色代码对不上：
        //   * 灰色的 H 是 **1/3**（不是 0）：走 else 分支，offset = 2，diff = 0
        //     时 H = offset + (colorone - colortwo) = 2，/6 得到 1/3。
        //   * 只有纯黑（V == 0）的 H 才是 0 —— 那是单独的 `V != 0` 判据。
        const float r = c.r;
        const float g = c.g;
        const float b = c.b;

        float offset = 2.0f;
        float dominant = g;
        float color_one = b;
        float color_two = r;
        if (b > g && b > r)
        {
            offset = 4.0f;
            dominant = b;
            color_one = r;
            color_two = g;
        }
        else if (r > g)
        {
            offset = 0.0f;
            dominant = r;
            color_one = g;
            color_two = b;
        }

        const float v = dominant;
        float h = 0.0f;
        float s = 0.0f;
        if (v != 0.0f)
        {
            const float small = (color_one > color_two) ? color_two : color_one;
            const float diff = v - small;
            if (diff != 0.0f)
            {
                s = diff / v;
                h = offset + (color_one - color_two) / diff;
            }
            else
            {
                h = offset + (color_one - color_two);
            }
            h /= 6.0f;
            if (h < 0.0f)
            {
                h += 1.0f;
            }
        }

        return Vector3(h, s, v);
    }

    ::Color32 Color::ToColor32(const ::Color& c)
    {
        return ::Color32(c);
    }

    ::Color Color::FromColor32(const ::Color32& c)
    {
        return static_cast<::Color>(c);
    }

    ::Color32 Color32::Lerp(const ::Color32& a, const ::Color32& b, float t)
    {
        return ::Color32::Lerp(a, b, t);
    }

    ::Color32 Color32::LerpUnclamped(const ::Color32& a, const ::Color32& b, float t)
    {
        return ::Color32::LerpUnclamped(a, b, t);
    }

    ::Color32 Color32::FromColor(const ::Color& c)
    {
        return ::Color32(c);
    }

    ::Color Color32::ToColor(const ::Color32& c)
    {
        return static_cast<::Color>(c);
    }

    std::string Color32::ToString(const ::Color32& c)
    {
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "RGBA(%d, %d, %d, %d)", static_cast<int>(c.r), static_cast<int>(c.g),
                      static_cast<int>(c.b), static_cast<int>(c.a));
        return std::string(buffer);
    }

    // ---- Unity 的实例属性（值类型，所以做成静态工具）------------------------

    float Color::GetGrayscale(const ::Color& c)
    {
        // Rec.601 的感知亮度权重，不是 (r+g+b)/3。
        return c.r * 0.299f + c.g * 0.587f + c.b * 0.114f;
    }

    float Color::GetMaxColorComponent(const ::Color& c)
    {
        float max = c.r;
        if (c.g > max)
        {
            max = c.g;
        }
        if (c.b > max)
        {
            max = c.b;
        }
        return max;
    }

    ::Color Color::ToLinear(const ::Color& c)
    {
        return Make(Mathf::GammaToLinearSpace(c.r), Mathf::GammaToLinearSpace(c.g),
                    Mathf::GammaToLinearSpace(c.b), c.a);
    }

    ::Color Color::ToGamma(const ::Color& c)
    {
        return Make(Mathf::LinearToGammaSpace(c.r), Mathf::LinearToGammaSpace(c.g),
                    Mathf::LinearToGammaSpace(c.b), c.a);
    }

    std::string Color::ToString(const ::Color& c)
    {
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "RGBA(%.3f, %.3f, %.3f, %.3f)", static_cast<double>(c.r),
                      static_cast<double>(c.g), static_cast<double>(c.b), static_cast<double>(c.a));
        return std::string(buffer);
    }

    ::Color Color::CorrelatedColorTemperatureToRGB(float kelvin)
    {
        // Planckian-locus 近似（Tanner Helland 的分段拟合）。见头文件里那条
        // ⚠️：Unity 没有公布它用的曲线，所以这里是"可信的近似"而不是"逐位一致"。
        const float t = kelvin / 100.0f;

        float red;
        if (t <= 66.0f)
        {
            red = 255.0f;
        }
        else
        {
            red = 329.698727446f * std::pow(t - 60.0f, -0.1332047592f);
        }

        float green;
        if (t <= 66.0f)
        {
            green = 99.4708025861f * std::log(t) - 161.1195681661f;
        }
        else
        {
            green = 288.1221695283f * std::pow(t - 60.0f, -0.0755148492f);
        }

        float blue;
        if (t >= 66.0f)
        {
            blue = 255.0f;
        }
        else if (t <= 19.0f)
        {
            blue = 0.0f;
        }
        else
        {
            blue = 138.5177312231f * std::log(t - 10.0f) - 305.0447927307f;
        }

        // 三段各自都可能算出 <0 或 >255（t 很小时 log(t) 是负的、很大时幂函数
        // 会溢出），夹住再归一 —— 不夹的话 1000K 会给出负的蓝通道。
        auto channel = [](float v) {
            if (v < 0.0f) v = 0.0f;
            if (v > 255.0f) v = 255.0f;
            return v / 255.0f;
        };
        return Make(channel(red), channel(green), channel(blue), 1.0f);
    }

    // ---- ColorUtility -------------------------------------------------------

    namespace
    {
        /// `Mathf.RoundToInt(Clamp01(v) * 255)`。
        ///
        /// ⚠️ **四舍五入**，与 `Color32(Color)` 的**截断**不是一回事（0.5 在这里
        /// 是 128，在 Color32 里是 127）。Unity 的 `ToHtmlString*` 走的是这条，
        /// 别图省事复用 `::Color32(c)`。
        int ChannelToByte(float v)
        {
            if (v < 0.0f)
            {
                v = 0.0f;
            }
            if (v > 1.0f)
            {
                v = 1.0f;
            }
            return static_cast<int>(std::floor(v * 255.0f + 0.5f));
        }

        bool IsHexDigit(char ch)
        {
            return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
        }

        int HexValue(char ch)
        {
            if (ch >= '0' && ch <= '9')
            {
                return ch - '0';
            }
            if (ch >= 'a' && ch <= 'f')
            {
                return ch - 'a' + 10;
            }
            return ch - 'A' + 10;
        }
    }  // namespace

    std::string ColorUtility::ToHtmlStringRGB(const ::Color& c)
    {
        char buffer[16];
        std::snprintf(buffer, sizeof(buffer), "%02X%02X%02X", ChannelToByte(c.r), ChannelToByte(c.g),
                      ChannelToByte(c.b));
        return std::string(buffer);
    }

    std::string ColorUtility::ToHtmlStringRGBA(const ::Color& c)
    {
        char buffer[16];
        std::snprintf(buffer, sizeof(buffer), "%02X%02X%02X%02X", ChannelToByte(c.r), ChannelToByte(c.g),
                      ChannelToByte(c.b), ChannelToByte(c.a));
        return std::string(buffer);
    }

    ColorParseResult ColorUtility::TryParseHtmlString(const eastl::string& html)
    {
        ColorParseResult result;
        result.valid = false;
        result.color = Make(0.0f, 0.0f, 0.0f, 0.0f);

        std::size_t begin = 0;
        if (!html.empty() && html[0] == '#')
        {
            begin = 1;
        }
        const std::size_t length = html.size() - begin;
        if (length != 3 && length != 4 && length != 6 && length != 8)
        {
            return result;
        }
        for (std::size_t i = begin; i < html.size(); ++i)
        {
            if (!IsHexDigit(html[i]))
            {
                return result;
            }
        }

        // 3 / 4 位的缩写形式：每一位复制成两位（"F0A" == "FF00AA"）。
        // ⚠️ 是 `n * 17` 而不是 `n * 16` —— 后者会把 "F" 变成 F0 而不是 FF，
        // 于是 "#FFF" 解析出来不是纯白（差在最低位，肉眼几乎看不出，但和
        // Unity 对不上）。
        int components[4] = {0, 0, 0, 255};
        if (length == 3 || length == 4)
        {
            const int count = static_cast<int>(length);
            for (int i = 0; i < count; ++i)
            {
                components[i] = HexValue(html[begin + static_cast<std::size_t>(i)]) * 17;
            }
            if (count == 3)
            {
                components[3] = 255;
            }
        }
        else
        {
            const int pairs = static_cast<int>(length) / 2;
            for (int i = 0; i < pairs; ++i)
            {
                const std::size_t at = begin + static_cast<std::size_t>(i) * 2;
                components[i] = HexValue(html[at]) * 16 + HexValue(html[at + 1]);
            }
            if (pairs == 3)
            {
                components[3] = 255;
            }
        }

        result.color =
            Make(static_cast<float>(components[0]) / 255.0f, static_cast<float>(components[1]) / 255.0f,
                 static_cast<float>(components[2]) / 255.0f, static_cast<float>(components[3]) / 255.0f);
        result.valid = true;
        return result;
    }

    // ---- Gradient -----------------------------------------------------------

    ::Gradient Gradient::Create()
    {
        return ::Gradient::CreateDefault();
    }

    ::Color Gradient::Evaluate(const ::Gradient& g, float time)
    {
        return g.Evaluate(time);
    }

    ::Gradient Gradient::SetKeys(const ::Gradient& g, const std::vector<GradientColorKey>& color_keys,
                                 const std::vector<GradientAlphaKey>& alpha_keys)
    {
        ::Gradient out = g;
        out.color_keys = color_keys;
        out.alpha_keys = alpha_keys;
        return out;
    }

    std::vector<GradientColorKey> Gradient::GetColorKeys(const ::Gradient& g)
    {
        return g.color_keys;
    }

    std::vector<GradientAlphaKey> Gradient::GetAlphaKeys(const ::Gradient& g)
    {
        return g.alpha_keys;
    }

    ::Gradient Gradient::WithColorKeys(const ::Gradient& g, const std::vector<GradientColorKey>& color_keys)
    {
        ::Gradient out = g;
        out.color_keys = color_keys;
        return out;
    }

    ::Gradient Gradient::WithAlphaKeys(const ::Gradient& g, const std::vector<GradientAlphaKey>& alpha_keys)
    {
        ::Gradient out = g;
        out.alpha_keys = alpha_keys;
        return out;
    }

    int Gradient::GetMode(const ::Gradient& g)
    {
        return static_cast<int>(g.mode);
    }

    ::Gradient Gradient::WithMode(const ::Gradient& g, int mode)
    {
        ::Gradient out = g;
        // 未知值一律当 Blend：mode 是位掩码式的枚举位，夹到合法集合比"存了个
        // 3 然后 Evaluate 走 default 分支"好 —— 后者在 Fixed 那一条上会静默
        // 变成插值。
        out.mode = (mode == static_cast<int>(::GradientMode::Fixed)) ? ::GradientMode::Fixed
                                                                    : ::GradientMode::Blend;
        return out;
    }
}  // namespace Runtime
