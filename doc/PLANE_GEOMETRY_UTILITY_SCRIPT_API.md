# `Plane` / `GeometryUtility`

对应 Unity：`UnityEngine.Plane`、`UnityEngine.GeometryUtility`。

⚠️ **这两个类型在 C++ 里早就有了**（`Runtime/Core/Math/Plane.h` 与
`Runtime/Core/Math/Frustum.h` 都写得很完整，视锥裁剪一直在用它们），但**脚本侧
一个都没有**：没有 `Converter<Plane>`、没有 `declare interface Plane`、没有门面。
于是脚本做不了"这个盒子在不在画面里"—— 只能把 `Physics.Raycast` 当万能钥匙用。

代码：

| 文件 | 内容 |
|---|---|
| `Runtime/Core/Math/Plane.h` | 值类型 `::Plane`（本次未改，已完整） |
| `Runtime/Core/Math/Frustum.h` | 六个平面的视锥（本次未改，已完整） |
| `Runtime/Scripting/ScriptPlane/ScriptPlane.{h,cpp}` | 门面 `Runtime::Plane` / `Runtime::GeometryUtility`（新） |
| `Runtime/Scripting/Binding/ScriptBinding.{h,cpp}` | `Converter<Plane>` + `declare interface Plane` |
| `Runtime/Scripting/ScriptCamera/ScriptCamera.{h,cpp}` | `pixelRect` 从 `Vector4` 换回 `Rect`（Unity 的形状） |

```ts
const ground: Plane = Plane.FromNormalAndPosition({x:0, y:0, z:1}, {x:0, y:0, z:3});
Plane.GetDistanceToPoint(ground, {x:0, y:0, z:5});      // 2

const wall: Plane = Plane.Create({x:1, y:0, z:0}, -5);  // x = 5，正面是 +x
Plane.Raycast(wall, {x:9, y:0, z:0}, {x:-1, y:0, z:0}); // 4
Plane.Raycast(wall, {x:0, y:0, z:0}, {x:1, y:0, z:0});  // -1（背面不打）

const planes: Plane[] = GeometryUtility.CalculateFrustumPlanes(Camera.main);
const visible: boolean = GeometryUtility.TestPlanesAABB(planes, bounds);
```

## 1. 形态：值类型 + 同名门面

与 `Bounds` / `Rect` / `Vector2Int` 同一套分工（完整说明见 `ScriptBounds.h` 开头）：

| | C++ | .d.ts | 运行时 |
|---|---|---|---|
| 值 | `::Plane` | `declare interface Plane` | `{normal: {x,y,z}, distance}` |
| 门面 | `Runtime::Plane` / `Runtime::GeometryUtility` | `declare class Plane` / `declare class GeometryUtility` | pesapi 绑定类（静态方法挂在上面） |

`normal` 是嵌套的 `Vector3`，不是扁平的四个数 —— 与 Unity 的 `Plane` 结构一致。

## 2. `distance` 的符号（最重要的一处）

平面方程是 `dot(normal, p) + distance == 0`。所以：

```
过 (0,0,3)、法线 +Z 的平面  ⇒  distance == -3   （不是 +3）
```

⚠️ 写反了的话"点在正面"整个反过来 —— 视锥的内外也就反了，而
`TestPlanesAABB` 照样返回一个看着合理的 `bool`。判据是 PL0。

`GetDistanceToPoint` 返回的是**有符号**距离，正 = 在 normal 指的那一侧。

## 3. 归一化：两个入口的行为**不一样**

| 入口 | 归一化 `normal` 吗 |
|---|---|
| `Plane.Create(normal, distance)` | ❌ **不**（照 Unity 的构造函数） |
| `Plane.FromNormalAndPosition(normal, point)` | ✅ |
| `Plane.FromPoints(a, b, c)` | ✅ |
| `GeometryUtility.CalculateFrustumPlanes*` | ✅（Gribb-Hartmann 抽出来就归一化） |
| 直接写 `plane.normal = v` | ❌ 自己负责 |

⚠️ 没归一化时 `GetDistanceToPoint` 返回的**不是距离**，只是"乘了法线长度的
有符号量"。要真正的距离用 `Plane.Normalize(plane)`。这与 Unity 一致 —— Unity
的 `new Plane(normal, distance)` 同样不归一化。

## 4. `Raycast` 返回 -1，不是 bool

Unity 是 `bool Plane.Raycast(Ray ray, out float enter)`。TS 没有 out 参数，
所以距离自己回来：

```ts
const t: number = Plane.Raycast(plane, origin, direction);
if (t >= 0) { /* 命中，hit = origin + direction * t */ }
```

⚠️ 两个细节：

* **只打正面**。射线朝着 normal 那一侧射过去算不中（`-1`）。`wall` 的正面是
  `x > 5`，所以得从 `x = 9` 往 `-x` 射 —— 从 `x = 0` 往 `+x` 射是打背面。
* **t 不是世界距离**。`direction` 未归一化时 t 是射线参数，与 Unity 一致。

用 `0` 表达"没打中"会和"正好打在起点上"（t = 0）撞车，所以用 `-1`。

## 5. `GeometryUtility`

| 方法 | 说明 |
|---|---|
| `CalculateFrustumPlanes(camera)` | Unity 的形状，返回 **6** 个平面（左/右/下/上/近/远，顺序同 Unity） |
| `CalculateFrustumPlanesVP(vp)` | 非 Unity 的重载：直接给 `projection * view`。编辑器 / 离屏那几台相机没有 `Camera` 门面 |
| `TestPlanesAABB(planes, bounds)` | 保守的"正顶点"判定 |

### ⚠️ 拿不到相机时返回**空数组**，不是 6 个退化平面

空的数组一眼能看出是"没拿到相机"；而 6 个退化平面会让后面的
`TestPlanesAABB` 恒真 —— 也就是"什么都看得见"，静默。判据是 PL45 / PL46。

### ⚠️ `TestPlanesAABB` 是**保守**的

判据是"正顶点"：对每个平面取盒子上沿法线方向最远的那个角，它若已在背面，
整个盒子就在锥外。盒子很大、斜穿过视锥的一角时可能返回 `true` 而其实不相交
—— 也就是会**多画**不会漏画。Unity 的 `TestPlanesAABB` 同样是这个精度
（文档里写了它 conservative）。漏画的代价（物体凭空消失）远大于多画。

门面与 `Frustum::Intersects` 用的是**同一套**判据（PL43 / PL44 钉住两者一致），
不是两份实现。

## 6. `pixelRect` 现在是 `Rect`

`Camera.pixelRect` 之前返回 `Vector4`（把 `x/y/width/height` 塞进四个分量），
头文件里的原话是"ZEngine 没有 `Rect` 值类型可以过脚本边界"。现在有了，就换回
Unity 的形状：

```ts
const r: Rect = camera.pixelRect;
r.width     // 原来是 r.z
r.height    // 原来是 r.w
```

## 7. 测试落点

| 组 | 位置 | 钉什么 |
|---|---|---|
| PL0–PL46 | `UnityParitySmokeTest` `TestPlaneAndGeometryUtility()` | distance 符号、归一化入口差异、边界规则、Raycast 只打正面、六个面逐个比对 |
| PLW0–PLW7 | `UnityParitySmokeTest` `TestPlaneWiring()` | 源码级：`Converter<Plane>`、`declare interface Plane`、三个 Unity 成员 |
| AR3 / AR22 / AR23 / AR23b | `UnityParitySmokeTest` `TestCameraExtras()` | `pixelRect` 是 `Rect`（读 `width`/`height`，不是 `z`/`w`） |
| Y0 / Y1 | `ScriptComponentSmokeTest` | **tsc 认 `Plane` 与 `GeometryUtility`** + 端到端取值 |

变异测试 `py .mutate_pl.py`：16 个变异体。

## 8. 一个容易踩的实现细节（给后来改矩阵的人）

`Frustum::FromViewProjection` 用的是**列向量**约定：第 i 行点乘 world 给出
clip 的第 i 个分量，平移项在 `m[i][3]`。所以搭一张正交矩阵时 z 的平移写在
`m[2][3]` —— 写成 `m[3][2]` 会污染 w 行，`w + x` 里混进 z 项，六个平面就全
都不是盒子的面了（`Matrix4x4::operator*(Vector3)` 用的是另一套行向量约定，别
照着它推）。
