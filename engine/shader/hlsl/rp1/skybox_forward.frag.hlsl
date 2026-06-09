// RP1 deferred subpass (Sky Pass): sample IBL specular cubemap into HDR backup_odd (no tonemap).

TextureCube g_skybox : register(t0, space0);
SamplerState g_skybox_sampler : register(s1, space0);

cbuffer SkyboxConstants : register(b0, space0)
{
    float4 g_camera_right_tan_aspect;
    float4 g_camera_up_tan;
    float4 g_camera_forward_padding;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float3 direction : TEXCOORD0;
};

float4 main(PSInput input) : SV_TARGET
{
    float3 camera_right = g_camera_right_tan_aspect.xyz;
    float  right_w = g_camera_right_tan_aspect.w;
    float3 camera_up = g_camera_up_tan.xyz;
    float  up_w = g_camera_up_tan.w;
    float3 camera_forward = g_camera_forward_padding.xyz;
    float3 world_direction = normalize(camera_forward + camera_right * input.direction.x * right_w -
                                       camera_up * input.direction.y * up_w);

    float3 sample_direction = float3(world_direction.x, world_direction.z, world_direction.y);
    float3 color = g_skybox.SampleLevel(g_skybox_sampler, sample_direction, 0.0f).rgb;
    return float4(color, 1.0f);
}
