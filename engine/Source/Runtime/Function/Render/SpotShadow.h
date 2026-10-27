#pragma once

// ============================================================================
// SpotShadow — 聚光灯阴影投影的纯标量核心（无引擎依赖，header-only）
// ----------------------------------------------------------------------------
// 把"聚光灯阴影用多大的透视锥、以及一个世界点是否落在阴影锥内"这两件可纯计算的
// 事抽成不依赖任何引擎数学类型的 inline 函数，与 spot_attenuation 同一手法：核心
// 是纯函数，可脱离 RenderScene / Matrix4x4 / RHI 单独单测（SpotShadowSmokeTest）。
//
// 这是阶段 2（聚光单面 2D 阴影）里唯一能 headless 验证的部分。完整的 view_proj
// 矩阵组装在引擎侧用 Math::MakePerspectiveMatrix + Math::MakeLookAtMatrix 完成
// （那些类型拖入整个 Math 链，不适合 headless），这里只钉死"标量语义"：
//   - 阴影透视锥的垂直 fov = 2 * outer_cone_half_angle（正好把整个光锥包进阴影图，
//     不浪费分辨率也不裁掉锥缘）。
//   - 阴影近平面取一个极小值（贴住光源），远平面 = 光的衰减半径（range）——超出
//     半径的光本来就衰减为零，无需阴影。
//
// 语义与 shader 侧一致：mesh_lighting.inl 里聚光采样会用这份 view_proj 把 world pos
// 变到阴影 NDC，再与 depth 比较。所以 fov/近远的取值必须 CPU/GPU 一致。
// ============================================================================

namespace spot_shadow
{
    // 聚光阴影透视锥的垂直半角（弧度）。等于聚光的外锥半角——阴影图正好覆盖整个
    // 可见光锥。outer_cone_angle 必须 >= 0；超过半圆（> pi/2）会被钳到 pi/2
    // （fov 最大 180°，透视投影的物理上限）。
    inline float ShadowFovYHalfAngle(float outer_cone_angle)
    {
        if (outer_cone_angle < 0.0f)
            return 0.0f;
        const float kMaxHalf = 1.5707963f;  // pi/2
        return outer_cone_angle > kMaxHalf ? kMaxHalf : outer_cone_angle;
    }

    // 完整垂直 fov（弧度）= 2 * 半角。供 Math::MakePerspectiveMatrix 的 fovy 参数。
    inline float ShadowFovY(float outer_cone_angle)
    {
        return 2.0f * ShadowFovYHalfAngle(outer_cone_angle);
    }

    // 阴影近平面：贴住光源的一个极小值，避免 z=0 处的投影退化（除零 / 精度爆炸）。
    inline float ShadowNear()
    {
        return 0.01f;
    }

    // 阴影远平面：光的衰减半径。超出半径光衰减为零，阴影无需更远。
    // range <= 0 视为无有效范围，退成一个极小正值（避免 znear==zfar 的退化投影）。
    inline float ShadowFar(float light_range)
    {
        const float kMinFar = 0.02f;
        return light_range > kMinFar ? light_range : kMinFar;
    }

    // 聚光阴影图用正方形（aspect = 1）：锥是圆形对称的，方形图刚好包住。
    inline float ShadowAspect()
    {
        return 1.0f;
    }
}  // namespace spot_shadow
