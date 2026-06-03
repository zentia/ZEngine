#ifndef MEGALIGHTS_COMPOSE_INL
#define MEGALIGHTS_COMPOSE_INL

highp vec3 megalights_decode_normal(highp vec3 packed)
{
    return normalize(packed * 2.0 - 1.0);
}

highp vec3 megalights_stable_contrib(highp vec3 world_pos,
                                     highp vec3 N,
                                     highp vec3 V,
                                     highp vec3 F0,
                                     highp vec3 basecolor,
                                     highp float metallic,
                                     highp float roughness,
                                     highp vec3 origin_samplecube_N,
                                     highp vec3 origin_samplecube_R)
{
    highp vec3 stable = vec3(0.0);

    {
        highp vec3 L   = normalize(scene_directional_light.direction);
        highp float NoL = min(dot(N, L), 1.0);
        if (NoL > 0.0)
        {
            highp float shadow;
            {
                highp vec4 position_clip = directional_light_proj_view * vec4(world_pos, 1.0);
                highp vec3 position_ndc  = position_clip.xyz / position_clip.w;
                highp vec2 uv            = ndcxy_to_uv(position_ndc.xy);
                highp float closest_depth = texture(directional_light_shadow, uv).r + 0.000075;
                highp float current_depth = position_ndc.z;
                shadow = (closest_depth >= current_depth) ? 1.0 : 0.0;
            }
            if (shadow > 0.0)
            {
                stable += BRDF(L, V, N, F0, basecolor, metallic, roughness) * scene_directional_light.color * NoL;
            }
        }
    }

    highp vec3 La = basecolor * ambient_light;

    highp vec3 irradiance = texture(irradiance_sampler, origin_samplecube_N).rgb;
    highp vec3 diffuse    = irradiance * basecolor;
    highp vec3 F          = F_SchlickR(clamp(dot(N, V), 0.0, 1.0), F0, roughness);
    highp vec2 brdfLUT    = texture(brdfLUT_sampler, vec2(clamp(dot(N, V), 0.0, 1.0), roughness)).rg;
    highp float lod       = roughness * MAX_REFLECTION_LOD;
    highp vec3 reflection = textureLod(specular_sampler, origin_samplecube_R, lod).rgb;
    highp vec3 specular   = reflection * (F * brdfLUT.x + brdfLUT.y);
    highp vec3 kD         = (1.0 - F) * (1.0 - metallic);
    highp vec3 Libl       = kD * diffuse + specular;

    return stable + La + Libl;
}

#endif
