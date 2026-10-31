#include "Runtime/Scripting/ScriptColor/ScriptColor.h"

#include <cmath>

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
}  // namespace Runtime
