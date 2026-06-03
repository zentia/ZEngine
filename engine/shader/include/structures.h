struct MeshInstance
{
    highp float enable_vertex_blending;
    highp float _padding_enable_vertex_blending_1;
    highp float _padding_enable_vertex_blending_2;
    highp float _padding_enable_vertex_blending_3;
    highp mat4  model_matrix;
};

struct MeshDrawPerDrawcall
{
    MeshInstance mesh_instances[m_mesh_per_drawcall_max_instance_count];
};

struct MeshDrawPerDrawcallVertexBlending
{
    highp mat4 joint_matrices[m_mesh_vertex_blending_max_joint_count * m_mesh_per_drawcall_max_instance_count];
};

struct MeshVertexJointBinding
{
    highp ivec4 indices;
    highp vec4  weights;
};

// Per-frame SSBO lighting payloads (C++ mirror: ScenePointLight / SceneDirectionalLight in RenderCommon.h).
struct DirectionalLight
{
    highp vec3 direction;
    highp float _padding_direction;
    highp vec3 color;
    highp float _padding_color;
};

struct PointLight
{
    highp vec3 position;
    highp float radius;
    highp vec3 intensity;
    highp float _padding_intensity;
};

// Shadow-pass per-frame SSBO payloads (C++ mirror: PointLightShadowPerFrame / DirectionalLightShadowPerFrame).
struct PointLightShadowPerFrame
{
    highp uint point_light_num;
    highp uint _padding_point_light_num_1;
    highp uint _padding_point_light_num_2;
    highp uint _padding_point_light_num_3;
    highp vec4 point_lights_position_and_radius[m_max_point_light_count];
};

struct DirectionalLightShadowPerFrame
{
    highp mat4 light_proj_view;
};

struct PickPassPerFrame
{
    highp mat4 proj_view_matrix;
    highp uint rt_width;
    highp uint rt_height;
};

struct PickPassPerDrawcall
{
    highp mat4  model_matrices[m_mesh_per_drawcall_max_instance_count];
    highp uint  node_ids[m_mesh_per_drawcall_max_instance_count];
    highp float enable_vertex_blendings[m_mesh_per_drawcall_max_instance_count];
};

// Main-camera per-frame SSBO (C++ mirror: MainCameraPerFrame in RenderCommon.h).
struct MainCameraPerFrame
{
    highp mat4 proj_view_matrix;
    highp vec3 camera_position;
    highp float _padding_camera_position;
    highp vec3 ambient_light;
    highp float _padding_ambient_light;
    highp uint point_light_num;
    highp uint show_skybox;
    highp uint _padding_point_light_num_2;
    highp uint _padding_point_light_num_3;
    PointLight scene_point_lights[m_max_point_light_count];
    DirectionalLight scene_directional_light;
    highp mat4 directional_light_proj_view;
};

struct AxisDrawStorage
{
    highp mat4 model_matrix;
    highp uint selected_axis;
};

struct ParticleBillboardPerFrame
{
    highp mat4 proj_view_matrix;
    highp vec3 right_direction;
    highp float _padding_right_direction;
    highp vec3 up_direction;
    highp float _padding_up_direction;
    highp vec3 forward_direction;
    highp float _padding_forward_direction;
};

struct ParticleCollisionPerFrame
{
    highp mat4 view_matrix;
    highp mat4 proj_view_matrix;
    highp mat4 proj_inv_matrix;
};
