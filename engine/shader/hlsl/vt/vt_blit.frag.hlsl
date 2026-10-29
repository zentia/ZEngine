// VirtualTexture off-screen raster verification -- fragment shader (DX12).
// HLSL twin of glsl/vt_blit.frag: the first fragment-stage consumer of the page table.
//
// Register map (RHI rule: descriptor binding N -> register N in space0; a combined image
// sampler is a t<N> SRV plus an s<N> sampler):
//   b0 = params, t1 = page table (R32G32_FLOAT), t2 = physical pool (R8G8B8A8), s2 = the pool's sampler
//   (LINEAR + clamp-to-edge; only the v2 bilinear path uses it, v1 Load()s).

#include "../common/vt_common.hlsli"

cbuffer VtBlitParams : register(b0, space0)
{
    uint4 lookup;    // x = page_size, y = pool_width_pages, z = pages_x, w = pages_y
    uint4 extent;    // x = virtual width, y = virtual height
    float4 fallback; // colour for non-resident pages
    uint4 v2;        // x = page_border, y = mip_count, z = filter (0 = point mip 0, 1 = mip select + bilinear)
};

Texture2D<float2> u_page_table    : register(t1, space0);
Texture2D<float4> u_physical_pool : register(t2, space0);
SamplerState u_pool_sampler       : register(s2, space0);

struct PSInput
{
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
};

float4 main(PSInput input) : SV_TARGET
{
    VTLookupParams p;
    p.page_size = lookup.x;
    p.pool_width_pages = lookup.y;
    p.pages_x = lookup.z;
    p.pages_y = lookup.w;
    p.border = v2.x;
    p.mip_count = v2.y;
    p.virtual_size = extent.xy;

    // Derivatives must be taken in uniform control flow, before the filter-mode branch.
    const float2 uv_ddx = ddx(input.uv);
    const float2 uv_ddy = ddy(input.uv);

    bool resident = false;
    if (v2.z == 0u)
    {
        return VTSampleUV(u_page_table, u_physical_pool, p, input.uv, p.virtual_size, fallback, resident);
    }
    uint used_mip = 0u;
    const float lod = VTLod(p, uv_ddx, uv_ddy);
    return VTSampleBilinear(u_page_table, u_physical_pool, u_pool_sampler, p, input.uv, lod, fallback, resident, used_mip);
}
