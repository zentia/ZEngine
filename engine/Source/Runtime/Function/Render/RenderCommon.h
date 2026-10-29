#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "Interface/RHI.h"
#include "Runtime/Core/Math/Matrix4.h"
#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Core/Math/Vector4.h"
#include "Runtime/Function/Render/MaterialCustomParamSlots.h"
#include "Runtime/Function/Render/RenderGPUResource.h"
#include "Runtime/Function/Render/RenderType.h"

static const uint32_t s_PointLightShadowMapDimension = 2048;
static const uint32_t s_DirectionalLightShadowMapDimension = 4096;

// TODO: 64 may not be the best
static uint32_t const s_MeshPerDrawcallMaxInstanceCount = 64;
static uint32_t const s_MeshVertexBlendingMaxJointCount = 1024;
static uint32_t const s_MaxPointLightCount = 15;
// CSM（级联阴影）的级联数上限。shader 侧是 `m_max_cascade_count`
// （engine/shader/include/constants.h），两边必须一致 —— per-frame 结构里那个
// 数组的长度就靠它，对不上就是 GPU 读到乱数据而且不报错。
static uint32_t const s_MaxCascadeCount = 4;
// CSM 的 split 距离在"对数分布"和"线性分布"之间插值：0 = 全线性，1 = 全对数。
// 0.5 是 UE/Unity 都用的折中值 —— 近处密一点（阴影分辨率给近处），远处不至于
// 密到浪费级数。TODO: 接 CVar，现在先当常量，改一处就能全局生效。
static float const s_CascadeSplitLambda = 0.5f;
// should sync the macros in "shader_include/constants.h"

struct SceneDirectionalLight
{
    Vector3 direction;
    float _padding_direction;
    Vector3 color;
    float _padding_color;
};

// Per-frame point/spot light carried in the MainCameraPerFrame UBO.
//
// Layout is 64 bytes (four vec4 pairs) so it is safe under both std140 and
// std430:
//   vec4_0 = {position.xyz, radius}
//   vec4_1 = {intensity.xyz, type}
//   vec4_2 = {spot_direction.xyz, spot_inner_cos}
//   vec4_3 = {spot_outer_cos, _padding_spot_0/1/2}
// Vector3 is 12 bytes (3 floats, 4-byte aligned), so each vec3 MUST be followed
// by exactly one float to complete a 16-byte vec4 pair -- hence the three
// trailing pads. With s_MaxPointLightCount == 15 the array grows from 240B to
// 960B -- negligible.
//
// `type` is a float (not an int/uint) on purpose: the shader branches on it
// with a plain float compare, which sidesteps int-alignment surprises across
// the four RHIs. 0.0 = point (unchanged behaviour), 1.0 = spot.
//
// The spot fields (spot_direction / spot_inner_cos / spot_outer_cos) are zero
// for point lights; the shader only reads them inside the `type > 0.5` branch.
struct ScenePointLight
{
    Vector3 position;
    float radius;
    Vector3 intensity;
    float type {0.0f};

    // ---- spot-only payload (point lights leave these at zero) ---------------
    // vec4_2 = {spot_direction.xyz, spot_inner_cos}; vec4_3 = {spot_outer_cos,
    // _padding_spot_0/1/2}. The three trailing pads are REQUIRED: Vector3 is 12
    // bytes (3 floats, 4-byte aligned), so without them the C++ struct is 60B
    // while the std140 shader struct is 64B (vec3 array elements pad to vec4
    // stride) -- a silent CPU/GPU desync. The static_assert below pins this.
    Vector3 spot_direction {Vector3(0.0f, 0.0f, 0.0f)};  // unit vector
    float spot_inner_cos {0.0f};
    float spot_outer_cos {0.0f};
    float _padding_spot_0 {0.0f};
    float _padding_spot_1 {0.0f};
    float _padding_spot_2 {0.0f};
};

// Pin the UBO element stride: 32 bytes = two vec4 pairs, safe under std140 and
// std430. The shader-side mirrors (scene_lighting_structs.hlsli / structures.h)
// must stay bit-identical to this layout -- if this assert fires, someone changed
// a field and desynced the GPU from the CPU (which fails silently as garbled
// light data, not a hard error).
static_assert(sizeof(ScenePointLight) == 64,
              "ScenePointLight must be 64 bytes (four vec4 pairs) to match the "
              "std140/std430 shader-side PointLight struct. If this fires, a field "
              "was added/removed and the CPU layout desynced from the GPU -- which "
              "fails silently as garbled light data, not a hard error. Note Vector3 "
              "is 12 bytes, so the spot payload needs THREE trailing pad floats to "
              "complete the fourth vec4 pair.");

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

    // ---- CSM（级联阴影）----
    //
    // 三份镜像必须逐字节一致：C++ 在这里，GLSL 在
    // engine/shader/include/structures.h，HLSL 在
    // engine/shader/hlsl/common/scene_lighting_structs.hlsli。

    /// 主相机朝向（世界空间单位向量）。采样侧要靠它把片元位置换算成"视图深度"
    /// 再去跟 split 距离比 —— per-frame 里原来只有 camera_position，光有位置
    /// 算不出沿视线的深度（径向距离 ≠ 视图深度，边缘会选错级）。
    Vector3 camera_forward {0.0f, 0.0f, -1.0f};
    float _padding_camera_forward {0.0f};
    /// 每一级 cascade 自己的 light proj-view。生成侧按级各画一次到 array 的对应
    /// slice，采样侧按深度挑一个出来用。
    Matrix4x4 directional_light_proj_view_cascades[s_MaxCascadeCount];
    /// 每一级的远平面，单位是**视图深度**（沿 camera_forward 的投影距离，正值、
    /// 递增）。x/y/z/w = 第 0/1/2/3 级。级数不足 4 时高位填 +inf，配合下面的
    /// count 做 clamp。
    Vector4 directional_light_cascade_split_depth {0.0f, 0.0f, 0.0f, 0.0f};
    /// 实际生效的级数。1 = 退化成单张（拿不到有效深度范围时的行为，与 CSM 之前一致）。
    uint32_t directional_light_cascade_count {1};
    float _padding_cascade_count_1 {0.0f};
    float _padding_cascade_count_2 {0.0f};
    float _padding_cascade_count_3 {0.0f};
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

// 描画一个 part 时的材质常量。成员顺序 / padding 必须与三套内置 shader 里的镜像
// 声明**逐字节**对上：
//   - Vulkan / Metal / WebGL：engine/shader/glsl/mesh_gbuffer.frag（std140）
//   - DX12：engine/shader/hlsl/common/render_storage_structs.hlsli（cbuffer 打包）
//
// 改这里必须同步改那两处，否则 CPU 写进 UBO 的字节会被 shader 按错的 offset 读走。
//
// 两个容易踩的点：
//   1. C++ 侧 Vector4 的对齐是 4 而不是 16，而 std140 下 vec4 必须落在 16 字节边界。
//      is_double_sided 结束在 52，下一个 vec4 在 GPU 上从 **64** 开始 —— 所以下面
//      那三个 _padding 不是可选的装饰，是布局的一部分。
//   2. 别把 customParams 写成 `float customParams[16]`：std140 里 **数组元素的
//      stride 会被撑到 vec4**，16 个 float 会占 256 字节而不是 64。用 vec4 数组，
//      stride 天然就是 16。
struct MeshMaterialUniform
{
    Vector4 baseColorFactor {0.0f, 0.0f, 0.0f, 0.0f};  // offset 0

    float metallicFactor = 0.0f;      // 16
    float roughnessFactor = 0.0f;     // 20
    float normalScale = 0.0f;         // 24
    float occlusionStrength = 0.0f;   // 28

    Vector3 emissiveFactor = {0.0f, 0.0f, 0.0f};  // 32（vec3 起始要 16 对齐）
    uint32_t is_blend = 0;                        // 44
    uint32_t is_double_sided = 0;                 // 48

    // 52 -> 64。见上面第 1 点。
    //
    // vt_flags（原 _padding_0）：材质里的 Virtual Texture 槽位开关，见 MaterialCustomParamSlots.h
    // （k_MaterialVtFlag_*）。置位时 customParams[2] / [3] 被 VT 参数占用。0 = 与没有这个功能时一样。
    uint32_t vt_flags = 0;    // 52
    uint32_t _padding_1 = 0;  // 56
    uint32_t _padding_2 = 0;  // 60

    /// 运行时属性块的落点：4 个 vec4 = 16 个标量槽。
    ///
    /// 内置 shader 按**约定**消费它们（[0].x 溶解阈值、[0].y 边缘宽度、[1] 边缘
    /// 颜色，见 shader/include/material_custom_params.h 与它的 HLSL 镜像）。
    /// [2] / [3] 是留给项目的自留槽，shader 不解释。
    ///
    /// 脚本侧不直接碰下标 —— 走 `BaseRenderer::SetFloat("_DissolveAmount", v)`，
    /// 由 `MaterialCustomParamNames` 那张静态表把名字翻成 (槽位, 分量)。
    ///
    /// 这不是 Unity 那种"任意 shader 的任意具名属性"：名字是引擎写死的，用户加不
    /// 了新属性名。要那个得先通 ShaderLab 运行时 + 拿到属性元数据（见报告）。
    Vector4 customParams[k_MaterialCustomParamCount] {};  // 64 / 80 / 96 / 112

    // total 128
};

static_assert(sizeof(MeshMaterialUniform) == 128, "MeshMaterialUniform must stay 128 bytes (ShaderLab blob, std140)");
static_assert(offsetof(MeshMaterialUniform, vt_flags) == 52, "vt_flags must sit at offset 52 (shader mirrors depend on it)");
static_assert(offsetof(MeshMaterialUniform, customParams) == 64, "customParams must start at 64");

/// customParams 是 float4，VT 参数是 uint：按位模式存取（HLSL 侧 asuint），不做数值转换。
inline float MaterialVtBitsToFloat(uint32_t value)
{
    float result;
    static_assert(sizeof(result) == sizeof(value), "float/uint32 size");
    std::memcpy(&result, &value, sizeof(result));
    return result;
}

inline uint32_t MaterialVtFloatToBits(float value)
{
    uint32_t result;
    std::memcpy(&result, &value, sizeof(result));
    return result;
}

/// 把 VT 槽位写进一块已经填好的材质 UBO 字节（128 字节）。`slot.enabled == false` 时**什么都不改**——
/// 调用方先填好常规字段，再叫这个函数，没启用 VT 的材质逐字节与过去一致。
/// 启用时改三处：vt_flags、customParams[2]、customParams[3]（见 MaterialCustomParamSlots.h）。
inline void ApplyMaterialVirtualTextureSlot(const MaterialVirtualTextureSlot& slot, uint8_t* ubo_bytes)
{
    if (!slot.enabled || ubo_bytes == nullptr)
    {
        return;
    }
    MeshMaterialUniform& ubo = *reinterpret_cast<MeshMaterialUniform*>(ubo_bytes);
    ubo.vt_flags = k_MaterialVtFlag_BaseColor | (slot.srgb ? k_MaterialVtFlag_Srgb : 0u);
    ubo.customParams[k_MaterialVtCustomParamSlot_Layout] =
        Vector4(MaterialVtBitsToFloat(slot.page_size),
                MaterialVtBitsToFloat(slot.page_border),
                MaterialVtBitsToFloat(slot.mip_count),
                0.0f);
    ubo.customParams[k_MaterialVtCustomParamSlot_Extent] =
        Vector4(MaterialVtBitsToFloat(slot.virtual_width), MaterialVtBitsToFloat(slot.virtual_height), 0.0f, 0.0f);
}

/// 反过来：从 UBO 字节读出 VT 槽位（测试用）。vt_flags 位 0 没置位 ⇒ enabled = false。
inline MaterialVirtualTextureSlot ReadMaterialVirtualTextureSlot(const uint8_t* ubo_bytes)
{
    MaterialVirtualTextureSlot slot;
    const MeshMaterialUniform& ubo = *reinterpret_cast<const MeshMaterialUniform*>(ubo_bytes);
    slot.enabled = (ubo.vt_flags & k_MaterialVtFlag_BaseColor) != 0;
    slot.srgb = (ubo.vt_flags & k_MaterialVtFlag_Srgb) != 0;
    if (slot.enabled)
    {
        const Vector4& layout = ubo.customParams[k_MaterialVtCustomParamSlot_Layout];
        const Vector4& extent = ubo.customParams[k_MaterialVtCustomParamSlot_Extent];
        slot.page_size = MaterialVtFloatToBits(layout.x);
        slot.page_border = MaterialVtFloatToBits(layout.y);
        slot.mip_count = MaterialVtFloatToBits(layout.z);
        slot.virtual_width = MaterialVtFloatToBits(extent.x);
        slot.virtual_height = MaterialVtFloatToBits(extent.y);
    }
    return slot;
}

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

// Spot-light shadow: unlike the point light (6-face cube via geometry shader,
// Vulkan-only) a spot light needs a SINGLE 2D depth map along its direction --
// structurally identical to one directional-light cascade, just a perspective
// projection with fov = 2*outer_cone instead of an orthographic one. This keeps
// the shadow pass far simpler than the point-light cube path and lets it reuse
// the directional pass's 2D depth sampling.
//
// s_MaxSpotShadowCount caps how many spot lights cast shadows per frame (a
// budget, like s_MaxPointLightCount). Lights beyond the cap simply don't cast
// shadows (they still light, via the phase-1 cone branch).
static const uint32_t s_MaxSpotShadowCount = 4;
static const uint32_t s_SpotLightShadowMapDimension = 1024;

struct SpotLightShadowPerFrame
{
    // Number of spot lights with a valid shadow map this frame (<= s_MaxSpotShadowCount).
    uint32_t spot_shadow_num;
    uint32_t _padding_spot_shadow_num_1;
    uint32_t _padding_spot_shadow_num_2;
    uint32_t _padding_spot_shadow_num_3;

    // xyz = light world position, w = light attenuation range (shadow far plane).
    Vector4 spot_position_and_range[s_MaxSpotShadowCount];
    // Perspective view-projection along each spot's direction (fov = 2*outer_cone).
    Matrix4x4 spot_view_proj[s_MaxSpotShadowCount];
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
