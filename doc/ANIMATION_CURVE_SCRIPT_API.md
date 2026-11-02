# `AnimationCurve` / `Keyframe` / `WrapMode`

对应 Unity：`UnityEngine.AnimationCurve`、`UnityEngine.Keyframe`、`UnityEngine.WrapMode`。

在这之前这三个名字在整个 `engine/` 里是**零命中**的。

代码：

| 文件 | 内容 |
|---|---|
| `Runtime/Core/Math/AnimationCurve.h` | 值类型 `::AnimationCurve` / `::Keyframe` + `ZENUM() WrapMode` |
| `Runtime/Scripting/ScriptAnimationCurve/ScriptAnimationCurve.{h,cpp}` | 门面 `Runtime::AnimationCurve`（静态方法） |
| `Runtime/Scripting/Binding/ScriptBinding.h` | `Converter<Keyframe>` / `Converter<AnimationCurve>` |
| `Runtime/Scripting/Binding/ScriptBinding.cpp` | `.d.ts` 里两个 `declare interface` |

```ts
const curve: AnimationCurve = AnimationCurve.Linear(0, 0, 10, 20);
const v: number = AnimationCurve.Evaluate(curve, 2.5);   // 5

// 值语义：编辑类方法返回**新**曲线，必须接住返回值。
let c: AnimationCurve = AnimationCurve.Create();
c = AnimationCurve.AddKey(c, 0, 0);
c = AnimationCurve.AddKey(c, 1, 3);
c = AnimationCurve.SetPostWrapMode(c, WrapMode.Loop);
```

## 1. 形态：值类型 + 同名门面

与 `Bounds` / `Color32` 同一套分工（完整说明见 `ScriptBounds.h` 开头）：

| | C++ | .d.ts | 运行时 |
|---|---|---|---|
| 值 | `::AnimationCurve` / `::Keyframe` | `declare interface AnimationCurve` / `declare interface Keyframe` | 普通 JS 对象 |
| 门面 | `Runtime::AnimationCurve`（ZCLASS） | `declare class AnimationCurve` | pesapi 绑定类的构造器（静态方法挂在上面） |

`WrapMode` 是 `ZENUM`，.d.ts 里是 `declare enum WrapMode`，数值与 Unity 一致：

```ts
declare enum WrapMode { Default = 0, Once = 1, Loop = 2, PingPong = 4, ClampForever = 8 }
```

### ⚠️ 与 Unity 唯一的形状差异：编辑类方法返回**新**曲线

Unity 的 `AnimationCurve` 是 class，`curve.AddKey(...)` 就地改自己；这里过边界时
是值（JS 端就是 `{keys, preWrapMode, postWrapMode}`），所以是
`c = AnimationCurve.AddKey(c, t, v)`。

**忘了接返回值的后果是静默的**：曲线一直是空的，而空曲线求值是 0。不报错、
不崩溃。这条差异写在 `ScriptAnimationCurve.h` 的注释里，也钉在
`ScriptComponentSmokeTest` 的 W1 上。

### 两个重载不同名

绑定表按脚本名索引（pesapi `set_method_info`），两个 `AddKey` 后者会覆盖前者。
所以是 `AddKey(time, value)` + `AddKeyFrame(key)`、`CreateKey(time, value)` +
`CreateKeyTangents(time, value, in, out)`。

## 2. 插值：三次 Hermite

相邻两帧 `k1(t1,v1)` / `k2(t2,v2)` 之间：

```
dt = t2 - t1;        u = (t - t1) / dt
m0 = k1.outTangent * dt;   m1 = k2.inTangent * dt
a  =  2u³ - 3u² + 1        b = u³ - 2u² + u
c  =  u³ - u²              d = -2u³ + 3u²
v  =  a*v1 + b*m0 + c*m1 + d*v2
```

**切线是斜率（dValue/dTime），不是角度也不是位移**，乘 `dt` 之后才进 Hermite
基。少了那个 `* dt`，时间间距不同的两段会得到不同的形状 —— 而它在 dt == 1 的
段上碰巧是对的，所以均匀采样的夹具看不出来（变异 ACM04 + 断言 AC8）。

## 3. 三个静态工厂

照 Unity 的实现抄：

```csharp
// Unity
Linear(t0, v0, t1, v1)  =>  slope = (v1 - v0) / (t1 - t0)
                            keys  = { (t0, v0, in=0,     out=slope),
                                      (t1, v1, in=slope, out=0)     }
EaseInOut(t0, v0, t1, v1) => 两条切线都是 0  ⇒ Hermite 退化成 smoothstep
Constant(t0, t1, value)   => 两端同值、切线 0 ⇒ 求值恒等于 value
```

⚠️ `Constant` 的参数顺序是 **(timeStart, timeEnd, value)**，value 在**最后** ——
与 `Linear` / `EaseInOut` 的 `(time, value, time, value)` 不同。这是 Unity 的签名
（它只有"一个"值要恒定）。

⚠️ `Linear` 的端点切线是 0 而不是 slope，这不是"随便填的"：代进 Hermite 基之后
它们正好把三次项消掉。**填成 slope 也得到一条直线**（`b + c` 恰好塌成 `u`），
所以这条没有变异体 —— 取而代之的是"斜率忘了除时长"（ACM01）。

## 4. 环绕模式

| 模式 | 语义 |
|---|---|
| `Default` (0) | 按 `Once` 处理 |
| `Once` (1) | 到端点之后**保持**端点值（Unity 编辑器里的 "Clamp"） |
| `Loop` (2) | 首尾相接，周期 = duration |
| `PingPong` (4) | 往复，周期 = 2 × duration |
| `ClampForever` (8) | 按端点那一条切线**继续外延**（编辑器里的 "Continue"） |

新建曲线的默认 pre/post 是 `Once`：Unity 的文档没写新建曲线的 wrap 是什么，而
"到端点后保持端点值"是绝大多数脚本的预期（`ClampForever` 会让缓动曲线在结束之后
继续冲出去）。脚本随时可以显式设。

### ⚠️ `ClampForever` 用哪一条切线是反直觉的

范围**之前**用的是首帧的 **out** 切线（曲线在首帧右侧的斜率，把它往左延长）；
范围**之后**用的是末帧的 **in** 切线（曲线在末帧左侧的斜率，往右延长）。

用反了的话 `Linear(0,0,1,1)` 之前/之后都不再是 `y = t`。**首末斜率相同**的曲线
分不出方向反没反 —— AC26 / AC27 用的是首帧 out = 1、末帧 in = 3 的曲线。

### ⚠️ Loop 与 PingPong 要用**不对称**的夹具才分得开

`0 → 3 → 0` 这种回原值的曲线关于中点镜像对称，Loop 和 PingPong 在它上面给出
同一个数（第一版 AC29 / AC30 就是这么白测的）。改成 `0 → 3 → 1` 之后，两段中点
分别是 1.5 和 2.0 —— 这才分得开。

## 5. `SmoothTangents`

加权平均，权重取**对面**那一段的时间长度：

```
s1 = 左段斜率, dt1 = 左段时长;   s2 = 右段斜率, dt2 = 右段时长
tangent = (s1 * dt2 + s2 * dt1) / (dt1 + dt2)
```

这样窄的那一段对切线影响更小，曲线不会在密集采样处抖。取算术平均在**时间间距
相等**时给出同一个数 —— AC13 用的是 dt1 = 1 / dt2 = 3 的样本。

首尾帧只有一侧有段，直接用那一侧的斜率。

## 6. 几条"不该是 NaN"的判据

| 情形 | 行为 |
|---|---|
| 空曲线 | 求值是 0 |
| 单帧曲线 | 任何时间都是那一帧的值 |
| duration == 0 且 wrap 是 Loop / PingPong | 取模会除零 ⇒ 显式判掉，返回那一帧的值 |
| 时间是 NaN / ±inf | 返回 0 |

NaN 参与 `<=` / `>=` 恒为 false，不判掉的话它会一路走到"没找到段"的兜底分支 ——
那和"真的没有段"是分不开的。

## 7. 测试与变异

| 组 | 位置 | 断言什么 |
|---|---|---|
| `AC0..AC34` | `UnityParitySmokeTest` | 算术（Hermite 的 dt、加权切线、四个 wrap 模式、排序/去重、NaN、非对称夹具） |
| `ACW0..ACW9` | 同上 | 接线门：三个工厂在门面上、WrapMode 是 ZENUM 且数值对得上、两个 Converter、两条 `declare interface` |
| `W0..W1` | `ScriptComponentSmokeTest` | 端到端：`Mover.ts` 里那六个量真的过了 tsc，产物真的跑通 |
| `.mutate_ac.py` | 20 个变异体 | 见下 |

`.mutate_ac.py` 覆盖：斜率忘除时长、EaseInOut 退化成直线、Constant 丢值、Hermite
漏 `* dt`、SmoothTangents 的两种错权重、AddKey 不排序 / 不去重、MoveKey 不重排、
ClampForever 切线用反、Loop/PingPong 周期互换、默认 wrap 变 ClampForever、NaN 判据
被删、GetDuration 取错、门面 AddKey 什么都没加、SetKeys 不排序、ClearKeys 顺手清
wrap、WrapMode 不是 ZENUM、d.ts 声明被删。

### 两个"夹具自己把判据抹平了"的实例

* **起点为 0 的曲线上，"跨度"和"末帧时间"是同一个数。** 第一版 AC21 用的
  `Linear(0,0,10,20)` 于是让"GetDuration 返回末帧时间"这个变异体活了下来
  （ACM15）。互补的一半是 AC21b：一条**起点不为 0** 的曲线。
* **`0 → 3 → 0` 这种回原值的曲线关于中点镜像对称**，Loop 与 PingPong 在它上面
  给出同一个数。改成 `0 → 3 → 1`（AC28/AC29/AC30）。

### 已知覆盖缺口

* "脚本忘了接 `AddKey` 的返回值"是**用法**错误，不是实现错误，没有变异体能表示
  —— 写在 `ScriptAnimationCurve.h` 的注释里。
