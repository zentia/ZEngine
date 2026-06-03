#include "PreviewRaster.h"

#include <algorithm>
#include <cmath>

namespace
{
    inline uint8_t ChannelR(uint32_t c) { return static_cast<uint8_t>(c & 0xFFu); }
    inline uint8_t ChannelG(uint32_t c) { return static_cast<uint8_t>((c >> 8) & 0xFFu); }
    inline uint8_t ChannelB(uint32_t c) { return static_cast<uint8_t>((c >> 16) & 0xFFu); }
    inline uint8_t ChannelA(uint32_t c) { return static_cast<uint8_t>((c >> 24) & 0xFFu); }

    inline uint8_t LerpU8(uint8_t a, uint8_t b, float t)
    {
        const float v = static_cast<float>(a) + (static_cast<float>(b) - static_cast<float>(a)) * t;
        return static_cast<uint8_t>(std::clamp(v + 0.5f, 0.0f, 255.0f));
    }
}  // namespace

void PreviewRaster::Resize(uint32_t width, uint32_t height)
{
    m_Width = width;
    m_Height = height;
    m_Pixels.assign(static_cast<size_t>(width) * static_cast<size_t>(height), 0u);
}

void PreviewRaster::Clear(uint32_t rgba)
{
    std::fill(m_Pixels.begin(), m_Pixels.end(), rgba);
}

void PreviewRaster::BlendPixel(uint32_t x, uint32_t y, uint32_t rgba)
{
    if (x >= m_Width || y >= m_Height)
    {
        return;
    }
    const size_t idx = static_cast<size_t>(y) * static_cast<size_t>(m_Width) + static_cast<size_t>(x);

    const uint8_t sa = ChannelA(rgba);
    if (sa == 255)
    {
        m_Pixels[idx] = rgba;
        return;
    }
    if (sa == 0)
    {
        return;
    }

    const uint32_t dst = m_Pixels[idx];
    const float a = static_cast<float>(sa) / 255.0f;
    const float ia = 1.0f - a;
    const uint8_t r = static_cast<uint8_t>(ChannelR(rgba) * a + ChannelR(dst) * ia + 0.5f);
    const uint8_t g = static_cast<uint8_t>(ChannelG(rgba) * a + ChannelG(dst) * ia + 0.5f);
    const uint8_t b = static_cast<uint8_t>(ChannelB(rgba) * a + ChannelB(dst) * ia + 0.5f);
    const uint8_t da = ChannelA(dst);
    const uint8_t out_a = static_cast<uint8_t>(sa + da * ia + 0.5f);
    m_Pixels[idx] = Pack(r, g, b, out_a);
}

void PreviewRaster::FillRect(int x0, int y0, int x1, int y1, uint32_t rgba)
{
    if (x1 < x0)
        std::swap(x0, x1);
    if (y1 < y0)
        std::swap(y0, y1);
    const int xmin = std::max(0, x0);
    const int ymin = std::max(0, y0);
    const int xmax = std::min(static_cast<int>(m_Width), x1);
    const int ymax = std::min(static_cast<int>(m_Height), y1);
    for (int y = ymin; y < ymax; ++y)
    {
        for (int x = xmin; x < xmax; ++x)
        {
            BlendPixel(static_cast<uint32_t>(x), static_cast<uint32_t>(y), rgba);
        }
    }
}

void PreviewRaster::FillRectVGradient(int x0, int y0, int x1, int y1, uint32_t top_rgba, uint32_t bottom_rgba)
{
    if (x1 < x0)
        std::swap(x0, x1);
    if (y1 < y0)
        std::swap(y0, y1);
    const int xmin = std::max(0, x0);
    const int ymin = std::max(0, y0);
    const int xmax = std::min(static_cast<int>(m_Width), x1);
    const int ymax = std::min(static_cast<int>(m_Height), y1);
    const float span = static_cast<float>(std::max(1, y1 - y0));
    for (int y = ymin; y < ymax; ++y)
    {
        const float t = static_cast<float>(y - y0) / span;
        const uint32_t row = Pack(LerpU8(ChannelR(top_rgba), ChannelR(bottom_rgba), t),
                                  LerpU8(ChannelG(top_rgba), ChannelG(bottom_rgba), t),
                                  LerpU8(ChannelB(top_rgba), ChannelB(bottom_rgba), t),
                                  LerpU8(ChannelA(top_rgba), ChannelA(bottom_rgba), t));
        for (int x = xmin; x < xmax; ++x)
        {
            BlendPixel(static_cast<uint32_t>(x), static_cast<uint32_t>(y), row);
        }
    }
}

void PreviewRaster::FillEllipse(float cx, float cy, float rx, float ry, uint32_t rgba)
{
    if (rx < 0.5f || ry < 0.5f)
    {
        return;
    }
    const int min_x = std::max(0, static_cast<int>(std::floor(cx - rx)));
    const int min_y = std::max(0, static_cast<int>(std::floor(cy - ry)));
    const int max_x = std::min(static_cast<int>(m_Width) - 1, static_cast<int>(std::ceil(cx + rx)));
    const int max_y = std::min(static_cast<int>(m_Height) - 1, static_cast<int>(std::ceil(cy + ry)));
    const float inv_rx2 = 1.0f / (rx * rx);
    const float inv_ry2 = 1.0f / (ry * ry);
    for (int y = min_y; y <= max_y; ++y)
    {
        const float dy = (static_cast<float>(y) + 0.5f) - cy;
        for (int x = min_x; x <= max_x; ++x)
        {
            const float dx = (static_cast<float>(x) + 0.5f) - cx;
            if (dx * dx * inv_rx2 + dy * dy * inv_ry2 <= 1.0f)
            {
                BlendPixel(static_cast<uint32_t>(x), static_cast<uint32_t>(y), rgba);
            }
        }
    }
}

void PreviewRaster::StrokeRect(int x0, int y0, int x1, int y1, uint32_t rgba, int thickness)
{
    if (x1 < x0)
        std::swap(x0, x1);
    if (y1 < y0)
        std::swap(y0, y1);
    const int t = std::max(1, thickness);
    FillRect(x0, y0, x1, y0 + t, rgba);          // top
    FillRect(x0, y1 - t, x1, y1, rgba);          // bottom
    FillRect(x0, y0, x0 + t, y1, rgba);          // left
    FillRect(x1 - t, y0, x1, y1, rgba);          // right
}

void PreviewRaster::FillTriangle(float ax, float ay, float bx, float by, float cx, float cy, uint32_t rgba)
{
    const int min_x = std::max(0, static_cast<int>(std::floor(std::min({ax, bx, cx}))));
    const int min_y = std::max(0, static_cast<int>(std::floor(std::min({ay, by, cy}))));
    const int max_x = std::min(static_cast<int>(m_Width) - 1, static_cast<int>(std::ceil(std::max({ax, bx, cx}))));
    const int max_y = std::min(static_cast<int>(m_Height) - 1, static_cast<int>(std::ceil(std::max({ay, by, cy}))));
    if (min_x > max_x || min_y > max_y)
    {
        return;
    }

    // Signed area * 2; reject degenerate triangles.
    const float area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
    if (std::fabs(area) < 1e-6f)
    {
        return;
    }
    const float inv_area = 1.0f / area;

    for (int y = min_y; y <= max_y; ++y)
    {
        const float py = static_cast<float>(y) + 0.5f;
        for (int x = min_x; x <= max_x; ++x)
        {
            const float px = static_cast<float>(x) + 0.5f;
            // Barycentric edge functions (consistent winding via inv_area sign).
            const float w0 = ((bx - px) * (cy - py) - (by - py) * (cx - px)) * inv_area;
            const float w1 = ((cx - px) * (ay - py) - (cy - py) * (ax - px)) * inv_area;
            const float w2 = 1.0f - w0 - w1;
            if (w0 >= 0.0f && w1 >= 0.0f && w2 >= 0.0f)
            {
                BlendPixel(static_cast<uint32_t>(x), static_cast<uint32_t>(y), rgba);
            }
        }
    }
}
