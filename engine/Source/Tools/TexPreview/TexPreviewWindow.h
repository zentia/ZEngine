#pragma once

// =============================================================================
// TexPreviewWindow — standalone block-compressed texture preview widget
// -----------------------------------------------------------------------------
// Dependencies: ZSlate (SWidget), D3D11 (GPU textures, Windows-only).
// No ZRuntime.  Decompressors (ASTC/BC7/ETC2) are compiled inline.
// =============================================================================

#include "ZSlate/Widgets/SWidget.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <d3d11.h>
#undef DrawText  // windows.h redefines DrawText → DrawTextA/W
#endif

#include <filesystem>
#include <memory>
#include <vector>
#include <string>
#include <cstdint>

enum class TexPreviewFormat { Unknown, ASTC, BC7, ETC2 };

class TexPreviewWindow : public ZSlate::SWidget
{
public:
    TexPreviewWindow();
    ~TexPreviewWindow() override;

    // SWidget interface
    ZSlate::Vector2 ComputeDesiredSize() const override;
    void OnPaint(const ZSlate::FPaintContext& ctx, const ZSlate::FGeometry& geom) const override;

    void OnMouseMove(const ZSlate::Vector2& screen_pos) override;
    ZSlate::FReply OnMouseButtonDown(const ZSlate::Vector2& screen_pos, int button) override;
    ZSlate::FReply OnMouseButtonUp(const ZSlate::Vector2& screen_pos, int button) override;
    ZSlate::FReply OnMouseWheel(const ZSlate::Vector2& screen_pos, float delta) override;
    bool SupportsKeyboardFocus() const override { return true; }
    void OnKeyChar(unsigned int codepoint) override;

    void SetTexture(const std::filesystem::path& texture_path);

#ifdef _WIN32
    // Must be called before first paint to set the D3D11 device for GPU textures.
    void SetD3D11Device(ID3D11Device* device, ID3D11DeviceContext* ctx)
    { m_D3DDevice = device; m_D3DContext = ctx; }
#endif

private:
    TexPreviewFormat DetectFormat(const std::filesystem::path& path) const;
    bool DecompressTexture();
    bool DecompressASTC(const std::vector<uint8_t>& file_data);
    bool DecompressBC7(const std::vector<uint8_t>& file_data);
    bool DecompressETC2(const std::vector<uint8_t>& file_data);
    bool CreateGPUTexture();
    void ReleaseGPUTexture();

    void DrawCheckerboard(ZSlate::ISlateRenderer* renderer, const ZSlate::UIRect& rect) const;
    void DrawPreviewImage(ZSlate::ISlateRenderer* renderer, const ZSlate::UIRect& rect) const;
    void DrawInfoOverlay(ZSlate::ISlateRenderer* renderer, const ZSlate::UIRect& rect) const;

    void OnZoomIn(); void OnZoomOut(); void OnFitToWindow();
    void OnSaveAsPNG(); void OnReloadTexture();

    std::filesystem::path m_TexturePath;
    TexPreviewFormat m_DetectedFormat {TexPreviewFormat::Unknown};

    std::vector<uint8_t> m_PreviewPixels;
    uint32_t m_PreviewWidth  {0};
    uint32_t m_PreviewHeight {0};
    bool m_NeedsDecompress {true};
    bool m_TextureLoaded {false};

#ifdef _WIN32
    ID3D11Device*        m_D3DDevice  {nullptr};
    ID3D11DeviceContext* m_D3DContext {nullptr};
    ID3D11ShaderResourceView* m_TextureSRV {nullptr};
    ID3D11Texture2D*           m_TextureTex {nullptr};
#endif
    void* m_TextureId {nullptr};  // opaque handle for ISlateRenderer::DrawTexturedQuad
    bool m_NeedsTextureCreate {true};

    float m_ZoomLevel {1.0f};
    float m_MinZoom {0.1f};
    float m_MaxZoom {10.0f};

    bool  m_IsPanning {false};
    float m_PanX {0.0f}, m_PanY {0.0f};
    float m_LastMouseX {0.0f}, m_LastMouseY {0.0f};
};
