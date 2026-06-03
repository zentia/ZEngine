// Per-frame lighting payload (C++ mirror: ScenePointLight / SceneDirectionalLight in RenderCommon.h).

struct PointLight
{
    float3 position;
    float radius;
    float3 intensity;
    float _pad;
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
};
