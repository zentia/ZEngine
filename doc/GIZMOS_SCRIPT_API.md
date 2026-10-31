# Gizmos —— 脚本侧的调试绘制

```ts
Gizmos.color = Color.red;
Gizmos.DrawLine(here, there);
Gizmos.DrawRay(here, direction);
Gizmos.DrawWireSphere(center, 1.5);
Gizmos.DrawWireCube(center, { x: 2, y: 2, z: 2 });
Gizmos.DrawCube(center, { x: 2, y: 2, z: 2 });
```

| TypeScript | C++ |
|---|---|
| `Gizmos.color: Color` | `Runtime::Gizmos::GetColor` / `SetColor` |
| `Gizmos.matrix: Matrix4x4` | `Runtime::Gizmos::GetMatrix` / `SetMatrix` |
| `Gizmos.DrawLine(from, to)` | 一条线段 |
| `Gizmos.DrawRay(from, direction)` | `from` → `from + direction` |
| `Gizmos.DrawWireSphere(center, radius)` | wire sphere |
| `Gizmos.DrawSphere(center, radius)` | solid（见下） |
| `Gizmos.DrawWireCube(center, size)` | 12 条边的 box 图元 |
| `Gizmos.DrawCube(center, size)` | 12 个实心三角形 |

`color` 与 `matrix` 是**静态属性**（走 pesapi 的 `Variables` 表，装在构造器上，
不是 prototype）。默认值与 Unity 一致：白色、单位矩阵。

## 一、状态是进程级的

`Gizmos.color` / `Gizmos.matrix` 是全局的（Unity 也是），不是 per-behaviour。
改了不复位会污染后面所有的绘制 —— 测试里每一段都显式复位，就是这个原因。

⚠️ 实现上必须是**文件级静态**。写成函数内的 `static` 局部变量，`GetColor` 和
`SetColor` 会各自持有一个互不相干的对象：编译能过、运行不崩，`Gizmos.color =
Color.red` 只是"不生效"。`UnityParitySmokeTest` 的 **GW7** 钉这一条。

## 二、后端与生命周期

所有绘制进 `DebugDrawManager` 的同一个 group（`"gizmos"`），生命周期是一帧
（`k_debug_draw_one_frame = 0.0f`）—— 和 Unity 一样，gizmo 不会自己留下来，必须
每帧重画。

Unity 的 Gizmos 是**深度测试**的（会被物体挡住），所以引擎侧显式传
`no_depth_test = false`，与 `AddLine` 的默认值相反。照默认值写会得到"永远浮在最
前面"，那是 `Handles` 而不是 `Gizmos`。

### ⚠️ 非 Vulkan 构建下整套是 no-op

`DebugDrawManager.cpp` 顶部有一整段 `#if defined(__APPLE__) || !defined(Z_HAS_VULKAN)`
的空实现，`TryGetOrCreateDebugDrawGroup` **恒返回 nullptr**。所以每个入口都必须判
空降级，而不是崩 —— 一个只在 macOS 上炸的 `Gizmos.DrawLine` 是最难查的那类 bug。

测试用 `#if defined(Z_HAS_VULKAN)` 分别断言"有后端 / 没有后端"两种真值，两边都不
是空测试。

## 三、`DrawCube` 为什么是 12 个三角形

后端只在 `WriteTriangleData` 里遍历 `m_Triangles`；**`m_Quads` 的实心图元从来不会
被写出来**（`GetTriangleCount` 也只数 `m_Triangles`）。所以：

```
AddQuad(..., _FillMode_solid)   →  编译通过、计数为 0、屏幕上什么都没有
AddTriangle(..., _FillMode_solid) →  真的画出来
```

一个完全静默的失败。所以 `DrawCube` 用 6 个面 × 2 个三角形 = 12 个
`AddTriangle(_FillMode_solid)`，而 `DrawWireCube` 用的是 `AddBox`（12 条边的 box
图元）。两者走**不同的图元类型**，计数上能区分，测试也就能区分（GZ13–GZ16 是互补
的一对）。

### `DrawSphere` 目前就是 `DrawWireSphere`

`DebugDrawSphere` 结构体没有 `FillMode` 字段，`DrawWireFrameObject` 一律按线框渲
染。所以 `DrawSphere` 现在转发给 `DrawWireSphere`。API 分开保留，等后端有实心光栅
化路径再接上 —— 在这之前两者行为一致这件事写在这里，不藏在实现里。

## 四、没做的 Unity 成员

| Unity | 状态 |
|---|---|
| `Gizmos.DrawIcon` | 没有图标资源管线 |
| `Gizmos.DrawMesh` | 绑定层搬不动网格句柄 |
| `Gizmos.DrawFrustum` | 需要一次性相机句柄 |
| `Gizmos.DrawGUITexture` | 没有 IMGUI 层 |
| `Gizmos.exposure` / `probeSize` | HDRP 专属 |

## 五、测试分布

| 位置 | 覆盖 |
|---|---|
| `UnityParitySmokeTest` GZ | 几何：顶点位置、颜色、图元计数。后端是 `DebugDrawGroup`，计数与 `WriteLineData` 都能在无 GPU 下读出来 |
| `UnityParitySmokeTest` GW | 接线门（声明 / 判空 / 文件级静态 / 不用 AddQuad） |
| `ScriptVMSmokeTest` L | 发布面：脚本里 `typeof Gizmos`、`Gizmos.color` 读写真的生效 |
| `ScriptComponentSmokeTest` F3 | 端到端：`Mover.ts` 里的 `Gizmos.*` 过 tsc 类型检查，再从 JS 画到后端 |

变异用例见 `.mutate_gz.py`。
