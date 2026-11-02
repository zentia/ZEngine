# `LayerMask` / `Vector3` 静态面补齐 / `Mathf.PerlinNoise1D`

对应 Unity：`UnityEngine.LayerMask`、`UnityEngine.Vector3` 的 `Slerp` /
`RotateTowards` / `ProjectOnPlane` / `SignedAngle`、`UnityEngine.Mathf.PerlinNoise1D`。

这一片的共同点是：**算法和数据一直都在，缺的是 Unity 的拼写**。

* 层表一直在 `TagLayerManager` 里，`Physics.LayerToName` / `Physics.NameToLayer`
  也早就有了 —— 但从 Unity 搬过来的代码写的是 `LayerMask.GetMask(...)`；
* `Slerp` / `RotateTowards` / `ProjectOnPlane` / `SignedAngle` 一直在
  `VectorMath` 上 —— 但 Unity 写的是 `Vector3.Slerp(...)`。

"方法不存在"在运行时是**静默无事发生**（`InvokeMethodNumber` 对找不到的方法
不报错），所以这几条只能靠源码门 + tsc 看得见。

| 文件 | 内容 |
|---|---|
| `Runtime/Scripting/ScriptLayerMask/ScriptLayerMask.{h,cpp}` | `Runtime::LayerMask`（新） |
| `Runtime/Scripting/ScriptVector/ScriptVector.{h,cpp}` | `Vector3.Slerp` / `RotateTowards` / `ProjectOnPlane` / `SignedAngle` |
| `Runtime/Scripting/ScriptMath/ScriptMath.h` | `Mathf.PerlinNoise1D` |

```ts
const mask: number = LayerMask.GetMask(["Enemy", "Player"]);
Physics.Raycast(origin, dir, 100, mask);
LayerMask.MaskContainsLayer(mask, LayerMask.NameToLayer("Enemy"));   // true

Vector3.Slerp(a, b, 0.5);                       // 走弧，长度不塌
Vector3.RotateTowards(cur, target, 0.1, 0);     // 弧度！不是度
Vector3.ProjectOnPlane(v, groundNormal);        // 法线不必是单位向量
Vector3.SignedAngle(from, to, up);              // 有符号，度
```

## 1. `LayerMask` 为什么不做成 struct

Unity 的 `LayerMask` 是包着一个 `int` 的 struct，带 `int <-> LayerMask` 的隐式
转换，所以 C# 里写得像整数。ZEngine 让静态方法直接返回 `number`：

1. TS 没有隐式转换。做成 `{value: number}` 之后每个调用点都要写
   `LayerMask.GetMask([...]).value`，而 Unity 那边不写 —— 搬代码时多出来的那
   半个调用点比"少一个 struct"更难发现。
2. `Physics.Raycast` 那批查询的 `layerMask` 参数已经是 `number`（Unity 同）。
   两边都是 number 才对得上。
3. 只有静态方法的 `declare class LayerMask` 与 `Mathf` / `Time` / `Physics`
   同一形态（ZCLASS 必须派生 Object，它们都是）—— 不引入新概念。

## 2. ⚠️ 第 31 位是负数

`<<` 在 JS 里是 32 位**有符号**运算，`1 << 31 === -2147483648`。层 31 的掩码
因此是负数 —— 这不是错（`Physics` 那边的 `layerMask` 也是 `int32_t`，位模式一致），
但"掩码是正数"这个直觉会让人写出 `mask > 0` 这类判断，而它在只有第 31 位置位时
是假的。

所以判某层在不在掩码里**用 `MaskContainsLayer`**，不要自己 `&`。判据 LM10–LM14。

## 3. ⚠️ `GetMask` 遇到未知层名：**跳过**，不是返回 -1

Unity 那边遇到不认识的层名会返回 -1（整张掩码作废）。ZEngine 选择跳过。

两者的失败方向相反，而方向决定了排查成本：

| | 掩码 | 表现 |
|---|---|---|
| 返回 -1（Unity） | 所有层 | 射线打到**所有**东西，包括 UI 和触发器 —— 像"物理有点吵"，很难联想到拼错的层名 |
| 跳过（**ZEngine**） | 少一层 | 射线**打不到**本该打到的东西 —— "这个物体不响应点击"，直接指向层名 |

与 `TagLayerManager::GetMask` 保持一致（那才是数据所在处）。判据 LM9。

`NameToLayer` 找不到时给 **-1**，不是 0 —— 0 是 Default，是合法且最常用的层。
给 0 的话"拼错层名"会静默变成"打 Default 层"，而那里东西最多，射线照样有结果。

## 4. `Vector3.Slerp` vs `Vector3.Lerp`：差别在**长度**

两者在同一条 45° 射线上，只有长度分得开：

| | `Slerp(+X, +Y, 0.5)` | `Lerp(+X, +Y, 0.5)` |
|---|---|---|
| 方向 | 45° | 45° |
| 长度 | **1.0000** | **0.7071** |

所以测试断言的是长度（VX0 / VX1），不是方向 —— 只测方向的夹具分不开这两个函数。

## 5. `Vector3.RotateTowards` 的角度是**弧度**

第一个角度参数是 `maxRadiansDelta`（**弧度**），而 `Vector3.Angle` 返回的是
**度**。这个不一致是 Unity 自己的（参数名就叫 maxRadiansDelta），照搬比"统一成度"
安全：搬过来的代码不会静默差 57 倍。

长度是**单独**按 `maxMagnitudeDelta` 走的：用"方向和长度一起走"的实现，
`maxMagnitudeDelta` 为 0 时等于一步没动。判据 VX7（`2 → 3` 的长度 + 到达 +Y
的方向）。

## 6. `Vector3.ProjectOnPlane` 的法线不必是单位向量

`ProjectOnPlane(v, n) == v - Project(v, n)`，而 `Project` 自己除了 `n·n`。所以
`ProjectOnPlane((1,2,3), (0,0,2))` 与 `(0,0,1)` 给同一个结果。

⚠️ 只测单位法线的夹具等于没测 —— 忘了除 `n·n` 的实现在单位法线上照样对。判据 VX9。

## 7. `Vector3.SignedAngle`：符号才是重点

符号取 `axis · (from × to)`。`Angle` 永远给 0..180，而"往左转还是往右转"必须有
符号 —— 少了它转向逻辑只能靠试。

⚠️ 少了符号的实现（`Angle`）**绝对值与正确答案相同**，所以只测 `== 90` 会漏。
判据是一对**互补**的：`SignedAngle(+X, +Y, +Z) == +90` 与
`SignedAngle(+Y, +X, +Z) == -90`（VX10 / VX11）。

## 8. `Mathf.PerlinNoise1D`

⚠️ **不是** Unity 的逐位实现（与 `Mathf.PerlinNoise` 同一个理由），值域、确定性、
连续性一致，但同一个 `x` 的具体数值不同。

⚠️ 也没有实现成 `PerlinNoise(x, 0)`：y = 0 落在格点上，`fade(0) = 0`，整条线会
退化成一条更平、更少细节的曲线。这里是**真正的 1D** 梯度噪声（格点上取 ±1 的
梯度，插值后归一）。

有意思的是"整数点上恒为 0.5"这条性质两种实现**都满足**（梯度噪声在格点处的值
就是 0），所以它抓不住这个区别 —— 只有黄金数（MF1 / MF2）能。

## 9. 端到端

`ScriptComponentSmokeTest` 的 Z2–Z5（`Mover.ProbeLayerMask` /
`ProbeVector3Statics`）是唯一能看见"tsc 认这些名字"的地方：

```
0|5|-1|UI|33|32|true|false
0.7071|1.0000|0.7071|0.7071|0.0000|90.000|-90.000|0.13477
```

变异测试：`.mutate_lm.py`。
