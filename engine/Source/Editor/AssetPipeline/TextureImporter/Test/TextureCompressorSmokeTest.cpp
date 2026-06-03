// =====================================================================
// TextureCompressorSmokeTest
// ---------------------------------------------------------------------
// Standalone executable that encodes a known synthetic image through
// every TextureCompressor::Format and asserts structural properties of
// the result: mip count, per-mip offset monotonicity, and exact
// block-stream byte sizes derived from the format's block geometry.
// It does NOT decode/PSNR-check (that would re-pull a decoder); the
// contract under test is "the cook produces a correctly-shaped,
// correctly-sized, mip-offset-consistent payload".
//
// Build: only when -DZENGINE_BUILD_TEXTURE_COMPRESSOR_SMOKE_TEST=ON.
// Mirrors the schema-evolution smoke test's opt-in pattern.
//
// Exit codes: 0 all passed, 1 a scenario failed, 77 environment error.
// =====================================================================

#include "TextureCompressor.h"

#include <cstdint>
#include <cstdio>
#include <vector>

using TextureCompressor::Format;

namespace
{
    int g_failures = 0;

    void reportFail(const char* scenario, const char* detail)
    {
        ++g_failures;
        std::fprintf(stderr, "[FAIL] %s: %s\n", scenario, detail);
    }
    void reportOK(const char* scenario) { std::fprintf(stdout, "[ OK ] %s\n", scenario); }

    uint32_t expectedFullMipCount(uint32_t w, uint32_t h)
    {
        uint32_t m = (w > h) ? w : h;
        uint32_t c = 1;
        while (m > 1) { m >>= 1; ++c; }
        return c;
    }

    uint32_t mipDim(uint32_t base, uint32_t level)
    {
        uint32_t v = base >> level;
        return v == 0 ? 1u : v;
    }

    // Expected payload byte size for a single mip of the given format/dims.
    size_t expectedMipBytes(Format f, uint32_t w, uint32_t h)
    {
        const uint32_t bx = (w + 3) / 4;
        const uint32_t by = (h + 3) / 4;
        switch (f)
        {
            case Format::RGBA8:    return static_cast<size_t>(w) * h * 4;
            case Format::BC1:      return static_cast<size_t>(bx) * by * 8;
            case Format::BC3:
            case Format::BC7:
            case Format::ASTC_4x4: return static_cast<size_t>(bx) * by * 16;
        }
        return 0;
    }

    // Build a deterministic w*h RGBA8 gradient with a non-trivial alpha ramp.
    std::vector<uint8_t> makeImage(uint32_t w, uint32_t h)
    {
        std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
        for (uint32_t y = 0; y < h; ++y)
        {
            for (uint32_t x = 0; x < w; ++x)
            {
                uint8_t* p = px.data() + (static_cast<size_t>(y) * w + x) * 4;
                p[0] = static_cast<uint8_t>((x * 255) / (w > 1 ? w - 1 : 1));
                p[1] = static_cast<uint8_t>((y * 255) / (h > 1 ? h - 1 : 1));
                p[2] = static_cast<uint8_t>((x ^ y) & 0xFF);
                p[3] = static_cast<uint8_t>(((x + y) * 255) / ((w + h) > 1 ? (w + h - 2) : 1));
            }
        }
        return px;
    }

    void runFormat(const char* name, Format fmt, uint32_t w, uint32_t h, bool srgb, bool mips)
    {
        const std::vector<uint8_t> src = makeImage(w, h);

        TextureCompressor::Options opts;
        opts.format = fmt;
        opts.generate_mips = mips;
        opts.srgb = srgb;

        TextureCompressor::CompressedTexture out;
        if (!TextureCompressor::Compress(src.data(), w, h, opts, out))
        {
            reportFail(name, "Compress() returned false");
            return;
        }

        const uint32_t expectMips = mips ? expectedFullMipCount(w, h) : 1u;
        if (out.mip_offsets.size() != expectMips)
        {
            char d[128];
            std::snprintf(d, sizeof(d), "mip count %zu != expected %u", out.mip_offsets.size(), expectMips);
            reportFail(name, d);
            return;
        }

        // Offsets monotonic, first is 0, and each mip's slice size matches geometry.
        size_t cursor = 0;
        bool sizes_ok = (out.mip_offsets[0] == 0);
        for (uint32_t level = 0; level < expectMips && sizes_ok; ++level)
        {
            const uint32_t mw = mipDim(w, level);
            const uint32_t mh = mipDim(h, level);
            const size_t mipBytes = expectedMipBytes(fmt, mw, mh);
            if (out.mip_offsets[level] != cursor) { sizes_ok = false; break; }
            cursor += mipBytes;
        }
        const bool total_ok = (cursor == out.pixels.size());

        if (!sizes_ok || !total_ok)
        {
            char d[160];
            std::snprintf(d, sizeof(d), "size/offset mismatch: computed total %zu vs blob %zu (sizes_ok=%d)",
                          cursor, out.pixels.size(), sizes_ok ? 1 : 0);
            reportFail(name, d);
            return;
        }

        const uint32_t expectFmt = TextureCompressor::ToRhiFormatOrdinal(fmt, srgb);
        if (out.rhi_format != expectFmt)
        {
            char d[128];
            std::snprintf(d, sizeof(d), "rhi_format %u != expected %u", out.rhi_format, expectFmt);
            reportFail(name, d);
            return;
        }

        std::fprintf(stdout, "       %s: %ux%u -> %u mips, %zu bytes (rhi=%u)\n",
                     TextureCompressor::ToString(fmt), w, h, expectMips, out.pixels.size(), out.rhi_format);
        reportOK(name);
    }
}  // namespace

int main()
{
    std::fprintf(stderr,
                 "ZEngine TextureCompressor smoke test\n"
                 "====================================\n");

    // 64x64 color textures (sRGB), mips on.
    runFormat("RGBA8 64x64 srgb mips", Format::RGBA8, 64, 64, true, true);
    runFormat("BC1   64x64 srgb mips", Format::BC1, 64, 64, true, true);
    runFormat("BC3   64x64 srgb mips", Format::BC3, 64, 64, true, true);
    runFormat("BC7   64x64 srgb mips", Format::BC7, 64, 64, true, true);
    runFormat("ASTC  64x64 srgb mips", Format::ASTC_4x4, 64, 64, true, true);

    // Non-power-of-two with partial edge blocks, linear (data texture), mips on.
    runFormat("BC7   40x24 linear mips", Format::BC7, 40, 24, false, true);
    runFormat("ASTC  40x24 linear mips", Format::ASTC_4x4, 40, 24, false, true);

    // Single-mip path.
    runFormat("BC7   16x16 no-mips", Format::BC7, 16, 16, false, false);

    if (g_failures > 0)
    {
        std::fprintf(stderr, "\n%d scenario(s) FAILED.\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "\nAll scenarios passed.\n");
    return 0;
}
