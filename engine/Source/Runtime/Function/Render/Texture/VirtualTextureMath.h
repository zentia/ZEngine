#pragma once

// =============================================================================
// CPU-side math of the virtual-texture sampling contract (v2). Header-only, no RHI, no engine types.
//
// Two audiences:
//   * production  -- VirtualTextureResource builds its mip chain and page geometry with these functions;
//   * verification -- the A/B tests and the in-editor pixel self-tests use the SAME header to compute what a
//                     shader MUST return (VTRef::SampleWithFallback), then compare against GPU output.
//
// The GLSL (engine/shader/include/vt_common.h) and HLSL (engine/shader/hlsl/common/vt_common.hlsli) copies of
// the contract mirror this file. Change a rule here and you must change both shader copies, and vice versa.
//
//   mip chain      mip m is max(1, dim >> m) per axis, built by a 2x2 box filter with clamp-to-edge and
//                  round-to-nearest (an odd source edge drops its last row/column -- the same rule everywhere).
//   sampled mips   mips 0 .. first mip whose page grid is 1x1 (inclusive). Beyond that every mip would be a
//                  separate nearly-empty page; sampling clamps to the last one instead.
//   page table     ONE 2D texture ("atlas"): width = pages_x(mip 0), height = sum over sampled mips of pages_y(m);
//                  mip m's page grid sits at row offset sum_{i<m} pages_y(i). (A hardware mip chain would not
//                  work: ceil((W>>m)/ps) is not the same as max(1, ceil(W/ps)>>m).)
//   mip selection  lod = 0.5 * log2(max(|d(uv*size)/dx|^2, |d(uv*size)/dy|^2)), mip = clamp(round(lod), 0, n-1)
//                  (nearest mip + bilinear inside it; no trilinear blend).
//   fallback       if the page under the sample is not resident at the chosen mip, walk to coarser mips until a
//                  resident one is found; none -> the caller's fallback colour.
//   bilinear       texel centres at +0.5, clamp-to-edge at the mip image border; the page's `border`-texel gutter
//                  (copied from the neighbouring texels of the SAME mip, clamped at the image edge) is what keeps
//                  the 2x2 footprint inside the slot.
// =============================================================================

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <vector>

namespace VTRef
{
    struct Image
    {
        uint32_t w {0};
        uint32_t h {0};
        std::vector<uint8_t> rgba;  // w * h * 4
    };

    inline uint32_t MipDim(uint32_t base, uint32_t mip)
    {
        return std::max(1u, base >> std::min(mip, 31u));
    }

    inline uint32_t PagesFor(uint32_t texels, uint32_t page_size)
    {
        return (texels + page_size - 1u) / page_size;
    }

    /// Page grid of `mip` (>= 1x1 for any mip that has texels).
    inline void MipPages(uint32_t vw, uint32_t vh, uint32_t page_size, uint32_t mip, uint32_t& px, uint32_t& py)
    {
        px = PagesFor(MipDim(vw, mip), page_size);
        py = PagesFor(MipDim(vh, mip), page_size);
    }

    /// Mips 0 .. first mip with a 1x1 page grid (inclusive).
    inline uint32_t SampledMipCount(uint32_t vw, uint32_t vh, uint32_t page_size)
    {
        uint32_t mip = 0;
        for (;; ++mip)
        {
            uint32_t px, py;
            MipPages(vw, vh, page_size, mip, px, py);
            if ((px <= 1 && py <= 1) || (MipDim(vw, mip) == 1 && MipDim(vh, mip) == 1))
            {
                return mip + 1;
            }
        }
    }

    /// First row of `mip`'s page grid inside the page-table atlas.
    inline uint32_t MipRowOffset(uint32_t vw, uint32_t vh, uint32_t page_size, uint32_t mip)
    {
        uint32_t offset = 0;
        for (uint32_t i = 0; i < mip; ++i)
        {
            uint32_t px, py;
            MipPages(vw, vh, page_size, i, px, py);
            offset += py;
        }
        return offset;
    }

    /// Total atlas height for `mip_count` sampled mips.
    inline uint32_t AtlasHeight(uint32_t vw, uint32_t vh, uint32_t page_size, uint32_t mip_count)
    {
        return MipRowOffset(vw, vh, page_size, mip_count);
    }

    inline Image Downsample(const Image& src)
    {
        Image dst;
        dst.w = std::max(1u, src.w >> 1);
        dst.h = std::max(1u, src.h >> 1);
        dst.rgba.resize(static_cast<size_t>(dst.w) * dst.h * 4u);
        for (uint32_t y = 0; y < dst.h; ++y)
        {
            for (uint32_t x = 0; x < dst.w; ++x)
            {
                const uint32_t x0 = std::min(2u * x, src.w - 1u);
                const uint32_t x1 = std::min(2u * x + 1u, src.w - 1u);
                const uint32_t y0 = std::min(2u * y, src.h - 1u);
                const uint32_t y1 = std::min(2u * y + 1u, src.h - 1u);
                for (uint32_t c = 0; c < 4; ++c)
                {
                    const uint32_t sum = src.rgba[(static_cast<size_t>(y0) * src.w + x0) * 4u + c] +
                                         src.rgba[(static_cast<size_t>(y0) * src.w + x1) * 4u + c] +
                                         src.rgba[(static_cast<size_t>(y1) * src.w + x0) * 4u + c] +
                                         src.rgba[(static_cast<size_t>(y1) * src.w + x1) * 4u + c];
                    dst.rgba[(static_cast<size_t>(y) * dst.w + x) * 4u + c] = static_cast<uint8_t>((sum + 2u) / 4u);
                }
            }
        }
        return dst;
    }

    /// chain[0] is a copy of the base; chain[i] = Downsample(chain[i-1]); `mip_count` entries.
    inline std::vector<Image> BuildMipChain(const uint8_t* rgba, uint32_t w, uint32_t h, uint32_t mip_count)
    {
        std::vector<Image> chain;
        chain.reserve(mip_count);
        Image base;
        base.w = w;
        base.h = h;
        base.rgba.assign(rgba, rgba + static_cast<size_t>(w) * h * 4u);
        chain.push_back(std::move(base));
        for (uint32_t m = 1; m < mip_count; ++m)
        {
            chain.push_back(Downsample(chain.back()));
        }
        return chain;
    }

    /// lod from the screen-space derivatives of uv (uv units per pixel).
    inline float Lod(uint32_t vw, uint32_t vh, float ddx_u, float ddx_v, float ddy_u, float ddy_v)
    {
        const float dxu = ddx_u * static_cast<float>(vw), dxv = ddx_v * static_cast<float>(vh);
        const float dyu = ddy_u * static_cast<float>(vw), dyv = ddy_v * static_cast<float>(vh);
        const float d = std::max(dxu * dxu + dxv * dxv, dyu * dyu + dyv * dyv);
        return 0.5f * std::log2(std::max(d, 1.0e-8f));
    }

    inline uint32_t SelectMip(float lod, uint32_t mip_count)
    {
        const float l = lod < 0.0f ? 0.0f : lod;
        return std::min(static_cast<uint32_t>(l + 0.5f), mip_count - 1u);
    }

    struct Rgba
    {
        float r {0}, g {0}, b {0}, a {0};  // 0..255 scale
    };

    /// Bilinear fetch of one mip image at normalised (u, v) with clamp-to-edge; texel centres at +0.5.
    inline Rgba BilinearClamp(const Image& img, float u, float v)
    {
        const float x = std::clamp(u, 0.0f, 1.0f) * static_cast<float>(img.w) - 0.5f;
        const float y = std::clamp(v, 0.0f, 1.0f) * static_cast<float>(img.h) - 0.5f;
        const float fx0 = std::floor(x), fy0 = std::floor(y);
        const float tx = x - fx0, ty = y - fy0;
        auto clampi = [](float f, uint32_t n) {
            return static_cast<uint32_t>(std::clamp(f, 0.0f, static_cast<float>(n - 1u)));
        };
        const uint32_t x0 = clampi(fx0, img.w), x1 = clampi(fx0 + 1.0f, img.w);
        const uint32_t y0 = clampi(fy0, img.h), y1 = clampi(fy0 + 1.0f, img.h);
        auto at = [&](uint32_t px, uint32_t py, uint32_t c) {
            return static_cast<float>(img.rgba[(static_cast<size_t>(py) * img.w + px) * 4u + c]);
        };
        float out[4];
        for (uint32_t c = 0; c < 4; ++c)
        {
            const float top = at(x0, y0, c) * (1.0f - tx) + at(x1, y0, c) * tx;
            const float bottom = at(x0, y1, c) * (1.0f - tx) + at(x1, y1, c) * tx;
            out[c] = top * (1.0f - ty) + bottom * ty;
        }
        return {out[0], out[1], out[2], out[3]};
    }

    struct SampleResult
    {
        Rgba color;
        bool resident {false};
        uint32_t mip {0};   // the mip actually sampled (after fallback)
    };

    /// What the shader must return: nearest-mip selection, residency fallback to coarser mips, bilinear.
    /// `is_resident(mip, page_x, page_y)` is the CPU truth of the page table.
    inline SampleResult SampleWithFallback(const std::vector<Image>& chain,
                                           uint32_t vw,
                                           uint32_t vh,
                                           uint32_t page_size,
                                           const std::function<bool(uint32_t, uint32_t, uint32_t)>& is_resident,
                                           float u,
                                           float v,
                                           float lod)
    {
        const uint32_t mip_count = static_cast<uint32_t>(chain.size());
        SampleResult result;
        for (uint32_t m = SelectMip(lod, mip_count); m < mip_count; ++m)
        {
            const float cu = std::clamp(u, 0.0f, 1.0f), cv = std::clamp(v, 0.0f, 1.0f);
            const uint32_t dw = MipDim(vw, m), dh = MipDim(vh, m);
            uint32_t pages_x, pages_y;
            MipPages(vw, vh, page_size, m, pages_x, pages_y);
            const uint32_t tx = std::min(static_cast<uint32_t>(cu * static_cast<float>(dw)), dw - 1u);
            const uint32_t ty = std::min(static_cast<uint32_t>(cv * static_cast<float>(dh)), dh - 1u);
            const uint32_t page_x = std::min(tx / page_size, pages_x - 1u);
            const uint32_t page_y = std::min(ty / page_size, pages_y - 1u);
            if (is_resident(m, page_x, page_y))
            {
                result.color = BilinearClamp(chain[m], cu, cv);
                result.resident = true;
                result.mip = m;
                return result;
            }
        }
        return result;
    }

    /// sRGB -> linear（IEC 61966-2-1 精确公式），与 vt_common.hlsli / vt_common.h 的 VTSrgbToLinear 同一契约。
    /// 输入输出都是 0..1 的浮点；材质把 VT 当 base color 时在滤波之后调用。
    inline float SrgbToLinear(float c)
    {
        return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
    }
}  // namespace VTRef
