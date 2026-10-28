// ===========================================================================
// VirtualTexture page-table indirect lookup (shared HLSL).
//
// HLSL twin of engine/shader/include/vt_common.h (GLSL). The two files implement
// ONE sampling contract and must change together -- the page table / pool layout
// is defined by the CPU side (VirtualTextureResource::UpdatePageTableTexture,
// PhysicalTexturePool), see the long comment at the top of vt_common.h:
//
//   page table : R32G32_FLOAT, one texel per mip-0 page.  .x = physical slot (>= 0)
//                or < 0 when not resident (only test "x < 0"), .y = mip level.
//   pool       : R8G8B8A8, slot -> (slot % pool_width_pages, slot / pool_width_pages),
//                a page is a whole page_size x page_size block at slot_xy * page_size.
//
// Point sampling only (Load): bilinear across a page boundary would read the
// neighbouring *physical* slot, which holds an unrelated page.
// ===========================================================================

#ifndef ZENGINE_VT_COMMON_HLSLI
#define ZENGINE_VT_COMMON_HLSLI

struct VTLookupParams
{
    uint page_size;
    uint pool_width_pages;
    uint pages_x;  // virtual texture width in pages (mip 0)
    uint pages_y;
};

// Resolve one virtual texel (mip 0) through the page table. Returns true and the
// physical texel coordinate when the page is resident; false (physical_texel = 0)
// when it is not.
bool VTResolveTexel(Texture2D<float2> page_table, VTLookupParams p, uint2 virtual_texel, out int2 physical_texel)
{
    physical_texel = int2(0, 0);

    const uint2 page = virtual_texel / p.page_size;
    if (page.x >= p.pages_x || page.y >= p.pages_y)
    {
        return false;
    }

    const float2 entry = page_table.Load(int3(int2(page), 0));
    if (entry.x < 0.0f)
    {
        return false;  // not resident
    }

    const uint slot = uint(entry.x);
    const uint2 slot_xy = uint2(slot % p.pool_width_pages, slot / p.pool_width_pages);
    const uint2 in_page = virtual_texel % p.page_size;
    physical_texel = int2(slot_xy * p.page_size + in_page);
    return true;
}

// Indirect fetch: virtual texel -> page table -> physical pool.
float4 VTFetch(Texture2D<float2> page_table,
               Texture2D<float4> physical_pool,
               VTLookupParams p,
               uint2 virtual_texel,
               float4 fallback,
               out bool resident)
{
    int2 physical_texel;
    resident = VTResolveTexel(page_table, p, virtual_texel, physical_texel);
    // Branch (not a ternary select): don't read the pool for a non-resident page.
    float4 result = fallback;
    if (resident)
    {
        result = physical_pool.Load(int3(physical_texel, 0));
    }
    return result;
}

// UV entry point (mip 0, point sampling): uv in [0,1), virtual_size = virtual texture size in texels.
float4 VTSampleUV(Texture2D<float2> page_table,
                  Texture2D<float4> physical_pool,
                  VTLookupParams p,
                  float2 uv,
                  uint2 virtual_size,
                  float4 fallback,
                  out bool resident)
{
    const float2 texel_f = saturate(uv) * float2(virtual_size);
    const uint2 texel = min(uint2(texel_f), virtual_size - uint2(1u, 1u));
    return VTFetch(page_table, physical_pool, p, texel, fallback, resident);
}

#endif  // ZENGINE_VT_COMMON_HLSLI
