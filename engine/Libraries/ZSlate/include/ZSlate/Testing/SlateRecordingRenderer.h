#pragma once

// ============================================================================
// FSlateRecordingRenderer - headless ISlateRenderer that records draw calls
// ============================================================================
//
// TEST-ONLY. Not a production backend: it draws nothing, it only records.
// (Same role as UE's SlateNullRenderer, which lives outside SlateCore.)
//
// Lets a ZSlate UI be verified without a GPU, a window or a human looking at
// pixels. Things a screenshot cannot show:
//
//   * whether list/tree virtualisation actually works - a correct frame must
//     emit a number of text draws proportional to the *visible* rows, not to
//     the total item count;
//   * whether the clip stack is balanced - every PushClipRect must have a
//     matching PopClipRect, otherwise later widgets get silently clipped away.
//
// Screenshots are also unreliable for this: a DXGI flip-model swapchain hands
// GDI (PrintWindow) a blank surface, so the capture is white regardless of
// what was rendered.
//
// MeasureText is deterministic (~0.6 em per byte) and needs no font file, so
// layout assertions are stable across machines. Compose it with
// FSlateDpiScaleRenderer to verify DPI scaling the same way.

#include "ZSlate/Core/SlatePaint.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ZSlate
{
struct FSlateRecordedText
{
    UIRect Rect;
    std::string Text;
    float FontSize;
};

class FSlateRecordingRenderer : public ISlateRenderer
{
public:
    // --- Recorded output ---------------------------------------------------
    std::vector<FSlateRecordedText> Texts;
    std::vector<UIRect> Quads;
    int RectCount {0};
    int PushClipCount {0};
    int PopClipCount {0};
    int MaxClipDepth {0};
    bool ClipUnderflow {false};  // PopClipRect with an empty stack

    void Reset()
    {
        Texts.clear();
        Quads.clear();
        RectCount = 0;
        PushClipCount = 0;
        PopClipCount = 0;
        MaxClipDepth = 0;
        ClipUnderflow = false;
        m_ClipDepth = 0;
    }

    bool IsClipBalanced() const { return !ClipUnderflow && m_ClipDepth == 0; }

    // True when any recorded text contains `needle`.
    bool HasText(const std::string& needle) const
    {
        for (const FSlateRecordedText& t : Texts)
        {
            if (t.Text.find(needle) != std::string::npos)
                return true;
        }
        return false;
    }

    // --- ISlateRenderer ----------------------------------------------------
    void DrawQuad(const UIRect& rect, const UIColor&) override { Quads.push_back(rect); }
    void DrawRect(const UIRect&, const UIColor&, float) override { ++RectCount; }
    void DrawConvexPoly(const Vector2*, int, const UIColor&) override {}
    void DrawRoundedRect(const UIRect& rect, float, const UIColor&) override { Quads.push_back(rect); }
    void DrawTexturedQuad(const UIRect&, void*, const UIColor&) override {}
    void DrawBox(const UIRect&, const FMargin&, void*, const UIColor&) override {}
    void DrawBorder(const UIRect&, const FMargin&, void*, const UIColor&) override {}

    void DrawTextLabel(const UIRect& rect, const std::string& text, float font_size,
                       const UIColor&, TextAnchor, TextWrapMode, void*) override
    {
        if (!text.empty())
            Texts.push_back(FSlateRecordedText {rect, text, font_size});
    }

    void DrawTextLabel(const std::string& text, const Vector2& pos, float font_size,
                       const UIColor&) override
    {
        if (!text.empty())
            Texts.push_back(FSlateRecordedText {UIRect(pos.x, pos.y, 0.0f, 0.0f), text, font_size});
    }

    // Deterministic metrics: ~0.6 em per glyph is close enough for layout
    // assertions and keeps the recorder independent of any font file.
    Vector2 MeasureText(const std::string& text, float font_size) const override
    {
        return Vector2(static_cast<float>(text.size()) * font_size * 0.6f, font_size);
    }

    void PushClipRect(const UIRect&) override
    {
        ++PushClipCount;
        ++m_ClipDepth;
        if (m_ClipDepth > MaxClipDepth)
            MaxClipDepth = m_ClipDepth;
    }

    void PopClipRect() override
    {
        ++PopClipCount;
        if (m_ClipDepth == 0)
            ClipUnderflow = true;
        else
            --m_ClipDepth;
    }

    void BeginFrame() override {}
    void EndFrame() override {}
    void Flush() override {}

private:
    int m_ClipDepth {0};
};

}  // namespace ZSlate
