#pragma once

// =============================================================================
// BC7Decompressor
// -----------------------------------------------------------------------------
// Decompresses BC7-compressed texture data into RGBA8 pixel data for
// preview/display purposes.
//
// BC7 is a block-compressed format where each 4x4 pixel block is stored as
// 128 bits (16 bytes). This is a CPU software decoder — no GPU dependency.
//
// Design notes:
//   * READ-ONLY decompressor (no re-compression).
//   * Stateless: all methods are static, no initialization required.
//   * Thread-safe: each call operates on its own stack / output buffer.
//   * Supports all 8 BC7 mode subsets per the DX11 / Vulkan spec.
//   * LDR only (matches ZEngine's texture pipeline).
// =============================================================================

#include <cstdint>
#include <string>
#include <vector>

namespace ZEngine::Render
{

// BC7 decompression result (mirrors ASTCDecompressResult)
struct BC7DecompressResult
{
    bool success {false};
    std::string error_message;
    uint32_t width {0};
    uint32_t height {0};
    std::vector<uint8_t> pixels;  // Decompressed RGBA8 data
};

class BC7Decompressor
{
public:
    // Decompress BC7 data to RGBA8.
    // @param compressed_data: BC7 block data (16 bytes per 4x4 block)
    // @param data_size: Size of compressed data in bytes
    // @param width: Texture width in pixels (must be multiple of 4, or padded)
    // @param height: Texture height in pixels (must be multiple of 4, or padded)
    // @return Decompression result with RGBA8 pixels
    static BC7DecompressResult Decompress(
        const uint8_t* compressed_data,
        size_t data_size,
        uint32_t width,
        uint32_t height);

    // Validate that data_size is consistent with width * height for BC7.
    // BC7 uses 16 bytes per 4x4 block.
    static bool ValidateSize(uint32_t width, uint32_t height, size_t data_size);

private:
    // Decode a single 128-bit BC7 block into 4x4 RGBA8 pixels.
    // @param block: 16 bytes of BC7 block data
    // @param out_pixels: Output buffer for 4*4*4 = 64 bytes of RGBA8 data
    static void DecodeBlock(const uint8_t block[16], uint8_t out_pixels[64]);
};

}  // namespace ZEngine::Render
