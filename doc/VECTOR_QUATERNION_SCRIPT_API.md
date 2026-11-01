# `Vector3` / `Quaternion` 的静态面（Unity 拼写）

TS 侧现在能写：

```ts
const dir: Vector3 = Vector3.up;              // 不是 VectorMath.UnitZ()
const d: number   = Vector3.Distance(a, b);   // 不是 VectorMath.Distance(a, b)
transform.rotation = Quaternion.identity;     // 不是 QuaternionMath.Identity()
```

代码位置：

| 文件 | 内容 |
| --- | --- |
| `Runtime/Scripting/ScriptVector/ScriptVector.h/.cpp` | 两个门面（`Runtime::Vector3` / `Runtime::Quaternion`） |
| `Runtime/Scripting/ScriptMath/ScriptMath.h` | 真正的实现（`VectorMath` / `QuaternionMath`） |
| `Runtime/Scripting/Binding/ScriptBinding.cpp` | `declare interface Vector3 / Quaternion`（值类型） |

---

## 1. 为什么是"又一对同名门面"

`Vector3` / `Quaternion` 是纯值类型（POD，不派生 `Object`），进不了绑定表
（`ZCLASS` 必须派生 `Object`）。它们作为**值类型**跨边界：`ZScript::Converter<T>`
把它们搬成 `{x,y,z}` / `{x,y,z,w}`，.d.ts 里是 `declare interface Vector3`。

于是 Unity 那些**静态成员**（`Vector3.zero`、`Vector3.up`、`Vector3.Distance`、
`Quaternion.identity`）没有地方挂 —— `VectorMath` / `QuaternionMath` 是另一个
名字，从 Unity 搬过来的代码每个调用点都得改。

做法与 `Runtime::Color`（`doc/COLOR_SCRIPT_API.md`）完全一样：

* 门面放进 `namespace Runtime`，值类型一律写全 `::Vector3` / `::Quaternion`
  （命名空间里裸 `Vector3` 指的是门面类，漏 `::` 会立刻编译报错，不静默）；
* TS 侧 `declare class Vector3` 与 `declare interface Vector3` **同名合并**
  （interface 给实例成员、class 带静态成员），已用真 tsc（`--strict`）验过；
* 实现全部转发 `VectorMath` / `QuaternionMath`，**不复制第二份算法**。

`VectorMath` / `QuaternionMath` 保留不动：既有脚本不用改，而且它们的名字在
"这里不是 Unity"这件事上更诚实。

## 2. ⚠️ 轴向：Z-up，不是 Unity 的 Y-up

| 名字 | 这里（Z-up，+X 是左） | Unity（Y-up，+X 是右） |
| --- | --- | --- |
| `up` | **(0, 0, 1)** | (0, 1, 0) |
| `down` | (0, 0, -1) | (0, -1, 0) |
| `forward` | **(0, -1, 0)** | (0, 0, 1) |
| `back` | (0, 1, 0) | (0, 0, -1) |
| `right` | **(-1, 0, 0)** | (1, 0, 0) |
| `left` | (1, 0, 0) | (-1, 0, 0) |

`forward` 取 -Y 的依据是相机默认朝向（`CameraComponent.cpp` 里的
`NEGATIVE_UNIT_Y`）。`right` 取 **-X** 的依据是 right = forward × up =
(-Y) × (+Z) = -X —— 也就是说本引擎里 **local +X 是"左"**。

**名字对的是语义轴，不是分量。** 从 Unity 搬过来的
`transform.position = Vector3.up * 2` 因此仍然把物体举到引擎的天花板方向；
照抄 Unity 的 `(0,1,0)` 才会把物体平推到墙里，而且不报错。想写纯轴向用
`VectorMath.UnitX/Y/Z`。

⚠️ **`Vector3.right` 必须与 `transform.right` 指同一边。** 这两处约定是分开
写下来的（`Transform.h` 的注释 vs 本文件），改一边另一边不会知道；而左右写反
**所有"是不是单位向量"的断言都还是绿的**（两边都还是单位向量、还是互相垂直），
只有把两边放在一起比才分得出来 —— 第一版就是这里写成了 `+X`（照 Unity 抄）。
`UnityParitySmokeTest` 的 BD1–BD8 干的就是这件事，S4/S5 钉 transform 那半个约定。

旋转方向（右手系）：绕 `up`(+Z) 转 +90°，`left`(+X) 落到 `back`(+Y)。
`ScriptVMSmokeTest` 的 K10 钉的就是这条。

## 3. 与 `VectorMath` 唯一的语义差别：`Lerp` 夹不夹 t

| | 夹 t | 不夹 |
| --- | --- | --- |
| Unity | `Vector3.Lerp` | `Vector3.LerpUnclamped` |
| 这里 | `Vector3.Lerp` | `Vector3.LerpUnclamped` |
| 旧门面 | —— | `VectorMath.Lerp`（历史遗留，不夹） |

夹住才是插值，不夹是外推：`t = 2` 时不夹会得到两倍远的坐标，接下去做
`MoveTowards` 或写进 transform 就是"一帧飞出去"。

## 4. 导出的 API

```
// Vector3 —— 静态属性
static readonly zero: Vector3              (0,0,0)
static readonly one: Vector3               (1,1,1)
static readonly up / down: Vector3         +Z / -Z
static readonly forward / back: Vector3    -Y / +Y
static readonly right / left: Vector3      +X / -X
static readonly positiveInfinity / negativeInfinity: Vector3

// Vector3 —— 方法（全部转发 VectorMath）
Distance(a, b): number          SqrDistance(a, b): number
Dot(a, b): number               Cross(a, b): number      // 右手系，顺序敏感
Lerp(a, b, t): Vector3          // 夹 t
LerpUnclamped(a, b, t): Vector3
Magnitude(v): number            SqrMagnitude(v): number
Normalize(v): Vector3           // 零向量返回零向量，不是 NaN
Scale(v, s): Vector3            MoveTowards(a, b, max): Vector3
ClampMagnitude(v, max): Vector3 Reflect(v, n): Vector3
Project(v, n): Vector3          Angle(a, b): number       // 度数
Min(a, b): Vector3              Max(a, b): Vector3

// Quaternion
static readonly identity: Quaternion       (0,0,0,1)
AngleAxis(degrees, axis): Quaternion
LookRotation(forward, up): Quaternion
Slerp(a, b, t): Quaternion      Lerp(a, b, t): Quaternion   // Lerp 是 NLerp
Inverse(q): Quaternion          Dot(a, b): number
Angle(a, b): number             // 度数
RotateTowards(from, to, max_degrees): Quaternion
FromToRotation(a, b): Quaternion
Rotate(q, v): Vector3           // TS 没有运算符重载，Unity 的 `q * v`
Euler(x, y, z): Quaternion      EulerVector(v): Quaternion
GetEulerAngles(q): Vector3      // 度；万向锁时 x 取 0
Normalize(q): Quaternion        // 零四元数返回单位，不是 NaN
Mul(a, b): Quaternion           // 先转 b 再转 a
Nlerp(a, b, t): Quaternion      // 不夹 t
SlerpUnclamped(a, b, t): Quaternion   LerpUnclamped(a, b, t): Quaternion
                                // ⚠️ 两个都关 shortest_path
```

Unity 的 `Quaternion.Slerp` / `Lerp` **夹** `t`，`SlerpUnclamped` /
`LerpUnclamped` 不夹。引擎的 `SLerp` / `NLerp` 都不夹，所以门面里有一个
`ClampT`。漏掉它是静默的：朝向"看着在往那个方向转"，只是转过了头。

`Unclamped` 那一对还额外关掉了 `shortest_path`。开着的话 `dot(a,b) < 0` 时
会把 `b` 取反再插 —— 取反是**同一个旋转**，所以看朝向分不出来，只有 `dot`
的符号能看见（QT25/QT26/QT27）。

## 4.1 ⚠️ `Euler` 的旋转顺序：X -> Y -> Z，且只有一处定义

`Quaternion.Euler(x, y, z)` 合成的是 `qz * qy * qx`（矩阵 `Rz·Ry·Rx`），
**先绕 X，再 Y，再 Z**。与 `Transform.eulerAngles` 是同一套 —— 实现只有
`Quaternion::FromEulerAngles` / `Quaternion::ToEulerAngles` 一处，
`Transform.cpp` 的两个 helper 只做转发。

为什么必须是同一处：两套约定一旦漂移，`Quaternion.Euler(v)` 和
`transform.eulerAngles = v` 会给出两个不同的朝向，而脚本里看不出来（两边都
"能转"，也都能往返）。这正是"定错 = 静默算错"的那一类，所以除了行为断言
（QT4/QT5）还有四条源码级门（QW3/QW4/QW8/QW12/QW13）盯着 owner 唯一。

不做 `GetYaw/GetPitch/GetRoll`：那三个是 Ogre 遗产，按 **Y-up** 定义（yaw 绕
Y、pitch 绕 X、roll 绕 Z）。ZEngine 是 Z-up，绕 Y 在这里是翻滚不是转向 ——
名字与语义对不上，比没有更危险。

万向锁（`|y| == 90`）时 `x` 与 `z` 无法分离，惯例是 `x` 取 0、剩下的全记到
`z` 上（与 Unity 一致）。

Unity 的 `Vector3.magnitude` / `normalized` / `sqrMagnitude` 是**实例**成员；
脚本侧的 Vector3 是 plain object（没有原型），挂不了实例方法，所以这里只能
是函数。同理 `q * v` 写成 `Quaternion.Rotate(q, v)`。

## 5. 测试

| 位置 | 钉住的东西 |
| --- | --- |
| `ScriptVMSmokeTest` K1–K11 | 真 QuickJS：常量、轴向（K2/K3 三个方向互不相等）、`Lerp` 夹不夹、四元数旋转方向 |
| `UnityParitySmokeTest` BC0–BC10 | 源码级：门面在命名空间里、值类型写全 `::`、实现是转发（不许抄第二份）、`Lerp` 夹 t |
| `UnityParitySmokeTest` QT0–QT28 | 欧拉角：单轴对轴、`qz*qy*qx` 顺序（不对称夹具 20/30/40）、90° 基向量落点、往返、万向锁、`Slerp`/`Lerp` 夹 t、`Unclamped` 不翻短弧 |
| `UnityParitySmokeTest` QW0–QW13 | 源码级：`Transform` 与脚本面共用同一个 owner（不许各写一份、不许退回 `GetYaw`） |
| `.mutate_qe.py` QE01–QE19 | 19 个变异体，18 杀。QE02（`FromEulerAngles` 漏 `normalise`）是**已记录的可接受存活项**：三个单位四元数相乘仍近乎单位，漂移 < 1e-7，由 QT28 钉住这个上界 |
