struct VsOutput
{
    float4 position : SV_POSITION;
    float2 texcoord : TEXCOORD0;
};

VsOutput main(uint vertex_id : SV_VertexID)
{
    float2 positions[3] = { float2(-1.0f, -1.0f), float2(3.0f, -1.0f), float2(-1.0f, 3.0f) };
    float2 uvs[3] = { float2(0.0f, 0.0f), float2(2.0f, 0.0f), float2(0.0f, 2.0f) };

    VsOutput output;
    output.position = float4(positions[vertex_id], 0.0f, 1.0f);
    output.texcoord = uvs[vertex_id];
    return output;
}
