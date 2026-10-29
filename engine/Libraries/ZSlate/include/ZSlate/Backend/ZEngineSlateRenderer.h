#pragma once

// Bridge between ZSlate's ISlateRenderer interface and ZEngine's UiGpuResources.
// This file lives OUTSIDE the ZSlate namespace and includes ZEngine headers,
// so it can only be used when linking against ZEngine.

#include "ZSlate/Core/SlatePaint.h"

// Forward declare ZEngine types to avoid including heavy headers
class UIGpuResources;
class UIRenderer;

namespace ZSlate
{
// ZEngine implementation of ISlateRenderer.
// Wraps UiGpuResources and UIRenderer and translates ZSlate draw calls into ZEngine's batch system.
class ZEngineSlateRenderer : public ISlateRenderer
{
public:
    explicit ZEngineSlateRenderer(UIGpuResources* gpu_resources, UIRenderer* renderer);
    ~ZEngineSlateRenderer() override = default;

    // ISlateRenderer implementation
    void DrawQuad(const UIRect& rect, const UIColor& color) override;
    void DrawRect(const UIRect& rect, const UIColor& color, float thickness = 1.0f) override;
    void DrawConvexPoly(const Vector2* points, int count, const UIColor& color) override;
    void DrawRoundedRect(const UIRect& rect, float radius, const UIColor& color) override;
    void DrawTexturedQuad(const UIRect& rect, void* texture_handle, const UIColor& tint = Colors::White) override;
    void DrawBox(const UIRect& rect, const FMargin& margin, void* texture_handle, const UIColor& tint) override;
    void DrawBorder(const UIRect& rect, const FMargin& margin, void* texture_handle, const UIColor& tint) override;

    void DrawTextLabel(const UIRect& rect, const std::string& text, float font_size, const UIColor& color,
                       TextAnchor alignment = TextAnchor::MiddleLeft, TextWrapMode wrap = TextWrapMode::NoWrap,
                       void* font_handle = nullptr) override;
    void DrawTextLabel(const std::string& text, const Vector2& pos, float font_size, const UIColor& color) override;
    Vector2 MeasureText(const std::string& text, float font_size) const override;

    void PushClipRect(const UIRect& rect) override;
    void PopClipRect() override;

    void BeginFrame() override;
    void EndFrame() override;
    void Flush() override;

private:
    UIGpuResources* m_GpuResources;
    UIRenderer* m_Renderer;
};

}  // namespace ZSlate
