// VirtualTexture off-screen raster verification -- fragment shader (DX12).
// HLSL twin of glsl/vt_blit.frag: the first fragment-stage consumer of the page table.
//
// Register map (RHI rule: descriptor binding N -> register N in space0; a combined image
// sampler is a t<N> SRV plus an s<N> sampler -- VT only Load()s, the samplers are unused):
//   b0 = params, t1 = page table (R32G32_FLOAT), t2 = physical pool (R8G8B8A8)

#include "../common/vt_common.hlsli"

cbuffer VtBlitParams : register(b0, space0)
{
    uint4 lookup;    // x = page_size, y = pool_width_pages, z = pages_x, w = pages_y
    uint4 extent;    // x = virtual width, y = virtual height
    float4 fallback; // colour for non-resident pages
};

Texture2D<float2> u_page_table    : register(t1, space0);
Texture2D<float4> u_physical_pool : register(t2, space0);

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

    bool resident = false;
    return VTSampleUV(u_page_table, u_physical_pool, p, input.uv, extent.xy, fallback, resident);
}
