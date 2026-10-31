# Bounds / Color32 脚本 API

`engine/Source/Runtime/Scripting/ScriptBounds/ScriptBounds.h`
`engine/Source/Runtime/Scripting/ScriptColor/ScriptColor.h`（`Color32` 那半）
`engine/Source/Runtime/Core/Math/Bounds.h`
`engine/Source/Runtime/Core/Color/Color.h`（`Color32` 值类型）

对照 Unity：`UnityEngine.Bounds` / `UnityEngine.Color32`。

在这之前，这两个名字在整个 `engine/` 里是**零命中**的。

---

## 1. 它们是什么形态

与 `Color` / `Vector3` 一样，分成两半（同一套分工的完整说明见
`ScriptColor.h` 开头）：

| | C++ | .d.ts | 运行时 |
|---|---|---|---|
| 值 | `::Bounds`（`center` + `extents`）/ `::Color32`（四个字节） | `declare interface Bounds` / `declare interface Color32` | 普通 JS 对象 |
| 门面 | `Runtime::Bounds` / `Runtime::Color32`（`ZCLASS`） | `declare class Bounds` / `declare class Color32` | pesapi 绑定类的构造器（静态方法挂在上面） |

TS 里 `declare class X` 与 `declare interface X` **同名合并**：interface 补实例
成员（也就是那两个/四个字段），class 带静态方法。所以 `{ center: ..., extents: ... }`
这样的字面量可以直接当 `Bounds` 用，同时 `Bounds.Create(...)` 也能调。

### 与 Unity 的形状差异（都是被工具链逼出来的，不是取舍）

1. **写不出 `new Bounds(center, size)`。**
   `ZHeaderTool` 绑不了构造函数（`parser.py`: *"a constructor cannot be bound
   (no return type)"*），所以脚本拿不到"带实例方法的 Bounds 对象"。给了
   `Bounds.Create(center, size)`，返回的是值。

2. **Unity 的实例方法变成静态函数，第一个参数是"自己"。**
   `b.Contains(p)` → `Bounds.Contains(b, p)`。凡 Unity 那边本来就是静态的
   （`Vector3.Distance` / `Color.Lerp`）形状完全一致；只有 Unity 的 struct 实例
   方法退化成这种形式。

3. **两个重载没法同名导出。**
   绑定表按脚本名索引（pesapi `set_method_info`），两个 `Encapsulate` 后者会覆盖
   前者，第一个永远调不到。所以是 `Encapsulate(point)` +
   `EncapsulateBounds(other)`、`Expand(amount)` + `ExpandVector(amount)`。

---

## 2. Bounds

```
interface Bounds { center: Vector3; extents: Vector3; }   // extents 是**半**尺寸

class Bounds {
    static Create(center: Vector3, size: Vector3): Bounds;      // ⚠️ size 不是 extents
    static FromMinMax(min: Vector3, max: Vector3): Bounds;
    static GetSize(b: Bounds): Vector3;
    static GetMin(b: Bounds): Vector3;
    static GetMax(b: Bounds): Vector3;
    static Contains(b: Bounds, point: Vector3): boolean;
    static Intersects(a: Bounds, b: Bounds): boolean;
    static ClosestPoint(b: Bounds, point: Vector3): Vector3;
    static SqrDistance(b: Bounds, point: Vector3): number;
    static IntersectRay(b: Bounds, ray: Ray): number;           // 不命中返回 -1
    static Encapsulate(b: Bounds, point: Vector3): Bounds;
    static EncapsulateBounds(b: Bounds, other: Bounds): Bounds;
    static Expand(b: Bounds, amount: number): Bounds;
    static ExpandVector(b: Bounds, amount: Vector3): Bounds;
    static ToString(b: Bounds): string;
}
```

### 三条照抄 Unity、写错又不报错的细节

**`extents` 是半尺寸。** `Create(c, s)` 里第二个参数是 `size`：`extents = s / 2`。
传错成 extents 会得到一个正好小一半的盒，而 min/max/size 全都自洽 —— 没有任何
征兆。

**`Expand(amount)` 只让 `size` 增大 `amount`。** Unity 的实现是
`amount *= .5f; extents += new Vector3(amount, amount, amount);`。漏掉那个 `0.5`
会得到大一倍的触发体积，照样"看起来对"。

**`Encapsulate(Bounds)` 包的是两个角，不是 center。** 只包 center 会漏掉对方
**比自己中心更远的那一半**：把 `[-5, 15]`（center 5、extents 10）包进 `[0, 0]`
时，只包 center 得到 `[0, 5]`，包角才得到 `[-5, 15]`。照 Unity 包角永远不会漏。

**`Contains` 的边界算包含**（`min <= p <= max`），`Intersects` 的贴面算相交。
Unity 的 `default(Bounds).Contains(Vector3.zero)` 是 `true`。

### `IntersectRay`

`IntersectRay(b, ray)` 返回射线参数 `t`（`hit = origin + direction * t`），
`direction` 未归一化时它不是世界距离 —— 与 Unity 的 `out distance` 一致。
起点在盒内时返回 `0`；不命中返回 **-1**（不是 NaN：NaN 参与 `if (d > 0)` 比较会
静默为 false，和"打中了但很近"没法区分）；`ray` 为 `null` 也是 -1。

方向某一分量为 0 时走"起点必须已经在该轴范围内"的分支，**不除零**。先除零会
产生 ±inf/NaN，而 NaN 参与后面的 `min`/`max` 比较会让 `t_enter > t_exit` 不成立
⇒ 假命中。

---

## 3. Color32

```
interface Color32 { r: number; g: number; b: number; a: number; }   // 0..255

class Color32 {
    static Lerp(a: Color32, b: Color32, t: number): Color32;        // t 夹到 [0,1]
    static LerpUnclamped(a: Color32, b: Color32, t: number): Color32;
    static FromColor(c: Color): Color32;
    static ToColor(c: Color32): Color;
    static ToString(c: Color32): string;                            // "RGBA(255, 0, 0, 255)"
}

// Color 侧也留了一对入口，省得"从 Color 出发"要跳到另一个类
Color.ToColor32(c: Color): Color32;
Color.FromColor32(c: Color32): Color;
```

### 转换是**截断**不是四舍五入

Unity 用 `(byte)(Clamp01(v) * 255f)`，C# 的 `(byte)float` 截断，所以：

- `Color(0.5)` → **127**（不是 128）
- `Color → Color32 → Color` **不保等**（0.5 → 127 → 0.498）

这是 Unity 的行为，不是 bug。想保等就别在两者之间反复转。

`LerpUnclamped` 的 "unclamped" 只针对 `t`：字节值域仍然夹到 0..255。否则
`static_cast<uint8_t>(510.0f)` 是 **UB**（MSVC 给 254，clang 给 255），同一个调用
在不同编译器下得到不同的颜色。

### 边界上的一条规则

TS 字面量省略 `a` 时按**不透明**处理（`a = 255`），对应 Unity 的
`new Color32(r, g, b)`。这条规则在 `ZScript::Converter<Color32>` 里，不在
`::Color32` 的默认值里 —— 后者的默认必须是全 0，才是 Unity 的
`default(Color32)`。

---

## 4. 测试分布

| 组 | 位置 | 断言什么 |
|---|---|---|
| `BC0..BC63` | `UnityParitySmokeTest` | 算术本身（截断/半尺寸/边界/平方/NaN 除零/非破坏性） |
| `BCW0..BCW11` | 同上 | 接线门：d.ts 真的声明了 interface、真的有 Converter、没收进取整助手、null 射线有判空 |
| `M1..M21` | `ScriptVMSmokeTest` | 发布面：从 JS 里 `typeof Bounds`、每个静态方法、字面量省略 `a` 的规则 |
| `J1..J2` | `ScriptComponentSmokeTest` | 端到端：`Mover.ts` 里的 `Bounds.Create` / `Color32.FromColor` 真的过了 tsc，产物真的跑通 |

`Mover.MeasureBounds()` 是唯一同时经过 tsc 类型检查和引擎后端的路径。
