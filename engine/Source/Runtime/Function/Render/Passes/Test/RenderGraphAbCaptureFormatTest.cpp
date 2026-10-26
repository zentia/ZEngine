// =====================================================================================
// RenderGraphAbCaptureFormatTest -- headless producer for scripts/rg_ab_selftest.sh.
// -------------------------------------------------------------------------------------
// Writes synthetic A/B captures with the ENGINE's encoder (RenderGraphAbCaptureFormat.h,
// the same code DX12RenderGraphAbCapture runs), from source buffers laid out with the
// real DX12 256-byte readback row pitch and garbage in the padding. The self-test then
// runs scripts/rg_ab_compare.py over them, so writer and reader are checked against each
// other byte for byte. Built with emcc -s NODERAWFS=1 so node writes real files.
//
// Scenarios (one directory each under argv[1]):
//   identical/      every pair bit-identical, both stages graph_used     -> PASS (0)
//   one_pixel/      rp1.graph differs at exactly (123, 45)               -> FAIL (1)
//   tiny_diff/      rp1.graph off by 1/1024 everywhere in a 4x2 block    -> FAIL at tol 0,
//                                                                          PASS at tol 0.01
//   fell_back/      identical images but rp2 graph_used = false          -> INCONCLUSIVE (2)
//   missing/        rp2.graph.backbuffer has a write error               -> INCONCLUSIVE (2)
//   pattern/        a single known pattern for exact pixel-value checks
// =====================================================================================

#include "Runtime/Function/Render/Passes/RenderGraphAbCaptureFormat.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace RenderGraphAbCapture;

namespace
{
    constexpr uint32_t kWidth = 300;  // 300*4 = 1200 -> pitch 1280; 300*8 = 2400 -> pitch 2560
    constexpr uint32_t kHeight = 70;

    uint16_t FloatToHalf(float value)
    {
        uint32_t bits;
        std::memcpy(&bits, &value, 4);
        const uint32_t sign = (bits >> 16) & 0x8000u;
        int32_t exponent = static_cast<int32_t>((bits >> 23) & 0xFFu) - 127 + 15;
        uint32_t mantissa = bits & 0x7FFFFFu;
        if (value == 0.0f)
        {
            return static_cast<uint16_t>(sign);
        }
        if (exponent <= 0 || exponent >= 31)
        {
            std::fprintf(stderr, "FloatToHalf: %g out of the test's range\n", value);
            return static_cast<uint16_t>(sign | 0x7C00u);
        }
        // round to nearest even on the 13 dropped bits
        uint32_t half = sign | (static_cast<uint32_t>(exponent) << 10) | (mantissa >> 13);
        const uint32_t dropped = mantissa & 0x1FFFu;
        if (dropped > 0x1000u || (dropped == 0x1000u && (half & 1u)))
        {
            ++half;
        }
        return static_cast<uint16_t>(half);
    }

    /// A GPU-readback-shaped buffer: rows at the DX12 pitch, padding filled with 0xCD.
    struct Readback
    {
        uint32_t bpp;
        uint64_t pitch;
        std::vector<uint8_t> bytes;

        explicit Readback(uint32_t bytes_per_pixel)
            : bpp(bytes_per_pixel), pitch(Dx12ReadbackRowPitch(kWidth, bytes_per_pixel)),
              bytes(static_cast<size_t>(pitch * kHeight), 0xCD)
        {
        }
        uint8_t* Pixel(uint32_t x, uint32_t y) { return bytes.data() + pitch * y + static_cast<uint64_t>(x) * bpp; }
    };

    Readback MakeHdr()
    {
        Readback rb(8);
        for (uint32_t y = 0; y < kHeight; ++y)
        {
            for (uint32_t x = 0; x < kWidth; ++x)
            {
                const uint16_t rgba[4] = {FloatToHalf(static_cast<float>(x)), FloatToHalf(static_cast<float>(y) * 0.5f),
                                          FloatToHalf(0.25f), FloatToHalf(1.0f)};
                std::memcpy(rb.Pixel(x, y), rgba, 8);
            }
        }
        return rb;
    }

    Readback MakeLdr()
    {
        Readback rb(4);
        for (uint32_t y = 0; y < kHeight; ++y)
        {
            for (uint32_t x = 0; x < kWidth; ++x)
            {
                const uint8_t rgba[4] = {static_cast<uint8_t>(x & 0xFF), static_cast<uint8_t>(y & 0xFF), 7, 255};
                std::memcpy(rb.Pixel(x, y), rgba, 4);
            }
        }
        return rb;
    }

    bool Write(const std::string& dir, const char* name, const char* format, const Readback& rb,
               std::vector<ManifestImage>& images)
    {
        ManifestImage info;
        info.name = name;
        info.file = std::string(name) + ".zcap";
        info.width = kWidth;
        info.height = kHeight;
        info.format = format;
        std::string error;
        info.written = WriteFileBytes(dir + "/" + info.file,
                                      EncodeZcap(kWidth, kHeight, rb.bpp, format, rb.bytes.data(), rb.pitch), &error);
        info.error = error;
        images.push_back(info);
        return info.written;
    }

    bool Scenario(const std::string& root, const char* name, const Readback& rp1_graph, bool rp2_graph_used,
                  bool rp2_backbuffer_missing)
    {
        const std::string dir = root + "/" + name;
        std::filesystem::create_directories(dir);
        const Readback hdr = MakeHdr();
        const Readback ldr = MakeLdr();

        std::vector<ManifestImage> images;
        bool ok = true;
        ok &= Write(dir, "rp1.handwritten.backup_odd", "R16G16B16A16_SFLOAT", hdr, images);
        ok &= Write(dir, "rp1.graph.backup_odd", "R16G16B16A16_SFLOAT", rp1_graph, images);
        ok &= Write(dir, "rp2.handwritten.backup_even", "R16G16B16A16_SFLOAT", hdr, images);
        ok &= Write(dir, "rp2.graph.backup_even", "R16G16B16A16_SFLOAT", hdr, images);
        ok &= Write(dir, "rp2.handwritten.backbuffer", "R8G8B8A8_UNORM", ldr, images);
        if (rp2_backbuffer_missing)
        {
            ManifestImage missing;
            missing.name = "rp2.graph.backbuffer";
            missing.file = "rp2.graph.backbuffer.zcap";
            missing.width = kWidth;
            missing.height = kHeight;
            missing.format = "R8G8B8A8_UNORM";
            missing.error = "map failed";
            images.push_back(missing);
        }
        else
        {
            ok &= Write(dir, "rp2.graph.backbuffer", "R8G8B8A8_UNORM", ldr, images);
        }

        std::vector<ManifestStage> stages(2);
        stages[0].stage = "rp1";
        stages[0].attempted = true;
        stages[0].graph_used = true;
        stages[1].stage = "rp2";
        stages[1].attempted = true;
        stages[1].graph_used = rp2_graph_used;
        const std::string json = EncodeManifest(120, stages, images, {"synthetic capture (self-test)", "quote \" and \\ escaped"});
        std::string error;
        ok &= WriteFileBytes(dir + "/manifest.json", std::vector<uint8_t>(json.begin(), json.end()), &error);
        return ok;
    }
}  // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: %s <out_dir>\n", argv[0]);
        return 2;
    }
    const std::string root = argv[1];

    // Pitch facts the reader relies on.
    if (Dx12ReadbackRowPitch(kWidth, 4) != 1280 || Dx12ReadbackRowPitch(kWidth, 8) != 2560 ||
        Dx12ReadbackRowPitch(64, 4) != 256 || kZcapHeaderBytes != 52)
    {
        std::fprintf(stderr, "pitch / header constants wrong\n");
        return 1;
    }

    bool ok = true;
    const Readback hdr = MakeHdr();
    ok &= Scenario(root, "identical", hdr, true, false);

    Readback one = MakeHdr();
    const uint16_t changed[4] = {FloatToHalf(999.0f), FloatToHalf(1.0f), FloatToHalf(0.25f), FloatToHalf(1.0f)};
    std::memcpy(one.Pixel(123, 45), changed, 8);
    ok &= Scenario(root, "one_pixel", one, true, false);

    Readback tiny = MakeHdr();
    for (uint32_t y = 10; y < 12; ++y)
    {
        for (uint32_t x = 20; x < 24; ++x)
        {
            uint16_t px[4];
            std::memcpy(px, tiny.Pixel(x, y), 8);
            px[2] = FloatToHalf(0.25f + 1.0f / 1024.0f);
            std::memcpy(tiny.Pixel(x, y), px, 8);
        }
    }
    ok &= Scenario(root, "tiny_diff", tiny, true, false);

    ok &= Scenario(root, "fell_back", hdr, false, false);
    ok &= Scenario(root, "missing", hdr, true, true);
    ok &= Scenario(root, "pattern", hdr, true, false);

    std::printf("%s\n", ok ? "captures written" : "WRITE FAILED");
    return ok ? 0 : 1;
}
