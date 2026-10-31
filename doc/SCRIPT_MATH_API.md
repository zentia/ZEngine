# 脚本侧数学 API（VectorMath / QuaternionMath / Mathf）

> 配套：时间查询（deltaTime / 累计时间 / timeScale）在 `doc/SCRIPT_TIME_API.md`。
> 注意脚本的 `OnUpdate(dt)` 本来就收到本帧步长 —— 缺的是 `Time.time` 这类
> 累计量与 timeScale，不是 delta 本身。
>
> 端到端（真起一个 QuickJS VM、真 eval JS 走一遍 pesapi 绑定链）见
> `doc/SCRIPT_VM_E2E.md`；`QuaternionMath.ToEuler` 的分量->轴映射就是靠那边的
> C1 确定下来的，见本文第 6 节。

## 1. 为什么是"装静态方法的空类"

脚本拿到的 `Vector3` 是**纯数据**：

```typescript
declare interface Vector3 { x: number; y: number; z: number }
```

一个方法都没有。所以脚本能读写 `transform.position`，却**做不了任何运算** ——
没有加减、没有 `normalized`、没有 `Distance` / `Lerp`。"朝目标走一步"这种最
基本的玩法代码写不出来；`Instantiate` + `Find` + `Destroy` + `SetActive` 全加
起来也只能做"开关对象"。

根因是 `ZCLASS` 的硬约束：

```
zht/parser.py:739:  ZCLASS '%s' must derive from an Object-based class
```

`Vector3` / `Quaternion` 是纯值类型，不派生 `Object`，**进不了绑定表**。它们到
脚本侧只有 `ZScript::Converter` 那一条路（转出一个 `{x, y, z}` plain object），
没有第二条可以挂方法的路。

于是补运算只能靠派生 `Object` 的空类装静态方法。代价是拼写：

| Unity | 这里 |
| --- | --- |
| `Vector3.Distance(a, b)` | `VectorMath.Distance(a, b)` |
| `Vector3.Lerp(a, b, t)` | `VectorMath.Lerp(a, b, t)` |
| `Quaternion.Slerp(a, b, t)` | `QuaternionMath.Slerp(a, b, t)` |
| `Mathf.Clamp01(v)` | `Mathf.Clamp01(v)`（这个一样） |

`Vector3` / `Quaternion` 这两个名字已经被 d.ts 的 interface 占了，同名 class
会撞，所以只能用别的类名。

## 2. 导出的 API

`VectorMath`（30 个）

```
Create(x, y, z) / Zero() / One() / UnitX() / UnitY() / UnitZ()
Add(a, b) / Sub(a, b) / Mul(a, b) / Scale(v, s) / Negate(v)
Magnitude(v) / SqrMagnitude(v) / Distance(a, b) / SqrDistance(a, b)
Dot(a, b) / Cross(a, b) / Normalize(v)
Angle(a, b) —— 度        AngleRad(a, b) —— 弧度
Lerp(a, b, t) / MoveTowards(a, b, maxDistance) / ClampMagnitude(v, max)
Clamp(v, min, max) / Min(a, b) / Max(a, b)
IsZero(v) / Reflect(v, normal) / Project(v, onNormal)
FromToRotation(a, b)
```

`QuaternionMath`（10 个）

```
Identity() / AngleAxis(degrees, axis) / LookRotation(forward, up)
Mul(a, b) / Rotate(q, v) / Inverse(q)
Slerp(a, b, t) / Nlerp(a, b, t) / Angle(a, b) / ToEuler(q)
```

`Mathf`（49 个）

```
-- 插值 / 夹取
Clamp(v, min, max) / Clamp01(v) / Lerp(a, b, t) / LerpUnclamped(a, b, t)
InverseLerp(a, b, v) / SmoothStep(from, to, t) / MoveTowards(a, b, maxDelta)

-- 取整
Ceil(f) / Floor(f) / Round(f) / CeilToInt(f) / FloorToInt(f) / RoundToInt(f)
Abs(f) / Sign(f) / Max(a, b) / Min(a, b)

-- 三角（弧度）/ 幂
Sin(r) / Cos(r) / Tan(r) / Asin(x) / Acos(x) / Atan(x) / Atan2(y, x)
Sqrt(f) / Pow(b, e) / Exp(p) / Log(f) / Log10(f)
Deg2Rad(d) / Rad2Deg(r)

-- 角度（度）
DeltaAngle(cur, target) / LerpAngle(a, b, t) / MoveTowardsAngle(cur, target, maxDelta)

-- 环绕
Repeat(t, length) / PingPong(t, length)

-- 位与比较
Approximately(a, b) / IsPowerOfTwo(n)
NextPowerOfTwo(n) / ClosestPowerOfTwo(n)

-- 噪声与色彩空间
PerlinNoise(x, y) / GammaToLinearSpace(v) / LinearToGammaSpace(v)

-- 常量
Pi() / HalfPi() / TwoPi() / Epsilon() / Infinity() / NegativeInfinity()
```

四个细节值得单独说：

- **`UnitZ()` 是竖直轴**（Z-up 引擎）。
- **`Mul(a, b)` 是分量乘**（Unity 的 `Vector3.Scale`），不是点积也不是叉积 ——
  三个放一起最容易搞混。
- **`Lerp` 不夹取 alpha**（同 Unity 的 `LerpUnclamped`）。想夹取自己先
  `Mathf.Clamp01`。`MoveTowards` 的 `maxDistance` 为负时朝**反方向**走，这也
  与 Unity 一致，不是笔误。`LerpUnclamped` 与 `Lerp` 是同一个实现，提供它只是
  为了把"不夹取"这件事说清楚。
- **`Round` 平局取偶**：`Round(2.5)` 是 **2** 不是 3（Unity 的 `Mathf.Round`
  同规则）。要"四舍五入"自己写 `Floor(f + 0.5)`。
- **`Atan2(y, x)` 是 y 在前**。参数顺序写反在 I 象限里"看起来是对的"。
- **`Repeat` 不是 `fmod`**：`Repeat(-1, 3)` 给 **2**（结果恒在 `[0, length)`），
  `fmod(-1, 3)` 给 -1。角度插值少了这个差别，跨 0° 会往反方向转一大圈。

## 2.1 Mathf 与 Unity 的已知偏差

| 项 | Unity | 本引擎 | 原因 |
| --- | --- | --- | --- |
| `Epsilon` / `Infinity` / `NegativeInfinity` | 静态只读**字段** | **方法**（`Mathf.Infinity()`） | `ZSCRIPTPROPERTY` 的 getter 不允许 static，静态属性在 ZFUNCTION 体系里做不出来。 |
| `Epsilon` 的值 | `float.Epsilon` ≈ 1.4e-45 | `FLT_EPSILON` ≈ 1.19e-7 | `Approximately` 拿它当下限；1.4e-45 那一项恒等于 0，`Approximately` 会退化成纯相对比较，在 0 附近直接失效。 |
| `SmoothDamp` / `SmoothDampAngle` | `ref currentVelocity` | **不提供** | TS 没有 out/ref 参数（同 `RaycastHit` 为什么自带 `valid` 字段）。要阻尼用 `MoveTowards`，或自己在脚本里存速度。 |
| `Log(f, p)` | 双参重载 | **不提供** | ZFUNCTION 体系里同名重载会被当成两个绑定的名字，脚本侧只看到一个。换底自己除：`Log(v) / Log(10)`。 |
| `PerlinNoise(x, y)` | Unity 自己的排列表 | 标准 2D 梯度噪声 | 值域、确定性、连续性一致，但**同一 (x, y) 的具体数值不同** —— 想从 Unity 工程搬一张"看起来一样"的噪声图搬不过来。 |
| `CorrelatedColorTemperatureToRGB` | 返回 `Color` | **不提供** | 本引擎没有 `Color` 值类型。 |

## 3. 刻意没做的两类 API

轴向约定**无法从代码可靠确定**的，一律不做 —— 猜错就是一个静默算错的 API，
比没有更糟。

| 没做 | 原因 |
| --- | --- |
| `Euler(x, y, z)`（欧拉 → 四元数） | 引擎没有现成实现；而 `Quaternion::GetRoll/GetPitch/GetYaw` 的注释是从 Ogre（Y-up）照搬的，三段互相矛盾（GetPitch 说 "local Z onto X and Y"，GetYaw 说 "local Z onto X and Z"），在 Z-up 下对应哪根轴读不出来。只提供反向的 `ToEuler`（读引擎自己的值）。 |
| `Forward()` / `Right()` / `Left()` | 相机默认 forward 是 `NEGATIVE_UNIT_Y`（CameraComponent.cpp:188/195），与 UE 的 X-forward、Unity 的 Z-forward 都不同，且"对象前方"和"相机前方"未必同一约定。只给纯轴向 `UnitX/Y/Z`，方向由脚本自己用 `AngleAxis` / `LookRotation` 造。 |

等真机验证确定了轴向，这两组再补。

## 4. 冒烟测试：这是目前唯一被真正执行过的脚本侧代码

```bash
cmake -B build -DZENGINE_BUILD_SCRIPT_MATH_SMOKE_TEST=ON
cmake --build build --target ScriptMathSmokeTest
ctest --test-dir build -R ScriptMathSmokeTest
```

**63 项检查，本机实跑全过。** 它钉的是两类东西：

1. 本文件自己定义的语义 —— `MoveTowards` 的负步长、`ClampMagnitude` 的边界、
   `Sign(0)`、`Lerp` 不夹取；
2. ScriptMath **依赖**的引擎行为 —— 最关键的是 **`Normalize(零向量)` 返回零向量
   而不是 NaN**（`Vector3::normalise` 内部对 `length == 0` 直接 return）。如果
   引擎哪天改了，V5 会失败，而不是让脚本拿到一个 NaN 坐标。

第二个意义更大：脚本绑定层（70 个类 / 265 个方法）此前**一次都没被执行过** ——
全仓没有任何 .ts 脚本示例、没有任何脚本冒烟测试，之前所有 API 只做过"语法检查
+ generate 能出绑定"两级验证。这三个类的方法全是 `static`，测试可以直接调，
**不经过脚本运行时**，于是成了这一层唯一能真正跑起来并失败的部分。

顺带确认的引擎事实（都对上了）：右手系 `Cross(X, Y) == Z`、绕 +Z 转 90 度把 X
转到 Y、`Slerp(t=0.5)` 正好在 45 度、`Reflect` 遵守"入射背离平面"的前提。

`LookRotation` 与 `ToEuler` 只测了 identity（全零）这一个无歧义的情形 —— 见 §3。
