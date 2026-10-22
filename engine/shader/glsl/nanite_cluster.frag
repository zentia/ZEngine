#version 450

// ===========================================================================
// Nanite 集群光栅化的片元阶段：写 **GBuffer**。
//
// 布局和 mesh_gbuffer.frag 用的是同一份（engine/shader/include/gbuffer.h）：
//   GBufferA.rgb = EncodeNormal(worldNormal)
//   GBufferB     = metallic, specular, roughness, shadingModelId / 255
//   GBufferC.rgb = baseColor
// 三个 target 一个不多一个不少 —— gbuffer.h 里那句 "EXACTLY THREE" 不是修辞，
// 多声明一个输出就配不上 GBuffer 的 render pass。
//
// **法线是妥协出来的，不是终局。** Nanite 今天只往 GPU 上传顶点位置，没有法线：
// NaniteMeshResource 只留 position + index，RenderResourceBase 的 CPU 几何缓存
// （Lumen 距离场那份同源数据）也只留这两样。所以这里只能拿屏幕空间导数取**面
// 法线**，结果是平直着色（flat shading）。对 Nanite 这种"三角形小到接近一个像素"
// 的网格不算灾难，但真正的平滑法线要等 Nanite 的几何数据带上 normal。
//
// **材质同样是占位。** Nanite 还没有材质绑定，metallic / specular / roughness 取
// mesh_gbuffer.frag 的默认参量，baseColor 暂时沿用集群的 LOD 调试色 —— 它一眼就
// 能看出"这里还没接材质"，比填一个看起来很像真材质的灰色诚实。
// ===========================================================================

#extension GL_GOOGLE_include_directive : enable

#include "gbuffer.h"

layout(location = 0) in vec3 in_world_position;
layout(location = 1) in vec3 in_cluster_color;

layout(location = 0) out vec4 out_gbuffer_a;
layout(location = 1) out vec4 out_gbuffer_b;
layout(location = 2) out vec4 out_gbuffer_c;

void main()
{
    // 面法线。cross(dFdx, dFdy) 的正负取决于后端对 dFdy 方向的定义，而背面剔除
    // 已经保证了这里只剩正面 —— 冒烟测试断言解码出来的法线指向 +Y（测试网格是
    // 一张 y = 0 的水平面、相机在上方），方向反了那一条会立刻变红。
    vec3 face_normal = normalize(cross(dFdx(in_world_position), dFdy(in_world_position)));

    PGBufferData gbuffer;
    gbuffer.worldNormal    = face_normal;
    gbuffer.baseColor      = in_cluster_color;  // 占位，见文件头
    gbuffer.metallic       = 0.0;
    gbuffer.specular       = 0.5;
    gbuffer.roughness      = 0.5;
    gbuffer.shadingModelID = SHADINGMODELID_DEFAULT_LIT;

    EncodeGBufferData(gbuffer, out_gbuffer_a, out_gbuffer_b, out_gbuffer_c);
}
