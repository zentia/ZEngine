#version 310 es

#extension GL_GOOGLE_include_directive : enable

#include "constants.h"

layout(input_attachment_index = 0, set = 0, binding = 0) uniform highp subpassInput in_scene_color;

layout(input_attachment_index = 1, set = 0, binding = 1) uniform highp subpassInput in_ui_color;

layout(location = 0) out highp vec4 out_color;

void main()
{
    highp vec4 scene_color = subpassLoad(in_scene_color).rgba;
    
    highp vec4 ui_color = subpassLoad(in_ui_color).rgba;
    
    // Tonemap leaves alpha=1 in backup_even; only blend real UI (0 < a < 254/255).
    if (ui_color.a < (1.0 / 255.0) || ui_color.a >= (254.0 / 255.0))
    {
        out_color = vec4(scene_color.rgb, 1.0);
    }
    else
    {
        ui_color = vec4(pow(ui_color.r, 1.0 / 2.2), pow(ui_color.g, 1.0 / 2.2), pow(ui_color.b, 1.0 / 2.2), pow(ui_color.a, 1.0 / 2.2));
        out_color = vec4(mix(scene_color.rgb, ui_color.rgb, ui_color.a), 1.0);
    }
}
