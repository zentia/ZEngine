// ===========================================================================
// VirtualTexture page-table indirect lookup (shared HLSL).  Sampling contract v2.
//
// HLSL twin of engine/shader/include/vt_common.h (GLSL) and Texture/VirtualTextureMath.h (C++). The three
// implement ONE contract and must change together. The long description of the page table atlas, the slot
// layout with its gutter, the mip rule and the fallback rule is at the top of vt_common.h; the short version:
//
//   page table : R32G32_FLOAT atlas; mip m's page grid starts at row sum_{i<m} pages_y(i); .x = slot or < 0.
//   pool       : R8G8B8A8, slot_size = page_size + 2*border; page interior starts `border` texels into the slot.
//   v1 (VTFetch / VTSampleUV)         mip 0, point (Load). Works for border 0 and 1.
//   v2 (VTLod / VTSampleBilinear)     nearest-mip selection from derivatives, coarser-mip residency fallback,
//                                     bilinear via the pool's LINEAR/clamp sampler. REQUIRES border >= 1.
// ===========================================================================

#ifndef ZENGINE_VT_COMMON_HLSLI
#define ZENGINE_VT_COMMON_HLSLI

struct VTLookupParams
{
    uint page_size;
    uint pool_width_pages;
    uint pages_x;  // virtual texture width in pages (mip 0)
    uint pages_y;
    uint border;     // gutter texels per page edge inside a slot
    uint mip_count;  // sampled mips (>= 1)
    uint2 virtual_size;
};

uint VTSlotSize(VTLookupParams p)
{
    return p.page_size + 2u * p.border;
}

uint2 VTMipTexels(VTLookupParams p, uint mip)
{
    return max(p.virtual_size >> mip, uint2(1u, 1u));
}

uint2 VTMipPages(VTLookupParams p, uint mip)
{
    return (VTMipTexels(p, mip) + uint2(p.page_size - 1u, p.page_size - 1u)) / p.page_size;
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

// Resolve one virtual texel (mip 0) through the page table. Returns true and the physical texel coordinate when the
// page is resident; false (physical_texel = 0) when it is not.
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
    physical_texel = int2(slot_xy * VTSlotSize(p) + uint2(p.border, p.border) + in_page);
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

// ---------------------------------------------------------------------------------------------------------------
// v2: mip selection + bilinear
// ---------------------------------------------------------------------------------------------------------------

// lod from the screen-space derivatives of uv (uv units per pixel). Call it from uniform control flow, passing
// ddx(uv) / ddy(uv).
float VTLod(VTLookupParams p, float2 uv_ddx, float2 uv_ddy)
{
    const float2 dx = uv_ddx * float2(p.virtual_size);
    const float2 dy = uv_ddy * float2(p.virtual_size);
    const float d = max(dot(dx, dx), dot(dy, dy));
    return 0.5f * log2(max(d, 1.0e-8f));
}

// Page-table lookup for (mip, page). True and the slot's grid coordinate when resident.
bool VTLookupSlot(Texture2D<float2> page_table, VTLookupParams p, uint mip, uint2 page, out uint2 slot_xy)
{
    slot_xy = uint2(0u, 0u);
    const float slot_f = page_table.Load(int3(int(page.x), int(VTMipRowOffset(p, mip) + page.y), 0)).x;
    if (slot_f < 0.0f)
    {
        return false;
    }
    const uint slot = uint(slot_f);
    slot_xy = uint2(slot % p.pool_width_pages, slot / p.pool_width_pages);
    return true;
}

// Nearest-mip selection from `lod`, residency fallback to coarser mips, bilinear inside the chosen mip. uv in
// [0,1] (clamped). `used_mip` is the mip actually sampled. Returns `fallback` (resident = false) when no mip from the
// selected one up to the coarsest has the page under the sample resident.
float4 VTSampleBilinear(Texture2D<float2> page_table,
                        Texture2D<float4> physical_pool,
                        SamplerState pool_sampler,
                        VTLookupParams p,
                        float2 uv,
                        float lod,
                        float4 fallback,
                        out bool resident,
                        out uint used_mip)
{
    resident = false;
    used_mip = 0u;
    const float2 cuv = saturate(uv);
    const uint first = uint(min(max(lod, 0.0f) + 0.5f, float(p.mip_count - 1u)));
    uint pool_w, pool_h;
    physical_pool.GetDimensions(pool_w, pool_h);
    const float2 pool_size = float2(pool_w, pool_h);
    const uint slot_size = VTSlotSize(p);

    for (uint m = first; m < p.mip_count; ++m)
    {
        const uint2 dims = VTMipTexels(p, m);
        const uint2 pages = VTMipPages(p, m);
        const float2 pos = cuv * float2(dims);                       // continuous texel position in mip m
        const uint2 texel = min(uint2(pos), dims - uint2(1u, 1u));   // the texel the sample point lies in
        const uint2 page = min(texel / p.page_size, pages - uint2(1u, 1u));
        uint2 slot_xy;
        if (VTLookupSlot(page_table, p, m, page, slot_xy))
        {
            const float2 in_page = pos - float2(page * p.page_size);
            const float2 physical = float2(slot_xy * slot_size + uint2(p.border, p.border)) + in_page;
            resident = true;
            used_mip = m;
            return physical_pool.SampleLevel(pool_sampler, physical / pool_size, 0.0f);
        }
    }
    return fallback;
}

// sRGB -> linear（IEC 61966-2-1 精确公式）。VT 页池存的是 UNORM 字节（不做硬件 sRGB 解码），材质把 VT 当 base color
// 用时在滤波之后解码——滤波在 sRGB 编码空间里做，是常见折中；要「线性空间滤波」需要 R8G8B8A8_SRGB 的页池。
float3 VTSrgbToLinear(float3 c)
{
    const float3 low = c / 12.92f;
    const float3 high = pow((c + 0.055f) / 1.055f, 2.4f);
    return float3(c.x <= 0.04045f ? low.x : high.x, c.y <= 0.04045f ? low.y : high.y, c.z <= 0.04045f ? low.z : high.z);
}

#endif  // ZENGINE_VT_COMMON_HLSLI
