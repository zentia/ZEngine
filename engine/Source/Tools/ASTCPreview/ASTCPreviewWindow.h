#pragma once

// =============================================================================
// ASTCPreviewWindow
// -----------------------------------------------------------------------------
// A standalone Slate widget for previewing ASTC-compressed textures.
// Uses ASTCDecompressor to decompress ASTC data to RGBA8 for display.
// Renders the preview via custom OnPaint() drawing.
//
// This is a standalone tool that does not depend on ZEditor.
// =============================================================================

#include "Runtime/Slate/Widgets/SWidget.h"
#include "Runtime/UI/Render/UIRenderer.h"
#include "Runtime/UI/Render/UiGpuResources.h"

#include <filesystem>
#include <memory>
#include <vector>
#include <string>
#include <cstdint>

class ASTCPreviewWindow : public ZSlate::SWidget
{
public:
    ASTCPreviewWindow();
    ~ASTCPreviewWindow() override = default;

    // SWidget interface
    Vector2 ComputeDesiredSize() const override;
    void OnPaint(const ZSlate::FPaintContext& ctx, const ZSlate::FGeometry& geom) const override;

    // Input handling - CORRECT signatures matching SWidget
    void OnMouseMove(const Vector2& screen_pos) override;
    ZSlate::FReply OnMouseButtonDown(const Vector2& screen_pos, int button) override;
    ZSlate::FReply OnMouseButtonUp(const Vector2& screen_pos, int button) override;
    ZSlate::FReply OnMouseWheel(const Vector2& screen_pos, float delta) override;

    // Keyboard focus: must return true for OnKeyChar/OnKeyDown to be delivered
    bool SupportsKeyboardFocus() const override { return true; }

    // Keyboard shortcuts (letter keys come via OnKeyChar)
    void OnKeyChar(unsigned int codepoint) override;

    // Set the texture to preview
    void SetTexture(const std::filesystem::path& texture_path);

    // Get the current texture path
    const std::filesystem::path& GetTexturePath() const { return m_TexturePath; }

private:
    // Decompress ASTC texture and cache the result
    bool DecompressTexture();

    // Create GPU texture from decompressed pixels
    bool CreateGPUTexture();

    // Draw checkerboard background for alpha visualization
    void DrawCheckerboard(UIRenderer* renderer, const UIRect& rect) const;

    // Draw the preview image with zoom/pan
    void DrawPreviewImage(UIRenderer* renderer, const UIRect& rect) const;

    // Draw info text overlay
    void DrawInfoOverlay(UIRenderer* renderer, const UIRect& rect) const;

    // Event handlers
    void OnZoomIn();
    void OnZoomOut();
    void OnFitToWindow();
    void OnSaveAsPNG();
    void OnReloadTexture();

private:
    std::filesystem::path m_TexturePath;

    // Decompressed texture data (RGBA8)
    std::vector<uint8_t> m_PreviewPixels;
    uint32_t m_PreviewWidth {0};
    uint32_t m_PreviewHeight {0};
    bool m_NeedsDecompress {true};
    bool m_TextureLoaded {false};

    // Rendering
    void* m_TextureId {nullptr};  // RHI texture handle for UIRenderer
    void* m_GpuTextureHandle {nullptr};  // GpuTexture handle for UiGpuResources
    bool m_NeedsTextureCreate {true};

    // UI state
    float m_ZoomLevel {1.0f};
    float m_MinZoom {0.1f};
    float m_MaxZoom {10.0f};

    // Pan state
    bool m_IsPanning {false};
    float m_PanX {0.0f};
    float m_PanY {0.0f};
    float m_LastMouseX {0.0f};
    float m_LastMouseY {0.0f};
};
