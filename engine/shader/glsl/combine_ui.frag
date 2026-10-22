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
    
    // UI over scene.
    //
    // The UI layer (backup_even) is cleared to 0 at the start of the UI subpass and
    // the UI pipelines blend colour as src*a + dst*(1-a) into it, so its rgb is
    // already premultiplied by coverage and its alpha is the coverage
    // (ZSlateEditorOverlay accumulates alpha as "over"). Premultiplied over is
    // therefore ui.rgb + scene * (1 - ui.a); mix() would multiply by a twice.
    //
    // This used to skip every pixel with ui.a >= 254/255 ("tonemap leaves alpha=1
    // in backup_even") and gamma-encode the rest -- written when the UI layer still
    // held the tonemap output. Since backup_even is cleared before any UI is drawn,
    // a == 1 can only mean opaque UI, and that test threw exactly those pixels away:
    // every editor panel vanished and the swapchain showed the scene edge to edge.
    out_color = vec4(ui_color.rgb + scene_color.rgb * (1.0 - ui_color.a), 1.0);
}
