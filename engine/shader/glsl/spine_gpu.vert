#version 450

#extension GL_GOOGLE_include_directive : enable

// §6 / §8 的公式层：只含公式、不含任何取址 —— 与 hlsl/rp2/spine_gpu.*.hlsl 共用同一份文件。
#include "spine_skinning.h"

// =====================================================================
// GPU-Spine 蒙皮（规格 doc/GPU_SPINE_INTERCHANGE_FORMAT.md §3、§5、§6）
// ---------------------------------------------------------------------
// 共享的静态数据（每个资产一份，所有实例共用 —— 这才是"VB 可共享"的意义）：
//   b0 vertex_words  容器 chunk 3 的原始字节（u32 视图）：rigid 段 8 B/顶点 = bindPos，
//                    weighted 段 8 B/顶点 = (influence_offset, influence_count)
//   b1 infl_bind     chunk 21：每个影响一份 bindPos（float2）
//   b2 infl_bone     chunk 22：u16 骨骼下标 | u8 权重 << 16（整条 4 B 一次读，再拆）
//   b3 uvs           chunk 4：float2，按 `uv_offset + 索引` 取（v13 语义）
// 每帧数据：
//   b4 bones         所有实例的骨骼世界变换打包（6 float/骨，§5.1）
//   b5 draws         每个可见绘制项一条（§5.2），由 gl_InstanceIndex（= firstInstance）选中
//   b6 insts         每实例一条：屏幕映射 + 根变换（§5.3，蒙皮**之后**施加）
//   b7 deform        本帧生效的 mesh deform（S6）：每个命中的绘制项一段，附件本地顺序
//                    rigid  = 2 float/顶点，**绝对**坐标，整体替换 bindPos（VertexAttachment.cpp:72-73）
//                    weighted = 2 float/影响，**偏移**，逐影响加在 bindPos 上（:122-123）
//                    寻址用 uint 回绕偏置（与 uv_offset 同一招）：
//                      rigid    deform[deform_base + 2·vi]            deform_base = 段起点 − 2·vertex_base
//                      weighted deform[deform_base + 2·(off + k)]     deform_base = 段起点 − 2·infl_base
//                    ⇒ shader 里不需要 vertex_base / infl_base。
//   b8 cpu_verts     本帧被 **CPU 裁剪**的项（v15，SpineGpuClipper）：每顶点 vec4 (x, y, u, v)，
//                    骨架空间（root 之前）。flags bit3 置位时整项走这条：位置 / UV 都不从静态缓冲取，
//                    `deform_base` 改作"这一项在 b8 里的起点"，索引来自每帧的动态索引缓冲（局部下标）。
//                    裁剪会**产生新三角形**（SkeletonClipping::clipTriangles），VS 里做不了 ⇒ CPU 做。
//
// 三条硬约束（§6）：混合的是**变换后的位置**不是矩阵；按影响存储顺序累加；
// 不许 FMA 收缩 ⇒ 全部用 `precise`（与 CPU 逐位一致的前提）。
// =====================================================================

layout(set = 0, binding = 0) readonly buffer VertexWords { uint vertex_words[]; };
layout(set = 0, binding = 1) readonly buffer InflBind { vec2 infl_bind[]; };
layout(set = 0, binding = 2) readonly buffer InflBone { uint infl_bone[]; };
layout(set = 0, binding = 3) readonly buffer Uvs { vec2 uvs[]; };
layout(set = 0, binding = 4) readonly buffer Bones { float bones[]; };

struct SpineDraw
{
    uint vertex_format;   // 0 rigid / 1 weighted（SpineVertexFormat）
    uint uv_offset;       // v13：uv = uvs[uv_offset + 索引]（uint 回绕）
    uint slot_bone;       // rigid：槽位骨骼（chunk 2）
    uint bone_base;       // 该实例骨骼表在 b4 里的起始骨下标
    uint inst_index;      // 进 b6
    uint tint;            // RGBA8 直 alpha（槽位色 × 骨架色）
    uint flags;           // bit0：贴图是直 alpha ⇒ 片元里预乘；bit1：输出 alpha = 0（加色）；bit2：有 deform（b7）；
                          // bit3：CPU 裁剪过的顶点（b8，见文件头）；bit4：tint black（只有片元读）
    uint weighted_base;   // weighted 段在 b0 里的起点（u32 下标）
    uint deform_base;     // bit2：b7 的寻址偏置；bit3：b8 里的起点（顶点）
    float dark_r;         // bit4：槽位暗色（float，不截断 —— 见 spine_gpu.frag）
    float dark_g;
    float dark_b;
};
layout(set = 0, binding = 5) readonly buffer Draws { SpineDraw draws[]; };

struct SpineInst
{
    mat4 clip_from_world;   // 骨架空间（root 之后）→ 裁剪空间
    vec4 root_abcd;         // x' = a*x + b*y + tx ; y' = c*x + d*y + ty
    vec4 root_t;            // xy = (tx, ty)
};
layout(set = 0, binding = 6) readonly buffer Insts { SpineInst insts[]; };
layout(set = 0, binding = 7) readonly buffer Deform { float deform[]; };
layout(set = 0, binding = 8) readonly buffer CpuVerts { vec4 cpu_verts[]; };

layout(location = 0) out vec2 v_uv;
layout(location = 1) flat out uint v_draw;

void main()
{
    const SpineDraw d = draws[gl_InstanceIndex];
    const uint vi = uint(gl_VertexIndex);
    const bool has_deform = (d.flags & SPINE_FLAG_HAS_DEFORM) != 0u;
    precise vec2 p = vec2(0.0);
    vec2 uv = vec2(0.0);

    if ((d.flags & SPINE_FLAG_CPU_CLIPPED) != 0u)
    {
        // CPU 裁剪过的项：位置已是骨架空间（与下面两支蒙皮的输出同一个空间）
        const vec4 cv = cpu_verts[d.deform_base + vi];
        p = cv.xy;
        uv = cv.zw;
    }
    else if (d.vertex_format == 0u)
    {
        // rigid：整块由槽位骨骼单矩阵变换（VertexAttachment.cpp:80-85）
        vec2 bp = vec2(uintBitsToFloat(vertex_words[vi * 2u]), uintBitsToFloat(vertex_words[vi * 2u + 1u]));
        if (has_deform)
        {
            // rigid 的 deform 是绝对坐标：整体替换（不是加）
            const uint f = d.deform_base + vi * 2u;
            bp = SpineApplyDeform(bp, vec2(deform[f], deform[f + 1u]), true);
        }
        const uint b = (d.bone_base + d.slot_bone) * 6u;
        p = SpineBoneApply(bp, bones[b + 0u], bones[b + 1u], bones[b + 2u], bones[b + 3u],
                           bones[b + 4u], bones[b + 5u]);
    }
    else
    {
        // weighted：Σ wᵢ·(Bᵢ·vᵢ)，按影响存储顺序（VertexAttachment.cpp:102-110）
        const uint w0 = d.weighted_base + vi * 2u;
        const uint off = vertex_words[w0];
        const uint cnt = vertex_words[w0 + 1u];
        precise float x = 0.0;
        precise float y = 0.0;
        for (uint k = 0u; k < cnt; ++k)
        {
            precise vec2 bp = infl_bind[off + k];
            if (has_deform)
            {
                // weighted 的 deform 是偏移：先加到 bindPos 上再变换（参考实现 vx = vertices[b] + deform[f]）
                const uint f = d.deform_base + (off + k) * 2u;
                bp = SpineApplyDeform(bp, vec2(deform[f], deform[f + 1u]), false);
            }
            const uint bw = infl_bone[off + k];
            const uint b = (d.bone_base + SpineInfluenceBoneIndex(bw)) * 6u;
            const float w = SpineInfluenceWeight(bw);
            const vec2 skinned = SpineBoneApply(bp, bones[b + 0u], bones[b + 1u], bones[b + 2u],
                                                bones[b + 3u], bones[b + 4u], bones[b + 5u]);
            x += skinned.x * w;
            y += skinned.y * w;
        }
        p = vec2(x, y);
    }

    // 根变换在蒙皮**之后**（§5.3）—— 乘进骨骼表会二次缩放
    const SpineInst inst = insts[d.inst_index];
    const vec2 root_p = SpineRootApply(p, inst.root_abcd.x, inst.root_abcd.y, inst.root_abcd.z,
                                       inst.root_abcd.w, inst.root_t.x, inst.root_t.y);
    const float rx = root_p.x;
    const float ry = root_p.y;

    gl_Position = inst.clip_from_world * vec4(rx, ry, 0.0, 1.0);
    v_uv = (d.flags & SPINE_FLAG_CPU_CLIPPED) != 0u ? uv : uvs[d.uv_offset + vi];
    v_draw = uint(gl_InstanceIndex);
}
