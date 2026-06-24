// =============================================================================
// TexPreviewWindow.cpp - Block-Compressed Texture Preview Widget
// -----------------------------------------------------------------------------
// A standalone Slate widget that previews ASTC and BC7 compressed textures.
// Uses custom painting in OnPaint() with UIRenderer.
// =============================================================================

#include "TexPreviewWindow.h"

#include "Runtime/Core/Base/Macro.h"
#include "ZSlate/Core/SlateReply.h"
#include "Runtime/Function/Render/Texture/ASTCDecompressor.h"
#include "Runtime/Function/Render/Texture/BC7Decompressor.h"
#include "Runtime/Function/Render/Texture/ETC2Decompressor.h"
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

    // Checkerboard cell size in pixels (used for both the pattern texture and UV tiling)
    constexpr float CHECKER_CELL = 16.0f;
    // The checkerboard texture is 2 cells wide and 2 cells tall
    constexpr uint32_t CHECKER_TEX_CELLS = 2;
    constexpr uint32_t CHECKER_TEX_SIZE = static_cast<uint32_t>(CHECKER_CELL) * CHECKER_TEX_CELLS; // 32x32

    // Build a 32x32 RGBA8 checkerboard bitmap (2x2 cells, each 16x16 pixels)
    std::vector<uint8_t> BuildCheckerboardBitmap()
    {
        std::vector<uint8_t> pixels(CHECKER_TEX_SIZE * CHECKER_TEX_SIZE * 4);
        for (uint32_t y = 0; y < CHECKER_TEX_SIZE; ++y)
        {
            for (uint32_t x = 0; x < CHECKER_TEX_SIZE; ++x)
            {
                const bool light = ((x / static_cast<uint32_t>(CHECKER_CELL)) +
                                    (y / static_cast<uint32_t>(CHECKER_CELL))) % 2 == 0;
                const uint8_t v = light ? 64 : 38;  // 0.25 or ~0.15 in 0-255
                uint8_t* p = &pixels[(y * CHECKER_TEX_SIZE + x) * 4];
                p[0] = v;
                p[1] = v;
                p[2] = v;
                p[3] = 255;
            }
        }
        return pixels;
    }

    // Convert TexPreviewFormat to display string
    const char* FormatToString(TexPreviewFormat fmt)
    {
        switch (fmt)
        {
        case TexPreviewFormat::ASTC: return "ASTC";
        case TexPreviewFormat::BC7:  return "BC7";
        case TexPreviewFormat::ETC2: return "ETC2";
        default:                     return "Unknown";
        }
    }
}  // namespace

TexPreviewWindow::TexPreviewWindow()
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
    , m_CheckerTextureId(nullptr)
{
    Visibility = ZSlate::EVisibility::Visible;
}

void TexPreviewWindow::SetTexture(const std::filesystem::path& texture_path)
{
    m_TexturePath = texture_path;
    m_DetectedFormat = DetectFormat(texture_path);
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
        LOG_ERROR(ZEditor, "TexPreviewWindow: failed to decompress texture");
    }
}

Vector2 TexPreviewWindow::ComputeDesiredSize() const
{
    return Vector2(1024.0f, 768.0f);
}

void TexPreviewWindow::OnPaint(const ZSlate::FPaintContext& ctx, const ZSlate::FGeometry& geom) const
{
    if (!ctx.Renderer)
        return;

    const UIRect rect = geom.ToRect();

    // 1. Background
    ctx.Renderer->DrawQuad(rect, UIColor(0.2f, 0.2f, 0.2f, 1.0f));

    // 2. Checkerboard (visible under transparent areas)
    DrawCheckerboard(ctx.Renderer, rect);

    // 3. Preview image
    DrawPreviewImage(ctx.Renderer, rect);

    // 4. Info overlay
    DrawInfoOverlay(ctx.Renderer, rect);
}

void TexPreviewWindow::OnMouseMove(const Vector2& screen_pos)
{
    if (m_IsPanning)
    {
        m_PanX += (screen_pos.x - m_LastMouseX);
        m_PanY += (screen_pos.y - m_LastMouseY);
        m_LastMouseX = screen_pos.x;
        m_LastMouseY = screen_pos.y;
    }
}

ZSlate::FReply TexPreviewWindow::OnMouseButtonDown(const Vector2& screen_pos, int button)
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

ZSlate::FReply TexPreviewWindow::OnMouseButtonUp(const Vector2& screen_pos, int button)
{
    if (button == 2 && m_IsPanning)
    {
        m_IsPanning = false;
        return ZSlate::FReply::Handled();
    }
    return ZSlate::FReply::Unhandled();
}

ZSlate::FReply TexPreviewWindow::OnMouseWheel(const Vector2& screen_pos, float delta)
{
    const float old_zoom = m_ZoomLevel;
    const float zoom_delta = delta > 0.0f ? 1.1f : 0.9f;
    m_ZoomLevel = std::clamp(m_ZoomLevel * zoom_delta, m_MinZoom, m_MaxZoom);

    if (m_ZoomLevel != old_zoom)
    {
        const float scale = m_ZoomLevel / old_zoom;
        m_PanX *= scale;
        m_PanY *= scale;
    }

    return ZSlate::FReply::Handled();
}

// ---- Format detection ----

TexPreviewFormat TexPreviewWindow::DetectFormat(const std::filesystem::path& path) const
{
    if (!std::filesystem::exists(path))
        return TexPreviewFormat::Unknown;

    // Read first 4 bytes to check magic numbers
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open())
        return TexPreviewFormat::Unknown;

    uint8_t magic[4] = {};
    file.read(reinterpret_cast<char*>(magic), 4);

    // Standard ASTC magic: 0x5CA1AB13 (LE: 0x13, 0xAB, 0xA1, 0x5C)
    const uint32_t magic32 = magic[0] | (magic[1] << 8) | (magic[2] << 16) | (magic[3] << 24);
    if (magic32 == 0x5CA1AB13)
        return TexPreviewFormat::ASTC;

    // Unity exported ASTC magic: 0x00000004
    if (magic32 == 0x00000004)
        return TexPreviewFormat::ASTC;

    // Check for DDS container with BC7 payload (Unity convention):
    //   magic "DDS " (0x20534444), fourCC "DX10", dxgiFormat 98/99
    if (magic32 == 0x20534444)
    {
        // Read enough for DDS header + DX10 extension (132 bytes from start)
        uint8_t dds_buf[132] = {};
        file.seekg(0);
        file.read(reinterpret_cast<char*>(dds_buf), 132);
        if (file.gcount() >= 132)
        {
            const uint32_t pf = dds_buf[80] | (dds_buf[81] << 8) | (dds_buf[82] << 16) | (dds_buf[83] << 24);
            if (pf & 0x4) // DDPF_FOURCC
            {
                const uint32_t fcc = dds_buf[84] | (dds_buf[85] << 8) | (dds_buf[86] << 16) | (dds_buf[87] << 24);
                if (fcc == 0x30315844) // "DX10"
                {
                    const uint32_t fmt = dds_buf[128] | (dds_buf[129] << 8) | (dds_buf[130] << 16) | (dds_buf[131] << 24);
                    if (fmt == 98 || fmt == 99) // BC7_UNORM / BC7_SRGB
                        return TexPreviewFormat::BC7;
                }
            }
        }
    }

    // Check extension for BC7 files
    auto ext = path.extension();
    if (ext == ".bc7" || ext == ".BC7")
        return TexPreviewFormat::BC7;

    // Check for ETC2 (PKM file format)
    // PKM magic: "PKM " (0x50, 0x4B, 0x4D, 0x20)
    // PKM version field (bytes 4-5): "10" = ETC2, "01" = ETC1
    if (magic[0] == 0x50 && magic[1] == 0x4B && magic[2] == 0x4D && magic[3] == 0x20)
    {
        // Read full 16-byte PKM header
        uint8_t pkm_buf[16] = {};
        file.seekg(0);
        file.read(reinterpret_cast<char*>(pkm_buf), 16);
        if (file.gcount() >= 16)
        {
            // Version "10" = ETC2
            if (pkm_buf[4] == 0x31 && pkm_buf[5] == 0x30)
                return TexPreviewFormat::ETC2;
        }
    }

    // .astc extension fallback
    if (ext == ".astc" || ext == ".ASTC")
        return TexPreviewFormat::ASTC;

    // .pkm extension fallback (ETC2)
    if (ext == ".pkm" || ext == ".PKM")
        return TexPreviewFormat::ETC2;

    return TexPreviewFormat::Unknown;
}

// ---- Decompression ----

bool TexPreviewWindow::DecompressTexture()
{
    if (m_TexturePath.empty() || !std::filesystem::exists(m_TexturePath))
    {
        LOG_ERROR(ZEditor, "TexPreviewWindow: texture path empty or file missing");
        return false;
    }

    // Read entire file
    std::ifstream file(m_TexturePath, std::ios::binary | std::ios::ate);
    if (!file.is_open())
    {
        LOG_ERROR(ZEditor, "TexPreviewWindow: failed to open file: {}", m_TexturePath.generic_string());
        return false;
    }

    const size_t file_size = static_cast<size_t>(file.tellg());
    file.seekg(0);

    if (file_size < 16)
    {
        LOG_ERROR(ZEditor, "TexPreviewWindow: file too small ({} bytes)", file_size);
        return false;
    }

    std::vector<uint8_t> file_data(file_size);
    file.read(reinterpret_cast<char*>(file_data.data()), file_size);
    file.close();

    // Dispatch based on detected format
    switch (m_DetectedFormat)
    {
    case TexPreviewFormat::ASTC:
        return DecompressASTC(file_data);
    case TexPreviewFormat::BC7:
        return DecompressBC7(file_data);
    case TexPreviewFormat::ETC2:
        return DecompressETC2(file_data);
    default:
        LOG_ERROR(ZEditor, "TexPreviewWindow: unrecognized texture format");
        return false;
    }
}

bool TexPreviewWindow::DecompressASTC(const std::vector<uint8_t>& file_data)
{
    // Read header (first 16 bytes)
    const uint8_t* header = file_data.data();

    const uint32_t magic = header[0] | (header[1] << 8) | (header[2] << 16) | (header[3] << 24);

    uint8_t block_x, block_y;
    uint32_t width, height;

    if (magic == 0x5CA1AB13)
    {
        // Standard ASTC header
        block_x = header[4];
        block_y = header[5];
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
        LOG_ERROR(ZEditor, "TexPreviewWindow: unrecognized ASTC header magic 0x{:08X}", magic);
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
        LOG_ERROR(ZEditor, "TexPreviewWindow: unsupported ASTC block size {}x{}", block_x, block_y);
        return false;
    }

    // Compressed data starts after 16-byte header
    const size_t compressed_size = file_data.size() - 16;
    const uint8_t* compressed_data = file_data.data() + 16;

    // Initialize decompressor
    if (!ZEngine::Render::ASTCDecompressor::Initialize())
    {
        LOG_ERROR(ZEditor, "TexPreviewWindow: failed to initialize ASTCDecompressor");
        return false;
    }

    // Decompress
    ZEngine::Render::ASTCDecompressResult result =
        ZEngine::Render::ASTCDecompressor::Decompress(
            compressed_data,
            compressed_size,
            width,
            height,
            block_size);

    if (!result.success)
    {
        LOG_ERROR(ZEditor, "TexPreviewWindow: ASTC decompression failed: {}", result.error_message);
        return false;
    }

    m_PreviewWidth = result.width;
    m_PreviewHeight = result.height;
    m_PreviewPixels = std::move(result.pixels);

    m_NeedsDecompress = false;
    m_TextureLoaded = true;

    return true;
}

bool TexPreviewWindow::DecompressBC7(const std::vector<uint8_t>& file_data)
{
    // DDS container with BC7 payload (Unity convention, same as
    // ImageConversion.cpp:516-546):
    //   Bytes 0-3:     magic   "DDS " (0x20534444 LE)
    //   Bytes 4-127:   DDS_HEADER (124 bytes)
    //     +12:          height  (uint32 LE)
    //     +16:          width   (uint32 LE)
    //     +80:          pf_flags — DDPF_FOURCC = 0x4
    //     +84:          fourCC
    //   Bytes 128-147: DDS_HEADER_DX10 (when fourCC == "DX10")
    //     +128:         dxgiFormat (98 = BC7_UNORM, 99 = BC7_SRGB)
    //   Bytes 148+:    raw BC7 block data (16 bytes per 4x4 block)

    if (file_data.size() < 148)
    {
        LOG_ERROR(ZEditor, "TexPreviewWindow: DDS/BC7 file too small for header");
        return false;
    }

    // Validate DDS magic: "DDS " (0x20534444)
    const uint32_t magic = file_data[0] | (file_data[1] << 8) | (file_data[2] << 16) | (file_data[3] << 24);
    if (magic != 0x20534444)
    {
        LOG_ERROR(ZEditor, "TexPreviewWindow: unrecognized DDS header magic 0x{:08X}", magic);
        return false;
    }

    // Validate DDPF_FOURCC flag
    const uint32_t pf_flags = file_data[80] | (file_data[81] << 8) | (file_data[82] << 16) | (file_data[83] << 24);
    if (!(pf_flags & 0x4))
    {
        LOG_ERROR(ZEditor, "TexPreviewWindow: DDS pixel format is not FOURCC (flags 0x{:08X})", pf_flags);
        return false;
    }

    // Check fourCC == "DX10" (0x30315844) — required for BC7
    const uint32_t fourCC = file_data[84] | (file_data[85] << 8) | (file_data[86] << 16) | (file_data[87] << 24);
    if (fourCC != 0x30315844) // "DX10"
    {
        LOG_ERROR(ZEditor, "TexPreviewWindow: DDS fourCC is not DX10 (got 0x{:08X}), BC7 requires DX10 extension", fourCC);
        return false;
    }

    // Validate DXGI format is BC7
    const uint32_t dxgiFormat = file_data[128] | (file_data[129] << 8) | (file_data[130] << 16) | (file_data[131] << 24);
    if (dxgiFormat != 98 && dxgiFormat != 99) // BC7_UNORM / BC7_SRGB
    {
        LOG_ERROR(ZEditor, "TexPreviewWindow: DXGI format {} is not BC7 (expected 98=BC7_UNORM or 99=BC7_SRGB)", dxgiFormat);
        return false;
    }

    // Read dimensions from DDS header
    const uint32_t height = file_data[12] | (file_data[13] << 8) | (file_data[14] << 16) | (file_data[15] << 24);
    const uint32_t width  = file_data[16] | (file_data[17] << 8) | (file_data[18] << 16) | (file_data[19] << 24);

    if (width == 0 || height == 0 || width > 16384 || height > 16384)
    {
        LOG_ERROR(ZEditor, "TexPreviewWindow: invalid BC7 dimensions {}x{}", width, height);
        return false;
    }

    const size_t compressed_size = file_data.size() - 148;
    const uint8_t* compressed_data = file_data.data() + 148;

    if (!ZEngine::Render::BC7Decompressor::ValidateSize(width, height, compressed_size))
    {
        LOG_ERROR(ZEditor, "TexPreviewWindow: BC7 data size mismatch (got {}, expected at least {})",
                  compressed_size, (static_cast<uint32_t>((width + 3) / 4) *
                                    static_cast<uint32_t>((height + 3) / 4) * 16));
        return false;
    }

    ZEngine::Render::BC7DecompressResult result =
        ZEngine::Render::BC7Decompressor::Decompress(
            compressed_data,
            compressed_size,
            width,
            height);

    if (!result.success)
    {
        LOG_ERROR(ZEditor, "TexPreviewWindow: BC7 decompression failed: {}", result.error_message);
        return false;
    }

    m_PreviewWidth = result.width;
    m_PreviewHeight = result.height;
    m_PreviewPixels = std::move(result.pixels);

    m_NeedsDecompress = false;
    m_TextureLoaded = true;

    return true;
}

bool TexPreviewWindow::DecompressETC2(const std::vector<uint8_t>& file_data)
{
    // PKM container with ETC2 payload (Unity convention, same as
    // ImageConversion.cpp:570-598 and Texture2D_EncodeTo.h:151-173):
    //   Bytes 0-3:     magic   "PKM " (0x50, 0x4B, 0x4D, 0x20)
    //   Bytes 4-5:     version ("10" = ETC2, "01" = ETC1)
    //   Bytes 6-7:     data type ("rR" = ETC2 RGB, "rG" = ETC2 RGBA1, "rA" = ETC2 RGBA8)
    //   Bytes 8-9:     extended width  (big-endian, padded to multiple of 4)
    //   Bytes 10-11:   extended height (big-endian, padded to multiple of 4)
    //   Bytes 12-13:   original width  (big-endian)
    //   Bytes 14-15:   original height (big-endian)
    //   Bytes 16+:     ETC2 compressed block data

    if (file_data.size() < 16)
    {
        LOG_ERROR(ZEditor, "TexPreviewWindow: PKM/ETC2 file too small for header");
        return false;
    }

    // Validate PKM magic: "PKM " (0x50, 0x4B, 0x4D, 0x20)
    if (file_data[0] != 0x50 || file_data[1] != 0x4B || file_data[2] != 0x4D || file_data[3] != 0x20)
    {
        LOG_ERROR(ZEditor, "TexPreviewWindow: unrecognized PKM header magic");
        return false;
    }

    // Check version: "10" = ETC2
    bool isETC2 = (file_data[4] == 0x31 && file_data[5] == 0x30);
    if (!isETC2)
    {
        LOG_ERROR(ZEditor, "TexPreviewWindow: PKM version is not ETC2 (got {:02x}{:02x})", file_data[4], file_data[5]);
        return false;
    }

    // Read dimensions from PKM header (original width/height at bytes 12-15, big-endian)
    const uint32_t width  = (static_cast<uint32_t>(file_data[12]) << 8) | file_data[13];
    const uint32_t height = (static_cast<uint32_t>(file_data[14]) << 8) | file_data[15];

    if (width == 0 || height == 0 || width > 16384 || height > 16384)
    {
        LOG_ERROR(ZEditor, "TexPreviewWindow: invalid ETC2 dimensions {}x{}", width, height);
        return false;
    }

    // Determine ETC2 variant from data type field (bytes 6-7)
    ZEngine::Render::ETC2Variant variant;
    if (file_data[6] == 'r' && file_data[7] == 'A')
        variant = ZEngine::Render::ETC2Variant::RGBA8;
    else if (file_data[6] == 'r' && file_data[7] == 'G')
        variant = ZEngine::Render::ETC2Variant::RGBA1;
    else
        variant = ZEngine::Render::ETC2Variant::RGB;

    const size_t compressed_size = file_data.size() - 16;
    const uint8_t* compressed_data = file_data.data() + 16;

    if (!ZEngine::Render::ETC2Decompressor::ValidateSize(width, height, compressed_size, variant))
    {
        LOG_ERROR(ZEditor, "TexPreviewWindow: ETC2 data size mismatch (got {} bytes)", compressed_size);
        return false;
    }

    ZEngine::Render::ETC2DecompressResult result =
        ZEngine::Render::ETC2Decompressor::Decompress(
            compressed_data,
            compressed_size,
            width,
            height,
            variant);

    if (!result.success)
    {
        LOG_ERROR(ZEditor, "TexPreviewWindow: ETC2 decompression failed: {}", result.error_message);
        return false;
    }

    m_PreviewWidth = result.width;
    m_PreviewHeight = result.height;
    m_PreviewPixels = std::move(result.pixels);

    m_NeedsDecompress = false;
    m_TextureLoaded = true;

    return true;
}

bool TexPreviewWindow::CreateGPUTexture()
{
    if (!m_TextureLoaded || m_PreviewPixels.empty())
        return false;

    auto* gpu_res = UiGpuResources::Get();
    if (gpu_res == nullptr)
    {
        LOG_ERROR(ZEditor, "TexPreviewWindow: UiGpuResources not available");
        return false;
    }

    void* handle = gpu_res->UpdateDynamicTexture(
        nullptr,  // Create new
        m_PreviewPixels.data(),
        m_PreviewWidth,
        m_PreviewHeight);

    if (handle == nullptr)
    {
        LOG_ERROR(ZEditor, "TexPreviewWindow: failed to create GPU texture");
        return false;
    }

    m_TextureId = handle;
    m_GpuTextureHandle = handle;

    m_NeedsTextureCreate = false;
    LOG_INFO(ZEditor, "TexPreviewWindow: GPU texture created ({}x{}, {})",
             m_PreviewWidth, m_PreviewHeight, FormatToString(m_DetectedFormat));
    return true;
}

void TexPreviewWindow::EnsureCheckerboardTexture()
{
    if (m_CheckerTextureId != nullptr)
        return;

    auto* gpu_res = UiGpuResources::Get();
    if (gpu_res == nullptr)
        return;

    auto pixels = BuildCheckerboardBitmap();
    m_CheckerTextureId = gpu_res->UpdateDynamicTexture(
        nullptr, pixels.data(), CHECKER_TEX_SIZE, CHECKER_TEX_SIZE);
}

void TexPreviewWindow::DrawCheckerboard(UIRenderer* renderer, const UIRect& rect) const
{
    if (m_CheckerTextureId == nullptr)
    {
        const_cast<TexPreviewWindow*>(this)->EnsureCheckerboardTexture();
    }

    if (m_CheckerTextureId != nullptr)
    {
        const float tile_u = rect.width / CHECKER_TEX_SIZE;
        const float tile_v = rect.height / CHECKER_TEX_SIZE;
        renderer->DrawTexturedQuad(rect, m_CheckerTextureId,
                                   UIColor(1.0f, 1.0f, 1.0f, 1.0f),
                                   Vector2(0.0f, 0.0f),
                                   Vector2(tile_u, tile_v));
    }
    else
    {
        renderer->DrawQuad(rect, UIColor(0.2f, 0.2f, 0.2f, 1.0f));
    }
}

void TexPreviewWindow::DrawPreviewImage(UIRenderer* renderer, const UIRect& rect) const
{
    if (!m_TextureLoaded)
        return;

    if (m_NeedsTextureCreate)
    {
        const_cast<TexPreviewWindow*>(this)->CreateGPUTexture();
    }

    if (m_TextureId == nullptr)
        return;

    const float img_w = static_cast<float>(m_PreviewWidth) * m_ZoomLevel;
    const float img_h = static_cast<float>(m_PreviewHeight) * m_ZoomLevel;
    const float img_x = rect.x + (rect.width - img_w) / 2.0f + m_PanX;
    const float img_y = rect.y + (rect.height - img_h) / 2.0f + m_PanY;

    renderer->PushClipRect(rect);

    const float clip_l = rect.x;
    const float clip_t = rect.y;
    const float clip_r = rect.x + rect.width;
    const float clip_b = rect.y + rect.height;

    const float draw_l = std::max(img_x, clip_l);
    const float draw_t = std::max(img_y, clip_t);
    const float draw_r = std::min(img_x + img_w, clip_r);
    const float draw_b = std::min(img_y + img_h, clip_b);

    if (draw_r > draw_l && draw_b > draw_t)
    {
        Vector2 uv0, uv1;
        if (img_w > 0.0f && img_h > 0.0f)
        {
            uv0.x = (draw_l - img_x) / img_w;
            uv0.y = (draw_t - img_y) / img_h;
            uv1.x = (draw_r - img_x) / img_w;
            uv1.y = (draw_b - img_y) / img_h;
        }
        else
        {
            uv0 = Vector2(0.0f, 0.0f);
            uv1 = Vector2(1.0f, 1.0f);
        }

        renderer->DrawTexturedQuad(ToRect(draw_l, draw_t, draw_r - draw_l, draw_b - draw_t),
                                   m_TextureId,
                                   UIColor(1.0f, 1.0f, 1.0f, 1.0f),
                                   uv0, uv1);
    }

    renderer->PopClipRect();
}

void TexPreviewWindow::DrawInfoOverlay(UIRenderer* renderer, const UIRect& rect) const
{
    if (!m_TextureLoaded)
        return;

    char buf[192];
    std::snprintf(buf, sizeof(buf), "Format: %s  |  Size: %ux%u  |  Zoom: %.0f%%  |  Pan: %.0f, %.0f",
                  FormatToString(m_DetectedFormat),
                  m_PreviewWidth, m_PreviewHeight,
                  m_ZoomLevel * 100.0f, m_PanX, m_PanY);

    // Info bar background
    renderer->DrawQuad(ToRect(rect.x, rect.y, rect.width, 28.0f),
                       UIColor(0.1f, 0.1f, 0.1f, 0.85f));

    renderer->DrawText(ToRect(rect.x + 8.0f, rect.y, rect.width - 16.0f, 28.0f),
                       buf, 13.0f,
                       UIColor(0.9f, 0.9f, 0.9f, 1.0f),
                       TextAnchor::MiddleLeft,
                       TextWrapMode::NoWrap);
}

void TexPreviewWindow::OnZoomIn()     { m_ZoomLevel = std::min(m_ZoomLevel * 1.2f, m_MaxZoom); }
void TexPreviewWindow::OnZoomOut()    { m_ZoomLevel = std::max(m_ZoomLevel / 1.2f, m_MinZoom); }
void TexPreviewWindow::OnFitToWindow() { m_ZoomLevel = 1.0f; m_PanX = m_PanY = 0.0f; }
void TexPreviewWindow::OnSaveAsPNG()   { /* TODO */ }
void TexPreviewWindow::OnReloadTexture()
{
    m_NeedsDecompress = true;
    if (DecompressTexture())
    {
        m_TextureLoaded = true;
        m_NeedsTextureCreate = true;
    }
}

void TexPreviewWindow::OnKeyChar(unsigned int codepoint)
{
    // O/o = open file dialog
    if (codepoint == 'O' || codepoint == 'o')
    {
#ifdef _WIN32
        OPENFILENAMEA ofn {};
        char file_path[MAX_PATH] {};
        ofn.lStructSize = sizeof(OPENFILENAMEA);
        ofn.hwndOwner = nullptr;
        ofn.lpstrFilter = "Texture Files (*.astc;*.bc7;*.pkm)\0*.astc;*.bc7;*.pkm\0ASTC Files (*.astc)\0*.astc\0BC7 Files (*.bc7)\0*.bc7\0ETC2/PKM Files (*.pkm)\0*.pkm\0All Files (*.*)\0*.*\0";
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
