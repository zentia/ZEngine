// ---------------------------------------------------------------------------
// GPU 粒子：片元着色器（DX12 / DXC）
//
// 第一版：不带贴图，只输出"渐变色 × uv 圆形衰减"的软圆点（常见的默认粒子外观），
// 这样在没有美术资源时也能看出粒子系统是否工作。将来接图集页时：
//   * set 1 / t3 挂图集纹理 + sampler（与 GPU-Spine 的 set 1 同款）；
//   * 这里改成 `tex.Sample(samp, uv)` 后乘顶点传下来的 color；
//   * 顶点里的 uv 已经是 particle_corner_uv（0..1），图集页要再按页矩形映射一次。
//
// 输出是**预乘 alpha**（rgb 已乘 a）：管线用的是 src=ONE / dst=ONE_MINUS_SRC_ALPHA，
// 与 GPU-Spine 的 normal 混合同一组因子（SpineGpuRenderer.cpp:854-856）。
// ---------------------------------------------------------------------------

struct PSInput
{
    float4 clip : SV_POSITION;
    float2 uv : TEXCOORD0;
    float4 color : TEXCOORD1;
};

float4 main(PSInput i) : SV_Target
{
    // uv 从 (0,0)..(1,1)：到中心的距离 → 边缘衰减，做出软圆点
    const float2 d = i.uv * 2.0 - 1.0;
    const float r = length(d);
    // smoothstep 在 r=1 处给 0、r=0.6 处给 1：中心实心、边缘一圈羽化
    const float mask = 1.0 - smoothstep(0.6, 1.0, r);
    if (mask <= 0.0)
    {
        discard;   // 四角全透明：省掉一批无意义的片元
    }
    const float alpha = i.color.a * mask;
    return float4(i.color.rgb * alpha, alpha);   // 预乘 alpha
}
