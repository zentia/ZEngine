// Include after declaring `MainCameraPerFrame per_frame` in SSBO/cbuffer binding 0.

#define proj_view_matrix per_frame.proj_view_matrix
#define camera_position per_frame.camera_position
#define ambient_light per_frame.ambient_light
#define point_light_num per_frame.point_light_num
#define show_skybox per_frame.show_skybox
#define scene_point_lights per_frame.scene_point_lights
#define scene_directional_light per_frame.scene_directional_light
#define directional_light_proj_view per_frame.directional_light_proj_view
#define pre_view_translation per_frame.pre_view_translation
#define render_tile per_frame.render_tile
