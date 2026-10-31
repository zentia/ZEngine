# 相机脚本 API（`Camera` / `Ray`）

对应 Unity：`UnityEngine.Camera` / `UnityEngine.Ray`。

代码：`Source/Runtime/Scripting/ScriptCamera/ScriptCamera.{h,cpp}`（门面）
宿主：`Source/Runtime/Function/Render/RenderCamera.{h,cpp}`
门禁：`UnityParitySmokeTest` 的 **Y 组**（行为）与 **Z 组**（接线）。
补全部分见第 7 节（**AR 组** 行为 / **AS 组** 接线）。

---

## 1. 为什么现在才有

`CameraComponent` 早就存在，但它只导出了 6 个方法（模式 / 是否 main / 位置 /
朝向）—— 投影相关的一个都没有。于是脚本能问"相机在哪"，却问不了"这个物体在
屏幕的哪个位置"，更没法做"点一下选中那个怪"。

而投影所需的一切（fov、aspect、near/far、正交与否、view/projection 矩阵）都
活在 **`RenderCamera`** 上，不在 `CameraComponent` 上。所以这层门面包的是
`RenderCamera*`：包错一层的话 `camera.fieldOfView = 60` 改不到渲染真正用的那个
值，画面毫无变化 —— 静默的错行为，不是报错。

顺带补了 `Ray`（`ScreenPointToRay` 的返回类型；引擎此前没有这个类型）。

---

## 2. API

```ts
// 静态
Camera.GetMain(): Camera          // 游戏视口那台（= Unity 的 Camera.main）
Camera.GetCount(): number
Camera.GetCameraAt(index): Camera // 越界返回 null，不夹紧到第 0 台
Camera.GetAllCameras(): Camera[]

// 属性（Unity 同拼写）
camera.fieldOfView       // 垂直视场角，**度**
camera.nearClipPlane
camera.farClipPlane
camera.aspect
camera.orthographic
camera.orthographicSize  // 半高（世界单位）
camera.pixelWidth        // 只读
camera.pixelHeight       // 只读

// 坐标转换
camera.WorldToScreenPoint(p: Vector3): Vector3
camera.ScreenToWorldPoint(p: Vector3): Vector3
camera.WorldToViewportPoint(p: Vector3): Vector3
camera.ViewportToWorldPoint(p: Vector3): Vector3
camera.ScreenPointToRay(p: Vector2): Ray
camera.ViewportPointToRay(p: Vector2): Ray

// Ray
ray.origin / ray.direction     // 属性，可读写
ray.GetPoint(distance): Vector3
```

---

## 3. 三处必须写对的细节

### 3.1 屏幕坐标原点在**左下角**，y 向上

和 Unity 一致，和 Win32 / Cocoa 的原生鼠标坐标**相反**。拿原生坐标直接喂
`ScreenPointToRay` 会得到上下翻转的结果，而且不报错 —— 表现是"点天花板选中
地板"。

代码里这一步是 `NdcToScreenY()`：**必须翻一次**。投影矩阵带一块 `fix_mat`
（`[1][1] = -1`），它已经把 y 翻了一道；再加上引擎是 Z-up，两道叠出来的结果是
"世界里的上方"落在**较小**的 ndc.y 上。直接用 `ndc.y * 0.5 + 0.5` 当屏幕 y，画
面上下颠倒。Y29 / Y38 / Y46 从三个方向钉住它，Z7 钉住"那一行算术还在"。

### 3.2 `WorldToScreenPoint` 的 z 是**沿视线方向的深度**，不是欧氏距离

Unity 同。取沿 `forward` 的投影，`ScreenToWorldPoint` 才能和它**精确**互逆。

两者只在画面正中间相等，越靠边差得越多。例：点 `(6, 10, 4)`，欧氏距离 7.2，
投影到视线方向是 10。所以往返测试（Y36 / Y39）必须带偏心样本 —— 只测正中间的
话，用欧氏距离实现也全绿。

### 3.3 `fieldOfView` 是**垂直**的

Unity 的 `fieldOfView` 是垂直视场角。渲染器内部主存的是**水平** fovx，fovy 是
由 fovx 和 aspect 推出来的。所以 `SetFieldOfView` 要先把用户给的垂直角换算回
fovx 再走同一条路（`RenderCamera::setFOVy`）；直接写 `m_Fovy` 的话下一次
`SetAspect` 就把它盖掉 —— 读出来还是 60，画面纹丝不动。

`setFOVy` 受引擎已有的 fovx 范围 `[4, 120]` 夹取：极端宽高比下（aspect > 约
3.2）想表达的垂直角需要超出上限的水平角，此时设进去和读出来不相等。这是夹取，
不是换算写错了。

---

## 4. 与 Unity 的刻意差异

| Unity | ZEngine | 原因 |
|---|---|---|
| `Camera.main`（静态属性） | `Camera.GetMain()` | `ZSCRIPTPROPERTY` 的 getter 不允许 static（`Screen` / `Time` / `Random` 同一处取舍） |
| `Camera.allCameras` | `Camera.GetAllCameras()` | 同上 |
| `Ray` 是 struct（值类型） | `Ray` 是 Object 派生类 | ZCLASS 必须派生自 Object。而且 `ScreenPointToRay` 每帧调一次的话，每次 new 一个就是每帧一个对象 —— 所以射线对象**按相机复用**：两次调用拿到的是同一个 `Ray`，要留着就自己拷一份 |
| `Camera.main` = "打了 MainCamera 标签的那台"，每次调用都要遍历 | `GetMain()` 直接取游戏视口那台 | ZEngine 的游戏视口就一台相机，同样的结果，不用每帧遍历 |

`Camera` 对象本身也按 `RenderCamera*` 缓存复用（和 `Scene` 按 `Level*` 缓存同
一套路）：序号会随 RenderSystem 增删相机而滑动，脚本手里那个 Camera 会突然指
向另一台。

---

## 5. 已知的两处引擎侧怪癖（门面绕开了，但值得知道）

* **`RenderCamera` 的 near/far 默认值是反的**（`m_Znear = 1000`、`m_Zfar = 0.1`）。
  不显式设的话投影出来的深度是反的，而画面照常能出。门面的 setter 会保持
  `far > near`，并且把 0 近平面夹到一个极小正数（近平面为 0 会让 w 为 0 →
  除零 → NaN 坐标散到整个场景）。
* **正交投影的深度范围是反的**：`GetProjectionMatrixForAspect` 传进
  `MakeOrthographicProjectionMatrix` 的是 `(m_Zfar, m_Znear)`，于是 ndc z 变大
  反而变近。所以正交射线**不能**用"ndc z=0 的点 → ndc z=1 的点"求方向 —— 那样
  算出来是**负的 forward**，整条射线朝相机背后走（这是实现时实测到的）。门面
  改为：方向直接取 forward，起点沿视线平移到近平面，两种深度约定下都成立。

两处都不在本切片里"修"：改渲染用的深度会影响剔除和深度测试，属于独立的风险。

---

## 6. 用法

```ts
const cam = Camera.GetMain();

// 把世界坐标画到 UI 上（UI 若用左上原点，自己翻一次 y）
const s = cam.WorldToScreenPoint(enemy.transform.position);
label.x = s.x;
label.y = cam.pixelHeight - s.y;

// 点一下选中
const ray = cam.ScreenPointToRay(mousePos);   // mousePos 必须是左下原点
const hit = Physics.Raycast(ray.origin, ray.direction, 1000);
```

---

## 7. 补全：屏幕 <-> 视口、三张矩阵、`ResetAspect`、`pixelRect`、朝向

（后加的一节。前面六节描述的是第一版门面；这一节是补的那一半。）

### 7.1 为什么补

Unity 的六个互转里原来只有**穿过相机的四条**，屏幕与视口之间得**先绕道世界**：
一次额外的矩阵求逆，还把浮点误差带进去。而 `worldToCameraMatrix` /
`cameraToWorldMatrix` / `projectionMatrix` 完全不存在 —— 自定义渲染、手算视锥、
把世界点投到 UI 都要它们。

### 7.2 新增 API

```ts
camera.ScreenToViewportPoint(p: Vector3): Vector3   // 像素（左下原点）-> 0..1
camera.ViewportToScreenPoint(p: Vector3): Vector3   // 0..1 -> 像素

camera.worldToCameraMatrix      // 只读，Matrix4x4
camera.cameraToWorldMatrix      // 只读
camera.projectionMatrix         // 只读
camera.GetNonJitteredProjectionMatrix(): Matrix4x4  // Unity 的 nonJitteredProjectionMatrix
camera.ResetAspect(): void
camera.pixelRect                // 只读，Vector4(x, y, w, h)，像素、左下原点
camera.position / forward / up / right   // 只读
```

### 7.3 `cameraToWorldMatrix` 必须**求逆**，不能组装

`RenderCamera` 的 view 矩阵在 LWC 下还含一块相对当前 tile 的平移。用
`position + rotation` 现组装一个"相机到世界"矩阵，得到的和渲染用的**不是同一张**
—— 脚本拿它变换出来的坐标会和画面差一整个 tile 的偏移，而且只在相机走远之后才
看得出来。所以直接 `GetViewMatrix().inverse()`（AR14 用矩阵相乘验，AR15 再拿一
个点来回一趟）。

### 7.4 `nonJitteredProjectionMatrix`

TAA 靠逐帧改变投影的亚像素偏移工作，所以带抖动的那张矩阵**每帧都不一样**。
拾取、UI 跟随这类"必须和画面严格对齐"的计算要用去掉抖动的那张，否则每帧差半个
像素，表现为"点不中静止的小物体"。门面内部的坐标转换一直用的是
`GetProjectionMatrixNoJitter()`（Z10 是这条的门禁）。

### 7.5 `pixelRect` 的形状差异

Unity 返回 `Rect`，ZEngine 没有能过脚本边界的 `Rect` 值类型，所以按
`(x, y, width, height)` 塞进 `Vector4`。形状差异，不是语义差异。

### 7.6 顺带修掉的三处真 bug（都是新 API 直接撞上的）

1. **`GetCameraTargetRect()` 无条件解引用 `targetTexture`**。没设 `targetTexture`
   的相机 —— 也就是绝大多数相机 —— 调 `GetCameraRect()` / `ResetAspect()` 是
   **段错误**。现在没有 RenderTexture 时退回屏幕尺寸（`WindowSystem::GetWindowSize()`），
   再没有就回空矩形。
2. **`normalizedViewPortRect` 恒为零值**。全仓只有一处读、没有任何一处写，所以它
   永远是 `Rectf()` 的 `0,0,0,0` ⇒ `pixelRect` 恒为 0×0、`ResetAspect()` 恒走
   `height == 0` 分支。现在默认给成 Unity 的 `(0,0,1,1)`（占满渲染目标）。
3. **`ResetAspect()` 写的是另一个成员**。`SetAspect()` 写 `m_Aspect`（投影矩阵读
   的是它），`ResetAspect()` 写 `m_State.aspect` ⇒ 一个"调用成功但什么都没发生"的
   空操作。现在 `ResetAspect()` 走 `SetAspect()`，只保留脏标记。

这三处都不是"为了让新测试过"而调的：它们让 `pixelRect` / `ResetAspect` 这两个
Unity API 在 ZEngine 里根本没法用。

### 7.7 已知不一致（本切片**没有**改）

`RenderCamera::GetProjectionMatrixForAspect()` 只在**正交**分支里调了
`ApplyTaaJitter`，透视分支没调 ⇒ 一台普通的透视相机即便开了 TAA 也拿不到抖动，
TAA 对它等于没开。改它会动到每一个画面的每一个像素，不在这里碰。

但它决定了 AR19（"开了 TAA 时两张投影必须不同"）**只能用正交相机写** —— 拿透视
相机会得到"两张相同"的假绿。注释写在测试里了。

### 7.8 测试

| 组 | 测什么 |
|---|---|
| AR | 未绑定降级；换算与往返；**交叉验证**（屏幕→视口 必须和 世界→视口 是同一套缩放 —— 各自单独测的话 width/height 用反了也能自洽）；两张 view 矩阵真的互为逆；TAA 开/关下去抖矩阵与抖动矩阵的关系；`ResetAspect` 回到屏幕宽高比；`pixelRect` 尺寸；朝向的单位性、正交性、**以及真的对上 `LookAt` 的方向**。 |
| AS | 源码级接线门：方法在门面上声明；`cameraToWorldMatrix` 是求逆不是组装；屏幕→视口没顺手把深度也除了；`ResetAspect` 真的到了 `RenderCamera`。 |

> ⚠️ 朝向只验"单位 + 两两正交"**不够**：把 `forward` 和 `up` 换一下三者仍然全部
> 通过，而"沿视线往前 5 米"就变成了"往上 5 米"。所以 AR26 / AR27 还对着
> `LookAt` 的目标与 up 验了一次。

> ⚠️ Z10 这条门禁原来是 `->GetProjectionMatrix() == 0`（"门面里一次都不许出现带
> 抖动的投影"）。加了 `projectionMatrix` 访问器后它必然出现一次，所以改成"**恰好
> 一次**"。写成 `== 0` 会在每次新增任何投影相关 API 时误报，写成"不判"就失去了
> 意义 —— 判个数才既准确又仍能抓到"有人把坐标换算换成了带抖动的那张"。
