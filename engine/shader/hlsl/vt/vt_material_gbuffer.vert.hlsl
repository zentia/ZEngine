// 材质 VT 槽位 A/B 测试用的顶点阶段（测试脚手架，不进任何相机 pass）。
//
// 被测对象是真的 rp1/mesh_gbuffer.frag.hlsl —— 材质 UBO 在 b0/space2、贴图在 t1..t3/space2、
// VT 页表 / 页池在 t6 / t7 / s7 / space2，全都照生产布局绑。只有顶点阶段是脚手架，
// 它的输出必须与 mesh_gbuffer.frag.hlsl 的 PsInput 逐字段对上。
//
// 全屏三角形：uv (0,0) 落在目标左上角（读回 row 0 == v 最小），与 vt_blit.vert.hlsl
// 同一约定（D3D NDC 的 y 是向上的，所以位置对 uv 取负斜率）。
// 本阶段不读任何资源，所以 set 0 / set 1 不需要给管线布局任何 binding。

struct VsOutput
{
    float4 position : SV_POSITION;
    float3 world_position : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float3 tangent : TEXCOORD2;
    float2 texcoord : TEXCOORD3;
};

VsOutput main(uint vid : SV_VertexID)
{
    const float2 uv = float2(float((vid << 1) & 2), float(vid & 2));

    VsOutput o;
    o.position = float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    o.texcoord = uv;
    // 溶解噪声的输入。测试里 customParams[0].x = 0（不溶解），所以这个值是啥都不影响输出。
    o.world_position = float3(uv * 10.0f, 0.0f);
    o.normal = float3(0.0f, 0.0f, 1.0f);
    o.tangent = float3(1.0f, 0.0f, 0.0f);
    return o;
}
