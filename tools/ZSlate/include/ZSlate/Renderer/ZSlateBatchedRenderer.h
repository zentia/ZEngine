#pragma once

// ============================================================================
// ZSlate/Renderer/ZSlateBatchedRenderer.h
//
// Records an ordered draw list every frame. The platform backend walks the
// list and submits it to the GPU. Keeping the list around also makes text
// measurement possible without a GPU round trip.
// ============================================================================

#include "../Core/SlatePaint.h"
#include "../Core/ZSlateTypes.h"
#include "ZSlateFontAtlas.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ZSlate
{

enum class EDrawCommandType
{
    Quad,    // solid rectangle
    Glyph    // textured quad sampled from the font atlas
};

struct FDrawCommand
{
    EDrawCommandType Type = EDrawCommandType::Quad;
    UIRect  Rect;                 // destination rectangle in pixels
    UIColor Color;
    UIRect  UVRect;               // Glyph: atlas UV rect (0..1)
    uint32_t Codepoint = 0;       // Glyph: for debugging / caching
};

class ZSlateBatchedRenderer : public ISlateRenderer
{
public:
    void BeginFrame();
    void EndFrame();
    void Clear();

    void SetFontAtlas(ZSlateFontAtlas* atlas) { m_FontAtlas = atlas; }
    ZSlateFontAtlas* GetFontAtlas() const { return m_FontAtlas; }

    // ---- ISlateRenderer ----------------------------------------------------
    void DrawQuad(const UIRect& rect, const UIColor& color) override;
    void DrawRect(const UIRect& rect, const UIColor& color, float thickness = 1.0f) override;
    void DrawTextLabel(const UIRect& rect,
                       const std::string& text,
                       float fontSize,
                       const UIColor& color,
                       TextAnchor anchor = TextAnchor::MiddleLeft,
                       TextWrapMode wrap = TextWrapMode::NoWrap,
                       void* fontInfo = nullptr) override;

    void PushClipRect(const UIRect& rect) override;
    void PopClipRect() override;

    // ---- Output ------------------------------------------------------------
    const std::vector<FDrawCommand>& GetCommands() const { return m_Commands; }

    // ---- Measurement -------------------------------------------------------
    Vector2 MeasureText(const std::string& text, float fontSize) const;
    float   GetLineHeight(float fontSize) const;

private:
    void DrawGlyphRun(const UIRect& rect,
                      const std::string& text,
                      float fontSize,
                      const UIColor& color,
                      TextAnchor anchor,
                      TextWrapMode wrap);

    UIRect CurrentClipRect() const;

    std::vector<FDrawCommand> m_Commands;
    std::vector<UIRect>       m_ClipStack;
    ZSlateFontAtlas*          m_FontAtlas = nullptr;
};

} // namespace ZSlate
