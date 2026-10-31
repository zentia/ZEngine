#pragma once

#include "Runtime/BaseClasses/Object.h"
#include "Runtime/Core/Math/Quaternion.h"
#include "Runtime/Core/Math/Vector2.h"
#include "Runtime/Core/Math/Vector3.h"

#include <cstdint>

#include "ScriptRandom.generated.h"

// 脚本侧的随机数（Unity 的 `UnityEngine.Random` 静态类）。
//
// 这一层只是转发 —— 实现全在 `Runtime/Core/Math/UnityRandom.h` 的
// `UnityRandom`（PCG32，跨机器确定性，状态是一个 uint64，可存档 / 回放）。
// 之前那个类就叫 `Random`，但它**一个 ZFUNCTION 都没有**，所以脚本其实用不
// 了；同时它又不派生自 Object，没法直接挂 ZCLASS。于是：
//
//   * 实现改名 `UnityRandom`（和文件名一致），留在全局；
//   * 脚本面新开一个 `ZCLASS() class Random : public Object`，名字就是 Unity
//     的那个 `Random`。
//
// 与 `Screen` / `Time` / `Input` / `Mathf` 同一套路：**全是静态方法**，所以
// 导出成 `GetX()` 而不是 `x` —— ZSCRIPTPROPERTY 的 getter 不允许 static，
// 静态属性做不出来。
//
// `RangeInt` 与 `RangeFloat` 而不是两个 `Range`
// ---------------------------------------------
// Unity 靠重载区分：
//     Random.Range(int,   int)   -> max **不含**
//     Random.Range(float, float) -> max **含**
// TypeScript 只有一个 number 类型，并且 ZHeaderTool 里**一个脚本名只能被一个
// 重载占用**（见 Transform::SetLocalPosition 的注释），两个 `Range` 是编译不过
// 的。所以按 `Translate` / `TranslateWorld` 的先例拆成两个显式名字：
//
//     Random.RangeInt(min, max)    max 不含（Unity 的整数语义）
//     Random.RangeFloat(min, max)  max 含  （Unity 的浮点语义）
//
// 这个不对称是 Unity **真实存在**的行为，不是笔误，也是著名的坑：
// `Range(0, 10)` 写成整数永远摇不到 10，写成浮点就可以。这里照原样复刻并由
// 测试钉住 —— "修掉"它会静默改变每一个移植过来的脚本摇出的数。

ZCLASS()
class Random : public Object
{
    ZGENERATED_BODY(Random)

public:
    /// 用固定种子重置全局流。同种子 + 同调用序列 = 同结果，**任何平台任何一次
    /// 运行都一样**。回放、lockstep 联网、"种子 12345 时会炸"这种 bug 报告全靠它。
    ZFUNCTION()
    static void InitState(int32_t seed);

    /// 用非确定性来源（时间 + 地址熵）播种。"我不在乎"用它；要可复现就用
    /// `InitState`。
    ZFUNCTION()
    static void InitUnseeded();

    /// 当前状态（一个 uint64）。存档中间点 / 回放起点用它。
    ZFUNCTION()
    static int64_t GetState();

    ZFUNCTION()
    static void SetState(int64_t state);

    /// [0, 1)，**永不返回 1.0** —— 用 `floor(v * n)` 去索引数组的代码依赖这条。
    ZFUNCTION()
    static float value();

    /// 整数区间，**max 不含**。`min >= max` 时返回 `min` 而不是玩花样：问一个空
    /// 区间的调用方本身就有 bug，而 `min` 是最不容易被误当成一次有效摇号的值。
    ZFUNCTION()
    static int32_t RangeInt(int32_t min, int32_t max);

    /// 浮点区间，**max 含**。
    ZFUNCTION()
    static float RangeFloat(float min, float max);

    /// 概率为 `p` 时返回 true。`p <= 0` 恒 false，`p >= 1` 恒 true：一个从浮点
    /// 计算里出来的概率不能被允许在调用方想表达"几乎不可能"时给出"必然"。
    ZFUNCTION()
    static bool Chance(float probability);

    /// 球**内**均匀点。按体积均匀，不是按半径 —— 按半径会在球心堆积，一用来
    /// 散布生成点就能看出来不对。
    ZFUNCTION()
    static Vector3 insideUnitSphere();

    /// 球**面**上的点（长度恰好 1）。
    ZFUNCTION()
    static Vector3 onUnitSphere();

    ZFUNCTION()
    static Vector2 insideUnitCircle();

    /// 圆周上的点（角度 [0, 2pi)）。
    ZFUNCTION()
    static Vector2 onUnitCircle();

    ZFUNCTION()
    static Quaternion rotation();
};
