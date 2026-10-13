#pragma once

// ============================================================================
// UiRecorder - headless ISlateRenderer that records draw calls
// ============================================================================
//
// Lets the previewer's UI be verified without a GPU, a window or a human
// looking at pixels. Two things matter here that a screenshot cannot show:
//
//   * whether the virtualisation actually works - a correct frame must emit a
//     number of text draws proportional to the *visible* rows, not to the
//     181K rows in the snapshot;
//   * whether the clip stack is balanced - every PushClipRect must have a
//     matching PopClipRect, otherwise later widgets get silently clipped away.
//
// It is also why PrintWindow screenshots are not enough: a DXGI flip-model
// swapchain hands GDI a blank surface, so the capture is white regardless of
// what was rendered.

#include "ZSlate/Core/SlatePaint.h"

#include <cstdint>
#include <string>
#include <vector>

namespace LuaMemPreview
{
struct FRecordedText
{
    ZSlate::UIRect Rect;
    std::string Text;
    float FontSize;
};

class FUiRecorder : public ZSlate::ISlateRenderer
{
public:
    // --- Recorded output ---------------------------------------------------
    std::vector<FRecordedText> Texts;
    std::vector<ZSlate::UIRect> Quads;
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
        for (const FRecordedText& t : Texts)
        {
            if (t.Text.find(needle) != std::string::npos)
                return true;
        }
        return false;
    }

    // --- ISlateRenderer ----------------------------------------------------
    void DrawQuad(const ZSlate::UIRect& rect, const ZSlate::UIColor&) override { Quads.push_back(rect); }
    void DrawRect(const ZSlate::UIRect&, const ZSlate::UIColor&, float) override { ++RectCount; }
    void DrawConvexPoly(const ZSlate::Vector2*, int, const ZSlate::UIColor&) override {}
    void DrawRoundedRect(const ZSlate::UIRect& rect, float, const ZSlate::UIColor&) override { Quads.push_back(rect); }
    void DrawTexturedQuad(const ZSlate::UIRect&, void*, const ZSlate::UIColor&) override {}
    void DrawBox(const ZSlate::UIRect&, const ZSlate::FMargin&, void*, const ZSlate::UIColor&) override {}
    void DrawBorder(const ZSlate::UIRect&, const ZSlate::FMargin&, void*, const ZSlate::UIColor&) override {}

    void DrawTextLabel(const ZSlate::UIRect& rect, const std::string& text, float font_size,
                       const ZSlate::UIColor&, ZSlate::TextAnchor, ZSlate::TextWrapMode, void*) override
    {
        if (!text.empty())
            Texts.push_back(FRecordedText {rect, text, font_size});
    }

    void DrawTextLabel(const std::string& text, const ZSlate::Vector2& pos, float font_size,
                       const ZSlate::UIColor&) override
    {
        if (!text.empty())
            Texts.push_back(FRecordedText {ZSlate::UIRect(pos.x, pos.y, 0.0f, 0.0f), text, font_size});
    }

    // Deterministic metrics: ~0.6 em per glyph is close enough for layout
    // assertions and keeps the recorder independent of any font file.
    ZSlate::Vector2 MeasureText(const std::string& text, float font_size) const override
    {
        return ZSlate::Vector2(static_cast<float>(text.size()) * font_size * 0.6f, font_size);
    }

    void PushClipRect(const ZSlate::UIRect&) override
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

}  // namespace LuaMemPreview
