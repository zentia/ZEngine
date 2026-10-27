// Per-frame lighting payload (C++ mirror: ScenePointLight / SceneDirectionalLight in RenderCommon.h).
#ifndef Z_SCENE_LIGHTING_STRUCTS_HLSLI
#define Z_SCENE_LIGHTING_STRUCTS_HLSLI

// 64-byte element (four vec4 pairs) — must match ScenePointLight in RenderCommon.h
// bit-for-bit:
//   vec4_0 = {position.xyz, radius}
//   vec4_1 = {intensity.xyz, type}
//   vec4_2 = {spot_direction.xyz, spot_inner_cos}
//   vec4_3 = {spot_outer_cos, _pad_spot_0/1/2}
// type: 0.0 = point, 1.0 = spot. The spot payload (direction + cone cosines) is
// zero for point lights and only read inside the type > 0.5 branch in
// mesh_lighting.inl.
struct PointLight
{
    float3 position;
    float  radius;
    float3 intensity;
    float  type;
    float3 spot_direction;
    float  spot_inner_cos;
    float  spot_outer_cos;
    float  _pad_spot_0;
    float  _pad_spot_1;
    float  _pad_spot_2;
};

struct DirectionalLight
{
    float3 direction;
    float _pad0;
    float3 color;
    float _pad1;
};

// Shadow-pass per-frame SSBO payloads (C++ mirror: PointLightShadowPerFrame / DirectionalLightShadowPerFrame).
struct PointLightShadowPerFrame
{
    uint point_light_num;
    uint3 _pad_pl;
    float4 point_lights_position_and_radius[16];
};

struct DirectionalLightShadowPerFrame
{
    float4x4 light_proj_view;
};

// Main-camera per-frame SSBO/cbuffer (C++ mirror: MainCameraPerFrame in RenderCommon.h).
struct MainCameraPerFrame
{
    float4x4 proj_view_matrix;
    float3 camera_position;
    float _pad_camera;
    float3 ambient_light;
    float _pad_ambient;
    uint point_light_num;
    uint show_skybox;
    uint2 _pad_pl;
    PointLight scene_point_lights[16];
    DirectionalLight scene_directional_light;
    float4x4 directional_light_proj_view;
    float3 pre_view_translation;
    float _pad_pre_view;
    float3 render_tile;
    float _pad_render_tile;
    /// Scene/game panel in framebuffer pixels (x, y, width, height).
    float4 viewport_rect;
    /// Lumen indirect: rgb = irradiance, a = blend weight (0 = keep ambient_light).
    float4 lumen_indirect;
    /// Lumen reflection: rgb = scene-level radiance, a = blend weight (0 = keep the baked
    /// specular cubemap). No consumer in the deferred pass yet: it has no specular IBL term.
    float4 lumen_reflection;
    // ---- CSM (cascaded shadow maps) ----
    /// Main camera forward; the sampling side needs it to turn a fragment position into
    /// view depth before comparing against the split distances.
    float3 camera_forward;
    float _pad_camera_forward;
    float4x4 directional_light_proj_view_cascades[4];
    /// Per-cascade far plane in view depth (ascending). +inf in the unused slots.
    float4 directional_light_cascade_split_depth;
    uint directional_light_cascade_count;
    float _pad_cascade_count_1;
    float _pad_cascade_count_2;
    float _pad_cascade_count_3;
};

#include "LargeWorldCoordinates.hlsl"

ZLwcFrame ZLwcFromPerFrame(MainCameraPerFrame pf)
{
    ZLwcFrame lwc;
    lwc.pre_view_translation = pf.pre_view_translation;
    lwc._pad_pre_view = pf._pad_pre_view;
    lwc.render_tile = pf.render_tile;
    lwc._pad_render_tile = pf._pad_render_tile;
    return lwc;
}

// CSM: which cascade a fragment belongs to, by view depth (the same quantity the CPU
// side uses when it computes the split distances -- a projection onto the camera
// forward, not a radial distance).
int ZSelectShadowCascade(MainCameraPerFrame pf, float3 world_position)
{
    float view_depth = dot(world_position - pf.camera_position, pf.camera_forward);

    // 1 where split[i] < view_depth; their count *is* the cascade index.
    float4 behind = 1.0f - step(float4(view_depth, view_depth, view_depth, view_depth),
                                pf.directional_light_cascade_split_depth);
    float index = behind.x + behind.y + behind.z + behind.w;

    uint last = max(pf.directional_light_cascade_count, 1u) - 1u;
    return int(min(index, float(last)));
}

#endif  // Z_SCENE_LIGHTING_STRUCTS_HLSLI
