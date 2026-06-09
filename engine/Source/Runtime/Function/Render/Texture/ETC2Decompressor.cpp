// =============================================================================
// ETC2Decompressor.cpp
// -----------------------------------------------------------------------------
// Software ETC2 decompressor. Decodes ETC2-compressed texture data into RGBA8
// pixel data for preview/display purposes.
//
// Based on Unity's ETC2Decompression.cpp (Runtime/Graphics/ETC2Decompression.cpp).
// Simplified: only RGBA8 output, no downscaling, no 565/4444 output formats.
//
// ETC2 block layout (64 bits / 8 bytes per 4x4 block for RGB):
//   The 64-bit block is read in big-endian order. Bit 63 is MSB of byte 0.
//
//   ETC2 extends ETC1 with three additional modes:
//     - T mode: when differential mode and R+delta_R out of [0,31]
//     - H mode: when differential mode and G+delta_G out of [0,31]
//     - Planar mode: when differential mode and B+delta_B out of [0,31]
//
//   EAC alpha (for RGBA8) is a separate 64-bit block per 4x4 pixels.
// =============================================================================

#include "ETC2Decompressor.h"

#include "Runtime/Core/Base/Macro.h"

#include <algorithm>
#include <cstring>

namespace ZEngine::Render
{

namespace
{

// ---------------------------------------------------------------------------
// Utilities (from Unity's ETC2Decompression.cpp)
// ---------------------------------------------------------------------------

inline uint8_t ClampToUInt8(int x)
{
    if (x < 0) return 0;
    if (x > 255) return 255;
    return static_cast<uint8_t>(x);
}

inline uint8_t Div4AndClampToUInt8(int x)
{
    return ClampToUInt8(x / 4);
}

inline uint8_t Clamp0(int x)
{
    return x < 0 ? 0 : static_cast<uint8_t>(x);
}

inline uint8_t Clamp255(int x)
{
    return x > 255 ? 255 : static_cast<uint8_t>(x);
}

inline int DivRoundUp(int a, int b)
{
    return a / b + ((a % b) ? 1 : 0);
}

inline uint32_t GetSingleBit(uint64_t src, int bit)
{
    return static_cast<uint32_t>((src >> bit) & 1);
}

inline uint32_t GetBitRange(uint64_t src, int low, int high)
{
    int numBits = (high - low) + 1;
    return static_cast<uint32_t>((src >> low) & ((1ULL << numBits) - 1));
}

inline uint8_t ExtendTo8(uint8_t src, uint32_t fromBits)
{
    return (src << (8 - fromBits)) | (src >> (2 * fromBits - 8));
}

inline int8_t Extend3BitSignedDelta(uint8_t src)
{
    bool isNeg = (src & (1 << 2)) != 0;
    return static_cast<int8_t>((isNeg ? ~((1 << 3) - 1) : 0) | src);
}

// Read a 64-bit block from ETC2 data in big-endian order
inline uint64_t Get64BitBlock(const uint8_t* src, int blockNdx)
{
    uint64_t block = 0;
    for (int i = 0; i < 8; i++)
        block = (block << 8) | static_cast<uint64_t>(src[blockNdx * 8 + i]);
    return block;
}

// Modifier lookup table for individual/differential modes
static const int16_t kModifierLUT[8][4] =
{
    {  2,   8,  -2,   -8 },
    {  5,  17,  -5,  -17 },
    {  9,  29,  -9,  -29 },
    { 13,  42, -13,  -42 },
    { 18,  60, -18,  -60 },
    { 24,  80, -24,  -80 },
    { 33, 106, -33, -106 },
    { 47, 183, -47, -183 }
};

// Distance table for T and H modes
static const uint8_t kDistTable[8] = { 3, 6, 11, 16, 23, 32, 41, 64 };

// EAC modifier lookup table
static const int8_t kEACModifierLUT[16][8] =
{
    { -3,  -6,  -9, -15,  2,  5,  8, 14 },
    { -3,  -7, -10, -13,  2,  6,  9, 12 },
    { -2,  -5,  -8, -13,  1,  4,  7, 12 },
    { -2,  -4,  -6, -13,  1,  3,  5, 12 },
    { -3,  -6,  -8, -12,  2,  5,  7, 11 },
    { -3,  -7,  -9, -11,  2,  6,  8, 10 },
    { -4,  -7,  -8, -11,  3,  6,  7, 10 },
    { -3,  -5,  -8, -11,  2,  4,  7, 10 },
    { -2,  -6,  -8, -10,  1,  5,  7,  9 },
    { -2,  -5,  -8, -10,  1,  4,  7,  9 },
    { -2,  -4,  -8, -10,  1,  3,  7,  9 },
    { -2,  -5,  -7, -10,  1,  4,  6,  9 },
    { -3,  -4,  -7, -10,  2,  3,  6,  9 },
    { -1,  -2,  -3, -10,  0,  1,  2,  9 },
    { -4,  -6,  -8,  -9,  3,  5,  7,  8 },
    { -3,  -5,  -7,  -9,  2,  4,  6,  8 }
};

enum AlphaMode
{
    kAlphaModePunchThrough,
    kAlphaModeOpaque,
    kAlphaModeEAC
};

enum BlockMode
{
    kBlockModeIndividual = 0,
    kBlockModeDifferential,
    kBlockModeT,
    kBlockModeH,
    kBlockModePlanar,
};

// Decode a single ETC2 block (64 bits) into 4x4 RGBA8 pixels
void DecompressETC2Block(uint64_t src, uint8_t* buffer, AlphaMode alphaMode)
{
    const bool punchThroughAlpha = (alphaMode == kAlphaModePunchThrough);
    const bool writeAlpha = (alphaMode == kAlphaModePunchThrough || alphaMode == kAlphaModeOpaque);
    const uint8_t alpha = writeAlpha ? 255 : 0;

    int     diffOpaqueBit = static_cast<int>(GetSingleBit(src, 33));
    int8_t  selBR = static_cast<int8_t>(GetBitRange(src, 59, 63));
    int8_t  selBG = static_cast<int8_t>(GetBitRange(src, 51, 55));
    int8_t  selBB = static_cast<int8_t>(GetBitRange(src, 43, 47));
    int8_t  selDR = Extend3BitSignedDelta(static_cast<uint8_t>(GetBitRange(src, 56, 58)));
    int8_t  selDG = Extend3BitSignedDelta(static_cast<uint8_t>(GetBitRange(src, 48, 50)));
    int8_t  selDB = Extend3BitSignedDelta(static_cast<uint8_t>(GetBitRange(src, 40, 42)));

    BlockMode mode;

    if (!punchThroughAlpha && diffOpaqueBit == 0)
        mode = kBlockModeIndividual;
    else if (selBR + selDR < 0 || selBR + selDR > 31)
        mode = kBlockModeT;
    else if (selBG + selDG < 0 || selBG + selDG > 31)
        mode = kBlockModeH;
    else if (selBB + selDB < 0 || selBB + selDB > 31)
        mode = kBlockModePlanar;
    else
        mode = kBlockModeDifferential;

    if (mode == kBlockModeIndividual || mode == kBlockModeDifferential)
    {
        int         flipBit = static_cast<int>(GetSingleBit(src, 32));
        uint32_t    table[2] = { GetBitRange(src, 37, 39), GetBitRange(src, 34, 36) };
        uint8_t     baseR[2], baseG[2], baseB[2];

        if (mode == kBlockModeIndividual)
        {
            baseR[0] = ExtendTo8(static_cast<uint8_t>(GetBitRange(src, 60, 63)), 4);
            baseR[1] = ExtendTo8(static_cast<uint8_t>(GetBitRange(src, 56, 59)), 4);
            baseG[0] = ExtendTo8(static_cast<uint8_t>(GetBitRange(src, 52, 55)), 4);
            baseG[1] = ExtendTo8(static_cast<uint8_t>(GetBitRange(src, 48, 51)), 4);
            baseB[0] = ExtendTo8(static_cast<uint8_t>(GetBitRange(src, 44, 47)), 4);
            baseB[1] = ExtendTo8(static_cast<uint8_t>(GetBitRange(src, 40, 43)), 4);
        }
        else
        {
            baseR[0] = ExtendTo8(static_cast<uint8_t>(selBR), 5);
            baseG[0] = ExtendTo8(static_cast<uint8_t>(selBG), 5);
            baseB[0] = ExtendTo8(static_cast<uint8_t>(selBB), 5);
            baseR[1] = ExtendTo8(static_cast<uint8_t>(selBR + selDR), 5);
            baseG[1] = ExtendTo8(static_cast<uint8_t>(selBG + selDG), 5);
            baseB[1] = ExtendTo8(static_cast<uint8_t>(selBB + selDB), 5);
        }

        uint32_t offset = 0;
        for (int y = 0; y < 4; ++y)
        {
            for (int x = 0; x < 4; ++x)
            {
                int     pixelNdx = x * 4 + y;
                int     subBlock = ((flipBit ? y : x) >= 2) ? 1 : 0;
                uint32_t tableNdx = table[subBlock];
                uint32_t modifierNdx = (GetSingleBit(src, 16 + pixelNdx) << 1) | GetSingleBit(src, pixelNdx);

                if (punchThroughAlpha && diffOpaqueBit == 0 && modifierNdx == 2)
                {
                    buffer[offset++] = 0;
                    buffer[offset++] = 0;
                    buffer[offset++] = 0;
                    buffer[offset++] = 0;
                }
                else
                {
                    int modifier;
                    if (punchThroughAlpha && diffOpaqueBit == 0 && (modifierNdx == 0 || modifierNdx == 2))
                        modifier = 0;
                    else
                        modifier = kModifierLUT[tableNdx][modifierNdx];

                    buffer[offset++] = ClampToUInt8(static_cast<int>(baseR[subBlock]) + modifier);
                    buffer[offset++] = ClampToUInt8(static_cast<int>(baseG[subBlock]) + modifier);
                    buffer[offset++] = ClampToUInt8(static_cast<int>(baseB[subBlock]) + modifier);
                    buffer[offset++] = alpha;
                }
            }
        }
    }
    else if (mode == kBlockModeT || mode == kBlockModeH)
    {
        uint8_t paintR[4], paintG[4], paintB[4];

        if (mode == kBlockModeT)
        {
            uint8_t R1a = static_cast<uint8_t>(GetBitRange(src, 59, 60));
            uint8_t R1b = static_cast<uint8_t>(GetBitRange(src, 56, 57));
            uint8_t G1  = static_cast<uint8_t>(GetBitRange(src, 52, 55));
            uint8_t B1  = static_cast<uint8_t>(GetBitRange(src, 48, 51));
            uint8_t R2  = static_cast<uint8_t>(GetBitRange(src, 44, 47));
            uint8_t G2  = static_cast<uint8_t>(GetBitRange(src, 40, 43));
            uint8_t B2  = static_cast<uint8_t>(GetBitRange(src, 36, 39));
            uint32_t distNdx = (GetBitRange(src, 34, 35) << 1) | GetSingleBit(src, 32);
            int dist = kDistTable[distNdx];

            paintR[0] = ExtendTo8((R1a << 2) | R1b, 4);
            paintG[0] = ExtendTo8(G1, 4);
            paintB[0] = ExtendTo8(B1, 4);
            paintR[2] = ExtendTo8(R2, 4);
            paintG[2] = ExtendTo8(G2, 4);
            paintB[2] = ExtendTo8(B2, 4);
            paintR[1] = Clamp255(static_cast<int>(paintR[2]) + dist);
            paintG[1] = Clamp255(static_cast<int>(paintG[2]) + dist);
            paintB[1] = Clamp255(static_cast<int>(paintB[2]) + dist);
            paintR[3] = Clamp0(static_cast<int>(paintR[2]) - dist);
            paintG[3] = Clamp0(static_cast<int>(paintG[2]) - dist);
            paintB[3] = Clamp0(static_cast<int>(paintB[2]) - dist);
        }
        else // kBlockModeH
        {
            uint8_t R1  = static_cast<uint8_t>(GetBitRange(src, 59, 62));
            uint8_t G1a = static_cast<uint8_t>(GetBitRange(src, 56, 58));
            uint8_t G1b = static_cast<uint8_t>(GetSingleBit(src, 52));
            uint8_t B1a = static_cast<uint8_t>(GetSingleBit(src, 51));
            uint8_t B1b = static_cast<uint8_t>(GetBitRange(src, 47, 49));
            uint8_t R2  = static_cast<uint8_t>(GetBitRange(src, 43, 46));
            uint8_t G2  = static_cast<uint8_t>(GetBitRange(src, 39, 42));
            uint8_t B2  = static_cast<uint8_t>(GetBitRange(src, 35, 38));
            uint8_t baseR[2], baseG[2], baseB[2];

            baseR[0] = ExtendTo8(R1, 4);
            baseG[0] = ExtendTo8((G1a << 1) | G1b, 4);
            baseB[0] = ExtendTo8((B1a << 3) | B1b, 4);
            baseR[1] = ExtendTo8(R2, 4);
            baseG[1] = ExtendTo8(G2, 4);
            baseB[1] = ExtendTo8(B2, 4);

            uint32_t baseValue0 = (static_cast<uint32_t>(baseR[0]) << 16) | (static_cast<uint32_t>(baseG[0]) << 8) | baseB[0];
            uint32_t baseValue1 = (static_cast<uint32_t>(baseR[1]) << 16) | (static_cast<uint32_t>(baseG[1]) << 8) | baseB[1];
            uint32_t distNdx = (GetSingleBit(src, 34) << 2) | (GetSingleBit(src, 32) << 1) | static_cast<uint32_t>(baseValue0 >= baseValue1);
            int dist = kDistTable[distNdx];

            paintR[0] = Clamp255(static_cast<int>(baseR[0]) + dist);
            paintG[0] = Clamp255(static_cast<int>(baseG[0]) + dist);
            paintB[0] = Clamp255(static_cast<int>(baseB[0]) + dist);
            paintR[1] = Clamp0(static_cast<int>(baseR[0]) - dist);
            paintG[1] = Clamp0(static_cast<int>(baseG[0]) - dist);
            paintB[1] = Clamp0(static_cast<int>(baseB[0]) - dist);
            paintR[2] = Clamp255(static_cast<int>(baseR[1]) + dist);
            paintG[2] = Clamp255(static_cast<int>(baseG[1]) + dist);
            paintB[2] = Clamp255(static_cast<int>(baseB[1]) + dist);
            paintR[3] = Clamp0(static_cast<int>(baseR[1]) - dist);
            paintG[3] = Clamp0(static_cast<int>(baseG[1]) - dist);
            paintB[3] = Clamp0(static_cast<int>(baseB[1]) - dist);
        }

        uint32_t offset = 0;
        for (int y = 0; y < 4; y++)
        {
            for (int x = 0; x < 4; x++)
            {
                int pixelNdx = x * 4 + y;
                uint32_t paintNdx = (GetSingleBit(src, 16 + pixelNdx) << 1) | GetSingleBit(src, pixelNdx);
                if (punchThroughAlpha && diffOpaqueBit == 0 && paintNdx == 2)
                {
                    buffer[offset++] = 0;
                    buffer[offset++] = 0;
                    buffer[offset++] = 0;
                    buffer[offset++] = 0;
                }
                else
                {
                    buffer[offset++] = paintR[paintNdx];
                    buffer[offset++] = paintG[paintNdx];
                    buffer[offset++] = paintB[paintNdx];
                    buffer[offset++] = alpha;
                }
            }
        }
    }
    else // kBlockModePlanar
    {
        uint8_t GO1 = static_cast<uint8_t>(GetSingleBit(src, 56));
        uint8_t GO2 = static_cast<uint8_t>(GetBitRange(src, 49, 54));
        uint8_t BO1 = static_cast<uint8_t>(GetSingleBit(src, 48));
        uint8_t BO2 = static_cast<uint8_t>(GetBitRange(src, 43, 44));
        uint8_t BO3 = static_cast<uint8_t>(GetBitRange(src, 39, 41));
        uint8_t RH1 = static_cast<uint8_t>(GetBitRange(src, 34, 38));
        uint8_t RH2 = static_cast<uint8_t>(GetSingleBit(src, 32));

        uint8_t RO = ExtendTo8(static_cast<uint8_t>(GetBitRange(src, 57, 62)), 6);
        uint8_t GO = ExtendTo8((GO1 << 6) | GO2, 7);
        uint8_t BO = ExtendTo8((BO1 << 5) | (BO2 << 3) | BO3, 6);
        uint8_t RH = ExtendTo8((RH1 << 1) | RH2, 6);
        uint8_t GH = ExtendTo8(static_cast<uint8_t>(GetBitRange(src, 25, 31)), 7);
        uint8_t BH = ExtendTo8(static_cast<uint8_t>(GetBitRange(src, 19, 24)), 6);
        uint8_t RV = ExtendTo8(static_cast<uint8_t>(GetBitRange(src, 13, 18)), 6);
        uint8_t GV = ExtendTo8(static_cast<uint8_t>(GetBitRange(src, 6, 12)), 7);
        uint8_t BV = ExtendTo8(static_cast<uint8_t>(GetBitRange(src, 0, 5)), 6);

        uint32_t offset = 0;
        for (int y = 0; y < 4; y++)
        {
            for (int x = 0; x < 4; x++)
            {
                int unclampedR = (x * (static_cast<int>(RH) - static_cast<int>(RO)) + y * (static_cast<int>(RV) - static_cast<int>(RO)) + 4 * static_cast<int>(RO) + 2);
                int unclampedG = (x * (static_cast<int>(GH) - static_cast<int>(GO)) + y * (static_cast<int>(GV) - static_cast<int>(GO)) + 4 * static_cast<int>(GO) + 2);
                int unclampedB = (x * (static_cast<int>(BH) - static_cast<int>(BO)) + y * (static_cast<int>(BV) - static_cast<int>(BO)) + 4 * static_cast<int>(BO) + 2);
                buffer[offset++] = Div4AndClampToUInt8(unclampedR);
                buffer[offset++] = Div4AndClampToUInt8(unclampedG);
                buffer[offset++] = Div4AndClampToUInt8(unclampedB);
                buffer[offset++] = alpha;
            }
        }
    }
}

// Decode a single EAC alpha block (64 bits) into 4x4 alpha values
void DecompressEACAlphaBlock(uint64_t src, uint8_t* out_alpha)
{
    int baseCodeword = static_cast<int>(GetBitRange(src, 56, 63));
    int multiplier = static_cast<int>(GetBitRange(src, 52, 55));
    const int8_t* tableRow = kEACModifierLUT[GetBitRange(src, 48, 51)];

    for (int y = 0; y < 4; ++y)
    {
        for (int x = 0; x < 4; ++x)
        {
            int pixelBitNdx = 45 - 3 * (x * 4 + y);
            uint32_t modifierNdx = GetBitRange(src, pixelBitNdx, pixelBitNdx + 2);
            int modifier = tableRow[modifierNdx];
            out_alpha[y * 4 + x] = ClampToUInt8(multiplier * modifier + baseCodeword);
        }
    }
}

}  // anonymous namespace

// ---- Public API ----

ETC2DecompressResult ETC2Decompressor::Decompress(
    const uint8_t* compressed_data,
    size_t data_size,
    uint32_t width,
    uint32_t height,
    ETC2Variant variant)
{
    ETC2DecompressResult result;

    if (width == 0 || height == 0 || width > 16384 || height > 16384)
    {
        result.error_message = "Invalid ETC2 dimensions";
        return result;
    }

    if (!ValidateSize(width, height, data_size, variant))
    {
        const size_t expected = variant == ETC2Variant::RGBA8
            ? static_cast<size_t>(DivRoundUp(width, 4) * DivRoundUp(height, 4)) * 16
            : static_cast<size_t>(DivRoundUp(width, 4) * DivRoundUp(height, 4)) * 8;
        result.error_message = "ETC2 data size mismatch (got " + std::to_string(data_size) +
                               ", expected " + std::to_string(expected) + ")";
        return result;
    }

    int numBlocksX = DivRoundUp(width, 4);
    int numBlocksY = DivRoundUp(height, 4);

    result.width = width;
    result.height = height;
    result.pixels.resize(static_cast<size_t>(width) * height * 4, 0);

    // Temporary block buffers
    uint8_t rgbBlock[64];   // 4x4 RGBA8 from ETC2 color block
    uint8_t alphaBlock[16]; // 4x4 alpha from EAC alpha block

    for (int blockY = 0; blockY < numBlocksY; ++blockY)
    {
        for (int blockX = 0; blockX < numBlocksX; ++blockX)
        {
            int blockNdx = blockY * numBlocksX + blockX;
            int baseX = blockX * 4;
            int baseY = blockY * 4;

            // Read and decode blocks
            // For RGB/RGBA1: 8 bytes per block (only color)
            // For RGBA8: 16 bytes per block = [alpha EAC 8 bytes][color ETC2 8 bytes]
            //   (matches Unity's Get128BitBlockStart/End convention)
            if (variant == ETC2Variant::RGBA8)
            {
                // Alpha EAC block is first 64 bits of 128-bit block
                uint64_t alphaBlockData = Get64BitBlock(compressed_data, 2 * blockNdx);
                DecompressEACAlphaBlock(alphaBlockData, alphaBlock);

                // Color ETC2 block is second 64 bits of 128-bit block
                uint64_t colorBlock = Get64BitBlock(compressed_data, 2 * blockNdx + 1);
                DecompressETC2Block(colorBlock, rgbBlock, kAlphaModeEAC);
            }
            else
            {
                AlphaMode alphaMode = (variant == ETC2Variant::RGBA1)
                    ? kAlphaModePunchThrough : kAlphaModeOpaque;
                uint64_t colorBlock = Get64BitBlock(compressed_data, blockNdx);
                DecompressETC2Block(colorBlock, rgbBlock, alphaMode);
            }

            // Write decoded pixels to output, handling partial blocks at edges
            for (int y = 0; y < 4; ++y)
            {
                int py = baseY + y;
                if (py >= static_cast<int>(height)) break;

                for (int x = 0; x < 4; ++x)
                {
                    int px = baseX + x;
                    if (px >= static_cast<int>(width)) break;

                    int srcOffset = (y * 4 + x) * 4;
                    size_t dstOffset = (static_cast<size_t>(py) * width + px) * 4;

                    result.pixels[dstOffset + 0] = rgbBlock[srcOffset + 0];
                    result.pixels[dstOffset + 1] = rgbBlock[srcOffset + 1];
                    result.pixels[dstOffset + 2] = rgbBlock[srcOffset + 2];

                    if (variant == ETC2Variant::RGBA8)
                        result.pixels[dstOffset + 3] = alphaBlock[y * 4 + x];
                    else
                        result.pixels[dstOffset + 3] = rgbBlock[srcOffset + 3];
                }
            }
        }
    }

    result.success = true;
    return result;
}

bool ETC2Decompressor::ValidateSize(uint32_t width, uint32_t height, size_t data_size, ETC2Variant variant)
{
    int numBlocks = DivRoundUp(width, 4) * DivRoundUp(height, 4);
    size_t expected = static_cast<size_t>(numBlocks) * (variant == ETC2Variant::RGBA8 ? 16 : 8);
    return data_size >= expected;
}

// ---- Private stubs (public API delegates to anonymous namespace) ----

void ETC2Decompressor::DecodeETC2Block(uint64_t src, uint8_t out_pixels[64], bool punch_through_alpha)
{
    DecompressETC2Block(src, out_pixels, punch_through_alpha ? kAlphaModePunchThrough : kAlphaModeOpaque);
}

void ETC2Decompressor::DecodeEACAlphaBlock(uint64_t src, uint8_t out_alpha[16])
{
    DecompressEACAlphaBlock(src, out_alpha);
}

}  // namespace ZEngine::Render
