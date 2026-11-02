# `Vector2Int` / `Vector3Int` / `Rect` / `RectInt`

对应 Unity：`UnityEngine.Vector2Int`、`UnityEngine.Vector3Int`、
`UnityEngine.Rect`、`UnityEngine.RectInt`。

在这之前这四个名字在整个 `engine/` 里是**零命中**的（`RectT` 有，但那是
`RectT<T>`，脚本侧没有 `Rect` / `RectInt` 这两个名字，也没有整数向量）。

代码：

| 文件 | 内容 |
|---|---|
| `Runtime/Core/Math/VectorInt.h` | 值类型 `::Vector2Int` / `::Vector3Int`（新） |
| `Runtime/Core/Math/Rect.h` | 值类型 `::Rect` / `::RectInt`（`RectT<float>` / `RectT<int>` 的 using，新加的 Unity 成员） |
| `Runtime/Scripting/ScriptIntVector/ScriptIntVector.{h,cpp}` | 门面 `Runtime::Vector2Int` / `Runtime::Vector3Int` |
| `Runtime/Scripting/ScriptRect/ScriptRect.{h,cpp}` | 门面 `Runtime::Rect` / `Runtime::RectInt` |
| `Runtime/Scripting/Binding/ScriptBinding.h` | 四个 `ZScript::Converter` |
| `Runtime/Scripting/Binding/ScriptBinding.cpp` | `.d.ts` 里四个 `declare interface` |

```ts
// 网格 / 瓦片坐标
const cell: Vector2Int = Vector2Int.FloorToInt({ x: -0.5, y: 1.7 });   // (-1, 1)
const step: Vector3Int = Vector3Int.Add(Vector3Int.forward, Vector3Int.up);

// UI / 裁剪矩形
const r: Rect = Rect.Create(0, 0, 10, 20);
const inside: boolean = Rect.Contains(r, { x: 10, y: 5 });              // false —— 半开区间
const clamped: RectInt = RectInt.ClampToBounds(RectInt.Create(9, 2, 3, 3),
                                               RectInt.Create(0, 0, 10, 10));
```

## 1. 形态：值类型 + 同名门面

与 `Bounds` / `AnimationCurve` 同一套分工（完整说明见 `ScriptBounds.h` 开头）：

| | C++ | .d.ts | 运行时 |
|---|---|---|---|
| 值 | `::Vector2Int` 等 | `declare interface Vector2Int` 等 | 普通 JS 对象（`{x, y}` / `{x, y, width, height}`） |
| 门面 | `Runtime::Vector2Int` 等（ZCLASS） | `declare class Vector2Int` 等 | pesapi 绑定类（静态方法挂在上面） |

TS 的 **class + interface 同名合并**让 `Vector2Int` 既是"一个 `{x, y}` 值"，又
挂着 `Vector2Int.zero` / `Vector2Int.Distance(...)`。这与 `Vector3` / `Color` /
`Bounds` 是同一套路。

```ts
declare interface Vector2Int { x: number; y: number; }
declare class Vector2Int {
    static readonly zero: Vector2Int;
    static Distance(a: Vector2Int, b: Vector2Int): number;   // float
    // ...
}
```

## 2. 整数向量：三处与 float 版不同

### ⚠️ `FloorToInt` 是 floor，不是截断

```ts
Vector2Int.FloorToInt({ x: -0.5, y: 1.7 });   // (-1, 1)
```

`(int)-0.5` 是 **0**，`floor(-0.5)` 是 **-1**。正数上两者完全一样，所以只用正数
的测试分不出这两者 —— 判据必须是负数（`UnityParitySmokeTest` 的 IV16）。

而 `Converter<Vector2Int>` 从 JS 拿字面量时是**截断**（`{x: 3.7}` → 3），因为它
只是在填一个整数字段。要 floor 语义必须显式调 `FloorToInt`。

### ⚠️ `RoundToInt` 平局取**偶**

与 `Mathf.RoundToInt` 同一规则（`std::rint`，banker's rounding），不是
`std::round` 的"平局远离零"：

| 输入 | `RoundToInt` | `std::round` |
|---|---|---|
| 0.5 | **0** | 1 |
| 1.5 | **2** | 2 |
| 2.5 | **2** | 3 |
| 3.5 | **4** | 4 |

只测 0.5 分不出来（"永远向下"的实现也给 0）。判据是 IV18 + IV18b 那一对。

### ⚠️ `magnitude` / `Distance` 是 **float**，`sqrMagnitude` 是 **int**

开方不可能是整数，Unity 的 `Vector2Int.magnitude` 也是 float。返回 int 的实现在
3-4-5 这种整勾股数上正好是绿的 —— 判据用的是 `(1,1)` → √2（IV1b / IV33）。

### 六个方向常量是 Unity 的 Y-up 命名

```ts
Vector3Int.up      // (0, 1, 0)
Vector3Int.forward // (0, 0, 1)     ⚠️ 不是 ZEngine 的 forward
```

⚠️ **这不是 ZEngine 的轴向约定** —— ZEngine 是 Z-up、forward = -Y（见
`doc/TRANSFORM_SCRIPT_API.md`）。这六个只是 Unity 的字面常量（网格 / 瓦片用）。
要"这个物体的前方"用 `transform.GetForward()`，别混用。

## 3. 矩形：三处反直觉

### ⚠️ 负的 `width` / `height` 是合法的，且 `min` / `max` 会归一化

Unity 内部存的是两条边（`m_XMin` / `m_XMax`），`x` 是较小那条、`width` 是
`xMax - xMin`。这里存的是 `x + width`，所以那四个 getter 自己做 Min/Max：

```ts
const flipped = Rect.Create(10, 0, -10, 10);
Rect.GetXMin(flipped);   // 0   —— 不是 10
Rect.GetXMax(flipped);   // 10
Rect.GetSize(flipped);   // (-10, 10)  —— size 是**原样**返回 width/height
Rect.GetCenter(flipped); // (5, 5)     —— center 才是几何量
```

⚠️ 注意 `size` / `position` **不**归一化：`size` 就是 `{width, height}`（可以为
负），`position` 就是 `{x, y}`（不是 `min`）。只有 `min` / `max` / `center` /
`Contains` / `Overlaps` 走归一化后的两条边。

### ⚠️ `Contains` 是**半开**区间 `[min, max)`

```ts
const r = Rect.Create(0, 0, 10, 20);
Rect.Contains(r, { x: 0,  y: 0  });   // true   (>= 低端)
Rect.Contains(r, { x: 10, y: 5  });   // false  (<  高端) ⚠️
Rect.Contains(r, { x: 5,  y: 20 });   // false
```

这与 `Bounds.Contains`（**闭**区间，边界算包含）相反，是 Unity 里最出名的一处
不一致。判据是 RC9。

### ⚠️ `Overlaps` 用严格不等号

只共享一条边的两个矩形**不算**相交：

```ts
Rect.Overlaps(Rect.Create(0, 0, 10, 10), Rect.Create(10, 0, 10, 10));   // false ⚠️
Bounds.Intersects(/* 同样的位置 */);                                     // true
```

## 4. `Rect` 与 `RectInt` 的成员**刻意不齐**

照搬 Unity 的真实形状，不是漏实现：

| | `Rect` | `RectInt` |
|---|---|---|
| `min` / `max` | `Vector2` | `Vector2Int` |
| `size` / `position` | `Vector2` | `Vector2Int` |
| `center` | `Vector2` | `Vector2` ⚠️（float —— 奇数尺寸的中心落在半格上） |
| `ClampToBounds` | ❌ Unity 没有 | ✅ |
| `PointToNormalized` / `NormalizedToPoint` / `OrderMinMax` | ✅ | ❌ Unity 没有 |
| `Contains(x, y)` 分量版 | `ContainsXY` | `ContainsXY` |

⚠️ 绑定表按脚本名索引，两个同名的 `Contains` 后者会覆盖前者，所以分量版只能
改名（`Contains(Vector2)` 与 `ContainsXY(x, y)`）。

没导出的：`RectInt.allPositions` / `allPositionsWithin` 是 `IEnumerator`，
ZEngine 的脚本没有迭代器协议（协程 yield 的是 `YieldInstruction`）。用 `Vector2Int`
双层 for 循环代替。

## 5. 所有"改"都是**非破坏**的

Unity 里 `rect.xMin = 3` 是原地改（struct 字段可写）。这里的值跨边界是**快照**，
原地改没有意义，所以一律返回新值：

```ts
const moved = Rect.SetXMin(r, 3);   // ⚠️ 不接返回值 = 没改，且不报错
```

这与 `AnimationCurve.AddKey` 的坑同源（见 `doc/ANIMATION_CURVE_SCRIPT_API.md`）。

## 6. 测试落点

| 组 | 位置 | 钉什么 |
|---|---|---|
| IV0–IV45 | `UnityParitySmokeTest` `TestIntVectors()` | 值类型 + 门面算术，floor / 平局取偶 / float magnitude |
| RC0–RC53 | `UnityParitySmokeTest` `TestRects()` | 半开区间、负 width 归一化、严格 Overlaps、ClampToBounds |
| RCW0–RCW16 | `UnityParitySmokeTest` `TestRectWiring()` | 源码级：两个 using、四个 Converter、四个 `declare interface`、`ClampToBounds` 只在 RectInt 上 |
| X0–X3 | `ScriptComponentSmokeTest` | **tsc 认这四个名字**（值类型 interface + 同名静态 class 的合并）+ 端到端取值 |

变异测试 `py .mutate_iv.py`：25 个变异体。
