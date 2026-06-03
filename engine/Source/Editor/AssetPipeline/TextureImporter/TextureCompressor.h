#pragma once

// =============================================================================
// TextureCompressor (editor-only)
// -----------------------------------------------------------------------------
// Turns a tightly-packed RGBA8 source image into a block-compressed, mipped
// payload matching Texture2D's on-disk layout (concatenated mip blob + per-mip
// byte offsets, see Texture2D.h). This is the encode half of the texture cook
// pipeline; it is HOST-side and platform-agnostic so the Windows editor can
// cross-encode mobile (ASTC) variants.
//
//   RGBA8 source --> sRGB-aware box mip chain (stb_image_resize2)
//                --> per-mip block encode (bc7enc / rgbcx / astc-encoder)
//                --> { concatenated blob, mip offsets, RHIFormat ordinal }
//
// Backends:
//   BC1 / BC3 / BC7 : richgel999 bc7enc_rdo  (desktop / WebGL)
//   ASTC 4x4 LDR    : ARM astc-encoder       (mobile)
//
// Keep this header free of third-party includes -- the encoder headers and
// stb_image_resize2 are pulled in by the .cpp only.
// =============================================================================

#include <cstdint>
#include <vector>

namespace TextureCompressor
{
    // Target GPU format family the source is encoded to.
    enum class Format
    {
        RGBA8,     // uncompressed passthrough (mips still generated)
        BC1,       // RGB(+1bit A), 8 bytes / 4x4 block  (desktop opaque)
        BC3,       // RGBA, 16 bytes / 4x4 block          (desktop alpha)
        BC7,       // RGBA, 16 bytes / 4x4 block, HQ      (desktop default)
        ASTC_4x4,  // RGBA, 16 bytes / 4x4 block          (mobile)
    };

    struct Options
    {
        Format format {Format::BC7};
        bool generate_mips {true};
        // sRGB-aware downsample for color textures (linearise -> filter ->
        // re-encode). Set false for data textures (normal / mask / metallic).
        bool srgb {false};
    };

    struct CompressedTexture
    {
        uint32_t width {0};
        uint32_t height {0};
        Format format {Format::RGBA8};
        // RHIFormat ordinal (Texture2D::m_Format) for the chosen format+srgb.
        uint32_t rhi_format {0};
        std::vector<uint8_t> pixels;        // mip0 first, all mips concatenated
        std::vector<uint32_t> mip_offsets;  // byte offset of each mip in `pixels`
    };

    // Encode `src_rgba8` (width*height*4 bytes, row-major, R first) per `opts`.
    // Returns false on invalid input or encoder failure.
    bool Compress(const uint8_t* src_rgba8, uint32_t width, uint32_t height,
                  const Options& opts, CompressedTexture& out);

    // Format <-> RHIFormat ordinal (RenderType.h). `srgb` selects the *_SRGB
    // vs *_UNORM block variant for the compressed families and RGBA8.
    uint32_t ToRhiFormatOrdinal(Format f, bool srgb);
    Format FromRhiFormatOrdinal(uint32_t rhi_ordinal);

    const char* ToString(Format f);

    // Monotonic version stamp of this encoder. Bump when the encode output for
    // a fixed input could change (algorithm / quality preset / backend swap).
    // Folded into the DDC cache key so cooked variants invalidate cleanly.
    uint32_t EncoderVersion();
}  // namespace TextureCompressor
