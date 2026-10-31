#pragma once

#include "Runtime/BaseClasses/Object.h"
#include "Runtime/Core/Color/Color.h"
#include "Runtime/Core/Math/Vector3.h"

#include "ScriptColor.generated.h"

// =============================================================================
// `Color` —— Unity 的 `UnityEngine.Color` 在脚本侧的那一半。
//
// 为什么分成两个 `Color`
// ----------------------
// 引擎里已经有一个 `class Color`（`Runtime/Core/Color/Color.h`）：r/g/b/a 四个
// float 的 POD，不派生 Object。它进不了绑定表（ZCLASS 必须派生 Object），所以
// 它作为**值类型**跨边界 —— `ZScript::Converter<Color>` 把它当普通
// `{r,g,b,a}` 对象搬运，.d.ts 里是 `declare interface Color`。
//
// 这个 `Runtime::Color` 是**门面**：Unity 那些静态成员（`Color.red`、
// `Color.Lerp`）得挂在某个绑定类上，脚本才能写 `Color.red`。名字必须叫
// `Color`（d.ts 的类名就是 C++ 类名），于是只能放进命名空间避开同名冲突 ——
// 全局再声明一个 `Color` 是编译错误。
//
// TS 侧的 `declare class Color` 与 `declare interface Color` **同名合并**
// （interface 补实例成员，class 带静态成员），合起来正是 Unity 的形状：
// `Color` 既是值类型，又是常量与工具函数的命名空间。
//
// 关于 `::Color` 的写法
// ---------------------
// 在 `namespace Runtime` 内部，裸 `Color` 指的是**这个门面类**，所以值类型一律
// 写全称 `::Color`。漏掉 `::` 会拿到门面类，编译器立刻报错（不会静默）。
//
// 为什么现在能做 `Color.red` 而不是 `Color.GetRed()`
// --------------------------------------------------
// `ZSCRIPTPROPERTY` 早先硬性拒绝静态访问器（parser.py: "getter must not be
// static"），所以 `Time.deltaTime` 只能导成 `Time.GetDeltaTime()`。现在放开
// 了：静态属性走 pesapi 的 `Variables` 表，由后端装到**构造器**上
// （papi-quickjs 的 `FindOrCreateClass` 对 `Variables` 调 `InitProperty(...,
// func)`），实例属性走 `Properties` 装到 prototype，两者互不影响。
// =============================================================================

namespace Runtime
{
    /// Unity 的 `Color` 静态面：常量 + 插值 + HSV 互转。
    ///
    /// 它不该被实例化 —— 它只是脚本侧的命名空间（同 `Resources` / `Time` /
    /// `Mathf`）。之所以还得是个 `Object` 派生类，是因为 ZCLASS 要求。
    ZCLASS()
    class Color : public Object
    {
        ZGENERATED_BODY(Color)

    public:
        // ---- 常量 ----
        //
        // Unity 的这些是静态**属性**（`Color.red`），不是方法。上面说的静态属性
        // 支持就是为它们加的：d.ts 里是 `static readonly red: Color;`，运行时由
        // 构造器上的 getter 提供。Get* 版本照旧存在（ZSCRIPTPROPERTY 的 getter
        // 本身必须是 ZFUNCTION），所以 `Color.GetRed()` 也能用。

        /// (0,0,0,0)。Unity 的 `Color.clear`。
        ZFUNCTION()
        static ::Color GetClear();
        ZSCRIPTPROPERTY(Name = "clear", Getter = "GetClear")

        /// (1,1,1,1)。
        ZFUNCTION()
        static ::Color GetWhite();
        ZSCRIPTPROPERTY(Name = "white", Getter = "GetWhite")

        /// (0,0,0,1)。注意 alpha 是 1 —— "黑色"不是"全透明"。
        ZFUNCTION()
        static ::Color GetBlack();
        ZSCRIPTPROPERTY(Name = "black", Getter = "GetBlack")

        ZFUNCTION()
        static ::Color GetRed();
        ZSCRIPTPROPERTY(Name = "red", Getter = "GetRed")

        ZFUNCTION()
        static ::Color GetGreen();
        ZSCRIPTPROPERTY(Name = "green", Getter = "GetGreen")

        ZFUNCTION()
        static ::Color GetBlue();
        ZSCRIPTPROPERTY(Name = "blue", Getter = "GetBlue")

        /// (1, 235/255, 4/255, 1)。Unity 的黄色不是纯 (1,1,0) —— 它是
        /// RGB(255,235,4)，照抄是为了和 Unity 工程里搬过来的美术资源对得上。
        ZFUNCTION()
        static ::Color GetYellow();
        ZSCRIPTPROPERTY(Name = "yellow", Getter = "GetYellow")

        ZFUNCTION()
        static ::Color GetCyan();
        ZSCRIPTPROPERTY(Name = "cyan", Getter = "GetCyan")

        ZFUNCTION()
        static ::Color GetMagenta();
        ZSCRIPTPROPERTY(Name = "magenta", Getter = "GetMagenta")

        /// (0.5,0.5,0.5,1)。`gray` 与 `grey` 是同一个值（Unity 两种拼写都留着）。
        ZFUNCTION()
        static ::Color GetGray();
        ZSCRIPTPROPERTY(Name = "gray", Getter = "GetGray")

        ZFUNCTION()
        static ::Color GetGrey();
        ZSCRIPTPROPERTY(Name = "grey", Getter = "GetGrey")

        // ---- 插值 ----

        /// 在 a、b 之间线性插值，`t` **被夹到 [0,1]**。Unity 的 `Color.Lerp`。
        ///
        /// 与 `LerpUnclamped` 的差别不是风格问题：把 `t` 从"进度"当成"外推系数"
        /// 用会得到 > 1 或 < 0 的通道值，接下去调 `ApplyToMaterial` 时颜色直接
        /// 溢出。夹住才是插值，不夹是外推。
        ZFUNCTION()
        static ::Color Lerp(const ::Color& a, const ::Color& b, float t);

        /// 同上，但 `t` 不夹。想做"过冲再回弹"的闪光效果时用这个。
        ZFUNCTION()
        static ::Color LerpUnclamped(const ::Color& a, const ::Color& b, float t);

        // ---- HSV ----

        /// H/S/V（各 0..1）转 RGB，alpha 恒为 1。Unity 的 `Color.HSVToRGB`。
        ///
        /// H 是环绕的：`1.25` 等价于 `0.25`。S == 0 时结果是灰（V,V,V,1），
        /// 无论 H 是多少 —— 这是 HSV 的定义，不是特例。
        ZFUNCTION()
        static ::Color HSVToRGB(float h, float s, float v);

        /// RGB 转 H/S/V，返回 `Vector3(h, s, v)`。
        ///
        /// ⚠️ Unity 的签名是 `RGBToHSV(Color color, out float H, out float S,
        /// out float V)` —— TS 没有 out 参数，所以这里把三个分量打包返回。脚本
        /// 侧写 `const hsv = Color.RGBToHSV(c);` 而不是解构三个变量。
        ///
        /// 与 Unity 逐位一致，包括两个"看着像 bug"的地方：灰色（S == 0）的 H 是
        /// **1/3** 而不是 0；只有纯黑（V == 0）的 H 才是 0。改掉任何一条，从
        /// Unity 搬过来的取色代码就会对不上。
        ZFUNCTION()
        static Vector3 RGBToHSV(const ::Color& c);
    };
}  // namespace Runtime
