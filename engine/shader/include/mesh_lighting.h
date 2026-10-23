#define PI 3.1416

// todo: param/const
#define MAX_REFLECTION_LOD 8.0

// Normal Distribution function --------------------------------------
highp float D_GGX(highp float dotNH, highp float roughness)
{
    highp float alpha  = roughness * roughness;
    highp float alpha2 = alpha * alpha;
    highp float denom  = dotNH * dotNH * (alpha2 - 1.0) + 1.0;
    return (alpha2) / (PI * denom * denom);
}

// Geometric Shadowing function --------------------------------------
highp float G_SchlicksmithGGX(highp float dotNL, highp float dotNV, highp float roughness)
{
    highp float r  = (roughness + 1.0);
    highp float k  = (r * r) / 8.0;
    highp float GL = dotNL / (dotNL * (1.0 - k) + k);
    highp float GV = dotNV / (dotNV * (1.0 - k) + k);
    return GL * GV;
}

// Fresnel function ----------------------------------------------------
highp float Pow5(highp float x)
{
    return (x * x * x * x * x);
}

highp vec3 F_Schlick(highp float cosTheta, highp vec3 F0) 
{ 
    return F0 + (1.0 - F0) * Pow5(1.0 - cosTheta); 
    }

highp vec3 F_SchlickR(highp float cosTheta, highp vec3 F0, highp float roughness)
{
    return F0 + (max(vec3(1.0 - roughness, 1.0 - roughness, 1.0 - roughness), F0) - F0) * Pow5(1.0 - cosTheta);
}

// Specular and diffuse BRDF composition --------------------------------------------
highp vec3 BRDF(highp vec3  L,
                highp vec3  V,
                highp vec3  N,
                highp vec3  F0,
                highp vec3  basecolor,
                highp float metallic,
                highp float roughness)
{
    // Precalculate vectors and dot products
    highp vec3  H     = normalize(V + L);
    highp float dotNV = clamp(dot(N, V), 0.0, 1.0);
    highp float dotNL = clamp(dot(N, L), 0.0, 1.0);
    highp float dotLH = clamp(dot(L, H), 0.0, 1.0);
    highp float dotNH = clamp(dot(N, H), 0.0, 1.0);

    // Light color fixed
    // vec3 lightColor = vec3(1.0);

    highp vec3 color = vec3(0.0);

    highp float rroughness = max(0.05, roughness);
    // D = Normal distribution (Distribution of the microfacets)
    highp float D = D_GGX(dotNH, rroughness);
    // G = Geometric shadowing term (Microfacets shadowing)
    highp float G = G_SchlicksmithGGX(dotNL, dotNV, rroughness);
    // F = Fresnel factor (Reflectance depending on angle of incidence)
    highp vec3 F = F_Schlick(dotNV, F0);

    highp vec3 spec = D * F * G / (4.0 * dotNL * dotNV + 0.001);
    highp vec3 kD   = (vec3(1.0) - F) * (1.0 - metallic);

    color += (kD * basecolor / PI + (1.0 - kD) * spec);
    // color += (kD * basecolor / PI + spec) * dotNL;
    // color += (kD * basecolor / PI + spec) * dotNL * lightColor;

    return color;
}

highp vec2 ndcxy_to_uv(highp vec2 ndcxy) { return ndcxy * vec2(0.5, 0.5) + vec2(0.5, 0.5); }

highp vec2 uv_to_ndcxy(highp vec2 uv) { return uv * vec2(2.0, 2.0) + vec2(-1.0, -1.0); }

// ---- CSM（级联阴影）选级 ----
//
// 方向光 shadow map 是 4 层 texture2DArray，一层一级。片元该用哪一级由它的
// **视图深度**决定 —— 跟 CPU 侧 `CalculateDirectionalLightCascadeCameras()` 算
// split 距离用的是同一个量（沿相机 forward 的投影距离），所以可以直接比；换成
// 径向距离（`length(world - camera_position)`）就会在画面边缘选错级。
//
// 本函数依赖 `main_camera_per_frame_access.inl` 把 per-frame 字段铺成裸名，所以
// 只能在那之后被包含（四个 frag 都是这个顺序）。
highp int z_select_shadow_cascade(highp vec3 world_position)
{
    highp float view_depth = dot(world_position - camera_position, camera_forward);

    // split_distance[i] < view_depth 的个数**就是**级号：split 是递增的远平面，
    // 前面有 n 个远平面被越过就落在第 n 级。用 vec4 一次性比较是为了躲开
    // 动态下标 —— SPIRV-Cross 转 MSL 时对 vec4 做动态分量下标会生成很难看的
    // 代码，而且老驱动上有踩过坑的历史。
    highp vec4  behind = vec4(lessThan(directional_light_cascade_split_depth, vec4(view_depth)));
    highp float index  = behind.x + behind.y + behind.z + behind.w;

    // 级数不足 4（退化成单张）或深度超出最后一级时，靠 clamp 收敛到最后一级。
    highp float last = float(max(directional_light_cascade_count, 1u) - 1u);
    return int(min(index, last));
}