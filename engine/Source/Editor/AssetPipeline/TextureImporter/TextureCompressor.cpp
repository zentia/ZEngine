// =============================================================================
// TextureCompressor.cpp  (editor-only)
// =============================================================================

#include "TextureCompressor.h"

#include "Runtime/Function/Render/RenderType.h"  // RHIFormat ordinals

#include "bc7enc.h"
#include "rgbcx.h"

#include "astcenc.h"

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image_resize2.h"

#include <algorithm>
#include <cstring>
#include <mutex>

namespace TextureCompressor
{
namespace
{
    // bc7enc / rgbcx carry process-global lookup tables that must be
    // initialised exactly once before any encode call.
    void EnsureBcEncodersInit()
    {
        static std::once_flag flag;
        std::call_once(flag, []() {
            rgbcx::init(rgbcx::bc1_approx_mode::cBC1Ideal);
            bc7enc_compress_block_init();
        });
    }

    uint32_t MipDim(uint32_t base, uint32_t level)
    {
        const uint32_t v = base >> level;
        return v == 0 ? 1u : v;
    }

    uint32_t FullMipCount(uint32_t width, uint32_t height)
    {
        uint32_t maxDim = std::max(width, height);
        uint32_t count = 1;
        while (maxDim > 1)
        {
            maxDim >>= 1;
            ++count;
        }
        return count;
    }

    // Bytes per encoded 4x4 block.
    uint32_t BlockBytes(Format f)
    {
        switch (f)
        {
            case Format::BC1: return 8;
            case Format::BC3:
            case Format::BC7:
            case Format::ASTC_4x4: return 16;
            case Format::RGBA8: default: return 0;  // not block-based
        }
    }

    // Gather a 4x4 RGBA block from a row-major RGBA8 image, clamping reads at
    // the image edge so partial edge blocks replicate the border pixel.
    void GatherBlock(const uint8_t* img, uint32_t w, uint32_t h,
                     uint32_t bx, uint32_t by, uint8_t out_block[64])
    {
        for (uint32_t ty = 0; ty < 4; ++ty)
        {
            uint32_t sy = std::min(by * 4 + ty, h - 1);
            for (uint32_t tx = 0; tx < 4; ++tx)
            {
                uint32_t sx = std::min(bx * 4 + tx, w - 1);
                const uint8_t* src = img + (static_cast<size_t>(sy) * w + sx) * 4;
                uint8_t* dst = out_block + (ty * 4 + tx) * 4;
                dst[0] = src[0];
                dst[1] = src[1];
                dst[2] = src[2];
                dst[3] = src[3];
            }
        }
    }

    // Encode one RGBA8 mip into a BCn block stream appended to `out`.
    void EncodeBcMip(const uint8_t* img, uint32_t w, uint32_t h, Format f,
                     std::vector<uint8_t>& out)
    {
        EnsureBcEncodersInit();

        bc7enc_compress_block_params bc7params;
        bc7enc_compress_block_params_init(&bc7params);

        const uint32_t blocksX = (w + 3) / 4;
        const uint32_t blocksY = (h + 3) / 4;
        const uint32_t blkBytes = BlockBytes(f);

        const size_t base = out.size();
        out.resize(base + static_cast<size_t>(blocksX) * blocksY * blkBytes);
        uint8_t* dst = out.data() + base;

        uint8_t block[64];
        for (uint32_t by = 0; by < blocksY; ++by)
        {
            for (uint32_t bx = 0; bx < blocksX; ++bx)
            {
                GatherBlock(img, w, h, bx, by, block);
                switch (f)
                {
                    case Format::BC1:
                        rgbcx::encode_bc1(/*level*/ 10, dst, block,
                                          /*allow_3color*/ true,
                                          /*use_transparent_texels_for_black*/ false);
                        break;
                    case Format::BC3:
                        rgbcx::encode_bc3(/*level*/ 10, dst, block);
                        break;
                    case Format::BC7:
                        bc7enc_compress_block(dst, block, &bc7params);
                        break;
                    default: break;
                }
                dst += blkBytes;
            }
        }
    }

    // Encode all mips of one image into an ASTC 4x4 LDR block stream. astcenc
    // handles block tiling + edge padding internally, so we feed each mip
    // whole. Returns false on encoder error.
    bool EncodeAstcAllMips(const std::vector<std::vector<uint8_t>>& mips,
                           uint32_t width, uint32_t height, bool srgb,
                           std::vector<uint8_t>& out_blob,
                           std::vector<uint32_t>& out_offsets)
    {
        astcenc_config config {};
        const astcenc_profile profile = srgb ? ASTCENC_PRF_LDR_SRGB : ASTCENC_PRF_LDR;
        if (astcenc_config_init(profile, 4, 4, 1, ASTCENC_PRE_MEDIUM, 0, &config) != ASTCENC_SUCCESS)
        {
            return false;
        }

        astcenc_context* ctx = nullptr;
        if (astcenc_context_alloc(&config, 1, &ctx, nullptr) != ASTCENC_SUCCESS)
        {
            return false;
        }

        astcenc_swizzle swz {ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};

        bool ok = true;
        for (uint32_t level = 0; level < mips.size(); ++level)
        {
            const uint32_t w = MipDim(width, level);
            const uint32_t h = MipDim(height, level);
            const uint32_t blocksX = (w + 3) / 4;
            const uint32_t blocksY = (h + 3) / 4;
            const size_t outLen = static_cast<size_t>(blocksX) * blocksY * 16;

            out_offsets.push_back(static_cast<uint32_t>(out_blob.size()));
            const size_t base = out_blob.size();
            out_blob.resize(base + outLen);

            // astcenc_image wants a non-const slice pointer array.
            void* slice = const_cast<uint8_t*>(mips[level].data());
            astcenc_image img {};
            img.dim_x = w;
            img.dim_y = h;
            img.dim_z = 1;
            img.data_type = ASTCENC_TYPE_U8;
            img.data = &slice;

            if (astcenc_compress_image(ctx, &img, &swz, out_blob.data() + base, outLen, 0) != ASTCENC_SUCCESS)
            {
                ok = false;
                break;
            }
            astcenc_compress_reset(ctx);
        }

        astcenc_context_free(ctx);
        return ok;
    }
}  // namespace

uint32_t ToRhiFormatOrdinal(Format f, bool srgb)
{
    switch (f)
    {
        case Format::RGBA8:    return srgb ? RHI_FORMAT_R8G8B8A8_SRGB : RHI_FORMAT_R8G8B8A8_UNORM;
        case Format::BC1:      return srgb ? RHI_FORMAT_BC1_RGB_SRGB_BLOCK : RHI_FORMAT_BC1_RGB_UNORM_BLOCK;
        case Format::BC3:      return srgb ? RHI_FORMAT_BC3_SRGB_BLOCK : RHI_FORMAT_BC3_UNORM_BLOCK;
        case Format::BC7:      return srgb ? RHI_FORMAT_BC7_SRGB_BLOCK : RHI_FORMAT_BC7_UNORM_BLOCK;
        case Format::ASTC_4x4: return srgb ? RHI_FORMAT_ASTC_4x4_SRGB_BLOCK : RHI_FORMAT_ASTC_4x4_UNORM_BLOCK;
        default:               return RHI_FORMAT_R8G8B8A8_UNORM;
    }
}

Format FromRhiFormatOrdinal(uint32_t o)
{
    switch (o)
    {
        case RHI_FORMAT_BC1_RGB_UNORM_BLOCK:
        case RHI_FORMAT_BC1_RGB_SRGB_BLOCK:
        case RHI_FORMAT_BC1_RGBA_UNORM_BLOCK:
        case RHI_FORMAT_BC1_RGBA_SRGB_BLOCK:   return Format::BC1;
        case RHI_FORMAT_BC3_UNORM_BLOCK:
        case RHI_FORMAT_BC3_SRGB_BLOCK:        return Format::BC3;
        case RHI_FORMAT_BC7_UNORM_BLOCK:
        case RHI_FORMAT_BC7_SRGB_BLOCK:        return Format::BC7;
        case RHI_FORMAT_ASTC_4x4_UNORM_BLOCK:
        case RHI_FORMAT_ASTC_4x4_SRGB_BLOCK:   return Format::ASTC_4x4;
        default:                               return Format::RGBA8;
    }
}

const char* ToString(Format f)
{
    switch (f)
    {
        case Format::RGBA8:    return "RGBA8";
        case Format::BC1:      return "BC1";
        case Format::BC3:      return "BC3";
        case Format::BC7:      return "BC7";
        case Format::ASTC_4x4: return "ASTC_4x4";
        default:               return "?";
    }
}

uint32_t EncoderVersion() { return 1; }

bool Compress(const uint8_t* src_rgba8, uint32_t width, uint32_t height,
              const Options& opts, CompressedTexture& out)
{
    if (src_rgba8 == nullptr || width == 0 || height == 0)
    {
        return false;
    }

    out = CompressedTexture {};
    out.width = width;
    out.height = height;
    out.format = opts.format;
    out.rhi_format = ToRhiFormatOrdinal(opts.format, opts.srgb);

    const uint32_t mipCount = opts.generate_mips ? FullMipCount(width, height) : 1u;

    // Build the RGBA8 mip chain. Each level is resized from mip0 (the source)
    // to avoid error accumulation; sRGB-aware filtering for color textures.
    std::vector<std::vector<uint8_t>> mips(mipCount);
    mips[0].assign(src_rgba8, src_rgba8 + static_cast<size_t>(width) * height * 4);
    for (uint32_t level = 1; level < mipCount; ++level)
    {
        const uint32_t w = MipDim(width, level);
        const uint32_t h = MipDim(height, level);
        mips[level].resize(static_cast<size_t>(w) * h * 4);
        unsigned char* res = opts.srgb
            ? stbir_resize_uint8_srgb(src_rgba8, static_cast<int>(width), static_cast<int>(height), 0,
                                      mips[level].data(), static_cast<int>(w), static_cast<int>(h), 0, STBIR_RGBA)
            : stbir_resize_uint8_linear(src_rgba8, static_cast<int>(width), static_cast<int>(height), 0,
                                        mips[level].data(), static_cast<int>(w), static_cast<int>(h), 0, STBIR_RGBA);
        if (res == nullptr)
        {
            return false;
        }
    }

    if (opts.format == Format::ASTC_4x4)
    {
        return EncodeAstcAllMips(mips, width, height, opts.srgb, out.pixels, out.mip_offsets);
    }

    for (uint32_t level = 0; level < mipCount; ++level)
    {
        const uint32_t w = MipDim(width, level);
        const uint32_t h = MipDim(height, level);
        out.mip_offsets.push_back(static_cast<uint32_t>(out.pixels.size()));

        if (opts.format == Format::RGBA8)
        {
            out.pixels.insert(out.pixels.end(), mips[level].begin(), mips[level].end());
        }
        else
        {
            EncodeBcMip(mips[level].data(), w, h, opts.format, out.pixels);
        }
    }
    return true;
}
}  // namespace TextureCompressor
