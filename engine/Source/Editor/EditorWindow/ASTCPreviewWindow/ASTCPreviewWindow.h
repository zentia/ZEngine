#pragma once

// =============================================================================
// ASTCPreviewWindow
// -----------------------------------------------------------------------------
// A standalone editor window for previewing ASTC-compressed textures.
// Launched from the Inspector when an ASTC texture is selected.
//
// Design notes:
//   * Uses ASTCDecompressor to decompress ASTC data to RGBA8 for display.
//   * Renders the preview via ZSlate widgets (mirrors Unity's TextureInspector).
//   * Supports zoom/pan for detailed inspection.
// =============================================================================

#include "Editor/EditorWindow/EditorWindow.h"

#include <filesystem>
#include <memory>
#include <vector>

namespace ZSlate
{
class SWidget;
class SVerticalBox;
class STextBlock;
class SButton;
class SScrollBox;
class SImage;  // TODO: Add SImage widget to ZSlate
}

class Texture2D;

class ASTCPreviewWindow : public EditorWindow
{
public:
    explicit ASTCPreviewWindow(EditorUI* editor_ui);
    ~ASTCPreviewWindow() override = default;

    void OnGUI() override;
    bool SupportsNativeHosting() const override { return true; }

    // Set the texture to preview
    void SetTexture(const std::filesystem::path& texture_path);

private:
    void BuildPreviewUI(float scale);
    void BuildToolbar(float scale);
    void BuildPreviewArea(float scale);
    void BuildInfoPanel(float scale);

    // Decompress ASTC texture and cache the result
    bool DecompressTexture();

    std::shared_ptr<ZSlate::SWidget> m_Root;
    std::filesystem::path m_TexturePath;
    
    // Decompressed texture data (RGBA8)
    std::vector<uint8_t> m_PreviewPixels;
    uint32_t m_PreviewWidth {0};
    uint32_t m_PreviewHeight {0};
    bool m_NeedsDecompress {true};

    // UI state
    float m_ZoomLevel {1.0f};
    bool m_ShowMipMaps {false};
    int m_SelectedMip {0};

    // Cached widgets
    std::shared_ptr<ZSlate::STextBlock> m_InfoLabel;
    std::shared_ptr<ZSlate::SButton> m_RefreshButton;
    std::shared_ptr<ZSlate::SButton> m_SaveAsPNGButton;

    float m_BuiltScale {-1.0f};
    bool m_ForceRebuild {false};
};
