// =============================================================================
// ASTCPreviewWindow.cpp - ASTC Texture Preview Widget
// -----------------------------------------------------------------------------
// A standalone Slate widget that previews ASTC-compressed textures.
// Uses custom painting in OnPaint() with UIRenderer.
// =============================================================================

#include "ASTCPreviewWindow.h"

#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Slate/Core/SlateReply.h"
#include "Runtime/Function/Render/Texture/ASTCDecompressor.h"
#include "Runtime/UI/Render/UiGpuResources.h"
#include "Runtime/Function/Render/RenderType.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>

#ifdef _WIN32
    #include <windows.h>
    #include <commdlg.h>
#endif

namespace
{
    inline UIRect ToRect(float x, float y, float w, float h)
    {
        return UIRect(x, y, w, h);
    }

    // Helper: build a checkerboard color
    bool IsCheckerLight(float x, float y, float cell)
    {
        return (static_cast<int>(x / cell) + static_cast<int>(y / cell)) % 2 == 0;
    }
}  // namespace

ASTCPreviewWindow::ASTCPreviewWindow()
    : m_ZoomLevel(1.0f)
    , m_MinZoom(0.1f)
    , m_MaxZoom(10.0f)
    , m_IsPanning(false)
    , m_PanX(0.0f)
    , m_PanY(0.0f)
    , m_LastMouseX(0.0f)
    , m_LastMouseY(0.0f)
    , m_NeedsDecompress(true)
    , m_TextureLoaded(false)
    , m_NeedsTextureCreate(true)
    , m_TextureId(nullptr)
{
    Visibility = ZSlate::EVisibility::Visible;
}

void ASTCPreviewWindow::SetTexture(const std::filesystem::path& texture_path)
{
    m_TexturePath = texture_path;
    m_NeedsDecompress = true;
    m_TextureLoaded = false;
    m_NeedsTextureCreate = true;
    m_PanX = m_PanY = 0.0f;
    m_ZoomLevel = 1.0f;

    if (DecompressTexture())
    {
        m_TextureLoaded = true;
        m_NeedsTextureCreate = true;
    }
    else
    {
        LOG_ERROR(ZEditor, "ASTCPreviewWindow: failed to decompress texture");
    }
}

Vector2 ASTCPreviewWindow::ComputeDesiredSize() const
{
    return Vector2(1024.0f, 768.0f);
}

void ASTCPreviewWindow::OnPaint(const ZSlate::FPaintContext& ctx, const ZSlate::FGeometry& geom) const
{
    if (!ctx.Renderer)
        return;

    const UIRect rect = geom.ToRect();

    // 1. Background
    ctx.Renderer->drawQuad(rect, UIColor(0.2f, 0.2f, 0.2f, 1.0f));

    // 2. Checkerboard (visible under transparent areas)
    DrawCheckerboard(ctx.Renderer, rect);

    // 3. Preview image
    DrawPreviewImage(ctx.Renderer, rect);

    // 4. Info overlay
    DrawInfoOverlay(ctx.Renderer, rect);
}

void ASTCPreviewWindow::OnMouseMove(const Vector2& screen_pos)
{
    if (m_IsPanning)
    {
        m_PanX += (screen_pos.x - m_LastMouseX);
        m_PanY += (screen_pos.y - m_LastMouseY);
        m_LastMouseX = screen_pos.x;
        m_LastMouseY = screen_pos.y;
    }
}  // 正确：返回 void，匹配 SWidget.h 中的虚函数签名

ZSlate::FReply ASTCPreviewWindow::OnMouseButtonDown(const Vector2& screen_pos, int button)
{
    if (button == 2)  // Middle button = pan
    {
        m_IsPanning = true;
        m_LastMouseX = screen_pos.x;
        m_LastMouseY = screen_pos.y;
        return ZSlate::FReply::Handled();
    }
    return ZSlate::FReply::Unhandled();
}

ZSlate::FReply ASTCPreviewWindow::OnMouseButtonUp(const Vector2& screen_pos, int button)
{
    if (button == 2 && m_IsPanning)
    {
        m_IsPanning = false;
        return ZSlate::FReply::Handled();
    }
    return ZSlate::FReply::Unhandled();
}

ZSlate::FReply ASTCPreviewWindow::OnMouseWheel(const Vector2& screen_pos, float delta)
{
    const float zoom_delta = delta > 0.0f ? 1.1f : 0.9f;
    m_ZoomLevel = std::clamp(m_ZoomLevel * zoom_delta, m_MinZoom, m_MaxZoom);
    return ZSlate::FReply::Handled();
}

// ---- Private helpers ----

bool ASTCPreviewWindow::DecompressTexture()
{
    if (m_TexturePath.empty() || !std::filesystem::exists(m_TexturePath))
    {
        LOG_ERROR(ZEditor, "ASTCPreviewWindow: texture path empty or file missing");
        return false;
    }

    // Read ASTC file
    // Two common 16-byte header layouts exist in the wild:
    //
    // 1) Standard ASTC (ASTC specification):
    //    Bytes 0-3:   magic 0x5CA1AB13 (LE: 0x13, 0xAB, 0xA1, 0x5C)
    //    Byte  4:      blockdim_x
    //    Byte  5:      blockdim_y
    //    Byte  6:      blockdim_z (always 1 for 2D)
    //    Bytes 7-9:   xsize  (3-byte LE, 24-bit)
    //    Bytes 10-12: ysize  (3-byte LE, 24-bit)
    //    Bytes 13-15: zsize  (3-byte LE, 24-bit)
    //
    // 2) Unity exported ASTC (Texture2D_EncodeTo.h::WriteASTCHeader):
    //    Bytes 0-3:   magic 0x04000000 (LE: 0x04, 0x00, 0x00, 0x00)
    //    Byte  4:      blockX
    //    Byte  5:      blockY
    //    Bytes 6-7:   width  (uint16 LE)
    //    Bytes 8-9:   height (uint16 LE)
    //    Bytes 10-15: reserved (0)
    //
    // Both are followed by raw ASTC compressed data at byte 16.

    std::ifstream file(m_TexturePath, std::ios::binary | std::ios::ate);
    if (!file.is_open())
    {
        LOG_ERROR(ZEditor, "ASTCPreviewWindow: failed to open file: {}", m_TexturePath.generic_string());
        return false;
    }

    const size_t file_size = static_cast<size_t>(file.tellg());
    file.seekg(0);

    if (file_size < 16)
    {
        LOG_ERROR(ZEditor, "ASTCPreviewWindow: file too small ({} bytes)", file_size);
        return false;
    }

    // Read header
    uint8_t header[16];
    file.read(reinterpret_cast<char*>(header), 16);

    // Detect header variant from magic number
    const uint32_t magic = header[0] | (header[1] << 8) | (header[2] << 16) | (header[3] << 24);

    uint8_t block_x, block_y;
    uint32_t width, height;

    if (magic == 0x5CA1AB13)
    {
        // Standard ASTC header
        block_x = header[4];
        block_y = header[5];
        // blockdim_z = header[6];  (1 for 2D textures)
        width  = header[7] | (header[8] << 8) | (header[9] << 16);
        height = header[10] | (header[11] << 8) | (header[12] << 16);
    }
    else if (magic == 0x00000004)
    {
        // Unity exported ASTC header
        block_x = header[4];
        block_y = header[5];
        width  = header[6] | (header[7] << 8);
        height = header[8] | (header[9] << 8);
    }
    else
    {
        LOG_ERROR(ZEditor, "ASTCPreviewWindow: unrecognized ASTC header magic 0x{:08X}", magic);
        return false;
    }

    // Map block size to enum
    ZEngine::Render::ASTCBlockSize block_size;
    if (block_x == 4 && block_y == 4)       block_size = ZEngine::Render::ASTCBlockSize::ASTC_4x4;
    else if (block_x == 5 && block_y == 5)  block_size = ZEngine::Render::ASTCBlockSize::ASTC_5x5;
    else if (block_x == 6 && block_y == 6)  block_size = ZEngine::Render::ASTCBlockSize::ASTC_6x6;
    else if (block_x == 8 && block_y == 8)  block_size = ZEngine::Render::ASTCBlockSize::ASTC_8x8;
    else if (block_x == 10 && block_y == 10) block_size = ZEngine::Render::ASTCBlockSize::ASTC_10x10;
    else if (block_x == 12 && block_y == 12) block_size = ZEngine::Render::ASTCBlockSize::ASTC_12x12;
    else
    {
        LOG_ERROR(ZEditor, "ASTCPreviewWindow: unsupported block size {}x{}", block_x, block_y);
        return false;
    }

    // Read compressed data
    const size_t compressed_size = file_size - 16;
    std::vector<uint8_t> compressed_data(compressed_size);
    file.read(reinterpret_cast<char*>(compressed_data.data()), compressed_size);
    file.close();

    // Initialize decompressor (should already be initialized by RegisterRuntime)
    if (!ZEngine::Render::ASTCDecompressor::Initialize())
    {
        LOG_ERROR(ZEditor, "ASTCPreviewWindow: failed to initialize ASTCDecompressor");
        return false;
    }

    // Decompress
    ZEngine::Render::ASTCDecompressResult result =
        ZEngine::Render::ASTCDecompressor::Decompress(
            compressed_data.data(),
            compressed_size,
            width,
            height,
            block_size);

    if (!result.success)
    {
        LOG_ERROR(ZEditor, "ASTCPreviewWindow: decompression failed: {}", result.error_message);
        return false;
    }

    // Store decompressed pixels
    m_PreviewWidth = result.width;
    m_PreviewHeight = result.height;
    m_PreviewPixels = std::move(result.pixels);

    m_NeedsDecompress = false;
    m_TextureLoaded = true;

    return true;
}

bool ASTCPreviewWindow::CreateGPUTexture()
{
    if (!m_TextureLoaded || m_PreviewPixels.empty())
        return false;

    // Create GPU texture via UiGpuResources
    auto* gpu_res = UiGpuResources::Get();
    if (gpu_res == nullptr)
    {
        LOG_ERROR(ZEditor, "ASTCPreviewWindow: UiGpuResources not available");
        return false;
    }

    // UpdateDynamicTexture creates a new dynamic texture (handle==nullptr)
    // and returns the stable handle_id for UIRenderer::drawTexturedQuad()
    void* handle = gpu_res->UpdateDynamicTexture(
        nullptr,  // Create new
        m_PreviewPixels.data(),
        m_PreviewWidth,
        m_PreviewHeight);

    if (handle == nullptr)
    {
        LOG_ERROR(ZEditor, "ASTCPreviewWindow: failed to create GPU texture");
        return false;
    }

    // handle IS the handle_id, use it directly for drawTexturedQuad
    m_TextureId = handle;
    m_GpuTextureHandle = handle;

    m_NeedsTextureCreate = false;
    LOG_INFO(ZEditor, "ASTCPreviewWindow: GPU texture created ({}x{})",
             m_PreviewWidth, m_PreviewHeight);
    return true;
}

void ASTCPreviewWindow::DrawCheckerboard(UIRenderer* renderer, const UIRect& rect) const
{
    constexpr float CELL = 16.0f;
    for (float y = rect.y; y < rect.y + rect.height; y += CELL)
    {
        for (float x = rect.x; x < rect.x + rect.width; x += CELL)
        {
            const bool light = IsCheckerLight(x, y, CELL);
            const UIColor c = light
                ? UIColor(0.25f, 0.25f, 0.25f, 1.0f)
                : UIColor(0.15f, 0.15f, 0.15f, 1.0f);
            renderer->drawQuad(ToRect(x, y, CELL, CELL), c);
        }
    }
}

void ASTCPreviewWindow::DrawPreviewImage(UIRenderer* renderer, const UIRect& rect) const
{
    if (!m_TextureLoaded)
        return;

    // Lazily create GPU texture
    if (m_NeedsTextureCreate)
    {
        // const_cast is safe here: drawing is logically const, but we need to create the GPU resource once.
        const_cast<ASTCPreviewWindow*>(this)->CreateGPUTexture();
    }

    if (m_TextureId == nullptr)
        return;

    const float img_w = static_cast<float>(m_PreviewWidth) * m_ZoomLevel;
    const float img_h = static_cast<float>(m_PreviewHeight) * m_ZoomLevel;
    const float img_x = rect.x + (rect.width - img_w) / 2.0f + m_PanX;
    const float img_y = rect.y + (rect.height - img_h) / 2.0f + m_PanY;

    renderer->drawTexturedQuad(ToRect(img_x, img_y, img_w, img_h), m_TextureId);
}

void ASTCPreviewWindow::DrawInfoOverlay(UIRenderer* renderer, const UIRect& rect) const
{
    if (!m_TextureLoaded)
        return;

    char buf[128];
    std::snprintf(buf, sizeof(buf), "Size: %ux%u  |  Zoom: %.0f%%  |  Pan: %.0f, %.0f",
                  m_PreviewWidth, m_PreviewHeight,
                  m_ZoomLevel * 100.0f, m_PanX, m_PanY);

    // Info bar background
    renderer->drawQuad(ToRect(rect.x, rect.y, rect.width, 28.0f),
                       UIColor(0.1f, 0.1f, 0.1f, 0.85f));

    renderer->drawText(ToRect(rect.x + 8.0f, rect.y, rect.width - 16.0f, 28.0f),
                       buf, 13.0f,
                       UIColor(0.9f, 0.9f, 0.9f, 1.0f),
                       TextAnchor::MiddleLeft,
                       TextWrapMode::NoWrap);
}

void ASTCPreviewWindow::OnZoomIn()     { m_ZoomLevel = std::min(m_ZoomLevel * 1.2f, m_MaxZoom); }
void ASTCPreviewWindow::OnZoomOut()    { m_ZoomLevel = std::max(m_ZoomLevel / 1.2f, m_MinZoom); }
void ASTCPreviewWindow::OnFitToWindow() { m_ZoomLevel = 1.0f; m_PanX = m_PanY = 0.0f; }
void ASTCPreviewWindow::OnSaveAsPNG()   { /* TODO */ }
void ASTCPreviewWindow::OnReloadTexture()
{
    m_NeedsDecompress = true;
    if (DecompressTexture())
    {
        m_TextureLoaded = true;
        m_NeedsTextureCreate = true;
    }
}

void ASTCPreviewWindow::OnKeyChar(unsigned int codepoint)
{
    // Handle letter/character shortcuts
    // O/o = open file dialog
    if (codepoint == 'O' || codepoint == 'o')
    {
#ifdef _WIN32
        OPENFILENAMEA ofn {};
        char file_path[MAX_PATH] {};
        ofn.lStructSize = sizeof(OPENFILENAMEA);
        ofn.hwndOwner = nullptr;
        ofn.lpstrFilter = "ASTC Files (*.astc)\0*.astc\0All Files (*.*)\0*.*\0";
        ofn.lpstrFile = file_path;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;

        if (GetOpenFileNameA(&ofn))
        {
            SetTexture(std::filesystem::path(file_path));
        }
#endif
        return;
    }

    // R/r = reload texture
    if (codepoint == 'R' || codepoint == 'r')
    {
        OnReloadTexture();
        return;
    }

    // F/f = fit to window
    if (codepoint == 'F' || codepoint == 'f')
    {
        OnFitToWindow();
        return;
    }

    // + = zoom in
    if (codepoint == '+')
    {
        OnZoomIn();
        return;
    }

    // - = zoom out
    if (codepoint == '-')
    {
        OnZoomOut();
        return;
    }

    // S/s = save as PNG (stub)
    if (codepoint == 'S' || codepoint == 's')
    {
        OnSaveAsPNG();
        return;
    }
}
