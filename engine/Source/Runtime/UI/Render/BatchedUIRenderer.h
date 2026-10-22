#pragma once

#include "Runtime/UI/Render/UIRenderer.h"
#include "Runtime/UI/Render/UIRenderBatch.h"

// Records Canvas draw commands into a CPU batch consumed by UIPass on the render thread.
class BatchedUIRenderer final : public UIRenderer
{
public:
    bool BeginFrame() override;
    void EndFrame() override {}

    void PushClipRect(const ZSlate::UIRect& clip_rect, bool intersect_with_current = true) override;
    void PopClipRect() override;

    void PushTransform(const UIAffine2D& transform) override;
    void PopTransform() override;

    void DrawQuad(const ZSlate::UIRect& rect, const ZSlate::UIColor& color) override;
    void DrawRect(const ZSlate::UIRect& rect, const ZSlate::UIColor& color, float thickness = 1.0f) override;
    void DrawConvexPoly(const Vector2* points, int count, const ZSlate::UIColor& color) override;

    void DrawTexturedQuad(const ZSlate::UIRect& rect,
                          void* texture_id,
                          const ZSlate::UIColor& color = ZSlate::UIColor(1, 1, 1, 1),
                          const Vector2& uv0 = Vector2(0.0f, 0.0f),
                          const Vector2& uv1 = Vector2(1.0f, 1.0f)) override;

    void DrawText(const ZSlate::UIRect& rect,
                  const std::string& text,
                  float font_size,
                  const ZSlate::UIColor& color,
                  TextAnchor alignment = TextAnchor::MiddleCenter,
                  TextWrapMode wrap = TextWrapMode::Wrap,
                  Font* font = nullptr) override;

    Vector2 MeasureText(const std::string& text,
                        float font_size,
                        TextWrapMode wrap = TextWrapMode::Wrap,
                        float wrap_width = 0.0f,
                        Font* font = nullptr) const override;

    Vector2 getDisplaySize() const override;

    // --- Batch ownership -------------------------------------------------
    // The game thread records into the *write* batch while the render thread
    // consumes a *different* one. Handing the render thread the batch that is
    // still being recorded is a data race, and it is not a theoretical one:
    // `RenderFramePipeline` lets the game thread run ahead
    // (`GetMaxFramesInFlight()`), so `BeginFrame()` -- whose first act is
    // `UIRenderBatch::Clear()` -- lands while the render thread is inside
    // `UploadBatch()` walking `GetVertices()`. The observed failure was
    //   vector subscript out of range, std::vector<UIVertex>::operator[]
    //   <- ZSlateEditorOverlay::UploadBatch
    // and because that assert parks the render thread in a modal dialog,
    // `RenderFramePipeline::WaitForFrameSlot()` never gets signalled and the
    // whole editor freezes with no further log output.
    //
    // `BeginFrame()` flips the write index, so what the render thread gets is
    // always the batch recorded by the *previous* `BeginFrame()` and therefore
    // never the live one.
    static constexpr int kBatchCount = 3;

    UIRenderBatch& getBatch() { return writeBatch(); }
    const UIRenderBatch& getBatch() const { return m_Batches[m_WriteIndex]; }

    // What the render thread must draw. With parallel rendering this is the
    // previous frame's batch; single-threaded builds record and draw in the same
    // frame, so there the live batch IS the right one.
    const UIRenderBatch& getConsumedBatch() const;

private:
    UIRenderBatch& writeBatch() { return m_Batches[m_WriteIndex]; }

    UIRenderBatch m_Batches[kBatchCount];
    int m_WriteIndex {0};
    Vector2 m_DisplaySize {0.0f, 0.0f};
    bool m_Active {false};
};

UIRenderer* CreateBatchedUIRenderer();
