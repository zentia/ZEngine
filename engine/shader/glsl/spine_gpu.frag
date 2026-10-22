#version 450
// GPU-Spine 片元：图集采样 × 槽位色，输出**预乘 alpha**（规格 §8）。
// 混合态（pipeline 侧）：normal / additive 都是 One, OneMinusSrcAlpha —— additive 靠
// 片元把 alpha 写 0（spine-unity 的 PMA 加色同一做法），这样它落进 UI 层后
// combine_ui 的 `ui.rgb + scene * (1 - ui.a)` 正好是"加到场景上"。

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
    uint reserved0;
    uint reserved1;
    uint reserved2;
};
layout(set = 0, binding = 5) readonly buffer Draws { SpineDraw draws[]; };

layout(set = 1, binding = 0) uniform sampler2D u_atlas;

layout(location = 0) in vec2 v_uv;
layout(location = 1) flat in uint v_draw;

layout(location = 0) out vec4 o_color;

void main()
{
    const SpineDraw d = draws[v_draw];
    vec4 tex = texture(u_atlas, v_uv);
    if ((d.flags & 1u) != 0u)
    {
        tex.rgb *= tex.a;   // 直 alpha 图集 ⇒ 先预乘
    }
    // 槽位色是直 alpha（§5.2）⇒ 预乘形式是 (rgb·a, a)
    const vec4 tint = unpackUnorm4x8(d.tint);
    vec4 c = tex * vec4(tint.rgb * tint.a, tint.a);
    if ((d.flags & 2u) != 0u)
    {
        c.a = 0.0;          // additive
    }
    o_color = c;
}
