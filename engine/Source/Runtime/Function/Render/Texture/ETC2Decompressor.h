#pragma once

// =============================================================================
// ETC2Decompressor
// -----------------------------------------------------------------------------
// Software ETC2 decompressor. Decodes ETC2-compressed texture data (PKM format)
// into RGBA8 pixel data for preview/display purposes.
//
// ETC2 is a block-compressed format where each 4x4 pixel block is stored as
// 64 bits (8 bytes) for RGB, or 128 bits (16 bytes) for RGBA (RGB + EAC alpha).
// This is a CPU software decoder — no GPU dependency.
//
// Reference: Khronos ETC2 / EAC specification
//   https://www.khronos.org/registry/OpenGL/extensions/OES/OES_compressed_ETC2_RGB8_texture.txt
//
// Based on Unity's ETC2Decompression.cpp (Runtime/Graphics/ETC2Decompression.cpp).
//
// Design notes:
//   * READ-ONLY decompressor (no re-compression).
//   * Stateless: all methods are static, no initialization required.
//   * Thread-safe: each call operates on its own stack / output buffer.
//   * Supports ETC2 RGB, ETC2 RGB + punch-through alpha (A1), and ETC2 RGBA (EAC).
//   * LDR only (matches ZEngine's texture pipeline).
// =============================================================================

#include <cstdint>
#include <string>
#include <vector>

namespace ZEngine::Render
{

// ETC2 sub-format (determined from PKM header bytes 6-7)
enum class ETC2Variant
{
    RGB,     // ETC2 RGB only (8 bytes/block), PKM type "rR"
    RGBA1,   // ETC2 RGB + 1-bit punch-through alpha (8 bytes/block), PKM type "rG"
    RGBA8,   // ETC2 RGB + EAC alpha (16 bytes/block), PKM type "rA"
};

// ETC2 decompression result (mirrors ASTCDecompressResult / BC7DecompressResult)
struct ETC2DecompressResult
{
    bool success {false};
    std::string error_message;
    uint32_t width {0};
    uint32_t height {0};
    std::vector<uint8_t> pixels;  // Decompressed RGBA8 data
};

class ETC2Decompressor
{
public:
    // Decompress ETC2 data to RGBA8.
    // @param compressed_data: ETC2 block data (8 or 16 bytes per 4x4 block depending on variant)
    // @param data_size: Size of compressed data in bytes
    // @param width: Texture width in pixels
    // @param height: Texture height in pixels
    // @param variant: ETC2 sub-format (RGB, RGBA1, or RGBA8)
    // @return Decompression result with RGBA8 pixels
    static ETC2DecompressResult Decompress(
        const uint8_t* compressed_data,
        size_t data_size,
        uint32_t width,
        uint32_t height,
        ETC2Variant variant);

    // Validate that data_size is consistent with width * height for the given ETC2 variant.
    // ETC2 RGB/RGBA1: 8 bytes per 4x4 block; ETC2 RGBA8: 16 bytes per 4x4 block.
    static bool ValidateSize(uint32_t width, uint32_t height, size_t data_size, ETC2Variant variant);

private:
    // Decode a single 64-bit ETC2 block into 4x4 RGBA8 pixels.
    // @param src: 8 bytes of ETC2 block data (big-endian 64-bit value)
    // @param out_pixels: Output buffer for 4*4*4 = 64 bytes of RGBA8 data
    // @param punch_through_alpha: if true, treat as ETC2 RGB+A1 mode
    static void DecodeETC2Block(uint64_t src, uint8_t out_pixels[64], bool punch_through_alpha);

    // Decode a single 64-bit EAC alpha block into 4x4 alpha values.
    // @param src: 8 bytes of EAC block data (big-endian 64-bit value)
    // @param out_alpha: Output buffer for 4*4 = 16 bytes of alpha values
    static void DecodeEACAlphaBlock(uint64_t src, uint8_t out_alpha[16]);
};

}  // namespace ZEngine::Render
