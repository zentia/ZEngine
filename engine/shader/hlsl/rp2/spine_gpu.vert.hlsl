// =====================================================================
// GPU-Spine 蒙皮（DX12 版）—— 与 glsl/spine_gpu.vert **逐行同一套公式**，改这边必须同步改那边。
// 数据布局、寻址、三条硬约束（混合的是变换后的位置；按影响存储顺序累加；不许 FMA 收缩 ⇒ precise）
// 都见 GLSL 版文件头与 doc/GPU_SPINE_INTERCHANGE_FORMAT.md §3、§5、§6。
//
// 寄存器 = DX12RHI::CreatePipelineLayout 的映射：set → space、binding → register。
//   set 0 的 9 个 SSBO ⇒ t0..t8 / space0（结构化 SRV，stride 由 CPU 侧 RHIDescriptorBufferInfo.stride 给）；
//   set 1 binding 0 的 combined image sampler ⇒ t0 + s0 / space1（片元）。
//
// 与 GLSL 版唯一的结构差异 —— **绘制项下标**：
//   D3D12 的 SV_InstanceID **不含** DrawIndexedInstanced 的 StartInstanceLocation（恒从 0 起），
//   而 Vulkan 的 gl_InstanceIndex 含 firstInstance。所以这里用一条 **per-instance 顶点属性**
//   （TEXCOORD0，uint，CPU 侧绑一块 0,1,2,… 的顶点缓冲）：per-instance 属性的取址**会**加上
//   StartInstanceLocation，于是 `draw_index` = 录制时传的 firstInstance，与 Vulkan 同一个语义。
// =====================================================================

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

struct SpineInst
{
    float4 clip_col[4];   // GLSL mat4 的 4 列（列主序）；不用 float4x4，免得踩 HLSL 矩阵打包方向
    float4 root_abcd;
    float4 root_t;
};

StructuredBuffer<uint> vertex_words : register(t0, space0);
StructuredBuffer<float2> infl_bind : register(t1, space0);
StructuredBuffer<uint> infl_bone : register(t2, space0);
StructuredBuffer<float2> uvs : register(t3, space0);
StructuredBuffer<float> bones : register(t4, space0);
StructuredBuffer<SpineDraw> draws : register(t5, space0);
StructuredBuffer<SpineInst> insts : register(t6, space0);
StructuredBuffer<float> deform : register(t7, space0);
StructuredBuffer<float4> cpu_verts : register(t8, space0);

struct VSInput
{
    uint draw_index : TEXCOORD0;   // per-instance（见文件头）
    uint vertex_id : SV_VertexID;
};

struct VSOutput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
    nointerpolation uint draw : TEXCOORD1;
};

VSOutput main(VSInput input)
{
    const SpineDraw d = draws[input.draw_index];
    const uint vi = input.vertex_id;
    const bool has_deform = (d.flags & 4u) != 0u;
    precise float2 p = float2(0.0, 0.0);
    float2 uv = float2(0.0, 0.0);

    if ((d.flags & 8u) != 0u)
    {
        const float4 cv = cpu_verts[d.deform_base + vi];
        p = cv.xy;
        uv = cv.zw;
    }
    else if (d.vertex_format == 0u)
    {
        float2 bp = float2(asfloat(vertex_words[vi * 2u]), asfloat(vertex_words[vi * 2u + 1u]));
        if (has_deform)
        {
            const uint f = d.deform_base + vi * 2u;
            bp = float2(deform[f], deform[f + 1u]);
        }
        const uint b = (d.bone_base + d.slot_bone) * 6u;
        precise float x = bp.x * bones[b + 0u] + bp.y * bones[b + 1u] + bones[b + 4u];
        precise float y = bp.x * bones[b + 2u] + bp.y * bones[b + 3u] + bones[b + 5u];
        p = float2(x, y);
    }
    else
    {
        const uint w0 = d.weighted_base + vi * 2u;
        const uint off = vertex_words[w0];
        const uint cnt = vertex_words[w0 + 1u];
        precise float x = 0.0;
        precise float y = 0.0;
        for (uint k = 0u; k < cnt; ++k)
        {
            precise float2 bp = infl_bind[off + k];
            if (has_deform)
            {
                const uint f = d.deform_base + (off + k) * 2u;
                bp += float2(deform[f], deform[f + 1u]);
            }
            const uint bw = infl_bone[off + k];
            const uint b = (d.bone_base + (bw & 0xFFFFu)) * 6u;
            const float w = float((bw >> 16u) & 0xFFu) / 255.0;
            precise float tx = bp.x * bones[b + 0u] + bp.y * bones[b + 1u] + bones[b + 4u];
            precise float ty = bp.x * bones[b + 2u] + bp.y * bones[b + 3u] + bones[b + 5u];
            x += tx * w;
            y += ty * w;
        }
        p = float2(x, y);
    }

    const SpineInst inst = insts[d.inst_index];
    precise float rx = inst.root_abcd.x * p.x + inst.root_abcd.y * p.y + inst.root_t.x;
    precise float ry = inst.root_abcd.z * p.x + inst.root_abcd.w * p.y + inst.root_t.y;

    VSOutput o;
    // = GLSL `clip_from_world * vec4(rx, ry, 0, 1)`（列主序：Σ col[i] · v[i]）
    o.position = inst.clip_col[0] * rx + inst.clip_col[1] * ry + inst.clip_col[3];
    o.uv = (d.flags & 8u) != 0u ? uv : uvs[d.uv_offset + vi];
    o.draw = input.draw_index;
    return o;
}
