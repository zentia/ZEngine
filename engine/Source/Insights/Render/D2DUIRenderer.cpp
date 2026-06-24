#include "Insights/Render/D2DUIRenderer.h"

// Include <windows.h> BEFORE <d2d1.h> so the winuser DrawText -> DrawTextW macro
// is active when d2d1.h declares ID2D1RenderTarget; the member is then named
// DrawTextW and our DrawText() call (rewritten by the same macro) matches it.
#include <windows.h>

#include <d2d1.h>
#include <dwrite.h>

#include <cmath>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

namespace ZInsights
{
namespace
{
D2D1_COLOR_F ToColor(const UIColor& c)
{
    return D2D1::ColorF(c.x, c.y, c.z, c.w);
}

D2D1_RECT_F ToRect(const UIRect& r)
{
    return D2D1::RectF(r.x, r.y, r.x + r.width, r.y + r.height);
}

std::wstring ToWide(const std::string& utf8)
{
    if (utf8.empty())
        return std::wstring();
    const int needed =
        MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0)
        return std::wstring();
    std::wstring out(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), out.data(), needed);
    return out;
}
}  // namespace

D2DUIRenderer::D2DUIRenderer()
{
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                        reinterpret_cast<IUnknown**>(&m_DWrite));
}

D2DUIRenderer::~D2DUIRenderer()
{
    for (auto& kv : m_Formats)
    {
        if (kv.second)
            kv.second->Release();
    }
    m_Formats.clear();
    if (m_Brush)
        m_Brush->Release();
    if (m_DWrite)
        m_DWrite->Release();
}

void D2DUIRenderer::SetRenderTarget(ID2D1HwndRenderTarget* target)
{
    // Brushes are device-dependent: a brush created against the old target is
    // invalid for a new one, so drop it and recreate lazily.
    if (m_Brush)
    {
        m_Brush->Release();
        m_Brush = nullptr;
    }
    m_Target = target;
}

void D2DUIRenderer::EnsureBrush()
{
    if (m_Brush == nullptr && m_Target != nullptr)
        m_Target->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), &m_Brush);
}

IDWriteTextFormat* D2DUIRenderer::GetTextFormat(float font_size)
{
    const int key = std::max(1, static_cast<int>(std::lround(font_size)));
    auto it = m_Formats.find(key);
    if (it != m_Formats.end())
        return it->second;

    IDWriteTextFormat* fmt = nullptr;
    if (m_DWrite)
    {
        m_DWrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                   DWRITE_FONT_STRETCH_NORMAL, static_cast<float>(key), L"en-us", &fmt);
    }
    m_Formats[key] = fmt;
    return fmt;
}

void D2DUIRenderer::PushClipRect(const UIRect& clip_rect, bool /*intersect_with_current*/)
{
    if (m_Target == nullptr)
        return;
    // Direct2D's PushAxisAlignedClip always intersects with the current clip, so
    // the intersect flag is implicit; nested pushes narrow the visible region.
    m_Target->PushAxisAlignedClip(ToRect(clip_rect), D2D1_ANTIALIAS_MODE_ALIASED);
    ++m_ClipDepth;
}

void D2DUIRenderer::PopClipRect()
{
    if (m_Target == nullptr || m_ClipDepth <= 0)
        return;
    m_Target->PopAxisAlignedClip();
    --m_ClipDepth;
}

void D2DUIRenderer::DrawQuad(const UIRect& rect, const UIColor& color)
{
    if (m_Target == nullptr)
        return;
    EnsureBrush();
    if (m_Brush == nullptr)
        return;
    m_Brush->SetColor(ToColor(color));
    m_Target->FillRectangle(ToRect(rect), m_Brush);
}

void D2DUIRenderer::DrawRect(const UIRect& rect, const UIColor& color, float thickness)
{
    if (m_Target == nullptr)
        return;
    EnsureBrush();
    if (m_Brush == nullptr)
        return;
    m_Brush->SetColor(ToColor(color));
    m_Target->DrawRectangle(ToRect(rect), m_Brush, thickness);
}

void D2DUIRenderer::DrawTexturedQuad(const UIRect& rect, void* /*texture_id*/, const UIColor& color,
                                     const Vector2& /*uv0*/, const Vector2& /*uv1*/)
{
    // The standalone viewer never draws textured quads; fall back to a solid
    // fill so any accidental call still produces something sensible.
    DrawQuad(rect, color);
}

void D2DUIRenderer::DrawText(const UIRect& rect, const std::string& text, float font_size, const UIColor& color,
                             TextAnchor alignment, TextWrapMode wrap, Font* /*font*/)
{
    if (m_Target == nullptr || text.empty())
        return;
    EnsureBrush();
    IDWriteTextFormat* fmt = GetTextFormat(font_size);
    if (m_Brush == nullptr || fmt == nullptr)
        return;

    DWRITE_TEXT_ALIGNMENT h = DWRITE_TEXT_ALIGNMENT_LEADING;
    switch (alignment)
    {
        case TextAnchor::UpperCenter:
        case TextAnchor::MiddleCenter:
        case TextAnchor::LowerCenter:
            h = DWRITE_TEXT_ALIGNMENT_CENTER;
            break;
        case TextAnchor::UpperRight:
        case TextAnchor::MiddleRight:
        case TextAnchor::LowerRight:
            h = DWRITE_TEXT_ALIGNMENT_TRAILING;
            break;
        default:
            h = DWRITE_TEXT_ALIGNMENT_LEADING;
            break;
    }

    DWRITE_PARAGRAPH_ALIGNMENT v = DWRITE_PARAGRAPH_ALIGNMENT_CENTER;
    switch (alignment)
    {
        case TextAnchor::UpperLeft:
        case TextAnchor::UpperCenter:
        case TextAnchor::UpperRight:
            v = DWRITE_PARAGRAPH_ALIGNMENT_NEAR;
            break;
        case TextAnchor::LowerLeft:
        case TextAnchor::LowerCenter:
        case TextAnchor::LowerRight:
            v = DWRITE_PARAGRAPH_ALIGNMENT_FAR;
            break;
        default:
            v = DWRITE_PARAGRAPH_ALIGNMENT_CENTER;
            break;
    }

    fmt->SetTextAlignment(h);
    fmt->SetParagraphAlignment(v);
    fmt->SetWordWrapping(wrap == TextWrapMode::NoWrap ? DWRITE_WORD_WRAPPING_NO_WRAP
                                                      : DWRITE_WORD_WRAPPING_WRAP);

    m_Brush->SetColor(ToColor(color));
    const std::wstring w = ToWide(text);
    m_Target->DrawText(w.c_str(), static_cast<UINT32>(w.size()), fmt, ToRect(rect), m_Brush,
                       D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
}

Vector2 D2DUIRenderer::MeasureText(const std::string& text, float font_size, TextWrapMode /*wrap*/,
                                   float wrap_width, Font* /*font*/) const
{
    if (m_DWrite == nullptr || text.empty())
        return Vector2(0.0f, 0.0f);

    // GetTextFormat is non-const (caches); re-fetch via a local create to keep
    // this method const. Reuse the cached format if present.
    auto it = m_Formats.find(std::max(1, static_cast<int>(std::lround(font_size))));
    IDWriteTextFormat* fmt = (it != m_Formats.end()) ? it->second : nullptr;
    IDWriteTextFormat* temp = nullptr;
    if (fmt == nullptr)
    {
        m_DWrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                   DWRITE_FONT_STRETCH_NORMAL, std::max(1.0f, font_size), L"en-us", &temp);
        fmt = temp;
    }
    if (fmt == nullptr)
        return Vector2(0.0f, 0.0f);

    const std::wstring w = ToWide(text);
    const float max_w = wrap_width > 0.0f ? wrap_width : 100000.0f;
    IDWriteTextLayout* layout = nullptr;
    m_DWrite->CreateTextLayout(w.c_str(), static_cast<UINT32>(w.size()), fmt, max_w, 100000.0f, &layout);
    Vector2 size(0.0f, 0.0f);
    if (layout)
    {
        DWRITE_TEXT_METRICS m {};
        if (SUCCEEDED(layout->GetMetrics(&m)))
            size = Vector2(m.widthIncludingTrailingWhitespace, m.height);
        layout->Release();
    }
    if (temp)
        temp->Release();
    return size;
}

Vector2 D2DUIRenderer::getDisplaySize() const
{
    if (m_Target == nullptr)
        return Vector2(0.0f, 0.0f);
    const D2D1_SIZE_F s = m_Target->GetSize();
    return Vector2(s.width, s.height);
}
}  // namespace ZInsights
