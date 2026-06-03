// Stochastic local-light shading (included from megalights_deferred.frag).
// Directional + IBL are applied by the parent shader after this include.

highp uint megalights_tile_index()
{
    highp ivec2 tile_coord =
        ivec2(in_texcoord * vec2(float(ml_tile_count_x), float(ml_tile_count_y)));
    tile_coord = clamp(tile_coord,
                       ivec2(0),
                       ivec2(int(ml_tile_count_x) - 1, int(ml_tile_count_y) - 1));
    return uint(tile_coord.y) * ml_tile_count_x + uint(tile_coord.x);
}

highp float megalights_hash(highp vec2 p)
{
    return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453);
}

highp float megalights_load_depth_at_uv(highp vec2 uv, highp float fallback_depth)
{
    // GLSL ES 3.1 subpassLoad has no pixel-offset overload; only sample the current pixel.
    if (ml_viewport_width == 0u || ml_viewport_height == 0u)
    {
        return subpassLoad(in_scene_depth).r;
    }
    highp vec2 pixel_delta = abs(uv - in_texcoord) * vec2(float(ml_viewport_width), float(ml_viewport_height));
    if (pixel_delta.x > 1.5 || pixel_delta.y > 1.5)
    {
        return fallback_depth;
    }
    return subpassLoad(in_scene_depth).r;
}

highp float megalights_shadow_ss(highp vec3 world_pos, highp vec3 light_pos, highp float scene_depth_at_pixel)
{
    highp vec3 to_light   = light_pos - world_pos;
    highp float ray_len   = length(to_light);
    if (ray_len < 1e-4)
    {
        return 1.0;
    }
    highp vec3 dir = to_light / ray_len;

    highp mat4 view_proj = proj_view_matrix;
    highp int steps      = int(ml_ss_steps);
    for (int step_index = 1; step_index < steps; ++step_index)
    {
        highp float t           = float(step_index) / float(steps);
        highp vec3 sample_pos   = world_pos + dir * (t * ray_len);
        highp vec4 clip         = view_proj * vec4(sample_pos, 1.0);
        highp vec3 ndc          = clip.xyz / clip.w;
        highp vec2 uv            = ndcxy_to_uv(ndc.xy);
        highp float stored_depth = megalights_load_depth_at_uv(uv, scene_depth_at_pixel);
        if (stored_depth < ndc.z - 0.0005 && stored_depth < scene_depth_at_pixel + 0.0001)
        {
            return 0.0;
        }
    }
    return 1.0;
}

highp vec3 megalights_eval_point_brdf(highp vec3 L,
                                     highp vec3 V,
                                     highp vec3 N,
                                     highp vec3 F0,
                                     highp vec3 basecolor,
                                     highp float metallic,
                                     highp float roughness,
                                     highp vec3 intensity,
                                     highp float attenuation)
{
    highp float NoL = min(dot(N, L), 1.0);
    if (NoL <= 0.0 || attenuation <= 0.0)
    {
        return vec3(0.0);
    }
    return BRDF(L, V, N, F0, basecolor, metallic, roughness) * intensity * attenuation * NoL;
}

highp vec3 megalights_stochastic_direct(highp vec3 world_pos,
                                        highp float scene_depth_at_pixel,
                                        highp vec3 N,
                                        highp vec3 V,
                                        highp vec3 F0,
                                        highp vec3 basecolor,
                                        highp float metallic,
                                        highp float roughness)
{
    highp uint tile_index = megalights_tile_index();
    highp uvec2 range      = ml_tile_ranges[tile_index];
    highp uint tile_offset = range.x;
    highp uint tile_count  = min(range.y, uint(ZENGINE_MEGALIGHTS_MAX_LIGHTS_PER_TILE));

    if (tile_count == 0u || ml_light_count == 0u)
    {
        return vec3(0.0);
    }

    highp uint sample_count = min(ml_num_samples, tile_count);
    highp vec3 accum = vec3(0.0);

    highp vec2 noise = vec2(
        megalights_hash(in_texcoord + vec2(float(ml_frame_index) * 0.1)),
        megalights_hash(in_texcoord.yx + vec2(float(ml_frame_index) * 0.07)));

    for (highp uint sample_index = 0u; sample_index < sample_count; ++sample_index)
    {
        highp float light_rand = fract(noise.x + float(sample_index) * 0.6180339);
        highp uint list_index  = tile_offset + min(uint(light_rand * float(tile_count)), tile_count - 1u);
        highp uint light_index = ml_tile_indices[list_index];
        if (light_index >= ml_light_count)
        {
            continue;
        }

        MegaLight light = ml_lights[light_index];
        highp vec3 light_position = light.position_radius.xyz;
        highp float light_radius  = light.position_radius.w;
        highp vec3 intensity      = light.intensity.xyz;

        highp vec3 L                = normalize(light_position - world_pos);
        highp float distance        = length(light_position - world_pos);
        highp float distance_att    = 1.0 / (distance * distance + 1.0);
        highp float radius_att      = 1.0 - ((distance * distance) / (light_radius * light_radius));
        highp float light_attenuation = max(0.0, radius_att) * distance_att;

        highp float visibility = megalights_shadow_ss(world_pos, light_position, scene_depth_at_pixel);
        highp vec3 contribution =
            megalights_eval_point_brdf(L, V, N, F0, basecolor, metallic, roughness, intensity, light_attenuation) *
            visibility;

        accum += contribution;
    }

    return accum / max(float(sample_count), 1.0);
}

highp vec3 megalights_temporal_denoise(highp vec3 lo_raw,
                                       highp vec3 world_pos,
                                       highp float scene_depth_at_pixel,
                                       highp vec2 texcoord)
{
    ivec2 pix = ivec2(gl_FragCoord.xy);

    if (ml_temporal_enable == 0u || ml_history_valid == 0u)
    {
        imageStore(ml_history_out, pix, vec4(lo_raw, 1.0));
        return lo_raw;
    }

    highp vec4 prev_clip = ml_prev_proj_view * vec4(world_pos, 1.0);
    highp vec3 prev_ndc = prev_clip.xyz / prev_clip.w;
    highp vec2 prev_uv = ndcxy_to_uv(prev_ndc.xy);

    if (prev_uv.x < 0.0 || prev_uv.x > 1.0 || prev_uv.y < 0.0 || prev_uv.y > 1.0)
    {
        imageStore(ml_history_out, pix, vec4(lo_raw, 1.0));
        return lo_raw;
    }

    highp vec3 history = texture(ml_history, prev_uv).rgb;

    highp float depth_at_prev = megalights_load_depth_at_uv(prev_uv, scene_depth_at_pixel);
    highp vec4 prev_ndc_full = vec4(uv_to_ndcxy(prev_uv), depth_at_prev, 1.0);
    highp vec4 world_from_prev_h = inverse(ml_prev_proj_view) * prev_ndc_full;
    highp vec3 world_from_prev = world_from_prev_h.xyz / world_from_prev_h.w;
    highp vec4 reproj_clip = proj_view_matrix * vec4(world_from_prev, 1.0);
    highp vec2 reproj_uv = ndcxy_to_uv(reproj_clip.xy / reproj_clip.w);
    highp float uv_delta = length(reproj_uv - texcoord);
    if (uv_delta > ml_disocclusion_threshold)
    {
        imageStore(ml_history_out, pix, vec4(lo_raw, 1.0));
        return lo_raw;
    }

    highp vec3 box_min = min(lo_raw, history);
    highp vec3 box_max = max(lo_raw, history);
    box_min = min(box_min, lo_raw * 0.85);
    box_max = max(box_max, lo_raw * 1.15);
    history = clamp(history, box_min, box_max);

    highp vec3 lo_denoised = mix(lo_raw, history, ml_temporal_blend);
    imageStore(ml_history_out, pix, vec4(lo_denoised, 1.0));
    return lo_denoised;
}
