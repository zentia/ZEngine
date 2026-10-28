// VirtualTexture off-screen raster verification -- full-screen triangle VS (DX12).
// HLSL twin of glsl/vt_blit.vert. No vertex buffer: position/uv come from SV_VertexID.
//
// uv (0,0) must land on the TOP-LEFT of the target (row 0 of the readback == row 0 of the
// virtual texture). Vulkan NDC has y down, D3D NDC has y up, so the y mapping is flipped
// relative to the GLSL file -- same trick as rp2/post_process.vert.hlsl.

struct VSOutput
{
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
};

VSOutput main(uint vid : SV_VertexID)
{
    VSOutput o;
    o.uv = float2((vid << 1) & 2, vid & 2);
    o.position = float4(o.uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return o;
}
