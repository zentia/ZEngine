#pragma once

class RenderSystem;

// Build the per-frame render/RHI command lists for parallel rendering.
void BuildRenderSystemFrameCommands(RenderSystem& render_system, float delta_time, uint32_t frame_draw_list_slot);
