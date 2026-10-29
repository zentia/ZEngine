// ---------------------------------------------------------------------------
// GPU-Spine 公式层 —— 唯一权威：doc/GPU_SPINE_INTERCHANGE_FORMAT.md §6（顶点）/ §8（片元）
//
// 这里**只有公式**：不声明任何 buffer / 纹理 / 寄存器，不做任何取址。
// 数据全部由调用方喂进来。因此同一份文件能被不同语言、不同通道同时消费：
//   * glsl/spine_gpu.{vert,frag}     —— glslangValidator，-I 本目录
//   * hlsl/rp2/spine_gpu.*.hlsl      —— DXC，调用方须在 include 前 `#define ZENGINE_HLSL 1`
//   * 将来的 Unity 侧 / 去 SSBO 的纹理通道（§5.4）—— 只换取址，公式不动
//
// 语言分支沿用 gbuffer.h 的约定：HLSL 侧定义 ZENGINE_HLSL，GLSL 侧不定义。
// ⇒ **HLSL 消费者必须在 #include 之前写 `#define ZENGINE_HLSL 1`**；
//   忘了会拿到 vec2 分支而编译失败 —— 这是有意的响亮失败，好过悄悄拿错类型。
//
// ⚠️ 改这里 = 同时改两端 + CPU 参考实现（`SpineGpuEmulateVertex` /
// `SpineGpuShadeReference`）。**不要顺手重排运算顺序**：f32 加法不满足结合律，
// 重排能编译、能跑，只是结果静默地变 —— 这正是 §6 存在的理由。
// ---------------------------------------------------------------------------

#ifndef ZENGINE_SHADER_INCLUDE_SPINE_SKINNING_H
#define ZENGINE_SHADER_INCLUDE_SPINE_SKINNING_H

// ---- 语言方言 ------------------------------------------------------------
#ifdef ZENGINE_HLSL
#define SPINE_FLOAT2 float2
#define SPINE_FLOAT3 float3
#define SPINE_FLOAT4 float4
#define SPINE_PRECISE precise
#else
#define SPINE_FLOAT2 vec2
#define SPINE_FLOAT3 vec3
#define SPINE_FLOAT4 vec4
#define SPINE_PRECISE precise
#endif

// ---- 绘制项 flags 位（§8）------------------------------------------------
#define SPINE_FLAG_STRAIGHT_ALPHA 1u   // bit0：图集是直 alpha ⇒ 先预乘
#define SPINE_FLAG_ADDITIVE       2u   // bit1：加色 ⇒ 片元把 alpha 写 0
#define SPINE_FLAG_HAS_DEFORM     4u   // bit2：本项有 deform
#define SPINE_FLAG_CPU_CLIPPED    8u   // bit3：走 b8 CPU 裁剪顶点
#define SPINE_FLAG_TINT_BLACK     16u  // bit4：双色（槽位暗色）
#define SPINE_FLAG_WORLD_LAYER    32u  // bit5：世界层 ⇒ 图集 sRGB 转线性

// ---- 2×3 仿射：骨骼与 root 数学同构（§5.3）-------------------------------
//
// 骨骼（世界变换）与 root（实例根变换）都是 2×3 仿射，但**语义不同**：
// 换用了能编译、能跑，只是角色整体位移/缩放悄悄错。C++ 侧 `SpineGpuFormat.h`
// 因此保持成两个类型，这里同样给两个名字 —— 换用的代价是读代码时看见名字不对。
SPINE_PRECISE SPINE_FLOAT2 SpineAffineApply(SPINE_FLOAT2 p, float a, float b, float c, float d,
                                            float tx, float ty)
{
    return SPINE_FLOAT2(p.x * a + p.y * b + tx,
                        p.x * c + p.y * d + ty);
}

// 骨骼世界变换（§5.1）—— 6 个 f32 的顺序固定 a,b,c,d,worldX,worldY
SPINE_PRECISE SPINE_FLOAT2 SpineBoneApply(SPINE_FLOAT2 bind_pos, float a, float b, float c, float d,
                                          float world_x, float world_y)
{
    return SpineAffineApply(bind_pos, a, b, c, d, world_x, world_y);
}

// 实例根变换（§5.3）—— 施加在蒙皮**之后**，预乘进骨骼表会二次缩放
SPINE_PRECISE SPINE_FLOAT2 SpineRootApply(SPINE_FLOAT2 p, float a, float b, float c, float d,
                                          float tx, float ty)
{
    return SpineAffineApply(p, a, b, c, d, tx, ty);
}

// ---- 影响条目 packed 解码（chunk 22：u16 boneIdx | u16 weight << 16，v17 起 unorm16）------
//
// 整条 4 B 一次读出来再拆，不能单独 Load @2 —— 那个地址不是 4 字节对齐的。
// ⚠️ 权重必须是 `x / 65535.0`，**不要**改成 `x * (1.0 / 65535.0)`：
//    两者在 f32 下不逐位等价，改了就与 CPU 侧 `SpineGpuDecodeWeight()` 差一点。
// v16 及以前是 unorm8（`(packed >> 16) & 0xFF) / 255`）—— 实测 weighted 顶点误差 5.2 骨架单位，
// u16 降到 0.07（GPUSpine 对拍台 781 资产全量）。
uint SpineInfluenceBoneIndex(uint packed)
{
    return packed & 0xFFFFu;
}

float SpineInfluenceWeight(uint packed)
{
    return float(packed >> 16u) / 65535.0;
}

// ---- deform 语义（§5 b7）：rigid 替换、weighted 相加 ---------------------
//
// rigid    = 2 float/顶点，**绝对**坐标，整体替换 bindPos（VertexAttachment.cpp:72-73）
// weighted = 2 float/影响，**偏移**，逐影响加在 bindPos 上（:122-123）
SPINE_FLOAT2 SpineApplyDeform(SPINE_FLOAT2 bind_pos, SPINE_FLOAT2 deform_value, bool rigid)
{
    return rigid ? deform_value : bind_pos + deform_value;
}

// ---- 片元着色（§8）-------------------------------------------------------
//
// tex  ：图集采样值；`flags & bit0` 置位表示它是**直 alpha**，这里负责先预乘
// tint ：槽位色，**直 alpha**（§5.2）⇒ 预乘形式是 (rgb·a, a)
// dark ：tintBlack 暗色，float 不截断（负值也要生效 —— TwoColor 首帧前的符号 bug）
//
// 顺序是规格的一部分：
//   ① 直 alpha 预乘 → ② 乘槽位色 → ③ 加暗色 → ④ 世界层 sRGB→线性 → ⑤ 加色清 alpha
//   ④ 必须在 ⑤ **之前**（去预乘要用 alpha）；改顺序会静默改颜色。
SPINE_FLOAT4 SpineShade(SPINE_FLOAT4 tex, SPINE_FLOAT4 tint, SPINE_FLOAT3 dark, uint flags)
{
    if ((flags & SPINE_FLAG_STRAIGHT_ALPHA) != 0u)
    {
        tex.rgb *= tex.a;
    }

    SPINE_FLOAT4 c = tex * SPINE_FLOAT4(tint.rgb * tint.a, tint.a);

    if ((flags & SPINE_FLAG_TINT_BLACK) != 0u)
    {
        c.rgb += (SPINE_FLOAT3(1.0, 1.0, 1.0) - tex.rgb) * dark * (tex.a * tint.a);
    }

    if ((flags & SPINE_FLAG_WORLD_LAYER) != 0u && c.a > 0.0)
    {
        c.rgb = pow(max(c.rgb / c.a, SPINE_FLOAT3(0.0, 0.0, 0.0)),
                    SPINE_FLOAT3(2.2, 2.2, 2.2)) * c.a;
    }

    if ((flags & SPINE_FLAG_ADDITIVE) != 0u)
    {
        c.a = 0.0;   // 加色：靠 alpha=0 落进 UI 层的 combine 公式（§8）
    }

    return c;
}

#endif  // ZENGINE_SHADER_INCLUDE_SPINE_SKINNING_H
