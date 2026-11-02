#include "Texture2D.h"

#include <algorithm>

// 未压缩 RGBA8 的每像素字节数。GetPixel / SetPixel 只认这个布局。
constexpr std::size_t kBytesPerPixel = 4;

bool Texture2D::GetHasUncompressedPixels() const
{
    if (m_Width == 0 || m_Height == 0)
    {
        return false;
    }
    // 只用"实际字节数对不对"判，不看 m_Format —— 理由见头文件那节注释。
    // 用 64 位中间量：宽高各 65535 时 w*h*4 会超过 32 位的一半，虽然在
    // size_t 为 64 位的机器上不会溢出，但为了在 32 位目标上也成立写成这样。
    const std::size_t expected = static_cast<std::size_t>(m_Width) *
                                 static_cast<std::size_t>(m_Height) * kBytesPerPixel;
    return m_Pixels.size() >= expected;
}

Color Texture2D::GetPixel(int x, int y) const
{
    if (x < 0 || y < 0)
    {
        return ::Color32(0, 0, 0, 0);
    }
    const auto ux = static_cast<uint32_t>(x);
    const auto uy = static_cast<uint32_t>(y);
    if (ux >= m_Width || uy >= m_Height)
    {
        return ::Color32(0, 0, 0, 0);
    }
    if (!GetHasUncompressedPixels())
    {
        // 压缩格式上读不出像素。返回透明黑 —— 与"越界"同值是有意的：两者都是
        // "这里没有你要的那个像素"。
        return ::Color32(0, 0, 0, 0);
    }

    // mip 0 排在 m_Pixels 的最前面（见 m_MipOffsets 的注释），所以不需要查
    // 偏移表。多 mip 也成立：offsets[0] 恒为 0。
    const std::size_t index = (static_cast<std::size_t>(uy) * static_cast<std::size_t>(m_Width) +
                               static_cast<std::size_t>(ux)) * kBytesPerPixel;
    if (index + kBytesPerPixel > m_Pixels.size())
    {
        return ::Color32(0, 0, 0, 0);
    }

    const uint8_t* pixel = m_Pixels.data() + index;
    // 0..255 -> 0..1。用 255 而不是 256：这样 255 映射回 1.0，与
    // ColorUtility.ToHtmlStringRGB 的 floor(v*255+0.5) 是同一套舍入，来回
    // 转换不会漂。
    // Color 没有构造函数（DECLARE_SERIALIZE 的存在让它不能用圆括号聚合初始
    // 化），走 Color32 的隐式转换：那一条路就是 /255 的精确除，正是这里要的。
    return ::Color32(pixel[0], pixel[1], pixel[2], pixel[3]);
}

bool Texture2D::SetPixel(int x, int y, const Color& color)
{
    if (x < 0 || y < 0)
    {
        return false;
    }
    const auto ux = static_cast<uint32_t>(x);
    const auto uy = static_cast<uint32_t>(y);
    if (ux >= m_Width || uy >= m_Height)
    {
        return false;
    }
    if (!GetHasUncompressedPixels())
    {
        return false;
    }

    const std::size_t index = (static_cast<std::size_t>(uy) * static_cast<std::size_t>(m_Width) +
                               static_cast<std::size_t>(ux)) * kBytesPerPixel;
    if (index + kBytesPerPixel > m_Pixels.size())
    {
        return false;
    }

    // 与 GetPixel 用同一套舍入（floor(v*255+0.5)），并夹紧到 [0,255] ——
    // Color 的通道是 float，脚本完全可能给 1.5 或 -0.2，不夹会写成 0xFF 的
    // 溢出（uint8_t 截断），那种"我写 1.5 结果变成 0"极难查。
    auto to_byte = [](float v) -> uint8_t {
        const float scaled = v * 255.0f + 0.5f;
        if (scaled <= 0.0f)
        {
            return 0;
        }
        if (scaled >= 255.0f)
        {
            return 255;
        }
        return static_cast<uint8_t>(scaled);
    };

    uint8_t* pixel = m_Pixels.data() + index;
    pixel[0] = to_byte(color.r);
    pixel[1] = to_byte(color.g);
    pixel[2] = to_byte(color.b);
    pixel[3] = to_byte(color.a);
    return true;
}

bool Texture2D::ClearPixels(const Color& color)
{
    if (!GetHasUncompressedPixels())
    {
        return false;
    }

    // 只填 mip 0。多 mip 的更高级 mip 不动 —— 理由见头文件 SetPixel 的注释
    // （没有 Apply 可以重算 mip，动一半不如不动，至少"mip 1 还是旧的"这件事
    // 是确定的）。
    const std::size_t mip0_bytes = static_cast<std::size_t>(m_Width) *
                                   static_cast<std::size_t>(m_Height) * kBytesPerPixel;

    auto to_byte = [](float v) -> uint8_t {
        const float scaled = v * 255.0f + 0.5f;
        if (scaled <= 0.0f)
        {
            return 0;
        }
        if (scaled >= 255.0f)
        {
            return 255;
        }
        return static_cast<uint8_t>(scaled);
    };

    const uint8_t channels[4] = {to_byte(color.r), to_byte(color.g), to_byte(color.b),
                                 to_byte(color.a)};
    for (std::size_t i = 0; i + 3 < mip0_bytes; i += kBytesPerPixel)
    {
        m_Pixels[i + 0] = channels[0];
        m_Pixels[i + 1] = channels[1];
        m_Pixels[i + 2] = channels[2];
        m_Pixels[i + 3] = channels[3];
    }
    return true;
}

bool Texture2D::Resize(int width, int height)
{
    if (width <= 0 || height <= 0)
    {
        return false;
    }
    // 上限 16384：超过这个尺寸的贴图在任何现存 GPU 上都创建失败，而失败会
    // 发生在"上传时"而不是这里。与其让脚本拿着一张注定上传不了的图，不如
    // 在这里拒。
    if (width > 16384 || height > 16384)
    {
        return false;
    }

    m_Width  = static_cast<uint32_t>(width);
    m_Height = static_cast<uint32_t>(height);

    const std::size_t bytes = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) *
                              kBytesPerPixel;
    m_Pixels.assign(bytes, 0);

    // 丢掉 mip 表：新缓冲是单 mip 的。留着旧表的话 GetMipSpan(level>0) 会
    // 拿旧偏移去切新缓冲 —— 越界读。
    m_MipOffsets.clear();
    return true;
}
