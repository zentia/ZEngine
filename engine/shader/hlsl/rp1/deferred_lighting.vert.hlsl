// Fullscreen triangle VS for deferred / MegaLights (must match post_process.vert.hlsl and
// shader/glsl/deferred_lighting.vert UV layout so sky world-direction reconstruction is correct).

struct VsOutput
{
    float4 position : SV_POSITION;
    float2 texcoord : TEXCOORD0;
};

VsOutput main(uint vertex_id : SV_VertexID)
{
    VsOutput output;
    output.texcoord = float2((vertex_id << 1) & 2, vertex_id & 2);
    output.position =
        float4(output.texcoord * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return output;
}
