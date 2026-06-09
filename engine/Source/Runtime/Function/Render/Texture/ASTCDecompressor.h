#pragma once

// =============================================================================
// ASTCDecompressor
// -----------------------------------------------------------------------------
// Wraps the ARM astcenc library to decompress ASTC-compressed texture data
// into RGBA8 pixel data for preview/display purposes.
//
// Design notes:
//   * This is a READ-ONLY decompressor -- we never re-compress on the CPU.
//     Compression is an offline cook step (TextureImporter -> astcenc).
//   * The astcenc library is built as a static library (astcenc) in
//     engine/3rdparty/CMakeLists.txt. We link it PRIVATE into the decompressor.
//   * Thread-safety: astcenc context objects are NOT thread-safe. We use a
//     simple per-thread context cache (like Unity's ASTCDecompressorContextPool).
//   * Supported block sizes: 4x4, 5x5, 6x6, 8x8, 10x10, 12x12 (LDR only).
//   * HDR ASTC is NOT supported in this initial version (matches ZEngine's
//     current texture pipeline which only handles LDR).
// =============================================================================

// Forward declaration - Texture2D is in global namespace, NOT in ZEngine::Render
class Texture2D;

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ZEngine::Render
{

// ASTC block size enumeration (matches TextureImporterSettings::Format)
enum class ASTCBlockSize
{
    ASTC_4x4,
    ASTC_5x5,
    ASTC_6x6,
    ASTC_8x8,
    ASTC_10x10,
    ASTC_12x12,
    // 3D block sizes (not yet supported)
    ASTC_3x3x3,
    ASTC_4x4x4,
};

// Decompression result
struct ASTCDecompressResult
{
    bool success {false};
    std::string error_message;
    uint32_t width {0};
    uint32_t height {0};
    std::vector<uint8_t> pixels;  // Decompressed RGBA8 data
};

class ASTCDecompressor
{
public:
    // Initialize the decompressor (call once at startup)
    static bool Initialize();

    // Shutdown and release all contexts
    static void Shutdown();

    // Decompress ASTC data to RGBA8
    // @param compressed_data: ASTC-compressed data buffer
    // @param data_size: Size of compressed data in bytes
    // @param width: Texture width in pixels
    // @param height: Texture height in pixels
    // @param block_size: ASTC block size (e.g., ASTC_4x4)
    // @return Decompression result with RGBA8 pixels
    static ASTCDecompressResult Decompress(
        const uint8_t* compressed_data,
        size_t data_size,
        uint32_t width,
        uint32_t height,
        ASTCBlockSize block_size);

    // Helper: Decompress directly from a Texture2D object
    // @param texture: Source Texture2D with ASTC-compressed m_Pixels
    // @return Decompression result (empty if texture is not ASTC)
    // Note: Texture2D is in global namespace, use ::Texture2D to disambiguate
    static ASTCDecompressResult DecompressTexture(const ::Texture2D* texture);

private:
    // Per-thread context cache (astcenc is NOT thread-safe)
    // Implementation in .cpp file
};

}  // namespace ZEngine::Render
