// =====================================================================
// PR7: Bindless texture-blit vertex shader
// ---------------------------------------------------------------------
// Fullscreen-triangle VS driven by SV_VertexID. No vertex buffer,
// no input layout. The classic "draw 3, no IA" trick: one over-sized
// triangle that covers the entire NDC quad after rasterisation, with
// UVs in [0..1] mapped to clip-space [-1..3] / [-3..1]. Cheaper than
// a six-vertex quad and perfectly fine for a 256x256 preview blit.
//
// This shader is read at runtime by `DX12ShaderCompiler::compileFromFile`
// using the legacy SM 6.0 default profile (entry "main" => vs_6_0).
// We do NOT need SM 6.6 here because the VS does not touch
// ResourceDescriptorHeap -- only the PS does. Keeping VS on SM 6.0
// matches the smoke-test (PR5c phase 3 VS path) and lets older DXCs
// in the wild still compile this file if anyone tries.
//
// MUST stay byte-aligned with bindless_blit_ps.hlsl on the PSInput /
// VSOutput struct shape, otherwise CreateGraphicsPipelineState will
// reject the linkage at PSO build time.
// =====================================================================

struct VSOutput
{
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
};

VSOutput main(uint vid : SV_VertexID)
{
    // Fullscreen oversized triangle:
    //   vid=0 -> (-1, -1) uv=(0,0)
    //   vid=1 -> ( 3, -1) uv=(2,0)
    //   vid=2 -> (-1,  3) uv=(0,2)
    // After clipping to NDC [-1..1] the visible portion has uv in
    // [0..1]. Saves three vertices and one index buffer over the
    // textbook quad approach -- standard idiom in DX12 / Vulkan
    // tutorials.
    VSOutput o;
    o.uv = float2((vid << 1) & 2, vid & 2);             // (0,0) (2,0) (0,2)
    o.position = float4(o.uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return o;
}
