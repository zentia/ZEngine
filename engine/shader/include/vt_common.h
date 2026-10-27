// ===========================================================================
// VirtualTexture page-table indirect lookup (shared GLSL).
//
// The sampling contract between the CPU-side VirtualTextureManager and any
// shader that wants to read a virtual texture. **Changing any rule here must be
// mirrored in C++** (otherwise every page lands in the wrong place):
//
//   page table  (VirtualTextureResource::UpdatePageTableTexture)
//     R32G32_SFLOAT, one texel per mip-0 page, row-major.
//     .x = physical slot index (>= 0) if resident, < 0 (VirtualTextureResource::
//          kPageTableNotResident = -1) if not. Only test "x < 0": the sentinel is
//          deliberately not float(UINT32_MAX), which rounds to 2^32 and turning it
//          back into uint is undefined behaviour.
//     .y = mip level (only 0 is uploaded for now).
//
//   physical pool (PhysicalTexturePool)
//     One 2D image (R8G8B8A8) of pool_width_pages x pool_height_pages slots,
//     page_size x page_size each.
//     slot -> (slot % pool_width_pages, slot / pool_width_pages)   (GetSlotCoordinates)
//     A page is uploaded as a whole page_size x page_size block at
//     slot_xy * page_size (edge pages are zero-padded; their padding is never
//     addressed because the virtual texel stays inside the virtual texture).
//
// Point sampling (texelFetch) only. Bilinear across a page boundary would read
// the neighbouring *physical* slot, which holds an unrelated page; it needs a
// border/gutter around every page, which the pool layout does not have yet.
// ===========================================================================

#ifndef ZENGINE_VT_COMMON_H
#define ZENGINE_VT_COMMON_H

struct VTLookupParams
{
    uint page_size;
    uint pool_width_pages;
    uint pages_x;  // virtual texture width in pages (mip 0)
    uint pages_y;
};

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
    physical_texel = ivec2(slot_xy * p.page_size + in_page);
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

#endif  // ZENGINE_VT_COMMON_H
