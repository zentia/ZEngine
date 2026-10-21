#pragma once

#include "Interface/RHI.h"
#include "Runtime/Core/Math/Matrix4.h"
#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Core/Math/Vector4.h"
#include "Runtime/Function/Render/RenderGPUResource.h"
#include "Runtime/Function/Render/RenderType.h"

static const uint32_t s_PointLightShadowMapDimension = 2048;
static const uint32_t s_DirectionalLightShadowMapDimension = 4096;

// TODO: 64 may not be the best
static uint32_t const s_MeshPerDrawcallMaxInstanceCount = 64;
static uint32_t const s_MeshVertexBlendingMaxJointCount = 1024;
static uint32_t const s_MaxPointLightCount = 15;
// should sync the macros in "shader_include/constants.h"

struct SceneDirectionalLight
{
    Vector3 direction;
    float _padding_direction;
    Vector3 color;
    float _padding_color;
};

struct ScenePointLight
{
    Vector3 position;
    float radius;
    Vector3 intensity;
    float _padding_intensity;
};

// Lumen 的间接光结果：CPU probe 网格 -> per-frame uniform 的桥。
//
// 故意是纯 POD 且**不依赖任何 Lumen 头**：RenderScene 和 GPU 侧都不应该知道
// Lumen 的存在，否则 Lumen 被 CMake 剥掉的配置（Web / 关掉 Vulkan 的 Windows）
// 会连 RenderScene 一起编译不过。
struct SceneIndirectLighting
{
    /// 间接辐照度（rgb）。已经乘过 r.Lumen.IndirectIntensity。
    Vector3 irradiance {0.0f, 0.0f, 0.0f};
    /// 混合权重：0 = 沿用 baked irradiance cubemap，1 = 完全用 Lumen 的间接光。
    /// 之所以用权重而不是"有没有值"：一个全黑的场景（被封闭几何体包住）算出来的
    /// 辐照度本来就是 0，那是有数据，不是没数据。
    float weight {0.0f};
    /// 产出这份数据的帧号，用来判断"是不是陈旧的"。
    uint64_t frame_index {0};

    /// 场景级的反射辐射（rgb）。已经对所有采样点做过平均。
    ///
    /// 注意这是**场景平均**，不是逐像素反射：CPU 路径既没有屏幕探针也没有 GBuffer，
    /// 拿不到逐像素的着色点和视线方向。它在 shader 里的角色等价于一个粗糙的
    /// specular 环境项（就像 baked specular cubemap 的最粗一级 mip）。
    /// 真要做逐像素反射必须有 GPU 侧的屏幕探针，那是另一件事。
    Vector3 reflection {0.0f, 0.0f, 0.0f};
    /// 反射的混合权重：0 = 沿用 baked specular cubemap，1 = 完全用 Lumen 的反射。
    /// 与 weight 分开是因为 GI 和反射可以各自独立开关（r.Lumen.GlobalIllumination
    /// 和 r.Lumen.Reflections 是两个 CVar）。
    float reflection_weight {0.0f};

    /// 打包成 MainCameraPerFrame::lumen_indirect。
    ///
    /// 放在这里而不是在两个 UpdatePerFrameBuffer 里各写一遍：那个搬运有两份实现
    /// （Function/Render/RenderResource.cpp 是 Vulkan 专属、RenderResourceDx12.cpp
    /// 是 Windows 专属），手抄两次迟早会漂，而且这两份在 Apple 上都编译不到 ——
    /// 收敛成一个函数，冒烟测试才有一个所有平台都能链接的断言对象。
    Vector4 ToUniform() const { return Vector4(irradiance, weight); }

    /// 打包成 MainCameraPerFrame::lumen_reflection。同样收敛成一处，理由同上。
    Vector4 ToReflectionUniform() const { return Vector4(reflection, reflection_weight); }
};

struct MainCameraPerFrame
{
    Matrix4x4 proj_view_matrix;
    Vector3 camera_position;
    float _padding_camera_position;
    Vector3 ambient_light;
    float _padding_ambient_light;
    uint32_t point_light_num;
    uint32_t show_skybox {1};
    uint32_t _padding_point_light_num_2;
    uint32_t _padding_point_light_num_3;
    ScenePointLight scene_point_lights[s_MaxPointLightCount];
    SceneDirectionalLight scene_directional_light;
    Matrix4x4 directional_light_proj_view;
    /// LWC (UE PreViewTranslation + render tile); valid when r.LWC.Enable is on.
    Vector3 pre_view_translation {0.0f, 0.0f, 0.0f};
    float _padding_pre_view_translation {0.0f};
    Vector3 render_tile {0.0f, 0.0f, 0.0f};
    float _padding_render_tile {0.0f};
    /// Scene/game panel in framebuffer pixels (x, y, width, height). Used by deferred sky
    /// to rebuild world rays from SV_Position (sub-viewport safe).
    Vector4 viewport_rect {0.0f, 0.0f, 1.0f, 1.0f};
    /// Lumen 的间接光：rgb = 辐照度，a = 混合权重。
    /// 0 = 沿用 baked irradiance cubemap（也就是 Lumen 关掉时的行为），1 = 完全用 Lumen。
    ///
    /// 之所以是"权重 + 颜色"而不是"有没有颜色"：被封闭几何体包住的场景算出来的辐照度
    /// 本来就是 0，那是有数据，不是没数据。
    /// probe 网格今天还没有 GPU 侧的 3D 纹理体积，所以 CPU 只发布一个全场景平均值；
    /// 等体积纹理就位，这里换成纹理绑定即可，struct 不用动。
    Vector4 lumen_indirect {0.0f, 0.0f, 0.0f, 0.0f};
    /// Lumen 的反射：rgb = 场景级反射辐射，a = 混合权重。
    /// 0 = 沿用 baked specular cubemap，1 = 完全用 Lumen 的反射。
    ///
    /// 与 lumen_indirect 分开是两个独立权重：GI 和反射由两个 CVar 各自开关。
    Vector4 lumen_reflection {0.0f, 0.0f, 0.0f, 0.0f};
};

struct RenderMeshInstance
{
    float enable_vertex_blending;
    float _padding_enable_vertex_blending_1;
    float _padding_enable_vertex_blending_2;
    float _padding_enable_vertex_blending_3;
    Matrix4x4 model_matrix;
};

struct MeshDrawPerDrawcall
{
    RenderMeshInstance mesh_instances[s_MeshPerDrawcallMaxInstanceCount];
};

struct MeshDrawPerDrawcallVertexBlending
{
    Matrix4x4 joint_matrices[s_MeshVertexBlendingMaxJointCount * s_MeshPerDrawcallMaxInstanceCount];
};

struct MeshMaterialUniform
{
    Vector4 baseColorFactor {0.0f, 0.0f, 0.0f, 0.0f};

    float metallicFactor = 0.0f;
    float roughnessFactor = 0.0f;
    float normalScale = 0.0f;
    float occlusionStrength = 0.0f;

    Vector3 emissiveFactor = {0.0f, 0.0f, 0.0f};
    uint32_t is_blend = 0;
    uint32_t is_double_sided = 0;
};

struct PointLightShadowPerFrame
{
    uint32_t point_light_num;
    uint32_t _padding_point_light_num_1;
    uint32_t _padding_point_light_num_2;
    uint32_t _padding_point_light_num_3;
    Vector4 point_lights_position_and_radius[s_MaxPointLightCount];
};

struct DirectionalLightShadowPerFrame
{
    Matrix4x4 light_proj_view;
};

// Shadow draw passes reuse the main-camera mesh instance / vertex-blending SSBO layouts.
using MeshShadowPerDrawcall = MeshDrawPerDrawcall;
using MeshShadowPerDrawcallVertexBlending = MeshDrawPerDrawcallVertexBlending;

struct AxisDrawStorage
{
    Matrix4x4 model_matrix = Matrix4x4::IDENTITY;
    uint32_t selected_axis = 3;
};

struct ParticleBillboardPerFrame
{
    Matrix4x4 proj_view_matrix;
    Vector3 right_direction;
    float _padding_right_direction;
    Vector3 up_direction;
    float _padding_up_direction;
    Vector3 forward_direction;
    float _padding_forward_direction;
};

struct ParticleCollisionPerFrame
{
    Matrix4x4 view_matrix;
    Matrix4x4 proj_view_matrix;
    Matrix4x4 proj_inv_matrix;
};

struct PickPassPerFrame
{
    Matrix4x4 proj_view_matrix;
    uint32_t rt_width;
    uint32_t rt_height;
};

struct PickPassPerDrawcall
{
    Matrix4x4 model_matrices[s_MeshPerDrawcallMaxInstanceCount];
    uint32_t node_ids[s_MeshPerDrawcallMaxInstanceCount];
    float enable_vertex_blendings[s_MeshPerDrawcallMaxInstanceCount];
};

using PickPassPerDrawcallVertexBlending = MeshDrawPerDrawcallVertexBlending;

// nodes
struct RenderMeshNode
{
    const Matrix4x4* model_matrix {nullptr};
    const Matrix4x4* joint_matrices {nullptr};
    uint32_t joint_count {0};
    RenderMeshGPUResource* ref_mesh {nullptr};
    RenderMaterialGPUResource* ref_material {nullptr};

    uint32_t node_id;
    bool enable_vertex_blending {false};
};

struct RenderAxisNode
{
    Matrix4x4 model_matrix {Matrix4x4::IDENTITY};
    RenderMeshGPUResource* ref_mesh {nullptr};
    size_t mesh_asset_id {0};

    uint32_t node_id;
    bool enable_vertex_blending {false};
};
