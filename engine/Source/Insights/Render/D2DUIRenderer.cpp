#include "Insights/Render/D2DUIRenderer.h"

// Include <windows.h> BEFORE <d2d1.h> so the winuser DrawText -> DrawTextW macro
// is active when d2d1.h declares ID2D1RenderTarget; the member is then named
// DrawTextW and our DrawText() call (rewritten by the same macro) matches it.
#include <windows.h>

#include <d2d1.h>
#include <dwrite.h>

#include <algorithm>
#include <cmath>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

namespace ZInsights
{
namespace
{
D2D1_COLOR_F ToColor(const ZSlate::UIColor& c)
{
    return D2D1::ColorF(c.x, c.y, c.z, c.w);
}

D2D1_RECT_F ToRect(const ZSlate::UIRect& r)
{
    return D2D1::RectF(r.x, r.y, r.x + r.w, r.y + r.h);
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

int FontKey(float font_size)
{
    return (std::max)(1, static_cast<int>(std::lround(font_size)));
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
    m_ClipDepth = 0;
}

void D2DUIRenderer::EnsureBrush()
{
    if (m_Brush == nullptr && m_Target != nullptr)
        m_Target->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), &m_Brush);
}

IDWriteTextFormat* D2DUIRenderer::GetTextFormat(float font_size) const
{
    // Text formats are device-independent DirectWrite objects, so caching them
    // from a const method (MeasureText) is safe; m_Formats is mutable.
    const int key = FontKey(font_size);
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

void D2DUIRenderer::PushClipRect(const ZSlate::UIRect& rect)
{
    if (m_Target == nullptr)
        return;
    // Direct2D's PushAxisAlignedClip always intersects with the current clip, so
    // nested pushes narrow the visible region (matches ZSlate's clip semantics).
    m_Target->PushAxisAlignedClip(ToRect(rect), D2D1_ANTIALIAS_MODE_ALIASED);
    ++m_ClipDepth;
}

void D2DUIRenderer::PopClipRect()
{
    if (m_Target == nullptr || m_ClipDepth <= 0)
        return;
    m_Target->PopAxisAlignedClip();
    --m_ClipDepth;
}

void D2DUIRenderer::DrawQuad(const ZSlate::UIRect& rect, const ZSlate::UIColor& color)
{
    if (m_Target == nullptr)
        return;
    EnsureBrush();
    if (m_Brush == nullptr)
        return;
    m_Brush->SetColor(ToColor(color));
    m_Target->FillRectangle(ToRect(rect), m_Brush);
}

void D2DUIRenderer::DrawRect(const ZSlate::UIRect& rect, const ZSlate::UIColor& color, float thickness)
{
    if (m_Target == nullptr)
        return;
    EnsureBrush();
    if (m_Brush == nullptr)
        return;
    m_Brush->SetColor(ToColor(color));
    m_Target->DrawRectangle(ToRect(rect), m_Brush, thickness);
}

void D2DUIRenderer::DrawConvexPoly(const ZSlate::Vector2* points, int count, const ZSlate::UIColor& color)
{
    if (m_Target == nullptr || points == nullptr || count < 3)
        return;
    EnsureBrush();
    if (m_Brush == nullptr)
        return;

    ID2D1Factory* factory = nullptr;
    m_Target->GetFactory(&factory);
    if (factory == nullptr)
        return;

    ID2D1PathGeometry* geometry = nullptr;
    if (SUCCEEDED(factory->CreatePathGeometry(&geometry)) && geometry)
    {
        ID2D1GeometrySink* sink = nullptr;
        if (SUCCEEDED(geometry->Open(&sink)) && sink)
        {
            sink->BeginFigure(D2D1::Point2F(points[0].x, points[0].y), D2D1_FIGURE_BEGIN_FILLED);
            for (int i = 1; i < count; ++i)
                sink->AddLine(D2D1::Point2F(points[i].x, points[i].y));
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            sink->Close();
            sink->Release();

            m_Brush->SetColor(ToColor(color));
            m_Target->FillGeometry(geometry, m_Brush);
        }
        geometry->Release();
    }
    factory->Release();
}

void D2DUIRenderer::DrawRoundedRect(const ZSlate::UIRect& rect, float radius, const ZSlate::UIColor& color)
{
    if (m_Target == nullptr)
        return;
    EnsureBrush();
    if (m_Brush == nullptr)
        return;
    m_Brush->SetColor(ToColor(color));
    m_Target->FillRoundedRectangle(D2D1::RoundedRect(ToRect(rect), radius, radius), m_Brush);
}

void D2DUIRenderer::DrawTexturedQuad(const ZSlate::UIRect& rect, void* /*texture_handle*/,
                                     const ZSlate::UIColor& tint)
{
    // The standalone viewer has no texture path; fall back to a solid fill so
    // any accidental call still produces something sensible.
    DrawQuad(rect, tint);
}

void D2DUIRenderer::DrawBox(const ZSlate::UIRect& rect, const ZSlate::FMargin& /*margin*/,
                            void* /*texture_handle*/, const ZSlate::UIColor& tint)
{
    DrawQuad(rect, tint);
}

void D2DUIRenderer::DrawBorder(const ZSlate::UIRect& rect, const ZSlate::FMargin& margin,
                               void* /*texture_handle*/, const ZSlate::UIColor& tint)
{
    // Untextured approximation: paint the four margin bands as solid strips.
    const float l = (std::min)(margin.Left, rect.w);
    const float r = (std::min)(margin.Right, rect.w);
    const float t = (std::min)(margin.Top, rect.h);
    const float b = (std::min)(margin.Bottom, rect.h);
    if (t > 0.0f)
        DrawQuad(ZSlate::UIRect(rect.x, rect.y, rect.w, t), tint);
    if (b > 0.0f)
        DrawQuad(ZSlate::UIRect(rect.x, rect.y + rect.h - b, rect.w, b), tint);
    if (l > 0.0f)
        DrawQuad(ZSlate::UIRect(rect.x, rect.y + t, l, rect.h - t - b), tint);
    if (r > 0.0f)
        DrawQuad(ZSlate::UIRect(rect.x + rect.w - r, rect.y + t, r, rect.h - t - b), tint);
}

void D2DUIRenderer::DrawTextLabel(const ZSlate::UIRect& rect, const std::string& text, float font_size,
                                  const ZSlate::UIColor& color, ZSlate::TextAnchor alignment,
                                  ZSlate::TextWrapMode wrap, void* /*font_handle*/)
{
    if (m_Target == nullptr || text.empty())
        return;
    EnsureBrush();
    IDWriteTextFormat* fmt = GetTextFormat(font_size);
    if (m_Brush == nullptr || fmt == nullptr)
        return;

    using ZSlate::TextAnchor;
    DWRITE_TEXT_ALIGNMENT h = DWRITE_TEXT_ALIGNMENT_LEADING;
    switch (alignment)
    {
        case TextAnchor::TopCenter:
        case TextAnchor::MiddleCenter:
        case TextAnchor::BottomCenter:
            h = DWRITE_TEXT_ALIGNMENT_CENTER;
            break;
        case TextAnchor::TopRight:
        case TextAnchor::MiddleRight:
        case TextAnchor::BottomRight:
            h = DWRITE_TEXT_ALIGNMENT_TRAILING;
            break;
        default:
            h = DWRITE_TEXT_ALIGNMENT_LEADING;
            break;
    }

    DWRITE_PARAGRAPH_ALIGNMENT v = DWRITE_PARAGRAPH_ALIGNMENT_CENTER;
    switch (alignment)
    {
        case TextAnchor::TopLeft:
        case TextAnchor::TopCenter:
        case TextAnchor::TopRight:
            v = DWRITE_PARAGRAPH_ALIGNMENT_NEAR;
            break;
        case TextAnchor::BottomLeft:
        case TextAnchor::BottomCenter:
        case TextAnchor::BottomRight:
            v = DWRITE_PARAGRAPH_ALIGNMENT_FAR;
            break;
        default:
            v = DWRITE_PARAGRAPH_ALIGNMENT_CENTER;
            break;
    }

    fmt->SetTextAlignment(h);
    fmt->SetParagraphAlignment(v);
    fmt->SetWordWrapping(wrap == ZSlate::TextWrapMode::NoWrap ? DWRITE_WORD_WRAPPING_NO_WRAP
                                                              : DWRITE_WORD_WRAPPING_WRAP);

    m_Brush->SetColor(ToColor(color));
    const std::wstring w = ToWide(text);
    m_Target->DrawText(w.c_str(), static_cast<UINT32>(w.size()), fmt, ToRect(rect), m_Brush,
                       D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
}

void D2DUIRenderer::DrawTextLabel(const std::string& text, const ZSlate::Vector2& pos, float font_size,
                                  const ZSlate::UIColor& color)
{
    const ZSlate::Vector2 size = MeasureText(text, font_size);
    DrawTextLabel(ZSlate::UIRect(pos.x, pos.y, size.x + 1.0f, size.y + 1.0f), text, font_size, color,
                  ZSlate::TextAnchor::TopLeft, ZSlate::TextWrapMode::NoWrap);
}

ZSlate::Vector2 D2DUIRenderer::MeasureText(const std::string& text, float font_size) const
{
    if (m_DWrite == nullptr || text.empty())
        return ZSlate::Vector2(0.0f, 0.0f);

    IDWriteTextFormat* fmt = GetTextFormat(font_size);
    if (fmt == nullptr)
        return ZSlate::Vector2(0.0f, 0.0f);

    const std::wstring w = ToWide(text);
    IDWriteTextLayout* layout = nullptr;
    m_DWrite->CreateTextLayout(w.c_str(), static_cast<UINT32>(w.size()), fmt, 100000.0f, 100000.0f, &layout);
    ZSlate::Vector2 size(0.0f, 0.0f);
    if (layout)
    {
        layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        DWRITE_TEXT_METRICS m {};
        if (SUCCEEDED(layout->GetMetrics(&m)))
            size = ZSlate::Vector2(m.widthIncludingTrailingWhitespace, m.height);
        layout->Release();
    }
    return size;
}
}  // namespace ZInsights
