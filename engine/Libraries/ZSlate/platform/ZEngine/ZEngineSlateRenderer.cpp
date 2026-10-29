#include "ZSlate/Backend/ZEngineSlateRenderer.h"

// Now we can include ZEngine headers
#include "Runtime/Core/Math/Vector2.h"
#include "Runtime/UI/Render/UIGpuResources.h"
#include "Runtime/UI/Render/UIRenderer.h"

namespace ZSlate
{
ZEngineSlateRenderer::ZEngineSlateRenderer(UIGpuResources* gpu_resources, UIRenderer* renderer)
    : m_GpuResources(gpu_resources), m_Renderer(renderer)
{
}

void ZEngineSlateRenderer::DrawQuad(const ZSlate::UIRect& rect, const UIColor& color)
{
    if (!m_Renderer)
        return;

    ::ZSlate::UIRect ui_rect(rect.x, rect.y, rect.w, rect.h);
    m_Renderer->DrawQuad(ui_rect, color);
}

void ZEngineSlateRenderer::DrawRect(const ZSlate::UIRect& rect, const UIColor& color, float thickness)
{
    if (!m_Renderer)
        return;

    ::ZSlate::UIRect ui_rect(rect.x, rect.y, rect.w, rect.h);
    m_Renderer->DrawRect(ui_rect, color, thickness);
}

void ZEngineSlateRenderer::DrawConvexPoly(const Vector2* points, int count, const UIColor& color)
{
    if (!m_Renderer || count < 3)
        return;

    // Convert ZSlate Vector2 to ZEngine Vector2
    std::vector<::Vector2> ui_points(count);
    for (int i = 0; i < count; ++i)
    {
        ui_points[i] = ::Vector2(points[i].x, points[i].y);
    }
    m_Renderer->DrawConvexPoly(ui_points.data(), count, color);
}

void ZEngineSlateRenderer::DrawRoundedRect(const ZSlate::UIRect& rect, float radius, const UIColor& color)
{
    (void)rect; (void)radius; (void)color;
}

void ZEngineSlateRenderer::DrawTexturedQuad(const ZSlate::UIRect& rect, void* texture_handle, const UIColor& tint)
{
    if (!m_Renderer)
        return;

    ::ZSlate::UIRect ui_rect(rect.x, rect.y, rect.w, rect.h);
    m_Renderer->DrawTexturedQuad(ui_rect, texture_handle, tint);
}

void ZEngineSlateRenderer::DrawBox(const ZSlate::UIRect& rect, const FMargin& margin, void* texture_handle, const UIColor& tint)
{
    if (!m_Renderer)
        return;

    ::ZSlate::UIRect ui_rect(rect.x, rect.y, rect.w, rect.h);
    ::FMargin ui_margin(margin.Left, margin.Top, margin.Right, margin.Bottom);
    m_Renderer->DrawTexturedQuad(ui_rect, texture_handle, tint, ::Vector2(0.0f, 0.0f), ::Vector2(1.0f, 1.0f));
}

void ZEngineSlateRenderer::DrawBorder(const ZSlate::UIRect& rect, const FMargin& margin, void* texture_handle, const UIColor& tint)
{
    if (!m_Renderer)
        return;

    ::ZSlate::UIRect ui_rect(rect.x, rect.y, rect.w, rect.h);
    m_Renderer->DrawRect(ui_rect, tint, 1.0f);
}

void ZEngineSlateRenderer::DrawTextLabel(const ZSlate::UIRect& rect, const std::string& text, float font_size,
                                      const UIColor& color, TextAnchor alignment, TextWrapMode wrap,
                                      void* font_handle)
{
    if (!m_Renderer)
        return;

    ::ZSlate::UIRect ui_rect(rect.x, rect.y, rect.w, rect.h);
    m_Renderer->DrawText(ui_rect, text, font_size, color, alignment, wrap,
                         static_cast<Font*>(font_handle));
}

void ZEngineSlateRenderer::DrawTextLabel(const std::string& text, const Vector2& pos, float font_size, const UIColor& color)
{
    if (!m_Renderer)
        return;

    ::ZSlate::UIRect ui_rect(pos.x, pos.y, 100.0f, font_size);
    m_Renderer->DrawText(ui_rect, text, font_size, color);
}

Vector2 ZEngineSlateRenderer::MeasureText(const std::string& text, float font_size) const
{
    if (!m_Renderer)
        return Vector2(0.0f, font_size);

    ::Vector2 result = m_Renderer->MeasureText(text, font_size, TextWrapMode::NoWrap, 0.0f, nullptr);
    return Vector2(result.x, result.y);
}

void ZEngineSlateRenderer::PushClipRect(const ZSlate::UIRect& rect)
{
    if (m_Renderer)
    {
        ::ZSlate::UIRect ui_rect(rect.x, rect.y, rect.w, rect.h);
        m_Renderer->PushClipRect(ui_rect);
    }
}

void ZEngineSlateRenderer::PopClipRect()
{
    if (m_Renderer)
        m_Renderer->PopClipRect();
}

void ZEngineSlateRenderer::BeginFrame()
{
    if (m_Renderer)
        m_Renderer->BeginFrame();
}

void ZEngineSlateRenderer::EndFrame()
{
    if (m_Renderer)
        m_Renderer->EndFrame();
}

void ZEngineSlateRenderer::Flush()
{
    // No-op
}
}  // namespace ZSlate
