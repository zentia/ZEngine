highp vec3 V = normalize(camera_position - in_world_position);
highp vec3 R = reflect(-V, N);

highp vec3 origin_samplecube_N = vec3(N.x, N.z, N.y);
highp vec3 origin_samplecube_R = vec3(R.x, R.z, R.y);

highp vec3 F0 = mix(vec3(dielectric_specular, dielectric_specular, dielectric_specular), basecolor, metallic);

// direct light specular and diffuse BRDF contribution
highp vec3 Lo = vec3(0.0, 0.0, 0.0);
for (highp int light_index = 0; light_index < int(point_light_num) && light_index < m_max_point_light_count;
     ++light_index)
{
    highp vec3  point_light_position = scene_point_lights[light_index].position;
    highp float point_light_radius   = scene_point_lights[light_index].radius;

    highp vec3  L   = normalize(point_light_position - in_world_position);
    highp float NoL = min(dot(N, L), 1.0);

    // point light
    highp float distance             = length(point_light_position - in_world_position);
    highp float distance_attenuation = 1.0 / (distance * distance + 1.0);
    highp float radius_attenuation   = 1.0 - ((distance * distance) / (point_light_radius * point_light_radius));

    highp float light_attenuation = radius_attenuation * distance_attenuation * NoL;
    if (light_attenuation > 0.0)
    {
        highp float shadow;
        {
            // world space to light view space
            // identity rotation
            // Z - Up
            // Y - Forward
            // X - Right
            highp vec3 position_view_space = in_world_position - point_light_position;

            highp vec3 position_spherical_function_domain = normalize(position_view_space);

            // use abs to avoid divergence
            // z > 0
            // (x_2d, y_2d, 0) + (0, 0, 1) = λ ((x_sph, y_sph, z_sph) + (0, 0, 1))
            // (x_2d, y_2d) = (x_sph, y_sph) / (z_sph + 1)
            // z < 0
            // (x_2d, y_2d, 0) + (0, 0, -1) = λ ((x_sph, y_sph, z_sph) + (0, 0, -1))
            // (x_2d, y_2d) = (x_sph, y_sph) / (-z_sph + 1)
            highp vec2 position_ndcxy =
                position_spherical_function_domain.xy / (abs(position_spherical_function_domain.z) + 1.0);

            // use sign to avoid divergence
            // -1.0 to 0
            // 1.0 to 1
            highp vec2  uv = ndcxy_to_uv(position_ndcxy);
            highp float layer_index =
                (0.5 + 0.5 * sign(position_spherical_function_domain.z)) + 2.0 * float(light_index);

            highp float depth          = texture(point_lights_shadow, vec3(uv, layer_index)).r + 0.000075;
            highp float closest_length = (depth)*point_light_radius;

            highp float current_length = length(position_view_space);

            shadow = (closest_length >= current_length) ? 1.0f : -1.0f;
        }

        if (shadow > 0.0f)
        {
            highp vec3 En = scene_point_lights[light_index].intensity * light_attenuation;
            Lo += BRDF(L, V, N, F0, basecolor, metallic, roughness) * En;
        }
    }
};

// direct ambient contribution
highp vec3 La = vec3(0.0f, 0.0f, 0.0f);
La            = basecolor * ambient_light;

// indirect environment
//
// Lumen 的 probe 网格只在有权重的时候才接管间接漫反射：权重 0（Lumen 关掉 / 还没产出）
// 时这里的行为跟以前一模一样，继续用 baked irradiance cubemap。两者都是辐照度，
// 所以是**替换**而不是相加 —— 加起来会把天光算两遍。
highp vec3 irradiance = texture(irradiance_sampler, origin_samplecube_N).rgb;
irradiance            = mix(irradiance, lumen_indirect.rgb, lumen_indirect.a);
highp vec3 diffuse    = irradiance * basecolor;

highp vec3 F       = F_SchlickR(clamp(dot(N, V), 0.0, 1.0), F0, roughness);
highp vec2 brdfLUT = texture(brdfLUT_sampler, vec2(clamp(dot(N, V), 0.0, 1.0), roughness)).rg;

highp float lod        = roughness * MAX_REFLECTION_LOD;
highp vec3  reflection = textureLod(specular_sampler, origin_samplecube_R, lod).rgb;
// Lumen 的反射同样只替换不相加：两者都是环境反射辐射，加起来会算两遍。
// 权重 0（Lumen 关掉 / 反射关掉 / 一个页面都还没更新）时继续用 baked specular cubemap。
reflection            = mix(reflection, lumen_reflection.rgb, lumen_reflection.a);
highp vec3  specular   = reflection * (F * brdfLUT.x + brdfLUT.y);

highp vec3 kD = 1.0 - F;
kD *= 1.0 - metallic;
highp vec3 Libl = (kD * diffuse + specular);

// directional light
{
    highp vec3  L   = normalize(scene_directional_light.direction);
    highp float NoL = min(dot(N, L), 1.0);

    if (NoL > 0.0)
    {
        highp float shadow;
        {
            // CSM：先按视图深度选级，再用**那一级**的 light proj-view 投影。
            // 只有一级时（count == 1）索引恒为 0，结果与改之前逐字节一致。
            highp int cascade = z_select_shadow_cascade(in_world_position);

            highp vec4 position_clip = directional_light_proj_view_cascades[cascade] * vec4(in_world_position, 1.0);
            highp vec3 position_ndc  = position_clip.xyz / position_clip.w;

            highp vec2 uv = ndcxy_to_uv(position_ndc.xy);

            highp float closest_depth = texture(directional_light_shadow, vec3(uv, float(cascade))).r + 0.000075;
            highp float current_depth = position_ndc.z;

            shadow = (closest_depth >= current_depth) ? 1.0f : -1.0f;
        }

        if (shadow > 0.0f)
        {
            highp vec3 En = scene_directional_light.color * NoL;
            Lo += BRDF(L, V, N, F0, basecolor, metallic, roughness) * En;
        }
    }
}

// screen-space ambient occlusion（只有 deferred_lighting_ssao.frag 定义这个宏）
//
// 三处各乘各的（不是把整个结果一把乘上去）：
//   * 环境光 La 与 IBL 漫反射 × r（间接光可见度）——遮蔽本来就是"这一点看得见多少天空"；
//   * IBL 镜面 × specular occlusion：Lagarde 2014（"Moving Frostbite to PBR"），也就是
//     UE 的 GetSpecularOcclusion；粗糙度越低越不受漫反射 AO 影响；
//   * 直接光 Lo × g（= lerp(1, r, DirectLightingStrength)，在 AppleSsaoPass 里算好）。
//     物理上直接光不该被 AO 压（UE 的做法），但完全不压的话被阳光照到的墙角毫无
//     接触感 —— URP 折中给 0.25，r.SSAO.DirectLightingStrength 可调。
// AO 关掉时绑的是 1x1 白图（r = g = 1）：spec_occlusion = clamp(pow(NoV + 1, e) - 1 + 1) = 1，
// 三个乘子全是 1 ⇒ 结果与没有这一段时逐位一致。
#ifdef Z_DEFERRED_SCREEN_SPACE_AO
{
    highp vec2  screen_space_ao = z_load_screen_space_ao();
    highp float indirect_ao     = screen_space_ao.r;
    highp float direct_ao       = screen_space_ao.g;
    highp float ao_NoV          = clamp(dot(N, V), 0.0, 1.0);
    highp float specular_ao =
        clamp(pow(ao_NoV + indirect_ao, exp2(-16.0 * roughness - 1.0)) - 1.0 + indirect_ao, 0.0, 1.0);

    La   = La * indirect_ao;
    Libl = kD * diffuse * indirect_ao + specular * specular_ao;
    Lo   = Lo * direct_ao;
}
#endif

// result
result_color = Lo + La + Libl;
