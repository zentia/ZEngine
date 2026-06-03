#pragma once

#include <cstdint>
#include <vector>

// Tiny ImGui-independent software rasterizer used by the native ZSlate preview
// window. Renders into a tightly-packed RGBA8 CPU buffer (byte order R,G,B,A --
// matching RHI_FORMAT_R8G8B8A8_UNORM) that callers upload to a ZSlate dynamic
// texture via UiGpuResources::UpdateDynamicTexture. Replaces the old ImGui
// draw-list path (AddTriangleFilled / AddRectFilled) used by the mesh and
// material previews, which is unavailable now that those panels paint through
// the retained ZSlate widget tree.
//
// Coordinates are in pixel space of the buffer (origin top-left, +y down).
// Fills are src-over alpha blended so translucent overlays (e.g. a tint quad on
// top of the gradient background) compose correctly; opaque fills (a == 255)
// take a fast overwrite path.
class PreviewRaster
{
public:
    static uint32_t Pack(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255)
    {
        return static_cast<uint32_t>(r) | (static_cast<uint32_t>(g) << 8) |
               (static_cast<uint32_t>(b) << 16) | (static_cast<uint32_t>(a) << 24);
    }

    void Resize(uint32_t width, uint32_t height);
    void Clear(uint32_t rgba);

    void FillRect(int x0, int y0, int x1, int y1, uint32_t rgba);
    // Vertical gradient: top color at y0, bottom color at y1 (linear in between).
    void FillRectVGradient(int x0, int y0, int x1, int y1, uint32_t top_rgba, uint32_t bottom_rgba);
    // Flat-shaded filled triangle (single color across the whole face).
    void FillTriangle(float ax, float ay, float bx, float by, float cx, float cy, uint32_t rgba);
    // Axis-aligned filled ellipse centered at (cx, cy) with radii (rx, ry).
    void FillEllipse(float cx, float cy, float rx, float ry, uint32_t rgba);
    // 1px-wide rectangle outline (4 edges).
    void StrokeRect(int x0, int y0, int x1, int y1, uint32_t rgba, int thickness = 1);

    uint32_t Width() const { return m_Width; }
    uint32_t Height() const { return m_Height; }
    const uint8_t* Data() const { return reinterpret_cast<const uint8_t*>(m_Pixels.data()); }
    bool Empty() const { return m_Pixels.empty(); }

private:
    void BlendPixel(uint32_t x, uint32_t y, uint32_t rgba);

    uint32_t m_Width {0};
    uint32_t m_Height {0};
    std::vector<uint32_t> m_Pixels;
};
