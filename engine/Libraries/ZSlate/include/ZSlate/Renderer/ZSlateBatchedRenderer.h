#pragma once

// ============================================================================
// ZSlateBatchedRenderer — Battery-included ISlateRenderer for third-party apps
// ============================================================================
//
// Records all ISlateRenderer draw calls into a CPU-side vertex/index buffer
// and a flat command list.  After EndFrame(), the caller reads:
//
//   GetVertices()  →  const std::vector<UIVertex>&
//   GetIndices()   →  const std::vector<uint16_t>&
//   GetCommands()  →  const std::vector<DrawCmd>&
//
// Each DrawCmd holds the texture_id, index range, and an optional clip rect.
// Upload the vertex/index data to the GPU, walk the command list, bind the
// texture for each command, set the scissor if requested, and draw indexed.
//
// ---------------------------------------------------------------------------
//
// This is the same accumulation model as ZEngine's BatchedUIRenderer, moved
// into ZSlate so that anyone linking against ZSlate gets a complete draw-call
// recorder without pulling in a full engine renderer.
// ============================================================================

#include "ZSlate/Core/SlatePaint.h"
#include "ZSlate/Core/SlateGeometry.h"
#include "ZSlate/Renderer/ZSlateFontAtlas.h"
#include "ZSlate/Renderer/ZSlateTextGenerator.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace ZSlate
{

// ---- Vertex layout (matches ZEngine UIVertex) -------------------------------

struct ZSBVertex
{
    float Pos[2] {};
    float UV[2] {0.0f, 0.0f};
    float Color[4] {1.0f, 1.0f, 1.0f, 1.0f};
};

// ---- Draw command -----------------------------------------------------------

struct ZSBDrawCmd
{
    void* TextureId {nullptr};
    uint32_t IndexOffset {0};
    uint32_t IndexCount {0};
    UIRect ClipRect {0.0f, 0.0f, 0.0f, 0.0f};
    bool HasClip {false};
};

// ---- Text measurer ----------------------------------------------------------

// Callback that returns the advance-width of a UTF-8 string for a given font
// and size.  Install via ZSlateBatchedRenderer::SetTextMeasurerCallback().
// Default: font_size * 0.6f * text.length() (rough ASCII estimate).
using ZSBTextMeasureFunc = std::function<Vector2(const std::string& text, float fontSize)>;

// ---- Batched Renderer -------------------------------------------------------

class ZSlateBatchedRenderer : public ISlateRenderer
{
public:
    ZSlateBatchedRenderer();
    ~ZSlateBatchedRenderer() override = default;

    // --- Access the batch after EndFrame() -----------------------------------
    const std::vector<ZSBVertex>&   GetVertices()  const { return m_Vertices; }
    const std::vector<uint16_t>&    GetIndices()   const { return m_Indices; }
    const std::vector<ZSBDrawCmd>&  GetCommands()  const { return m_Commands; }
    bool                            IsEmpty()      const { return m_Indices.empty(); }

    // --- Optional text measurer ----------------------------------------------
    void SetTextMeasurerCallback(ZSBTextMeasureFunc fn) { m_TextMeasurer = std::move(fn); }

    // --- Optional font atlas -------------------------------------------------
    // Attach a ZSlateFontAtlas for real glyph rendering. nullptr = fallback to
    // blocky per-character bars (visible but not pretty).
    void SetFontAtlas(ZSlateFontAtlas* atlas);
    ZSlateFontAtlas* GetFontAtlas() const { return m_FontAtlas; }

    // Magic texture ID for font atlas glyphs (0x1).  The GPU backend should
    // bind the atlas texture when it sees this ID in a draw command.
    // NOTE: reinterpret_cast is not allowed in a constant expression, so this
    // cannot be constexpr (clang rejects it even though MSVC accepts it).
    static inline void* const kFontAtlasTextureId = reinterpret_cast<void*>(0x1);

    // --- ISlateRenderer interface (PascalCase) -------------------------------
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
    void Flush() override {}

private:
    void ForceNewCommand(void* textureId);
    void AppendTexturedQuad(const UIRect& rect, const UIColor& color,
                            void* textureId, float u0 = 0, float v0 = 0,
                            float u1 = 1, float v1 = 1);
    void BeginCommand(void* textureId);

    std::vector<ZSBVertex>  m_Vertices;
    std::vector<uint16_t>   m_Indices;
    std::vector<ZSBDrawCmd> m_Commands;

    std::vector<UIRect>    m_ClipStack;
    UIRect                 m_ActiveClip {0, 0, 0, 0};
    bool                   m_HasClip {false};

    void* m_CurrentTexture {nullptr};
    bool  m_Active {false};

    ZSBTextMeasureFunc m_TextMeasurer;

    // Font atlas (optional — nullptr → blocky placeholder text)
    ZSlateFontAtlas* m_FontAtlas {nullptr};
    std::unique_ptr<ZSlateTextGenerator> m_TextGen;
};

}  // namespace ZSlate
