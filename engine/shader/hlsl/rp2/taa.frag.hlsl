// TAA resolve -- depth reprojection, no velocity buffer.
// Ported from engine/shader/glsl/taa.frag. VS: hlsl/rp2/post_process.vert.hlsl.
//
// Runs on the jittered HDR colour BEFORE bloom and tone mapping; see the GLSL version
// for the full rationale behind the depth->world->prev-clip reprojection.

Texture2D in_color   : register(t0, space0);  // current (jittered) frame
SamplerState in_color_sampler : register(s0, space0);
Texture2D in_history : register(t1, space0);  // previous frame's resolve
SamplerState in_history_sampler : register(s1, space0);
Texture2D in_depth   : register(t2, space0);
SamplerState in_depth_sampler : register(s2, space0);

cbuffer TAAUB : register(b0, space0)
{
    float4x4 clip_to_world;       // inverse(current view-projection)
    float4x4 world_to_prev_clip;  // previous view-projection
    // x: base blend, y: reset flag, z: variance gamma, w: depth mode (1 = reversed-Z)
    float4 params;
    // xy: render resolution, zw: 1 / resolution
    float4 resolution;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
};

float3 RgbToYcocg(float3 c)
{
    const float co = c.r - c.b;
    const float t  = c.b + co * 0.5;
    const float cg = c.g - t;
    const float y  = t + cg * 0.5;
    return float3(y, co, cg);
}

float3 YcocgToRgb(float3 c)
{
    const float t = c.x - c.z * 0.5;
    const float g = c.z + t;
    const float b = t - c.y * 0.5;
    const float r = b + c.y;
    return float3(r, g, b);
}

// Two targets, one draw (mirrors the GLSL version):
//   SV_TARGET0 -> the stable resolve target, bound once by bloom / tone mapping
//   SV_TARGET1 -> this frame's history slot, read by the next frame
struct PSOutput
{
    float4 resolved : SV_TARGET0;
    float4 history_out : SV_TARGET1;
};

PSOutput main(PSInput input)
{
    const float2 texel = resolution.zw;
    const float3 current = in_color.Sample(in_color_sampler, input.uv).rgb;

    // ---- 3x3 neighbourhood in YCoCg ----
    float3 nmin = float3(1e8, 1e8, 1e8);
    float3 nmax = float3(-1e8, -1e8, -1e8);
    float3 m1 = float3(0.0, 0.0, 0.0);
    float3 m2 = float3(0.0, 0.0, 0.0);

    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            const float3 s = RgbToYcocg(in_color.Sample(in_color_sampler, input.uv + float2(float(x), float(y)) * texel).rgb);
            nmin = min(nmin, s);
            nmax = max(nmax, s);
            m1 += s;
            m2 += s * s;
        }
    }

    const float3 mu1 = m1 / 9.0;
    const float3 sigma = sqrt(max(m2 / 9.0 - mu1 * mu1, float3(0.0, 0.0, 0.0)));

    // ---- Reproject ----
    const float raw_depth = in_depth.Sample(in_depth_sampler, input.uv).r;
    const float depth = (params.w > 0.5) ? (1.0 - raw_depth) : raw_depth;

    const float4 clip = float4(input.uv * 2.0 - float2(1.0, 1.0), depth, 1.0);
    float4 world = mul(clip_to_world, clip);
    world /= (world.w == 0.0) ? 1e-6 : world.w;

    const float4 prev_clip = mul(world_to_prev_clip, world);
    const float prev_w = prev_clip.w;
    const float2 prev_uv = (prev_clip.xy / max(abs(prev_w), 1e-6)) * 0.5 + float2(0.5, 0.5);

    const bool offscreen = any(prev_uv < float2(0.0, 0.0)) || any(prev_uv > float2(1.0, 1.0));
    const bool behind = prev_w <= 0.0;
    const bool reset = (params.y > 0.5) || offscreen || behind;

    float3 history = in_history.Sample(in_history_sampler, prev_uv).rgb;
    float3 history_ycocg = RgbToYcocg(history);

    // ---- Variance clipping ----
    const float3 gamma = float3(max(params.z, 0.0), max(params.z, 0.0), max(params.z, 0.0));
    const float3 lo = max(nmin, mu1 - gamma * sigma);
    const float3 hi = min(nmax, mu1 + gamma * sigma);
    history_ycocg = clamp(history_ycocg, lo, hi);
    history = YcocgToRgb(history_ycocg);

    float blend = clamp(params.x, 0.0, 1.0);
    if (reset)
    {
        blend = 1.0;
    }

    PSOutput output;
    output.resolved = float4(lerp(history, current, blend), 1.0);
    output.history_out = output.resolved;
    return output;
}
