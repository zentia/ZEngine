# SpotLight 实现方案（可直接落地）

> 目标：在现有 PointLight/DirectionalLight 通道上补齐 **SpotLight**，复用已有的
> per-frame UBO、光照循环、阴影 pass 骨架，改动收敛、可分阶段验证。
>
> 本文所有"现状"均为代码实测（文件:行号），非推测。

---

## ✅ 阶段 1 落地记录（2026-10-27，已完成并验证）

阶段 1（出光，无阴影）已全部落地并通过验证。**关键踩坑**：`Vector3` 是 12 字节
（3 float，4 字节对齐），不是 16。最初 spot payload 只放了 2 个 pad float，导致
C++ 结构体 60B 而 std140 shader 结构体 64B（vec3 数组元素 pad 到 vec4 stride）——
**静默 CPU/GPU desync**。靠 `static_assert(sizeof(ScenePointLight) == 64)` 当场抓出，
补第 3 个 pad 后修复。这条 assert 已永久钉死布局。

最终布局（64B = 四个 vec4 对）：
```
vec4_0 = {position.xyz, radius}
vec4_1 = {intensity.xyz, type}          // type: 0.0=point, 1.0=spot
vec4_2 = {spot_direction.xyz, spot_inner_cos}
vec4_3 = {spot_outer_cos, pad0, pad1, pad2}
```

**验证结果**：
- `SpotLightSmokeTest`（纯 CPU，header-only）：20/20 全绿（SmoothStep 边界/钳制/退化、
  锥内=1/锥外=0/中间平滑、单调递减、非法配置纠正）。
- ZRuntime 干净编译（含 64B static_assert）。
- GLSL `mesh.frag`（含 mesh_lighting.inl 锥角分支）glslangValidator 编译通过。
- HLSL `deferred_lighting.frag.hlsl`（含 scene_lighting_structs.hlsli）dxc 编译通过。
- 点光回归：type=0 时 shader 跳过锥角分支，行为逐位不变。

**改动文件**：`LightComponent.h`（+4 字段+getter）、`Light.h`（中间载体 PointLight +spot 字段）、
`RenderCommon.h`（ScenePointLight 64B + static_assert）、`RenderScene.cpp`（spot 收集）、
`RenderResourcePerFrame.cpp`（UBO 上传填 spot）、`SpotAttenuation.h`（纯核心，新）、
`SpotLightSmokeTest.cpp`（新）、两份 shader 结构体 + `mesh_lighting.inl`（锥角分支）、
两个 CMakeLists（测试 target + carve-out）。

**诚实边界**：纯 CPU 衰减算术 + 两侧 shader 编译已验；**真实渲染出光需真机**（放一个
Spot Light 组件、旋转方向、RenderDoc 抓帧看锥形光斑）。阶段 2（单面 2D 阴影）未开始。

---

## ✅ 阶段 2 落地记录（2026-10-27，数据层 + 纯核心已落地；pass 主体待真机）

阶段 2（聚光单面 2D 阴影）的**可本地验证部分**已落地。**pass 主体**（建 2D depth、
画深度、shader 采样）是 GPU/真机绑定，本机无法验证，未实现——见下方边界说明。

**已落地**：
- **纯核心 `SpotShadow.h`**（`spot_shadow::ShadowFovY / ShadowFovYHalfAngle / ShadowNear /
  ShadowFar / ShadowAspect`）：聚光阴影透视锥的标量语义。关键决策——阴影 fov = 2*outer_cone
  （正好包住整个光锥，不浪费分辨率也不裁锥缘）；近平面贴光源（0.01，避免 z=0 投影退化）；
  远平面 = 光衰减半径（超出即无光，无需阴影）；正方形图（锥圆形对称）。超半圆钳到 pi/2
  （透视投影物理上限 fov=180°）。
- **数据结构 `SpotLightShadowPerFrame`**（RenderCommon.h）：`spot_shadow_num` + 
  `spot_position_and_range[]`（xyz=pos, w=range）+ `spot_view_proj[]`（透视 view-proj）。
  `s_MaxSpotShadowCount=4`（每帧阴影聚光预算，超出的聚光仍出光但不投影），
  `s_SpotLightShadowMapDimension=1024`。结构上等价于"单级方向光 CSM"，只是透视而非正交——
  这让 pass 主体可以复用方向光的 2D depth 采样，比点光立方体（6 面 + geometry shader）简单得多。
- **验证**：`SpotShadowSmokeTest` 16/16 全绿（fov=2*outer_cone、负角归零、超半圆钳制、
  near<far、端到端锥半宽=光半径）；ZRuntime 干净编译。

**未落地（pass 主体，真机绑定）**：
- 新建 `SpotLightShadowPass`（仿 `DirectionalLightShadowPass` 的 2D depth 路径，1286 行级别样板：
  attachments/renderpass/framebuffer/descriptor/pipeline）。
- 接进 4 个后端管线模块（Desktop/Mobile/Apple/Web 的 `*RenderPipelineModule.cpp`）。
- 新 shadow depth shader（可复用方向光的 `mesh_directional_light_shadow_*`，改用透视 proj）。
- `mesh_lighting.inl` 聚光分支加阴影采样（用 `spot_view_proj` 变换 + 2D depth 比较）。
- `RenderScene` 填 `SpotLightShadowPerFrame`（算透视 view_proj：`MakePerspectiveMatrix(2*outer, 1, near, range)`
  × `MakeLookAtMatrix(pos, pos+dir, up)`）。

**为什么停下**：连**点光阴影在 DX12 上都还没建**（`PointLightPass.cpp:986` 明注 geometry-shader
cubemap 是 Vulkan-only），方向光 CSM 多级也是后端相关。聚光阴影 pass 主体是跨 4 后端、无法本地
验证的大工程——继续写只会堆无法验证的代码。数据层 + 纯核心已就位，真机一到即可按上述清单接线。

---

---

## 0. 现状盘点（接线点已全部确认）

SpotLight 本质是"带方向 + 锥角衰减的点光"。现有渲染链已经把点光走通，SpotLight
只需在 4 个环节各加一个字段/分支：

| 环节 | 文件 | 现状 |
|---|---|---|
| 组件 | `Function/Framework/Component/Light/LightComponent.h` | 只有 `m_Flux`(Vector3) + `m_Radius`(float)，**无类型/方向/锥角** |
| 场景收集 | `Function/Render/RenderScene.cpp:123` `SyncPointLightsFromLevel` | 遍历 `LightComponent` → 填 `PointLight{position, flux}` |
| per-frame UBO | `Function/Render/RenderCommon.h:36` `ScenePointLight` | `{position, radius, intensity, pad}` = **16 字节**，无方向/锥角 |
| UBO 上限 | `RenderCommon.h:17` `s_MaxPointLightCount = 15` | 单数组，点光/聚光共用 |
| shader 结构 | `shader/hlsl/common/scene_lighting_structs.hlsli:5` + `shader/include/structures.h:35` | `PointLight{position,radius,intensity}` |
| shader 光照循环 | `shader/include/mesh_lighting.inl:11-66` | 遍历 `scene_point_lights`，算 `radius_attenuation * distance_attenuation * NoL` |
| 阴影 pass | `Function/Render/Passes/PointLightPass.{h,cpp}`（1171 行） | 点光 6 面立方体阴影（Vulkan 用 GS 展开，DX12 未建） |

**关键约束**：`ScenePointLight` 是 16 字节对齐的 UBO 元素。加方向+锥角**不能破坏 std140/std430
布局**——最干净的做法是把每个聚光扩成 **32 字节**（两个 vec4），或复用现有 16 字节槽位、
把方向/锥角塞进第二个并行数组。方案 A 采用前者（单数组扩到 32B），语义最清晰。

---

## 1. 设计决策

### 1.1 复用点光通道，不新建独立 pass

SpotLight 与 PointLight 共用：
- 同一个 `scene_point_lights[]` UBO（扩字段）；
- 同一个 `mesh_lighting.inl` 光照循环（加锥角衰减分支）；
- 同一个 `PointLightShadowPass`（聚光阴影比点光**更简单**：只需 1 张 2D depth，不需要立方体 6 面）。

理由：点光通道已通到 4 个后端（Vulkan/DX12/Metal/WebGL2），新开独立通道 = 4 后端各写一遍。
复用则改动集中在结构体 + 一个 shader 分支。

### 1.2 光源类型用枚举区分，物理量沿用

```
enum class LightType : uint8_t { Point, Spot, Directional };
```
- Point：现有行为不变。
- Spot：新增 `direction`(单位向量)、`inner_cone_angle`、`outer_cone_angle`（弧度）。
- 衰减：`spot_factor = smoothstep(cos(outer), cos(inner), dot(L, -direction))`，
  乘到现有 `light_attenuation` 上。

### 1.3 阴影策略分阶段

- **阶段 1（无阴影）**：SpotLight 只做出光，`shadow` 分支跳过。先让光看得见。
- **阶段 2（单面阴影）**：聚光只需沿 `direction` 一张 2D depth（透视投影，fov = outer_cone*2），
  比点光立方体简单得多。复用 `DirectionalLightPass` 的 2D shadow 采样代码而非点光的 cube 采样。

---

## 2. 分阶段落地

### 阶段 1：SpotLight 出光（无阴影）——最小闭环

**Step 1.1 — 组件加字段**（`LightComponent.h`）
```cpp
enum class LightType : uint8_t { Point, Spot };

ZPROPERTY(meta=(Tooltip="Point or Spot."))
uint8_t m_Type {0};                       // 0=Point, 1=Spot（用 uint8 避免枚举序列化改动）

ZPROPERTY(meta=(Tooltip="Spot forward direction (world, normalized at use)."))
Vector3 m_Direction {Vector3(0.0f, 0.0f, -1.0f)};

ZPROPERTY(meta=(Tooltip="Inner cone half-angle, radians.", ClampMin=0, ClampMax=1.5708))
float m_InnerConeAngle {0.4363f};         // ~25°

ZPROPERTY(meta=(Tooltip="Outer cone half-angle, radians.", ClampMin=0, ClampMax=1.5708))
float m_OuterConeAngle {0.7854f};         // ~45°
```
> 注意：`m_Direction` 也可从 Transform 的朝向推导。这里显式存一份，避免依赖 Transform
> 的旋转约定；后续可加"跟随 Transform 朝向"的开关。

**Step 1.2 — UBO 结构扩字段**（`RenderCommon.h`）

把 `ScenePointLight` 从 16B 扩到 32B，聚光字段对点光填 0：
```cpp
struct ScenePointLight
{
    Vector3 position;
    float   radius;
    Vector3 intensity;
    float   type;              // 0=point, 1=spot（float 便于 shader 分支，避免 int/uint 布局坑）

    // ---- 以下仅 spot 有效，point 填 0 ----
    Vector3 spot_direction;    // 单位向量
    float   spot_inner_cos;    // cos(inner)
    float   spot_outer_cos;    // cos(outer)
    float   _pad0 {0.0f};
    float   _pad1 {0.0f};
};
```
> 32B = 2 个 vec4 对，std140/std430 都安全。`s_MaxPointLightCount=15` → UBO 从 240B 增到 480B，可忽略。

**Step 1.3 — 场景收集填字段**（`RenderScene.cpp:149`）
```cpp
ScenePointLight light {};   // 注意：这里要改成填 ScenePointLight，而不是旧的 PointLight
light.position  = transform->GetPosition();
light.intensity = light_component->GetFlux();
light.radius    = light_component->GetRadius();
if (light_component->GetType() == LightType::Spot) {
    light.type            = 1.0f;
    light.spot_direction  = Normalize(light_component->GetDirection());
    light.spot_inner_cos  = cosf(light_component->GetInnerConeAngle());
    light.spot_outer_cos  = cosf(light_component->GetOuterConeAngle());
} else {
    light.type = 0.0f;
}
```
> `SyncPointLightsFromLevel` 现在填的是 `PointLight`（Light.h），要改成直接填 `ScenePointLight`
> （RenderCommon.h）。`Light.h` 的 `PointLight`/`PointLightList` 是旧结构，可保留但不再是 UBO 源。

**Step 1.4 — shader 光照循环加锥角**（`mesh_lighting.inl`，第 11-66 行循环内）
```glsl
float spot_factor = 1.0;
if (scene_point_lights[light_index].type > 0.5) {
    float cos_theta = dot(L, -scene_point_lights[light_index].spot_direction);
    spot_factor = smoothstep(scene_point_lights[light_index].spot_outer_cos,
                             scene_point_lights[light_index].spot_inner_cos,
                             cos_theta);
}
// 现有第 66 行：
highp vec3 En = scene_point_lights[light_index].intensity * light_attenuation * spot_factor;
```
> 三处 shader 结构体（`scene_lighting_structs.hlsli`、`structures.h`、`main_camera_per_frame_access`）
> 必须同步扩字段，否则 GPU 读到乱数据且不报错（与 CSM 级联数注释同一条铁律）。

**Step 1.5 — 同步三份 shader 结构体**
- `shader/hlsl/common/scene_lighting_structs.hlsli:5`
- `shader/include/structures.h:35`
- `shader/hlsl/common/main_camera_per_frame_access.hlsli`（若有独立镜像）

**验证**：`example_standard.shader` 场景放一个 Spot Light，旋转方向，观察锥形光斑；
点光场景回归不变。

---

### 阶段 2：SpotLight 阴影（单面 2D depth）

**Step 2.1 — 阴影数据结构**（`RenderCommon.h`，仿 `PointLightShadowPerFrame`）
```cpp
struct SpotLightShadowPerFrame
{
    uint32_t spot_shadow_num;
    Vector4  spot_position_and_range[s_MaxSpotShadowCount];   // xyz=pos, w=range
    Matrix4x4 spot_view_proj[s_MaxSpotShadowCount];           // 透视，fov=2*outer_cone
};
static const uint32_t s_MaxSpotShadowCount = 4;
static const uint32_t s_SpotLightShadowMapDimension = 1024;
```

**Step 2.2 — 阴影 pass**：新建 `SpotLightShadowPass`，**复用 `DirectionalLightPass` 的单面
2D depth 路径**（不是点光的立方体 GS）。聚光透视投影矩阵：
```
proj = perspective(2*outer_cone, 1.0, near, range);
view = lookAt(position, position + direction, up);
```

**Step 2.3 — 光照循环采样**：`mesh_lighting.inl` 的 spot 分支里，用 `spot_view_proj` 把
world pos 变到 shadow NDC，2D depth 比较（比点光的 cube layer 采样简单）。

**验证**：SpotLight 下方放遮挡物，观察锥形阴影投射。

---

## 3. 改动清单（阶段 1）

| 文件 | 改动 |
|---|---|
| `LightComponent.h` | +`m_Type/m_Direction/m_InnerConeAngle/m_OuterConeAngle` + getter/setter |
| `RenderCommon.h` | `ScenePointLight` 16B→32B，加 spot 字段 |
| `RenderScene.cpp:123` | `SyncPointLightsFromLevel` 改填 `ScenePointLight` + spot 分支 |
| `scene_lighting_structs.hlsli` | `PointLight` 扩字段 |
| `structures.h` | 同上（GLSL 侧） |
| `mesh_lighting.inl` | 光照循环加 `spot_factor` |
| （可选）`Light.h` | `PointLight`/`PointLightList` 标注 deprecated 或删除 |

**风险点**：
1. 三份 shader 结构体必须同步，否则静默乱数据（最高优先级验证项）。
2. `ScenePointLight` 变 32B 后，所有 `sizeof`/stride 依赖点要复查（`RenderResourcePerFrame.cpp` 上传处）。
3. WebGL2 后端 GLSL ES 对 `smoothstep`/结构体布局的兼容性需实测。

---

## 4. 验证与测试

- **冒烟测试**（纯 CPU，headless）：仿 `GasCooldownSmokeTest` 手法，把"锥角衰减函数"
  抽成 header-only 纯函数 `SpotAttenuation(cos_theta, inner_cos, outer_cos)`，单测
  inner 内=1、outer 外=0、中间平滑、边界值。落 `Function/Render/Test/SpotLightSmokeTest.cpp`。
- **真机验证**：`example_standard.shader` 场景 + Spot Light，RenderDoc 抓帧看光照贡献。
- **回归**：点光场景渲染结果逐像素不变（type=0 分支不进 spot 计算）。
