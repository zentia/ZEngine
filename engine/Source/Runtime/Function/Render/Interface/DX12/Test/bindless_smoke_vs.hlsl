// =====================================================================
// PR5c: Bindless smoke-test vertex shader
// ---------------------------------------------------------------------
// Trivial fullscreen-triangle vertex shader paired with
// bindless_smoke.hlsl. Stays at SM 6.0 (default profile) because
// nothing in this VS needs SM 6.6 -- we want the smoke-test to also
// confirm that mixing a legacy-profile VS with a bindless-profile PS
// in the same PSO works, which is the exact configuration the
// engine's eventual bindless materials will use.
// =====================================================================

struct VSOutput
{
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
};

// Fullscreen triangle expressed via SV_VertexID -- no vertex buffer
// needed, no input layout in the PSO. Three vertices, hard-coded
// positions covering the [-1, 1] x [-1, 1] viewport, with UVs in
// [0, 2] so the [0, 1] sub-region maps to the visible quad.
VSOutput main(uint vertex_id : SV_VertexID)
{
    VSOutput output;
    // Standard fullscreen-triangle trick: vertices at (-1,-1), (3,-1), (-1,3).
    output.uv       = float2((vertex_id << 1) & 2u, vertex_id & 2u);
    output.position = float4(output.uv * 2.0f - 1.0f, 0.0f, 1.0f);
    output.uv.y     = 1.0f - output.uv.y; // flip V to match D3D image-space convention
    return output;
}
