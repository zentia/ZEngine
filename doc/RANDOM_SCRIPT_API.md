# 随机数脚本 API（`Random`）

对应 Unity：`UnityEngine.Random`。

代码：`Source/Runtime/Scripting/ScriptRandom/ScriptRandom.{h,cpp}`（门面）
实现：`Source/Runtime/Core/Math/UnityRandom.{h,cpp}`（PCG32）
门禁：`UnityParitySmokeTest` 的 **E 组**（实现的行为）、**W 组**（门面的行为）、
**X 组**（接线）。

---

## 1. 为什么是两个类

随机数生成器其实**早就有了**，只是脚本一个入口都调不到：

```
class Random { ... }        // Core/Math/UnityRandom.h——一个 ZFUNCTION 都没有
                            // 而且不派生自 Object，ZCLASS 挂不上去
```

于是出现了一个尴尬的中间状态：C++ 侧能摇号，脚本侧只能自己去写
`Math.random()`——而那个既不可播种、也不可存档，回放和 lockstep 全都免谈。

所以：

* 实现改名 **`UnityRandom`**（顺便和文件名一致了），留在全局，C++ 侧照旧用；
* 新开一个 **`ZCLASS() class Random : public Object`**，名字就是 Unity 的那个
  `Random`，13 个静态方法**纯转发**到 `UnityRandom`。

两个名字都不能省。叫回 `Random` 会和 `ZCLASS Random` 撞成 ODR 违规（症状是
随机崩，不是编译错，X13 守着）；只留 `UnityRandom` 则脚本里还是得写
`UnityRandom.RangeInt(...)`，和每一份移植过来的 C# 都不一样。

---

## 2. API

```ts
// 播种 / 状态
Random.InitState(seed: number): void   // 固定种子，同种子 = 同序列，跨机器一致
Random.InitUnseeded(): void            // 用时间 + 地址熵播种（"我不在乎"）
Random.GetState(): number              // 一个 uint64，够存进存档 / 回放头
Random.SetState(state: number): void   // 精确续上

// 值
Random.value(): number                 // [0, 1)，永不返回 1.0
Random.RangeInt(min, max): number      // max **不含**
Random.RangeFloat(min, max): number    // max **含**
Random.Chance(p: number): boolean      // p<=0 恒 false，p>=1 恒 true

// 形状
Random.insideUnitSphere(): Vector3     // 球内，按**体积**均匀
Random.onUnitSphere(): Vector3         // 球面，长度恰为 1
Random.insideUnitCircle(): Vector2     // 圆内，按**面积**均匀
Random.onUnitCircle(): Vector2         // 圆周，长度恰为 1
Random.rotation(): Quaternion          // 均匀分布在 SO(3) 上
```

---

## 3. 与 Unity 的三处刻意差异

### 3.1 `RangeInt` / `RangeFloat`，不是两个 `Range`

Unity 靠重载区分整数版和浮点版：

```csharp
Random.Range(0, 10);      // int   —— 永远摇不到 10
Random.Range(0f, 10f);    // float —— 可以摇到 10
```

TypeScript 只有一个 `number`，而且 ZHeaderTool 里**一个脚本名只能被一个重载
占用**（同一个坑在 `Transform` 上把 `Translate` 拆成了 `Translate` /
`TranslateWorld`）。两个 `Range` 是过不了 codegen 的，所以按先例拆成两个显式
名字。

**那个不对称本身被原样复刻了**，它不是笔误，是 Unity 真实存在的行为，也是著名
的坑：`Range(0, 10)` 写成整数永远摇不到 10。W9 / W10 与 W13 / W14 从两侧钉住
它，X6 钉住"头文件里写清了这件事"——没有说明的话下一个人会当 bug "修"掉，而
那会**静默改变每一个移植过来的脚本摇出来的数**，且没有任何编译错误。

### 3.2 静态属性做不出来

Unity 里 `Random.value` / `Random.insideUnitSphere` 是**静态属性**，不是方法。

ZHeaderTool 的 `ZSCRIPTPROPERTY` 的 getter 不允许 `static`（`zht/parser.py` 直接
报错），所以静态成员只能发布成方法：脚本写 `Random.value()`，不写
`Random.value`。这和 `Screen` / `Time` / `Application` 那一批是同一个取舍。

X3 守的是"别有人顺手加一个 `ZSCRIPTPROPERTY`"——它会在 codegen 期炸，而报错
指向的是**生成**文件。

### 3.3 门面没有自己的状态

`Random` 和 `UnityRandom` 打在**同一条全局流**上（`Random::GetState()` 读到的
就是 `UnityRandom::s_State`）。

这条看着像废话，其实是最容易坏的一处：门面自带一份状态的话，C++ 侧摇出的数和
脚本侧摇出的数会静默分叉，两边单看都完全正确，要等到"回放对不上"才被发现。
W3（同种子同序列）、W4（状态同一份）、X9（13 个方法全是转发）、X10（门面里
不许出现第二个 RNG）一起守着。

---

## 4. 实现要点（都在 `UnityRandom`，早于本切片）

* **PCG32（xsh-rr 64/32）**而不是 `std::mt19937`。两个理由：`std::` 的分布是
  **实现定义**的，同种子同序列在 MSVC 和 libstdc++ 上不一样，回放和 lockstep 直接
  失去意义；`mt19937` 带 2.5 KB 状态且没法跨平台序列化，而这里整个状态是一个
  `uint64`。
* 整数区间用**拒绝采样**，不是 `% span`。`span` 不整除 2³² 时取模会偏袒小值，
  肉眼看不出来，而它正是"抽奖表不公"投诉的成因。
* `insideUnitSphere` / `insideUnitCircle` 按**体积 / 面积**均匀（拒绝采样），不是
  "随机方向 × 随机半径"——后者会在球心堆点。W22 / W25 用 `E[r²]` 判这件事
  （球 3/5、圆 1/2，按半径则都是 1/3）：只看"有没有出球"是测不出来的。
* `rotation()` 用 Shoemake 三均匀数法，均匀分布在 SO(3) 上；"随机三个欧拉角"
  会在万向节极点附近过采样。W28 用 `E[|w|] = 4/(3π) ≈ 0.424` 判（恒等四元数
  是 1，随机欧拉也明显偏离）。
* `Chance` 对 `p <= 0` / `p >= 1` 短路。一个从浮点计算里出来的概率不能被允许在
  调用方想表达"几乎不可能"时给出"必然"。

---

## 5. 用法

```ts
// 可复现的一局
Random.InitState(this.seed);

// 抽奖：整数区间 max 不含
const index = Random.RangeInt(0, this.lootTable.length);

// 掉落率
if (Random.Chance(this.dropRate)) { /* ... */ }

// 存档中间点
saved.randomState = Random.GetState();
// ... 读档
Random.SetState(saved.randomState);
```
