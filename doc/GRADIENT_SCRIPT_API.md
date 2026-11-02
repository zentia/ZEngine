# `Gradient` 脚本门面 + `Collider.bounds` / `GameObject` 属性补项

TS 侧：`Gradient`、`GradientColorKey`、`GradientAlphaKey`、`collider.bounds`、
`collider.ClosestPointOnBounds(p)`、`go.name`、`go.transform`。

代码位置：

| 文件 | 内容 |
| --- | --- |
| `Runtime/Core/Color/Gradient.h` | `GradientMode` / `GradientColorKey` / `GradientAlphaKey` / `Gradient`（值类型，内联 `Evaluate`） |
| `Runtime/Scripting/ScriptColor/ScriptColor.{h,cpp}` | `Runtime::Gradient` 门面（9 个静态/工具方法） |
| `Runtime/Scripting/Binding/ScriptBinding.{h,cpp}` | 三个 `Converter` + 三个 `declare interface` |
| `Runtime/Scripting/ScriptCollider/ScriptCollider.{h,cpp}` | `GetBounds()` / `bounds` 属性 / `ClosestPointOnBounds()` |
| `Runtime/BaseClasses/GameObject.h` | `name` / `transform` 两个 ZSCRIPTPROPERTY |

---

## 1. `Gradient`：值类型 + 门面（与 `Color` / `AnimationCurve` 同套路）

`Gradient` 是 Unity 的**值类型**（struct）：脚本里传的是一个普通 JS 对象
（`declare interface Gradient`），9 个工具方法挂在同名的门面类上
（`declare class Gradient`，由 ZHeaderTool 从 `ScriptColor.h` 生成）。TS 的
class + interface 同名合并正好给出 Unity 的形状：

```ts
const g: Gradient = { colorKeys: [...], alphaKeys: [...], mode: GradientMode.Blend };
const c: Color = Gradient.Evaluate(g, 0.5);
```

### 非破坏式修改约定（与 `Rect` / `AnimationCurve` 一致）

`WithColorKeys` / `WithAlphaKeys` / `WithMode` **返回新值**，不改传入的那个：

```ts
const g2 = Gradient.WithMode(g, GradientMode.Fixed);   // g 不变
```

⚠️ 同理，**绝不给值类型的集合挂 `ZSCRIPTPROPERTY` 的 setter**：
`g.colorKeys = [...]` 这种写法如果只改了脚本侧那个临时对象，返回值会被静默丢掉。
要改集合就走 `WithColorKeys`（返回新值）或 `SetKeys`（作用于门面持有的那一份）。

### `Evaluate` 的语义（照 Unity 复刻）

* 颜色轨道（`colorKeys`）与 alpha 轨道（`alphaKeys`）**分开插值**；
* **alpha 只来自 `alphaKeys`**。即使 colorKeys 里的颜色带了 alpha，也不参与 ——
  从 Unity 搬过来的渐变如果只填了颜色轨道，会得到一个**全不透明**的渐变，这是
  Unity 的真实行为，不是 bug；
* `GradientMode.Fixed` 只影响**颜色**轨道（该段取左端颜色，不做插值）；alpha 轨道
  永远是 Blend —— Unity 也是这样；
* 两条轨道都按 `time` 排序（`Evaluate` 内部排，不要求调用方排好）；
* 空轨道 → 不透明白（`Color(1,1,1,1)`）。

---

## 2. `Collider.bounds` / `ClosestPointOnBounds`

### `bounds`

返回一个 `Bounds` 值（中心 + 半长）。与 Unity 一致：是**世界空间**的轴对称包围盒。

⚠️ 它是**快照**：读一次算一次。每帧读多次的场合自己缓存 —— 但缓存了就要在物体
移动之后失效，否则拿到的是旧位置的盒子。

### `ClosestPointOnBounds`

Unity 的语义是"包围盒**表面**上离给定点最近的点"，与 `ClosestPoint`（碰撞体表面）
不同：

* 点在盒**外**：与 `ClosestPoint` 同值（最近的表面点）；
* 点在盒**内**：**只夹一个轴**（最近的那一面），不是夹到角上。这是 Unity 的行为
  —— 它给的是"最近的那个面上的点"，不是"最近顶点"；
* 包围盒无效时返回**点本身**（不是原点 —— 返回原点会让"没算出来"看起来像
  "算出来是原点"）。

---

## 3. `GameObject.name` / `GameObject.transform`

Unity 里这两个是**属性**不是方法。ZSCRIPTPROPERTY 把已有的 ZFUNCTION getter/setter
对发布成 pesapi 访问器属性，所以脚本两种拼写法都能用：

```ts
go.name = "Enemy";        // 走 SetName
const t = go.transform;   // 走 GetTransform（只读）
```

`transform` **只读** —— Unity 的 `transform` 也是只读（每个 GameObject 有且只有一个
Transform，换掉它没有意义）。

与 `tag` / `layer` 同一批：Unity 搬过来的代码写的是属性，缺了它们每个调用点都要
改写 —— 而"方法不存在"在运行时是静默无事发生，只能靠源码门看得见。

---

## 4. 三个新 `Converter` 的摆放位置

`Converter<GradientColorKey>` / `<GradientAlphaKey>` / `<Gradient>` 必须放在
`Converter<Color>` **之后**（`ScriptBinding.h`）。

原因：`Converter<GradientColorKey>::ToJs` 内部要用 `Converter<Color>::ToJs`。
声明在前的话，MSVC 报一串

```
error C2039: 'ToJs': is not a member of 'ZScript::Converter<Color,void>'
error C2908: ... already instantiated
```

—— 错误信息指向 `Color`，真正的原因却是"顺序错了"。这条坑在
`Converter<ColorParseResult>` 上已经踩过一次。
