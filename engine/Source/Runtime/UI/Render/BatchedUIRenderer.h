#pragma once

#include "Runtime/UI/Render/UIRenderer.h"
#include "Runtime/UI/Render/UiRenderBatch.h"

// Records Canvas draw commands into a CPU batch consumed by UIPass on the render thread.
class BatchedUIRenderer final : public UIRenderer
{
public:
    bool BeginFrame() override;
    void EndFrame() override {}

    void PushClipRect(const UIRect& clip_rect, bool intersect_with_current = true) override;
    void PopClipRect() override;

    void PushTransform(const UiAffine2D& transform) override;
    void PopTransform() override;

    void DrawQuad(const UIRect& rect, const UIColor& color) override;
    void DrawRect(const UIRect& rect, const UIColor& color, float thickness = 1.0f) override;
    void DrawConvexPoly(const Vector2* points, int count, const UIColor& color) override;

    void DrawTexturedQuad(const UIRect& rect,
                          void* texture_id,
                          const UIColor& color = UIColor(1, 1, 1, 1),
                          const Vector2& uv0 = Vector2(0.0f, 0.0f),
                          const Vector2& uv1 = Vector2(1.0f, 1.0f)) override;

    void DrawText(const UIRect& rect,
                  const std::string& text,
                  float font_size,
                  const UIColor& color,
                  TextAnchor alignment = TextAnchor::MiddleCenter,
                  TextWrapMode wrap = TextWrapMode::Wrap,
                  Font* font = nullptr) override;

    Vector2 MeasureText(const std::string& text,
                        float font_size,
                        TextWrapMode wrap = TextWrapMode::Wrap,
                        float wrap_width = 0.0f,
                        Font* font = nullptr) const override;

    Vector2 getDisplaySize() const override;

    UiRenderBatch& getBatch() { return m_Batch; }
    const UiRenderBatch& getBatch() const { return m_Batch; }

private:
    UiRenderBatch m_Batch;
    Vector2 m_DisplaySize {0.0f, 0.0f};
    bool m_Active {false};
};

UIRenderer* CreateBatchedUIRenderer();
