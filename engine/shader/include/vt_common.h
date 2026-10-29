// ===========================================================================
// VirtualTexture page-table indirect lookup (shared GLSL).  Sampling contract v2.
//
// The sampling contract between the CPU-side VirtualTextureManager and any shader that wants to read a virtual
// texture. **Changing any rule here must be mirrored in C++ (Texture/VirtualTextureMath.h) and in the HLSL copy
// (shader/hlsl/common/vt_common.hlsli)** -- otherwise every page lands in the wrong place.
//
//   page table  (VirtualTextureResource::UpdatePageTableTexture)
//     R32G32_SFLOAT, ONE 2D texture ("atlas"). Width = mip-0 pages_x; height = sum of the sampled mips' pages_y.
//     Mip m's page grid starts at row  sum_{i<m} pages_y(i)  (VTMipRowOffset). pages(m) = ceil(max(1, size>>m) / page_size).
//     .x = physical slot index (>= 0) if resident, < 0 (kPageTableNotResident = -1) if not. Only test "x < 0": the
//          sentinel is deliberately not float(UINT32_MAX) (rounds to 2^32; turning it back into uint is UB).
//     .y = mip level of the entry (informational).
//
//   physical pool (PhysicalTexturePool)
//     One 2D image (R8G8B8A8). Each slot is slot_size = page_size + 2*border texels square, laid out on a
//     pool_width_pages-wide grid:  slot -> (slot % pool_width_pages, slot / pool_width_pages) * slot_size.
//     The page interior starts `border` texels into the slot; the ring around it is a copy of the neighbouring
//     texels of the SAME mip (clamped at the mip image edge). That ring is what lets a bilinear footprint cross a
//     page edge without reading another page's pixels.
//
//   mips
//     mip_count sampled mips (>= 1; 1 = the original mip-0-only layout). The mip is chosen from screen-space
//     derivatives:  lod = 0.5*log2(max(|d(uv*size)/dx|^2, |d(uv*size)/dy|^2)),  mip = clamp(round(lod), 0, n-1).
//     If the page under the sample is not resident at that mip, coarser mips are tried until one is.
//
// Two families of entry points:
//   v1  VTResolveTexel / VTFetch / VTSampleUV     mip 0, integer-texel / point. Works for border 0 AND 1.
//   v2  VTLod / VTSampleBilinear                  mip selection + bilinear. REQUIRES border >= 1.
//
// For v2 the pool's sampler must be LINEAR + clamp-to-edge; the page table is only ever texelFetch'ed.
// ===========================================================================

#ifndef ZENGINE_VT_COMMON_H
#define ZENGINE_VT_COMMON_H

struct VTLookupParams
{
    uint page_size;
    uint pool_width_pages;
    uint pages_x;  // virtual texture width in pages (mip 0)
    uint pages_y;
    uint border;     // gutter texels per page edge inside a slot
    uint mip_count;  // sampled mips (>= 1)
    uvec2 virtual_size;
};

uint VTSlotSize(VTLookupParams p)
{
    return p.page_size + 2u * p.border;
}

uvec2 VTMipTexels(VTLookupParams p, uint mip)
{
    return max(p.virtual_size >> mip, uvec2(1u));
}

uvec2 VTMipPages(VTLookupParams p, uint mip)
{
    return (VTMipTexels(p, mip) + uvec2(p.page_size - 1u)) / p.page_size;
}

uint VTMipRowOffset(VTLookupParams p, uint mip)
{
    uint offset = 0u;
    for (uint i = 0u; i < mip; ++i)
    {
        offset += VTMipPages(p, i).y;
    }
    return offset;
}

// Resolve one virtual texel (mip 0) through the page table.
// Returns true and writes the physical texel coordinate when the page is
// resident; returns false (physical_texel untouched) when it is not.
bool VTResolveTexel(sampler2D page_table, VTLookupParams p, uvec2 virtual_texel, out ivec2 physical_texel)
{
    const uvec2 page = virtual_texel / p.page_size;
    if (page.x >= p.pages_x || page.y >= p.pages_y)
    {
        return false;
    }

    const vec2 entry = texelFetch(page_table, ivec2(page), 0).xy;
    if (entry.x < 0.0)
    {
        return false;  // not resident
    }

    const uint slot = uint(entry.x);
    const uvec2 slot_xy = uvec2(slot % p.pool_width_pages, slot / p.pool_width_pages);
    const uvec2 in_page = virtual_texel % p.page_size;
    physical_texel = ivec2(slot_xy * VTSlotSize(p) + uvec2(p.border) + in_page);
    return true;
}

// Indirect fetch: virtual texel -> page table -> physical pool. Returns fallback
// when the page is not resident.
vec4 VTFetch(sampler2D page_table, sampler2D physical_pool, VTLookupParams p, uvec2 virtual_texel, vec4 fallback,
             out bool resident)
{
    ivec2 physical_texel;
    resident = VTResolveTexel(page_table, p, virtual_texel, physical_texel);
    return resident ? texelFetch(physical_pool, physical_texel, 0) : fallback;
}

// UV entry point (mip 0, point sampling): uv in [0,1), virtual_size = virtual texture size in texels.
vec4 VTSampleUV(sampler2D page_table, sampler2D physical_pool, VTLookupParams p, vec2 uv, uvec2 virtual_size,
                vec4 fallback, out bool resident)
{
    const vec2 texel_f = clamp(uv, vec2(0.0), vec2(1.0)) * vec2(virtual_size);
    const uvec2 texel = min(uvec2(texel_f), virtual_size - uvec2(1u));
    return VTFetch(page_table, physical_pool, p, texel, fallback, resident);
}

// ---------------------------------------------------------------------------------------------------------------
// v2: mip selection + bilinear
// ---------------------------------------------------------------------------------------------------------------

// lod from the screen-space derivatives of uv (uv units per pixel).
float VTLod(VTLookupParams p, vec2 uv_ddx, vec2 uv_ddy)
{
    const vec2 dx = uv_ddx * vec2(p.virtual_size);
    const vec2 dy = uv_ddy * vec2(p.virtual_size);
    const float d = max(dot(dx, dx), dot(dy, dy));
    return 0.5 * log2(max(d, 1.0e-8));
}

// Page-table lookup for (mip, page). True and the slot's grid coordinate when resident.
bool VTLookupSlot(sampler2D page_table, VTLookupParams p, uint mip, uvec2 page, out uvec2 slot_xy)
{
    slot_xy = uvec2(0u);
    const float slot_f = texelFetch(page_table, ivec2(int(page.x), int(VTMipRowOffset(p, mip) + page.y)), 0).x;
    if (slot_f < 0.0)
    {
        return false;
    }
    const uint slot = uint(slot_f);
    slot_xy = uvec2(slot % p.pool_width_pages, slot / p.pool_width_pages);
    return true;
}

// Nearest-mip selection from `lod`, residency fallback to coarser mips, bilinear inside the chosen mip.
// uv in [0,1] (clamped). `used_mip` is the mip actually sampled. Returns `fallback` (resident = false) when no mip
// from the selected one up to the coarsest has the page under the sample resident.
vec4 VTSampleBilinear(sampler2D page_table, sampler2D physical_pool, VTLookupParams p, vec2 uv, float lod,
                      vec4 fallback, out bool resident, out uint used_mip)
{
    resident = false;
    used_mip = 0u;
    const vec2 cuv = clamp(uv, vec2(0.0), vec2(1.0));
    const uint first = uint(min(max(lod, 0.0) + 0.5, float(p.mip_count - 1u)));
    const vec2 pool_size = vec2(textureSize(physical_pool, 0));
    const uint slot_size = VTSlotSize(p);

    for (uint m = first; m < p.mip_count; ++m)
    {
        const uvec2 dims = VTMipTexels(p, m);
        const uvec2 pages = VTMipPages(p, m);
        const vec2 pos = cuv * vec2(dims);                      // continuous texel position in mip m
        const uvec2 texel = min(uvec2(pos), dims - uvec2(1u));  // the texel the sample point lies in
        const uvec2 page = min(texel / p.page_size, pages - uvec2(1u));
        uvec2 slot_xy;
        if (VTLookupSlot(page_table, p, m, page, slot_xy))
        {
            const vec2 in_page = pos - vec2(page * p.page_size);
            const vec2 physical = vec2(slot_xy * slot_size + uvec2(p.border)) + in_page;
            resident = true;
            used_mip = m;
            return textureLod(physical_pool, physical / pool_size, 0.0);
        }
    }
    return fallback;
}

#endif  // ZENGINE_VT_COMMON_H
