# Gizmos —— 脚本侧的调试绘制

```ts
Gizmos.color = Color.red;
Gizmos.DrawLine(here, there);
Gizmos.DrawRay(here, direction);
Gizmos.DrawWireSphere(center, 1.5);
Gizmos.DrawWireCube(center, { x: 2, y: 2, z: 2 });
Gizmos.DrawCube(center, { x: 2, y: 2, z: 2 });
Gizmos.DrawFrustum(center, 90, /*far*/ 1000, /*near*/ 0.3, /*aspect*/ 16 / 9);
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
| `Gizmos.DrawFrustum(center, fov, farClipPlane, nearClipPlane, aspect)` | 12 条线的视锥（见下） |

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

## 三之二、`DrawFrustum`：轴向与 Unity 不同，语义相同

近平面 4 条 + 远平面 4 条 + 4 条侧棱 = **12 条线**（不是实心，与 `DrawCube` 不同
图元类型）。

```
        far (半高 = far * tan(fov/2))
       +--------+
      /|       /|
     / |      / |
near+---+----+  |          forward
    |  +----|---+
    | /     | /
    +-------+/

forward = -Y      up = +Z      right = forward × up = -X
```

⚠️ Unity 的 `DrawFrustum` 沿 **+Z** 张开（那是 Unity 相机的 forward）。本引擎的
语义前是 **-Y**（`Vector3.forward` / `Transform::GetForward()` 都是它），所以这里
照**语义**对齐而不是照 Unity 的分量数字 —— 理由与 `Vector3.forward` 一致。

照抄 +Z 是最难发现的一类错：结果仍然是 12 条线、仍然是台体、角点仍然两两对称，
所有计数型断言（FR3 / FR4 / FR5 / FR6）**全绿**。只有"远端落在 -Y 那一侧"这种
带符号的断言分得出来 —— 那就是 **FR7**。

`fov` 是**竖直**视野角、**度**（Unity 同）；`aspect` = 宽/高，只拉宽不拉高
（FR10）。

### 退化参数：一条线都不画

`fov ∉ (0,180)`、`near <= 0`、`far <= near`、`aspect <= 0` 时直接返回。忘了这个
守卫的话 `fov = 180` 会给出 `tan(90°) = inf`，而**一个 inf 顶点会污染整条 draw
call** —— "某个 gizmo 没出现"比"整屏乱掉"好查得多（与 `Matrix4x4.Inverse` 对奇异
矩阵的处理同一个道理）。FR14 逐个验这四种退化，且额外断言没有 NaN / inf 落进顶点。

## 四、`OnDrawGizmos` / `OnDrawGizmosSelected`

`Gizmos.DrawXxx` 只能在这两个钩子里调 —— Unity 的 `Gizmos` 类就是这么规定的
（画在别处的东西活不过一帧，见上一节"后端与生命周期"）。

```ts
class Mover extends Behaviour {
    OnDrawGizmos(): void {                 // 常驻：总是画
        Gizmos.color = Color.green;
        Gizmos.DrawWireSphere(this.transform.position, 1);
    }
    OnDrawGizmosSelected(): void {         // 只在被选中时**额外**画
        Gizmos.DrawWireCube(this.transform.position, { x: 2, y: 2, z: 2 });
    }
}
```

### ⚠️ 判据是第三种：`activeInHierarchy` 要，`enabled` **不要**

引擎里现在有三套派发判据，别互相抄：

| 派发 | `activeInHierarchy` | `enabled` | 编辑器白名单 | 对应 Unity |
|---|---|---|---|---|
| `Tick` / `FixedTick` / `LateTick` | ✅ | ✅ | ✅ | `Update` / `FixedUpdate` / `LateUpdate` |
| `BroadcastAppEvent` | ❌ | ❌ | ❌ | `OnApplicationPause` 等（"sent to all GameObjects"） |
| **`BroadcastDrawGizmos`** | ✅ | **❌** | ❌ | `OnDrawGizmos` / `OnDrawGizmosSelected` |

第三条不是凑出来的：Unity 的 MonoBehaviour 参考页明文写着

> The checkbox for disabling a MonoBehavior (on the editor) will only prevent
> `Start()`, `Awake()`, `Update()`, `FixedUpdate()`, and `OnGUI()` from executing.

也就是说**停用的脚本照样画 gizmo**。理由也很直接：调试辅助线属于"我写在哪个
对象上就该在那里看见"，被 `enabled` 静默藏掉的 gizmo 与"压根没写"长得一模
一样 —— 现场（一条该出现却没出现的线）不留任何痕迹，排查方向必然跑偏。

代价是这一行**特别像 bug**，容易被后来者"顺手修掉"。所以 `DW6` 是源码级反向
门（函数体里不许出现 `isEnabled`），`DG22` 是运行时断言 —— 两道门都要有：
运行时那道证明行为，源码这道证明"函数体里没有那行"。

### 派发路径

```
Application::LogicalTick                  // 每帧，LateTick **之后**
  └─ Application::DrawGizmosFrame()       // Player 里整趟跳过（g_isEditor == false）
       ├─ ResolveGizmoSelection(level)    // 选中 ID -> 指针；两张对象表都查
       └─ WorldManager::DrawGizmos(selected)          // 世界分区开 → 所有已加载关卡；否则活动关卡
            └─ Level::DrawGizmos(selected)            // m_Gobjects + m_RuntimeGObjects（两张表）
                 └─ GameObject::BroadcastDrawGizmos(selected)   // 只判 IsActiveInHierarchy
                      └─ Component::OnDrawGizmos()              // 总是
                           Component::OnDrawGizmosSelected()    // 且 selected 时**额外**再来
```

* 在 `LateTick` **之后**：gizmo 画的是这一帧的最终位姿，而相机跟随、骨骼朝向
  修正这类逻辑恰好在 LateUpdate 里改 transform。挪到前面会让辅助线永远落后
  一帧，表现为"线在物体旁边飘"。
* ⚠️ `Player` 里整趟跳过：Unity 的 gizmo 只画在编辑器里，而这一趟要走遍**全
  关卡** —— 为一个画不出来的东西每帧走一遍全关卡是纯亏。编辑态下的 Play 模式
  （`g_isEditor` 仍为 true）照发。
* 选中项用**实例 ID** 交给引擎，每帧重新解析成指针：关卡重载之后同一个 ID 是
  另一个对象，缓存下来的指针已经悬空；而一个解析不到的 ID 只是"没选中"。
* `ResolveGizmoSelection` 单独成一个方法，不是实现细节：它是这条链上唯一有
  判断的一步（两张表、解析不到就跳过），而 `DrawGizmosFrame` 剩下的部分在
  headless 下（没有活动关卡）是走空的 —— 藏起来就没有任何断言能碰到它。

* 没塞进 `AppEventKind`：那是个 `uint8` 枚举，塞不进"哪些对象被选中"这个集合。
* 选中判定是**线性查找**，不是先建 `unordered_set`：这一趟要走遍全关卡（N），
  而选中项通常只有几个（S），S 这么小的时候线性更快，也不必为一个参数多引容器。
* `OnDrawGizmosSelected` 是**叠加**的，不是二选一：选中对象的常驻辅助线照常
  来一次。写反成二选一，选中后常驻那条就消失了 —— 又是"看起来像没写"。
* ⚠️ 这里**不**挑相机。Unity 的 `OnDrawGizmos` 是"场景视图/游戏视图重绘时"
  发的，与"有没有被某台相机看见"无关；可见性那件事属于 `OnBecameVisible` /
  `Renderer.isVisible`，是另一条链路。

## 五、没做的 Unity 成员

| Unity | 状态 |
|---|---|
| `Gizmos.DrawIcon` | 没有图标资源管线 |
| `Gizmos.DrawMesh` | 绑定层搬不动网格句柄 |
| `Gizmos.DrawGUITexture` | 没有 IMGUI 层 |
| `Gizmos.exposure` / `probeSize` | HDRP 专属 |
| `MonoBehaviour.OnDrawGizmos` 的"Inspector 折叠时不调" | 纯编辑器 UI 细节，不影响语义 |

## 六、测试分布

| 位置 | 覆盖 |
|---|---|
| `UnityParitySmokeTest` GZ | 几何：顶点位置、颜色、图元计数。后端是 `DebugDrawGroup`，计数与 `WriteLineData` 都能在无 GPU 下读出来 |
| `UnityParitySmokeTest` FR | `DrawFrustum`：12 条线、轴向（-Y）、台体而非盒、aspect、fov 单位、matrix / color、五种退化参数 |
| `UnityParitySmokeTest` GW | 接线门（声明 / 判空 / 文件级静态 / 不用 AddQuad / 视锥的轴向与三个循环） |
| `UnityParitySmokeTest` DG | 两个钩子的**判据**：不判 enabled（含"Tick 必须被跳过"的互补半边）、判 activeInHierarchy（含父级停用）、多选、跨关卡选中项、未加载关卡、一帧一次 |
| `UnityParitySmokeTest` DW | 接线门（两个虚函数、正向判据、两条反向门、两张对象表都派发、主循环里 `DrawGizmosFrame` 的位置与 `g_isEditor` 判据、脚本面三处声明） |
| `UnityParitySmokeTest` DG36–DG41 | `SetGizmoSelection` 往返 + ID 解析（两张表、未知 ID 缺席、null 关卡降级） |
| `ScriptVMSmokeTest` L | 发布面：脚本里 `typeof Gizmos`、`Gizmos.color` 读写真的生效 |
| `ScriptComponentSmokeTest` F3 | 端到端：`Mover.ts` 里的 `Gizmos.*` 过 tsc 类型检查，再从 JS 画到后端 |
| `ScriptComponentSmokeTest` R | 脚本侧的同一套判据（停用照发 / 停用对象不发 / Selected 是叠加） |

变异用例见 `.mutate_gz.py`（Gizmos 本体）、`.mutate_fr.py`（`DrawFrustum`）、
`.mutate_dg.py`（两个钩子 + 主循环驱动）。

### 已知覆盖缺口

`WorldManager::DrawGizmos`（"活动关卡 / 所有已加载关卡"那条分支）**只有源码级
门 DW15，没有行为级门**：`m_CurrentActiveLevel` 只能由真实加载流程设置，而
`m_LoadedLevels` 既私有也没有 setter，headless 下做不出夹具。它下面那一层
（`Level::DrawGizmos` → 两张表 → 判据 → 脚本转发）是全覆盖的。补它的代价是
往生产 API 里加一个"把这张 Level 设成活动关卡"的测试接缝 —— 不划算，先记着。
