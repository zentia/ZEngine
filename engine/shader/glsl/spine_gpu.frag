#version 450

#extension GL_GOOGLE_include_directive : enable

// §6 / §8 的公式层：只含公式、不含任何取址 —— 与 hlsl/rp2/spine_gpu.*.hlsl 共用同一份文件。
#include "spine_skinning.h"

// GPU-Spine 片元：图集采样 × 槽位色，输出**预乘 alpha**（规格 §8）。
// 着色公式本身在 spine_skinning.h 的 `SpineShade()` 里（与 hlsl/rp2/spine_gpu.frag.hlsl 同一份），
// 这里只负责取址（读图集、读绘制项）与传参。
//
// flags 语义见 spine_skinning.h 的 SPINE_FLAG_* 常量：
//   bit4 = tint black（双色）：照 spine-unity 的 Spine-SkeletonGraphic-TintBlack.shader
//     out.rgb += (1 - tex.rgb) * dark * tex.a * vc.a    （tex 已预乘，vc.a = 槽位色 alpha）
//     `_Color` / `_Black` 取材质默认 (1,1,1,1) / (0,0,0,0)；加色项 vc.a = 0 ⇒ CPU 侧不置 bit4。
//     暗色是 float、不截断：参考把它放进 float UV 通道，负值（TwoColor 首帧前的符号 bug）照样生效。
// 混合态（pipeline 侧）：normal / additive 都是 One, OneMinusSrcAlpha —— additive 靠
// 片元把 alpha 写 0（spine-unity 的 PMA 加色同一做法），这样它落进 UI 层后
// combine_ui 的 `ui.rgb + scene * (1 - ui.a)` 正好是"加到场景上"。
// flags bit5 = 世界层（RP1 透明 subpass，线性 HDR，随后 tonemap + pow(1/2.2)）：图集是 sRGB 值 ⇒
//   先去预乘、pow 2.2 转线性、再预乘回去（与 tone_mapping.frag 的 gamma 互逆；透明网格的 SRGB 贴图由硬件做同一件事）。
//   必须在 additive 把 alpha 清 0 之前做（去预乘要用 alpha）。

struct SpineDraw
{
    uint vertex_format;
    uint uv_offset;
    uint slot_bone;
    uint bone_base;
    uint inst_index;
    uint tint;
    uint flags;
    uint weighted_base;
    uint deform_base;
    float dark_r;
    float dark_g;
    float dark_b;
};
layout(set = 0, binding = 5) readonly buffer Draws { SpineDraw draws[]; };

layout(set = 1, binding = 0) uniform sampler2D u_atlas;

layout(location = 0) in vec2 v_uv;
layout(location = 1) flat in uint v_draw;

layout(location = 0) out vec4 o_color;

void main()
{
    const SpineDraw d = draws[v_draw];
    // 槽位色是直 alpha（§5.2）⇒ 预乘形式在 SpineShade() 内部做
    const vec4 tint = unpackUnorm4x8(d.tint);
    o_color = SpineShade(texture(u_atlas, v_uv), tint, vec3(d.dark_r, d.dark_g, d.dark_b), d.flags);
}
