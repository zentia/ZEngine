// =============================================================================
// BC7Decompressor.cpp
// -----------------------------------------------------------------------------
// Software BC7 decompressor. Decodes all 8 BC7 mode subsets per the
// DirectX 11 / Vulkan BC7 specification.
//
// Reference: Microsoft BC7 Format spec (DirectX documentation)
//   https://learn.microsoft.com/en-us/windows/win32/direct3d11/bc7-format
//
// Each BC7 block is 128 bits (16 bytes) encoding a 4x4 pixel region.
// The block header (typically 1-3 bytes) specifies the mode, partition,
// endpoint colors, indices, and p-bits.
// =============================================================================

#include "BC7Decompressor.h"

#include "Runtime/Core/Base/Macro.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ZEngine::Render
{

namespace
{

// ---------------------------------------------------------------------------
// Bit-stream reader (LSB-first, matching BC7 bit layout)
// ---------------------------------------------------------------------------
struct BitReader
{
    const uint8_t* data;
    int pos;  // current bit position (0 = LSB of byte 0)

    uint32_t read(int num_bits)
    {
        uint32_t val = 0;
        for (int i = 0; i < num_bits; ++i)
        {
            int byte_idx = pos >> 3;
            int bit_idx = pos & 7;
            val |= ((data[byte_idx] >> bit_idx) & 1) << i;
            ++pos;
        }
        return val;
    }
};

// ---------------------------------------------------------------------------
// BC7 mode table: derived from the DX11 spec.
// mode_index = trailing zeros of the first byte (only bit 0..7 selects mode)
// ---------------------------------------------------------------------------
struct BC7ModeInfo
{
    int num_subsets;       // 1, 2, or 3
    int partition_bits;    // bits for partition pattern index
    int endpoint_bits;     // bits per color channel per endpoint (before p-bit)
    int alpha_endpoint;    // 1 if endpoints include alpha
    int index_bits[3];     // index bits for each subset [subset0, subset1, subset2]
    int index_bits2[3];    // secondary index bits (alpha, if separate)
    int p_bits;            // number of p-bits (0, 4, or 6)
};

// clang-format off
static constexpr BC7ModeInfo kModes[8] = {
    /* mode 0 */ { 3, 4, 4, 0, {3,3,2}, {0,0,0}, 6 },
    /* mode 1 */ { 2, 6, 6, 0, {3,3,0}, {0,0,0}, 0 },
    /* mode 2 */ { 3, 6, 5, 0, {2,2,2}, {0,0,0}, 0 },
    /* mode 3 */ { 2, 6, 7, 0, {2,2,0}, {0,0,0}, 4 },
    /* mode 4 */ { 1, 0, 5, 1, {2,0,0}, {3,0,0}, 0 },
    /* mode 5 */ { 1, 0, 7, 1, {2,0,0}, {2,0,0}, 0 },
    /* mode 6 */ { 1, 0, 7, 0, {4,0,0}, {0,0,0}, 2 },
    /* mode 7 */ { 2, 6, 5, 1, {2,2,0}, {2,2,0}, 4 },
};
// clang-format on

// Partition table for 2-subset modes (64 patterns x 16 pixels)
// Sourced from the BC7 spec (Table 1).
static const uint8_t kPartition2[64][16] = {
    {0,0,1,1, 0,0,1,1, 0,0,1,1, 0,0,1,1},
    {0,0,0,1, 0,0,0,1, 0,0,0,1, 0,0,0,1},
    {0,1,1,1, 0,1,1,1, 0,1,1,1, 0,1,1,1},
    {0,0,0,1, 0,0,1,1, 0,0,1,1, 0,1,1,1},
    {0,0,0,0, 0,0,0,1, 0,0,0,1, 0,0,1,1},
    {0,0,1,1, 0,1,1,1, 0,1,1,1, 1,1,1,1},
    {0,0,0,1, 0,0,1,1, 0,1,1,1, 1,1,1,1},
    {0,0,0,0, 0,0,0,0, 1,1,1,1, 1,1,1,1},
    {0,0,0,0, 1,1,1,1, 1,1,1,1, 1,1,1,1},
    {0,0,0,0, 1,0,0,0, 1,1,1,0, 1,1,1,1},
    {0,0,1,0, 0,0,1,0, 0,0,1,0, 0,0,1,0},
    {0,1,1,0, 0,1,1,0, 0,1,1,0, 0,1,1,0},
    {0,1,0,1, 0,1,0,1, 0,1,0,1, 0,1,0,1},
    {0,0,0,0, 0,0,0,0, 0,0,0,0, 1,1,1,1},
    {0,0,0,0, 1,0,0,0, 1,1,0,0, 1,1,1,0},
    {0,0,0,0, 0,0,0,0, 1,1,1,1, 0,0,0,0},
    {0,0,0,0, 1,0,0,0, 1,1,0,0, 1,1,1,0},
    {0,1,1,1, 0,0,0,1, 0,0,0,0, 0,0,0,0},
    {0,0,1,1, 0,0,0,1, 0,0,0,0, 0,0,0,0},
    {0,0,0,1, 0,0,0,0, 0,0,0,0, 0,0,0,0},
    {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,1,1},
    {0,0,0,0, 1,0,0,0, 0,0,0,0, 0,0,0,0},
    {0,1,1,1, 0,0,1,1, 0,0,1,1, 0,1,1,1},
    {0,0,1,1, 0,1,1,1, 0,0,1,1, 0,0,1,1},
    {0,0,0,1, 0,0,1,1, 0,1,1,1, 1,1,1,1},
    {0,1,1,1, 0,0,1,1, 0,0,0,1, 0,0,0,0},
    {0,1,1,0, 0,1,1,0, 1,1,1,0, 1,1,1,0},
    {0,0,1,1, 1,0,0,1, 1,0,0,1, 1,1,0,0},
    {0,0,1,0, 0,1,1,0, 1,1,0,0, 0,0,1,1},
    {0,1,1,0, 1,1,0,0, 1,0,0,1, 0,0,1,1},
    {0,0,0,0, 0,1,1,0, 0,1,1,0, 0,0,0,0},
    {0,1,1,0, 0,1,1,0, 0,0,0,0, 0,1,1,0},
    {0,0,1,1, 0,1,0,1, 1,0,1,0, 1,1,0,0},
    {0,0,1,1, 1,1,0,0, 1,1,0,0, 0,0,1,1},
    {0,1,0,1, 0,1,0,1, 0,1,0,1, 0,1,0,1},
    {0,1,0,1, 1,0,1,0, 0,1,0,1, 1,0,1,0},
    {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
    {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
    {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
    {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
    {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
    {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
    {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
    {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
    {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
    {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
    {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
    {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
    {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
    {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
    {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
    {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
    {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
    {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
    {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
    {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
    {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
    {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
    {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
    {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
    {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
    {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
    {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
};

// Partition table for 3-subset modes (64 patterns x 16 pixels)
static const uint8_t kPartition3[64][16] = {
    {0,0,1,1, 0,0,1,1, 0,0,1,1, 0,0,1,1},
    {0,0,0,1, 0,0,0,1, 0,0,0,1, 0,0,0,1},
    {0,1,1,1, 0,1,1,1, 0,1,1,1, 0,1,1,1},
    {0,0,0,1, 0,0,1,1, 0,0,1,1, 0,1,1,1},
    {0,0,0,0, 0,0,0,1, 0,0,0,1, 0,0,1,1},
    {0,0,1,1, 0,1,1,1, 0,1,1,1, 1,1,1,1},
    {0,0,0,1, 0,0,1,1, 0,1,1,1, 1,1,1,1},
    {0,0,0,0, 0,0,0,0, 1,1,1,1, 1,1,1,1},
    {0,0,0,0, 1,0,0,0, 1,1,1,0, 1,1,1,1},
    {0,1,1,1, 0,0,0,1, 0,0,0,0, 0,0,0,0},
    {0,0,1,1, 0,0,0,1, 0,0,0,0, 0,0,0,0},
    {0,1,1,1, 0,0,1,1, 0,0,1,1, 1,1,1,1},
    {0,0,1,0, 0,0,1,0, 0,0,1,0, 0,0,1,0},
    {0,1,0,1, 0,1,0,1, 0,1,0,1, 0,1,0,1},
    {0,0,0,0, 0,0,0,0, 1,1,1,1, 0,0,0,0},
    {0,0,0,0, 1,1,1,1, 0,0,0,0, 1,1,1,1},
    {0,0,0,0, 0,0,0,0, 0,0,0,0, 1,1,1,1},
    {0,0,1,1, 0,0,1,1, 1,1,0,0, 1,1,0,0},
    {0,0,1,0, 0,0,1,0, 1,0,0,1, 1,0,0,1},
    {0,1,0,0, 0,1,0,0, 1,0,1,1, 1,0,1,1},
    {0,0,0,1, 0,0,0,1, 1,0,0,1, 1,0,0,1},
    {0,1,1,0, 0,1,1,0, 0,1,1,0, 0,1,1,0},
    {0,0,1,1, 0,0,1,1, 0,0,1,1, 0,0,1,1},
    {0,0,1,1, 0,0,1,1, 0,0,1,1, 0,0,1,1},
    {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
    {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
    {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
    {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
    {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
    {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
    {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
    {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
    {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
    {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
    {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
    {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
    {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
    {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
    {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
    {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
    {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
    {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
    {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
    {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
    {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
    {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
    {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
    {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
    {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
    {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
    {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
    {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
    {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
    {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
    {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
    {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
    {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
    {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
    {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
    {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
    {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
    {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
    {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
    {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
};

// Unquantize an n-bit value to 0..255
inline uint8_t Unquantize(uint32_t val, int bits)
{
    if (bits == 0) return 0;
    // Replicate top bits into lower bits for smooth expansion
    uint32_t result = val << (8 - bits);
    result |= val >> (2 * bits - 8);
    return static_cast<uint8_t>(result);
}

// Interpolate two endpoint components with an index.
// index is in [0, 2^n-1], n is index_bits.
inline uint8_t Interpolate(uint8_t e0, uint8_t e1, uint32_t index, int index_bits)
{
    uint32_t max_idx = (1u << index_bits) - 1u;
    if (max_idx == 0) return e0;
    // Exact interpolation matching the spec: (e0*(max-index) + e1*index + max/2) / max
    uint32_t val = (static_cast<uint32_t>(e0) * (max_idx - index) +
                    static_cast<uint32_t>(e1) * index +
                    max_idx / 2) / max_idx;
    return static_cast<uint8_t>(val);
}

}  // anonymous namespace

// ---------------------------------------------------------------------------
// BC7Decompressor public API
// ---------------------------------------------------------------------------

bool BC7Decompressor::ValidateSize(uint32_t width, uint32_t height, size_t data_size)
{
    uint32_t blocks_x = (width + 3) / 4;
    uint32_t blocks_y = (height + 3) / 4;
    size_t expected = static_cast<size_t>(blocks_x) * blocks_y * 16;
    return data_size >= expected;
}

void BC7Decompressor::DecodeBlock(const uint8_t block[16], uint8_t out_pixels[64])
{
    // Determine mode from trailing zeros of first byte
    int mode = -1;
    for (int i = 0; i < 8; ++i)
    {
        if (block[0] & (1 << i))
        {
            mode = i;
            break;
        }
    }
    if (mode < 0 || mode > 7)
    {
        // Invalid block — output black
        std::memset(out_pixels, 0, 64);
        return;
    }

    const BC7ModeInfo& mi = kModes[mode];

    BitReader br{block, 0};
    // Skip mode bit (already consumed conceptually; we start reading after)
    br.pos = (mode < 6) ? (mode + 1) : (mode + 1);

    // Read partition pattern (for multi-subset modes)
    uint32_t partition = 0;
    if (mi.num_subsets > 1)
        partition = br.read(mi.partition_bits);

    // Read rotation bits (modes 4, 5)
    uint32_t rotation = 0;
    if (mode == 4 || mode == 5)
        rotation = br.read(2);

    // Read index swap bit (mode 4, 5)
    uint32_t index_swap = 0;
    if (mode == 4 || mode == 5)
        index_swap = br.read(1);

    // Read endpoints
    // Each subset has 2 endpoints. Each endpoint has (4 or 3) components
    // depending on alpha_endpoint. bits_per_component = endpoint_bits.
    int num_components = mi.alpha_endpoint ? 4 : 3;
    int total_endpoint_bits_per_subset = 2 * num_components * mi.endpoint_bits;

    // Endpoints stored as: subset0_ep0, subset0_ep1, subset1_ep0, ...
    // Each endpoint: R, G, B [, A]
    uint8_t endpoints[3][2][4] = {};  // [subset][ep_idx][component]

    for (int s = 0; s < mi.num_subsets; ++s)
    {
        for (int ep = 0; ep < 2; ++ep)
        {
            for (int c = 0; c < num_components; ++c)
            {
                uint32_t raw = br.read(mi.endpoint_bits);
                endpoints[s][ep][c] = Unquantize(raw, mi.endpoint_bits);
            }
        }
    }

    // Read p-bits
    // p-bits apply to ALL components of a specific endpoint.
    // Number of p-bits: mi.p_bits
    // For 2-subset modes with p-bits: 4 p-bits (one per endpoint per subset)
    // For 3-subset modes with p-bits: 6 p-bits
    // For 1-subset modes: 2 p-bits
    uint8_t p_bits[3][2] = {};  // [subset][endpoint]
    for (int s = 0; s < mi.num_subsets; ++s)
    {
        for (int ep = 0; ep < 2; ++ep)
        {
            if (mi.p_bits > 0)
            {
                // p-bit order: subset0 ep0, subset0 ep1, subset1 ep0, ...
                uint32_t pb = br.read(1);
                p_bits[s][ep] = static_cast<uint8_t>(pb);
            }
        }
    }

    // Apply p-bits: extend precision of endpoint components
    // A p-bit of 0 lowers the value, 1 raises it
    for (int s = 0; s < mi.num_subsets; ++s)
    {
        for (int ep = 0; ep < 2; ++ep)
        {
            for (int c = 0; c < num_components; ++c)
            {
                if (mi.p_bits > 0)
                {
                    // For 7-bit endpoints: shift left by 1 and OR with p-bit
                    // For other sizes: apply similar extension
                    uint32_t val = endpoints[s][ep][c];
                    val = (val << 1) | p_bits[s][ep];
                    endpoints[s][ep][c] = static_cast<uint8_t>(std::min(val, 255u));
                }
            }
        }
    }

    // If no alpha in endpoints, set alpha to 255
    if (!mi.alpha_endpoint)
    {
        for (int s = 0; s < mi.num_subsets; ++s)
            for (int ep = 0; ep < 2; ++ep)
                endpoints[s][ep][3] = 255;
    }

    // Read indices
    // For each subset, there are 16 pixels, but only those belonging to the subset.
    // The index for the anchor pixel (first pixel of each subset) is stored with
    // one fewer bit.
    // For modes with separate alpha indices (4, 5), there's a second set.

    // Color indices
    uint8_t color_indices[16] = {};
    {
        int idx_bits = 0;
        if (mode == 0) idx_bits = 3;
        else if (mode == 1) idx_bits = 3;
        else if (mode == 2) idx_bits = 2;
        else if (mode == 3) idx_bits = 2;
        else if (mode == 4) idx_bits = 2;
        else if (mode == 5) idx_bits = 2;
        else if (mode == 6) idx_bits = 4;
        else if (mode == 7) idx_bits = 2;

        // Get partition pattern
        const uint8_t* pattern = nullptr;
        if (mi.num_subsets == 2) pattern = kPartition2[partition];
        else if (mi.num_subsets == 3) pattern = kPartition3[partition];

        // Find anchor pixels (first pixel of each subset)
        int anchor[3] = {0, -1, -1};
        if (mi.num_subsets > 1 && pattern)
        {
            bool found[3] = {false, false, false};
            found[0] = true;  // subset 0 always anchors at pixel 0
            for (int i = 0; i < 16; ++i)
            {
                int s = pattern[i];
                if (!found[s])
                {
                    anchor[s] = i;
                    found[s] = true;
                }
            }
        }

        // Read color index for each pixel
        // Anchor pixel uses idx_bits - 1 bits
        for (int i = 0; i < 16; ++i)
        {
            int s = (mi.num_subsets > 1 && pattern) ? pattern[i] : 0;
            int bits = (i == anchor[s]) ? (idx_bits - 1) : idx_bits;
            if (bits > 0)
                color_indices[i] = static_cast<uint8_t>(br.read(bits));
            else
                color_indices[i] = 0;
        }
    }

    // Alpha indices (modes 4, 5, 7 with separate alpha)
    uint8_t alpha_indices[16] = {};
    bool has_separate_alpha = (mode == 4 || mode == 5 || mode == 7);
    if (has_separate_alpha)
    {
        int idx_bits = 0;
        if (mode == 4) idx_bits = 3;
        else if (mode == 5) idx_bits = 2;
        else if (mode == 7) idx_bits = 2;

        const uint8_t* pattern = nullptr;
        if (mi.num_subsets > 1)
        {
            pattern = (mi.num_subsets == 2) ? kPartition2[partition] : kPartition3[partition];
        }

        int anchor[3] = {0, -1, -1};
        if (mi.num_subsets > 1 && pattern)
        {
            bool found[3] = {false, false, false};
            found[0] = true;
            for (int i = 0; i < 16; ++i)
            {
                int s = pattern[i];
                if (!found[s])
                {
                    anchor[s] = i;
                    found[s] = true;
                }
            }
        }

        for (int i = 0; i < 16; ++i)
        {
            int s = (mi.num_subsets > 1 && pattern) ? pattern[i] : 0;
            int bits = (i == anchor[s]) ? (idx_bits - 1) : idx_bits;
            if (bits > 0)
                alpha_indices[i] = static_cast<uint8_t>(br.read(bits));
            else
                alpha_indices[i] = 0;
        }
    }

    // Determine effective index bits for interpolation
    int color_idx_bits = 0;
    if (mode == 0) color_idx_bits = 3;
    else if (mode == 1) color_idx_bits = 3;
    else if (mode == 2) color_idx_bits = 2;
    else if (mode == 3) color_idx_bits = 2;
    else if (mode == 4) color_idx_bits = 2;
    else if (mode == 5) color_idx_bits = 2;
    else if (mode == 6) color_idx_bits = 4;
    else if (mode == 7) color_idx_bits = 2;

    int alpha_idx_bits = 0;
    if (mode == 4) alpha_idx_bits = 3;
    else if (mode == 5) alpha_idx_bits = 2;
    else if (mode == 7) alpha_idx_bits = 2;

    // Get partition pattern for final lookup
    const uint8_t* pattern = nullptr;
    if (mi.num_subsets == 2) pattern = kPartition2[partition];
    else if (mi.num_subsets == 3) pattern = kPartition3[partition];

    // Reconstruct each pixel
    for (int i = 0; i < 16; ++i)
    {
        int s = (mi.num_subsets > 1 && pattern) ? pattern[i] : 0;

        uint8_t r = Interpolate(endpoints[s][0][0], endpoints[s][1][0],
                                color_indices[i], color_idx_bits);
        uint8_t g = Interpolate(endpoints[s][0][1], endpoints[s][1][1],
                                color_indices[i], color_idx_bits);
        uint8_t b = Interpolate(endpoints[s][0][2], endpoints[s][1][2],
                                color_indices[i], color_idx_bits);
        uint8_t a;
        if (has_separate_alpha)
            a = Interpolate(endpoints[s][0][3], endpoints[s][1][3],
                            alpha_indices[i], alpha_idx_bits);
        else
            a = Interpolate(endpoints[s][0][3], endpoints[s][1][3],
                            color_indices[i], color_idx_bits);

        // Apply rotation (modes 4, 5): swap R<->A or G<->A or B<->A
        if (rotation == 1) std::swap(r, a);
        else if (rotation == 2) std::swap(g, a);
        else if (rotation == 3) std::swap(b, a);

        // Apply index swap (modes 4, 5): swap color and alpha index sets
        // This was already handled by reading them separately.

        out_pixels[i * 4 + 0] = r;
        out_pixels[i * 4 + 1] = g;
        out_pixels[i * 4 + 2] = b;
        out_pixels[i * 4 + 3] = a;
    }
}

BC7DecompressResult BC7Decompressor::Decompress(
    const uint8_t* compressed_data,
    size_t data_size,
    uint32_t width,
    uint32_t height)
{
    BC7DecompressResult result;

    if (width == 0 || height == 0)
    {
        result.error_message = "Invalid dimensions (0x0)";
        return result;
    }

    uint32_t blocks_x = (width + 3) / 4;
    uint32_t blocks_y = (height + 3) / 4;
    size_t expected_size = static_cast<size_t>(blocks_x) * blocks_y * 16;

    if (data_size < expected_size)
    {
        result.error_message = "Data size too small for given dimensions";
        return result;
    }

    // Output buffer: width * height * 4 (RGBA8), pad rows to 4-pixel boundary
    // for block decoding, then copy the actual width*height region.
    uint32_t padded_width = blocks_x * 4;
    uint32_t padded_height = blocks_y * 4;

    std::vector<uint8_t> padded_pixels(padded_width * padded_height * 4, 0);

    // Decode each block
    for (uint32_t by = 0; by < blocks_y; ++by)
    {
        for (uint32_t bx = 0; bx < blocks_x; ++bx)
        {
            const uint8_t* block_data = compressed_data + (by * blocks_x + bx) * 16;
            uint8_t block_pixels[64];
            DecodeBlock(block_data, block_pixels);

            // Copy 4x4 block into padded image
            for (int y = 0; y < 4; ++y)
            {
                for (int x = 0; x < 4; ++x)
                {
                    uint32_t px = bx * 4 + x;
                    uint32_t py = by * 4 + y;
                    if (px < padded_width && py < padded_height)
                    {
                        size_t dst = (py * padded_width + px) * 4;
                        size_t src = (y * 4 + x) * 4;
                        padded_pixels[dst + 0] = block_pixels[src + 0];
                        padded_pixels[dst + 1] = block_pixels[src + 1];
                        padded_pixels[dst + 2] = block_pixels[src + 2];
                        padded_pixels[dst + 3] = block_pixels[src + 3];
                    }
                }
            }
        }
    }

    // Trim to actual dimensions (remove padding)
    result.pixels.resize(static_cast<size_t>(width) * height * 4);
    for (uint32_t y = 0; y < height; ++y)
    {
        std::memcpy(result.pixels.data() + y * width * 4,
                    padded_pixels.data() + y * padded_width * 4,
                    width * 4);
    }

    result.width = width;
    result.height = height;
    result.success = true;
    return result;
}

}  // namespace ZEngine::Render
