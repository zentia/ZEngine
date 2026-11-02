# `Mathf.SmoothDamp` / `Mathf.SmoothDampAngle` / `Vector3.SmoothDamp`

对应 Unity：`UnityEngine.Mathf.SmoothDamp`、`UnityEngine.Mathf.SmoothDampAngle`、
`UnityEngine.Vector3.SmoothDamp`。

一个"弹簧-阻尼"式的渐进：不用自己记时间、不用手动积分，给 `smoothTime`（大致
"到达目标所需的时间"）就能得到不抖的跟随。相机跟随、UI 缓动、角色转向都靠它。

代码：

| 文件 | 内容 |
|---|---|
| `Runtime/Core/Math/SmoothDamp.h` | `SmoothDampResult` / `Vector3SmoothDampResult` + `SmoothDampDetail::Step`（新） |
| `Runtime/Scripting/ScriptMath/ScriptMath.h` | `Mathf::SmoothDamp` / `Mathf::SmoothDampAngle` |
| `Runtime/Scripting/ScriptVector/ScriptVector.{h,cpp}` | `Runtime::Vector3::SmoothDamp` |
| `Runtime/Scripting/Binding/ScriptBinding.{h,cpp}` | 两个 `Converter` + 两个 `declare interface` |

```ts
// ⚠️ 速度是跨帧状态：必须接住 r.velocity，否则每帧都从静止重来。
let pos: number = 0;
let vel: number = 0;

OnUpdate(dt: number): void {
    const r: SmoothDampResult = Mathf.SmoothDamp(pos, 10, vel, 0.3, 0, dt);
    pos = r.value;
    vel = r.velocity;
}

// Vector3 版：三个分量各一套
const v: Vector3SmoothDampResult = Vector3.SmoothDamp(
    this.transform.position, target, this.vel, 0.3, 0, dt);

// 角度版：走最短弧（350° -> 10° 是 +20，不是 -340）
const a: SmoothDampResult = Mathf.SmoothDampAngle(350, 10, 0, 0.5, 0, dt);
```

## 1. ⚠️ 为什么返回的是**结构体**，不是一个数

Unity 的签名是

```csharp
static float SmoothDamp(float current, float target, ref float currentVelocity,
                        float smoothTime, float maxSpeed = Infinity,
                        float deltaTime = Time.deltaTime);
```

速度是**跨帧状态**，用 `ref` 传进传出。TS 既没有 `ref` 也没有 `out`，而 ZEngine
的值类型跨边界时是**快照**（改一个传进来的 `{value, velocity}` 不会写回调用方）。
所以只能把"新值 + 新速度"**一起**返回：

```ts
const r = Mathf.SmoothDamp(pos, target, vel, 0.3, 0, dt);
pos = r.value;
vel = r.velocity;      // ⚠️ 忘了这行 = 每帧从静止重新开始 = 永远慢一拍
```

这与 `AnimationCurve.AddKey` 那个坑同源：**不接住返回值是静默的**。这里多一层
危险：只接 `value` 不接 `velocity` 看着"在动"，只是永远慢一拍 —— 没有报错、没有
NaN，只有手感不对。

## 2. 参数与两个哨兵

| 参数 | 含义 |
|---|---|
| `current` | 当前值 |
| `target` | 目标值 |
| `current_velocity` | 上一帧返回的 `velocity`（第一帧传 0） |
| `smooth_time` | 大致"到达目标所需的时间"（秒）。<= 0 时内部取 0.0001 |
| `max_speed` | 速度上限。**`<= 0` = Unity 的 `Mathf.Infinity`（不限速）** |
| `delta_time` | 步长。**`<= 0` = Unity 的 `Time.deltaTime`（取引擎当前帧的 delta）** |

TS 没有可选参数的默认实参，所以两个默认值用哨兵表达。想显式"不限速"就传 `0`
（或 `Mathf.Infinity`，效果一样）；想用引擎的 dt 就传 `0`。

## 3. ⚠️ `exp` 是 Unity 的**多项式近似**，不是 `std::exp`

Unity 的 SmoothDamp 用的是

```
exp = 1 / (1 + x + 0.48x² + 0.235x³)      （x = omega * deltaTime）
```

而不是 `std::exp(-x)`。两者在 `x` 小的时候非常接近（x = 1/15 时差 6e-5），但
**不相等**；`smoothTime` 很小（`x` 很大）时差得明显。

同一帧 `SmoothDamp(0, 1, 0, 0.5, 0, 1/60)`：

| | value | velocity |
|---|---|---|
| Unity 的多项式（**ZEngine 用的**） | **0.002061** | **0.249485** |
| `std::exp(-x)` | 0.002126 | 0.249469 |

两条曲线**都平滑、都收敛、都不过冲** —— 看不出错了，只有 1e-6 的黄金数能分清。
照抄 Unity 的近似，否则同一份 `smoothTime` 在 ZEngine 里与在 Unity 里不是同一条
曲线。判据是 SD0 / SD1 / SD3，容差取 1e-6（1e-4 会让 `std::exp` 静默通过）。

## 4. ⚠️ `maxSpeed` 的上限是 `maxSpeed * smoothTime`，**不是** `maxSpeed * deltaTime`

夹的是 `change`（当前到目标的距离），上限是 `maxSpeed * smoothTime`。

写成 `maxSpeed * deltaTime` 的话限速仍然存在、单帧看也"确实在限速"，但**随帧率
变** —— 30Hz 与 144Hz 的阻尼不一样。这是最容易顺手写错的一处，因为"速度上限 ×
时间"口语上就是 `maxSpeed * dt`。

| | `SmoothDamp(0, 1000, 0, 0.5, 1, 1/60)` |
|---|---|
| `maxSpeed * smoothTime`（**正确**） | **0.001031** |
| `maxSpeed * deltaTime` | 0.0000344 |
| 完全不限速 | 2.061 |

判据是 SD5（黄金数）+ SD6b（30Hz 与 60Hz 的结果必须不同）。

## 5. 过冲守卫

最后那段"如果已经越过了目标就钉在目标上"不是可有可无的收尾：少了它，`deltaTime`
大 / `smoothTime` 小时输出会**绕着目标来回抖**，而它仍然平滑、仍然收敛 —— 只有逐
帧比对才看得出来。

```
SmoothDamp(0, 1, 100, 0.5, 0, 1/60)  =>  value == 1,  velocity == 0
                                         （不钉住的话会冲到 1.56）
```

注意"初速背离目标"**不**钉住（`SmoothDamp(0, 1, -100, ...)` 得到 -1.56）—— 先退
再进是弹簧的正常行为。判据是 SD7 / SD8 / SD9。

## 6. `SmoothDampAngle`：先走最短弧

```
SmoothDampAngle == SmoothDamp(current, current + DeltaAngle(current, target), ...)
```

少了 `DeltaAngle` 那一步，350° → 10° 会往**反方向**转 340°；而它仍然平滑、仍然
收敛到 10°，只是绕了一大圈。判据是 SD12（`value > 350`，不是 `< 350`）。

## 7. `Vector3.SmoothDamp` 是**分量式**的

三个分量**各自**跑标量版 —— Unity 就是这么做的。于是：

* `maxSpeed` 是"每个分量的上限"，不是合成速度的上限；
* 对角方向（x 和 y 同时到 100）的合成位移是 `√2` 倍（三维是 `√3` 倍）。

别"修"成整向量限速（先把 delta 归一化再夹长度）：那样轴向运动时与 Unity 完全一
致，只有对角夹具才分得开 —— 而它更"合理"，所以更难发现。判据是 SD15（分量必须
等于标量版的结果）+ SD18（对角是 √2 倍）。

## 8. `deltaTime <= 0`：ZEngine 与 Unity 不同

Unity 在 `deltaTime == 0` 的帧上（比如暂停的第一帧）会得到 **NaN**，而 NaN 一旦
进到位置里就再也洗不掉（后面每一帧都是 NaN，视觉上物体消失）。

ZEngine 选择"这一帧不动、速度原样带过去"：

| 输入 | 行为 |
|---|---|
| `delta_time <= 0`（哨兵） | 取 `Time.deltaTime` |
| 取到的引擎 delta 也是 0（暂停） | 返回 `current`，`velocity` 原样返回 |

判据是 SD10 / SD10b / SD10d / SD10e。

## 9. 端到端

`ScriptComponentSmokeTest` 的 Z 段（`Mover.ProbeSmoothDamp`）是**唯一**能看见
"结构体返回值跨边界"的地方：C++ 的 `TestSmoothDamp` 直接调 `Mathf::SmoothDamp`，
压根不经过 `Converter<SmoothDampResult>`；少了那个 Converter，TS 侧拿到
`undefined`，`.value` 也是 `undefined`，`String(undefined)` 照样能拼进探针字符串
—— 不报错。

```
0.002061|0.249485|0.001031|2.061|350.041|0.001031|0.001031|0
```

变异测试：`.mutate_sd.py`（20 个变异体，20/20 被抓）。

第一轮是 17/19，漏掉的两个都是**测试本身的洞**：

* `SDM06`（`Step` 里 `dt <= 0` 的早退）：`dt == 0` 时公式"碰巧"算出"不动"
  （`x == 0` ⇒ `exp == 1`、`temp == 0`、`output == current`）。唯一会触发守卫的
  角落是 `current == target`，那里 `out_velocity = 0/0` 变成 NaN —— 而 **value
  仍然是对的**。补了 SD10b。
* `SDM10`（`Mathf::SmoothDamp` 的 `dt <= 0` 哨兵）：唯一的 `dt <= 0` 用例跑在
  "引擎 delta 也是 0"的时候，于是"取 Time.deltaTime"和"什么都不做"都给出
  "不动"。补了 SD10d / SD10e（先把引擎 delta 设成真值）。
