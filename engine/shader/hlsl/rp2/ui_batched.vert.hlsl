struct VSInput
{
    float2 pos : POSITION;
    float4 color : COLOR0;
    float2 uv : TEXCOORD0;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float4 color : COLOR0;
    float2 uv : TEXCOORD0;
};

PSInput main(VSInput input)
{
    PSInput o;
    o.position = float4(input.pos, 0.0f, 1.0f);
    o.color = input.color;
    o.uv = input.uv;
    return o;
}
