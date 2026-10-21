#version 450

// Nanite 集群光栅化的片元阶段：调试用的纯色（按 LOD 层级染色）。
// 真正的材质 / GBuffer 输出要等 Nanite 有自己的材质绑定之后再接。

layout(location = 0) in vec3 in_world_position;
layout(location = 1) in vec3 in_cluster_color;

layout(location = 0) out vec4 out_color;

void main()
{
    // 一个很轻的朗伯项，让集群的朝向看得出来 —— 纯色的话整个网格会糊成一片。
    vec3 face_normal = normalize(cross(dFdx(in_world_position), dFdy(in_world_position)));
    float lambert = clamp(dot(face_normal, normalize(vec3(0.4, 0.8, 0.45))), 0.0, 1.0);
    out_color = vec4(in_cluster_color * (0.35 + 0.65 * lambert), 1.0);
}
