#version 450
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
    uint flags;           // bit0：贴图是直 alpha ⇒ 片元里预乘；bit1：输出 alpha = 0（加色）
    uint weighted_base;   // weighted 段在 b0 里的起点（u32 下标）
};
layout(set = 0, binding = 5) readonly buffer Draws { SpineDraw draws[]; };

struct SpineInst
{
    mat4 clip_from_world;   // 骨架空间（root 之后）→ 裁剪空间
    vec4 root_abcd;         // x' = a*x + b*y + tx ; y' = c*x + d*y + ty
    vec4 root_t;            // xy = (tx, ty)
};
layout(set = 0, binding = 6) readonly buffer Insts { SpineInst insts[]; };

layout(location = 0) out vec2 v_uv;
layout(location = 1) flat out uint v_draw;

void main()
{
    const SpineDraw d = draws[gl_InstanceIndex];
    const uint vi = uint(gl_VertexIndex);
    precise vec2 p = vec2(0.0);

    if (d.vertex_format == 0u)
    {
        // rigid：整块由槽位骨骼单矩阵变换（VertexAttachment.cpp:80-85）
        const vec2 bp = vec2(uintBitsToFloat(vertex_words[vi * 2u]), uintBitsToFloat(vertex_words[vi * 2u + 1u]));
        const uint b = (d.bone_base + d.slot_bone) * 6u;
        precise float x = bp.x * bones[b + 0u] + bp.y * bones[b + 1u] + bones[b + 4u];
        precise float y = bp.x * bones[b + 2u] + bp.y * bones[b + 3u] + bones[b + 5u];
        p = vec2(x, y);
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
            const vec2 bp = infl_bind[off + k];
            const uint bw = infl_bone[off + k];
            const uint b = (d.bone_base + (bw & 0xFFFFu)) * 6u;
            const float w = float((bw >> 16u) & 0xFFu) / 255.0;
            precise float tx = bp.x * bones[b + 0u] + bp.y * bones[b + 1u] + bones[b + 4u];
            precise float ty = bp.x * bones[b + 2u] + bp.y * bones[b + 3u] + bones[b + 5u];
            x += tx * w;
            y += ty * w;
        }
        p = vec2(x, y);
    }

    // 根变换在蒙皮**之后**（§5.3）—— 乘进骨骼表会二次缩放
    const SpineInst inst = insts[d.inst_index];
    precise float rx = inst.root_abcd.x * p.x + inst.root_abcd.y * p.y + inst.root_t.x;
    precise float ry = inst.root_abcd.z * p.x + inst.root_abcd.w * p.y + inst.root_t.y;

    gl_Position = inst.clip_from_world * vec4(rx, ry, 0.0, 1.0);
    v_uv = uvs[d.uv_offset + vi];
    v_draw = uint(gl_InstanceIndex);
}
