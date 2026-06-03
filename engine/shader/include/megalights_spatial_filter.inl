#ifndef MEGALIGHTS_SPATIAL_FILTER_INL
#define MEGALIGHTS_SPATIAL_FILTER_INL

highp float megalights_spatial_load_depth(highp sampler2D depth_tex, highp vec2 uv)
{
    return texture(depth_tex, uv).r;
}

highp vec3 megalights_spatial_load_normal(highp sampler2D normal_tex, highp vec2 uv)
{
    return megalights_decode_normal(texture(normal_tex, uv).rgb);
}

highp vec3 megalights_spatial_filter(highp sampler2D direct_tex,
                                     highp sampler2D depth_tex,
                                     highp sampler2D normal_tex,
                                     highp vec2 texcoord,
                                     highp float center_depth,
                                     highp vec3 center_normal)
{
    if (ml_spatial_enable == 0u || ml_viewport_width == 0u || ml_viewport_height == 0u)
    {
        return texture(direct_tex, texcoord).rgb;
    }

    highp vec2 texel_size = vec2(1.0) / vec2(float(ml_viewport_width), float(ml_viewport_height));
    highp int radius = int(ml_spatial_radius);
    highp vec3 center_direct = texture(direct_tex, texcoord).rgb;
    highp vec3 accum = center_direct;
    highp float weight_sum = 1.0;

    for (int y = -radius; y <= radius; ++y)
    {
        for (int x = -radius; x <= radius; ++x)
        {
            if (x == 0 && y == 0)
            {
                continue;
            }

            highp vec2 sample_uv = texcoord + vec2(float(x), float(y)) * texel_size;
            if (sample_uv.x < 0.0 || sample_uv.x > 1.0 || sample_uv.y < 0.0 || sample_uv.y > 1.0)
            {
                continue;
            }

            highp vec3 sample_direct = texture(direct_tex, sample_uv).rgb;
            highp float sample_depth = megalights_spatial_load_depth(depth_tex, sample_uv);
            highp vec3 sample_normal = megalights_spatial_load_normal(normal_tex, sample_uv);

            highp float depth_delta = abs(sample_depth - center_depth);
            highp float depth_weight = exp(-depth_delta / max(ml_spatial_depth_sigma, 1e-5));
            highp float normal_weight = pow(max(dot(center_normal, sample_normal), 0.0), ml_spatial_normal_power);
            highp float weight = depth_weight * normal_weight;
            accum += sample_direct * weight;
            weight_sum += weight;
        }
    }

    return accum / max(weight_sum, 1e-4);
}

#endif
