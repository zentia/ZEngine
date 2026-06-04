// UE-style LWC helpers (ZEngine render tile + pre-view translation).
// CPU mirror: LargeWorldCoordinates.h, MainCameraPerFrame tail fields.

#ifndef Z_LWC_RENDER_TILE_SIZE
#define Z_LWC_RENDER_TILE_SIZE 2097152.0
#endif

struct ZLwcFrame
{
    float3 pre_view_translation;
    float _pad_pre_view;
    float3 render_tile;
    float _pad_render_tile;
};

// Reconstruct absolute world from render-space sample (model matrices are already render-space when LWC is on).
float3 ZLwcRenderToAbsolute(float3 render_world, ZLwcFrame lwc)
{
    return render_world + lwc.render_tile;
}

float3 ZLwcAbsoluteToRender(float3 absolute_world, ZLwcFrame lwc)
{
    return absolute_world - lwc.render_tile;
}

float3 ZLwcCameraAbsolute(float3 render_camera, ZLwcFrame lwc)
{
    return ZLwcRenderToAbsolute(render_camera, lwc);
}
