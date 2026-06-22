#pragma once

// =============================================================================
// TexPreviewWindow
// -----------------------------------------------------------------------------
// A standalone Slate widget for previewing block-compressed textures.
// Supports ASTC, BC7, and ETC2 formats.
// Uses ASTCDecompressor / BC7Decompressor / ETC2Decompressor to decompress data to RGBA8 for
// display. Renders the preview via custom OnPaint() drawing.
//
// This is a standalone tool that does not depend on ZEditor.
// =============================================================================

#include "ZSlate/Widgets/SWidget.h"
#include "Runtime/UI/Render/UIRenderer.h"
#include "Runtime/UI/Render/UiGpuResources.h"

#include <filesystem>
#include <memory>
#include <vector>
#include <string>
#include <cstdint>

// Detected texture format
enum class TexPreviewFormat
{
    Unknown,
    ASTC,
    BC7,
    ETC2,
};

class TexPreviewWindow : public ZSlate::SWidget
{
public:
    TexPreviewWindow();
    ~TexPreviewWindow() override = default;

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

    // Get detected format
    TexPreviewFormat GetDetectedFormat() const { return m_DetectedFormat; }

private:
    // Detect texture format from file content (magic / extension)
    TexPreviewFormat DetectFormat(const std::filesystem::path& path) const;

    // Decompress texture and cache the result (dispatches by format)
    bool DecompressTexture();

    // Format-specific decompression
    bool DecompressASTC(const std::vector<uint8_t>& file_data);
    bool DecompressBC7(const std::vector<uint8_t>& file_data);
    bool DecompressETC2(const std::vector<uint8_t>& file_data);

    // Create GPU texture from decompressed pixels
    bool CreateGPUTexture();

    // Create the checkerboard pattern GPU texture (small tiled texture)
    void EnsureCheckerboardTexture();

    // Draw checkerboard background for alpha visualization
    void DrawCheckerboard(UIRenderer* renderer, const UIRect& rect) const;

    // Draw the preview image with zoom/pan (clipped to widget bounds)
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
    TexPreviewFormat m_DetectedFormat {TexPreviewFormat::Unknown};

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

    // Checkerboard pattern texture (small 2x2-cell texture, tiled via UV)
    void* m_CheckerTextureId {nullptr};

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
