#include "Runtime/UI/Render/BatchedUIRenderer.h"

#include "Runtime/UI/Core/Font.h"
#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Function/Render/Texture/Texture2D.h"
#include "Runtime/Function/Render/WindowSystem.h"
#include "Runtime/UI/Render/TextGenerator.h"
#include "Runtime/UI/Render/UIGpuResources.h"
#include "Runtime/UI/Render/ZFontAtlas.h"

#include <algorithm>
#include <cstdint>

namespace
{
    void* GetWhiteTextureId()
    {
        UIGpuResources* gpu = UIGpuResources::Get();
        return gpu != nullptr ? gpu->GetWhiteTextureId() : nullptr;
    }
}  // namespace

bool BatchedUIRenderer::BeginFrame()
{
    m_Batch.Clear();
    m_DisplaySize = getDisplaySize();
    m_Active = m_DisplaySize.x > 0.0f && m_DisplaySize.y > 0.0f;
    return m_Active;
}

void BatchedUIRenderer::PushClipRect(const ZSlate::UIRect& clip_rect, bool intersect_with_current)
{
    if (!m_Active)
    {
        return;
    }
    m_Batch.PushClipRect(clip_rect, intersect_with_current);
}

void BatchedUIRenderer::PopClipRect()
{
    if (!m_Active)
    {
        return;
    }
    m_Batch.PopClipRect();
}

void BatchedUIRenderer::PushTransform(const UIAffine2D& transform)
{
    if (!m_Active)
    {
        return;
    }
    m_Batch.PushTransform(transform);
}

void BatchedUIRenderer::PopTransform()
{
    if (!m_Active)
    {
        return;
    }
    m_Batch.PopTransform();
}

void BatchedUIRenderer::DrawQuad(const ZSlate::UIRect& rect, const ZSlate::UIColor& color)
{
    if (!m_Active)
    {
        return;
    }
    m_Batch.DrawQuad(rect, color, GetWhiteTextureId());
}

void BatchedUIRenderer::DrawRect(const ZSlate::UIRect& rect, const ZSlate::UIColor& color, float thickness)
{
    if (!m_Active)
    {
        return;
    }
    m_Batch.DrawRect(rect, color, thickness, GetWhiteTextureId());
}

void BatchedUIRenderer::DrawConvexPoly(const Vector2* points, int count, const ZSlate::UIColor& color)
{
    if (!m_Active)
    {
        return;
    }
    m_Batch.DrawConvexPoly(points, count, color, GetWhiteTextureId());
}

void BatchedUIRenderer::DrawTexturedQuad(const ZSlate::UIRect& rect,
                                         void* texture_id,
                                         const ZSlate::UIColor& color,
                                         const Vector2& uv0,
                                         const Vector2& uv1)
{
    if (!m_Active)
    {
        return;
    }
    m_Batch.DrawTexturedQuad(rect, texture_id, color, uv0, uv1, GetWhiteTextureId());
}

void BatchedUIRenderer::DrawText(const ZSlate::UIRect& rect,
                                 const std::string& text,
                                 float font_size,
                                 const ZSlate::UIColor& color,
                                 TextAnchor alignment,
                                 TextWrapMode wrap,
                                 Font* font_asset)
{
    if (!m_Active || text.empty())
    {
        return;
    }

    UIGpuResources* gpu = UIGpuResources::Get();

    // Native glyph atlas path (stb_truetype via ZFontAtlas). The renderer only
    // resolves the atlas' GPU texture and submits geometry -- TextGenerator owns
    // the layout (UTF-8 decode, wrapping, tab stops, anchoring).
    if (gpu != nullptr)
    {
        ZFontAtlas* native = gpu->ResolveNativeFont(font_asset);
        if (native != nullptr && native->IsLoaded())
        {
            void* native_tex = gpu->GetNativeFontTextureId(native);
            if (native_tex != nullptr)
            {
                TextGenerator::Settings settings;
                settings.rect = UIRect(rect.x, rect.y, rect.w, rect.h);
                settings.font_size = font_size;
                settings.alignment = alignment;
                settings.wrap = wrap;

                TextGenerator generator;
                generator.Generate(*native, text, settings);

                void* white = GetWhiteTextureId();
                for (const TextGenerator::Glyph& g : generator.GetGlyphs())
                {
                    m_Batch.DrawTexturedQuad(g.dest, native_tex, color, g.uv0, g.uv1, white);
                }
                return;
            }
        }
    }

    // Native font atlas not ready yet -- draw a solid placeholder block sized to
    // the measured text extent so layout still reserves the right space.
    const Vector2 measured = MeasureText(text, font_size, wrap, rect.w, font_asset);
    ZSlate::UIRect text_rect = rect;
    text_rect.w = std::min(text_rect.w, measured.x);
    text_rect.h = std::min(text_rect.h, measured.y);
    DrawQuad(text_rect, color);
}

Vector2 BatchedUIRenderer::MeasureText(const std::string& text,
                                       float font_size,
                                       TextWrapMode wrap,
                                       float wrap_width,
                                       Font* font_asset) const
{
    if (text.empty())
    {
        return Vector2(0.0f, 0.0f);
    }

    UIGpuResources* gpu = UIGpuResources::Get();

    if (gpu != nullptr)
    {
        ZFontAtlas* native = gpu->ResolveNativeFont(font_asset);
        if (native != nullptr && native->IsLoaded())
        {
            return TextGenerator::Measure(*native, text, font_size, wrap, wrap_width);
        }
    }

    // Native atlas not loaded yet: rough monospace estimate (matches the
    // placeholder block DrawText paints in the same situation).
    return Vector2(static_cast<float>(text.size()) * font_size * 0.5f, font_size * 1.2f);
}

Vector2 BatchedUIRenderer::getDisplaySize() const
{
    // ZSlate records geometry in logical framebuffer coordinates; keep this in
    // sync with the UIPass NDC mapping. The runtime UI surface owns the whole
    // framebuffer, so the framebuffer size IS the display size.
    if (auto window = GET_SYSTEM(WindowSystem))
    {
        const std::array<int, 2> size = window->GetFramebufferSize();
        return Vector2(static_cast<float>(size[0]), static_cast<float>(size[1]));
    }
    return Vector2(0.0f, 0.0f);
}

UIRenderer* CreateBatchedUIRenderer()
{
    return new BatchedUIRenderer();
}
