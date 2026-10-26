#version 310 es

#extension GL_GOOGLE_include_directive : enable

#include "constants.h"
precision highp float;
precision highp int;

// =====================================================================================
// TAA resolve -- depth reprojection, no velocity buffer
// -------------------------------------------------------------------------------------
// The engine has no motion-vector attachment, so instead of reading a per-pixel velocity
// we reconstruct one: the depth buffer plus the inverse of THIS frame's view-projection
// gives the world position behind every pixel, and the PREVIOUS frame's view-projection
// then tells us where that world position landed last frame. That is exact for all
// opaque geometry -- including moving objects -- and only degrades where there is no
// meaningful depth (transparent surfaces, sky written at the far plane), which is
// precisely the set of pixels that ghosts in every velocity-buffer-free TAA.
//
// Ordering constraint: this runs on the jittered HDR colour BEFORE bloom and tone
// mapping. Sampling a tone-mapped / bloomed history would make the AABB clip fight the
// non-linear colour curve and produce crawling.
// =====================================================================================

layout(set = 0, binding = 0) uniform sampler2D in_color;    // current (jittered) frame
layout(set = 0, binding = 1) uniform sampler2D in_history;  // previous frame's resolve
layout(set = 0, binding = 2) uniform sampler2D in_depth;

layout(set = 0, binding = 3) uniform TAAUB
{
    mat4 clip_to_world;       // inverse(current view-projection)
    mat4 world_to_prev_clip;  // previous view-projection
    // x: base blend (how much of the CURRENT frame to keep, ~0.05-0.15)
    // y: reset flag (1 = ignore history entirely this frame)
    // z: variance clipping gamma (~0.75-1.25)
    // w: depth mode (0 = standard [0,1] depth, 1 = reversed-Z)
    vec4 params;
    // xy: render resolution, zw: 1 / resolution
    vec4 resolution;
} ubo;

layout(location = 0) in vec2 in_uv;

// Two outputs, one draw:
//   location 0 -> the stable resolve target. Bloom / tone mapping bind this ONCE and
//                 never re-bind, which is the whole reason it is not ping-ponged.
//   location 1 -> this frame's history slot. The history has to alternate (a pass may
//                 not sample and write the same image), but only this pass reads it, so
//                 the alternation stays internal.
layout(location = 0) out vec4 out_color;
layout(location = 1) out vec4 out_history;

vec3 RgbToYcocg(vec3 c)
{
    float co = c.r - c.b;
    float t  = c.b + co * 0.5;
    float cg = c.g - t;
    float y  = t + cg * 0.5;
    return vec3(y, co, cg);
}

vec3 YcocgToRgb(vec3 c)
{
    float t = c.x - c.z * 0.5;
    float g = c.z + t;
    float b = t - c.y * 0.5;
    float r = b + c.y;
    return vec3(r, g, b);
}

void main()
{
    vec2 texel = ubo.resolution.zw;
    vec3 current = texture(in_color, in_uv).rgb;

    // ---- 1. 3x3 neighbourhood of the current frame, in YCoCg ------------------------
    // YCoCg because clipping an AABB in RGB lets a bright red history survive inside a
    // dark green neighbourhood and shows up as coloured fringing.
    vec3 nmin = vec3(1e8);
    vec3 nmax = vec3(-1e8);
    vec3 m1 = vec3(0.0);
    vec3 m2 = vec3(0.0);

    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            vec3 s = RgbToYcocg(texture(in_color, in_uv + vec2(float(x), float(y)) * texel).rgb);
            nmin = min(nmin, s);
            nmax = max(nmax, s);
            m1 += s;
            m2 += s * s;
        }
    }
    vec3 mu1 = m1 / 9.0;
    vec3 sigma = sqrt(max(m2 / 9.0 - mu1 * mu1, vec3(0.0)));

    // ---- 2. Reproject: depth -> world -> previous clip -> previous uv ---------------
    float raw_depth = texture(in_depth, in_uv).r;
    // Reversed-Z stores 1 at the near plane, so the NDC z has to be flipped back before
    // it can go through an inverse projection.
    float depth = mix(raw_depth, 1.0 - raw_depth, step(0.5, ubo.params.w));

    vec4 clip = vec4(in_uv * 2.0 - vec2(1.0), depth, 1.0);
    vec4 world = ubo.clip_to_world * clip;
    float world_w = (abs(world.w) > 1e-6) ? world.w : 1.0;
    world /= world_w;

    vec4 prev_clip = ubo.world_to_prev_clip * world;
    float prev_w = prev_clip.w;
    vec2 prev_uv = (prev_clip.xy / max(abs(prev_w), 1e-6)) * 0.5 + vec2(0.5);

    // ---- 3. History validity ---------------------------------------------------------
    // Anything that leaves the screen, or lands behind the previous camera, has no valid
    // history: fall back to the current frame so a disocclusion resolves in one step
    // instead of smearing across the whole frame.
    bool offscreen = any(lessThan(prev_uv, vec2(0.0))) || any(greaterThan(prev_uv, vec2(1.0)));
    bool behind = prev_w <= 0.0;
    bool reset = (ubo.params.y > 0.5) || offscreen || behind;

    vec3 history = texture(in_history, prev_uv).rgb;
    vec3 history_ycocg = RgbToYcocg(history);

    // ---- 4. Variance clipping (Karis, "High Quality Temporal Supersampling") --------
    // Without this a stale history that no longer matches the neighbourhood shows up as
    // a ghost. The clip box is the intersection of the 3x3 AABB and a gamma-scaled
    // standard-deviation box around the mean, which is what kills ghosting without
    // over-blurring genuine detail.
    vec3 gamma = vec3(max(ubo.params.z, 0.0));
    vec3 lo = max(nmin, mu1 - gamma * sigma);
    vec3 hi = min(nmax, mu1 + gamma * sigma);
    history_ycocg = clamp(history_ycocg, lo, hi);
    history = YcocgToRgb(history_ycocg);

    // ---- 5. Blend --------------------------------------------------------------------
    // `params.x` is "how much of the current frame wins". Reset means all of it.
    float blend = clamp(ubo.params.x, 0.0, 1.0);
    if (reset)
    {
        blend = 1.0;
    }

    vec3 resolved = mix(history, current, blend);

    out_color = vec4(resolved, 1.0);
    // Same value into both targets: next frame reads this as its history.
    out_history = vec4(resolved, 1.0);
}
