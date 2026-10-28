// VirtualTexture in-frame consumer (DX12): the editor Scene-viewport overlay.
//
// First consumer that samples a virtual texture INSIDE the frame, through the page table, with the page
// table and the physical pool reached through the bindless SRV heap (SM 6.6 ResourceDescriptorHeap) --
// the way a real material would reach them. Companion of vt_blit.frag.hlsl (offscreen, descriptor
// tables); same sampling contract, one shared include.
//
// Root signature = DX12MainCameraPass's bindless production signature:
//   b0 (root constant, unused here), b1 (root CBV, pixel) = the block below, CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED.
// Pair this with vt_blit.vert.hlsl (full-screen triangle; the C++ side sets viewport/scissor to the
// overlay rect, so uv spans exactly the rect with (0,0) at its top-left).

#include "../common/vt_common.hlsli"

cbuffer VtOverlayParams : register(b1)
{
    uint4 lookup;    // x = page_size, y = pool_width_pages, z = pages_x, w = pages_y
    uint4 extent;    // x = virtual width, y = virtual height, z = page-table bindless slot, w = pool bindless slot
    float4 border;   // rgb = frame colour drawn on the outer 2 px
    float4 rect;     // xy = overlay size in pixels (for the border test)
};

struct PSInput
{
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
};

float4 main(PSInput input) : SV_TARGET
{
    Texture2D<float2> page_table    = ResourceDescriptorHeap[extent.z];
    Texture2D<float4> physical_pool = ResourceDescriptorHeap[extent.w];

    VTLookupParams p;
    p.page_size = lookup.x;
    p.pool_width_pages = lookup.y;
    p.pages_x = lookup.z;
    p.pages_y = lookup.w;

    // Non-resident pages must be visible as such (a transparent hole would look like "nothing drawn"):
    // an 8 px magenta/dark-magenta checkerboard.
    const uint2 pixel = uint2(input.position.xy);
    const bool odd = (((pixel.x >> 3) ^ (pixel.y >> 3)) & 1u) != 0u;
    const float4 fallback = odd ? float4(1.0f, 0.0f, 1.0f, 1.0f) : float4(0.25f, 0.0f, 0.25f, 1.0f);

    bool resident = false;
    float4 color = VTSampleUV(page_table, physical_pool, p, input.uv, extent.xy, fallback, resident);
    color.a = 1.0f;

    // 2 px frame so the overlay reads as a window. uv * rect size = position inside the rect (px).
    const float2 inside = input.uv * rect.xy;
    const float2 inside_to_edge = min(inside, rect.xy - inside);
    if (min(inside_to_edge.x, inside_to_edge.y) < 2.0f)
    {
        color = float4(border.rgb, 1.0f);
    }
    return color;
}
