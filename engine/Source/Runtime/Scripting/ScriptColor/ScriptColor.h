#pragma once

#include "Runtime/BaseClasses/Object.h"
#include "Runtime/Core/Color/Color.h"
// `Gradient` / `GradientColorKey` / `GradientAlphaKey` / `GradientMode`：Unity 的
// "颜色随时间变化"（粒子一生、拖尾、伤害数字上色）。纯 POD + 一个求值函数，
// 不依赖引擎其它部分。
#include "Runtime/Core/Color/Gradient.h"
#include "Runtime/Core/Math/Vector3.h"

#include <EASTL/string.h>
#include <vector>

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

        // ---- Color32 -----------------------------------------------------

        /// Unity 里 `Color -> Color32` 是**隐式转换**（`Color32 c = Color.red;`）。
        /// TS 没有转换运算符，所以给一对显式的互转函数放在 `Color32` 门面上
        /// （`Color32.FromColor` / `Color32.ToColor`）。这里也留一份 `Color`
        /// 侧的入口，让"从 Color 出发"的代码不用跳到另一个类。
        ZFUNCTION()
        static ::Color32 ToColor32(const ::Color& c);

        ZFUNCTION()
        static ::Color FromColor32(const ::Color32& c);

        // ---- Unity 的实例属性（`Color` 是值类型，所以在门面上是静态工具）----
        //
        // Unity 里 `c.grayscale` / `c.maxColorComponent` / `c.linear` / `c.gamma`
        // 是**实例属性**。`::Color` 是跨边界的值类型（d.ts 里的
        // `declare interface Color` 只有 r/g/b/a 四个字段，加不了属性），所以这
        // 批只能像 `Bounds.Contains(bounds, p)` 那样把值当第一个参数传进来：
        //
        //     Unity:      c.grayscale
        //     ZEngine:    Color.GetGrayscale(c)
        //
        // 与 `Bounds` / `Plane` / `AnimationCurve` 同一套路，不是新发明。

        /// 亮度：`0.299r + 0.587g + 0.114b`（Unity 的 `Color.grayscale`）。
        ///
        /// 这组权重来自 Rec.601 的**感知**亮度，不是平均值 —— 正因为不是平均，
        /// "纯绿"比"纯蓝"亮得多（0.587 vs 0.114）。拿 `(r+g+b)/3` 顶替的话，
        /// 做"按亮度自动选前景色"时会在蓝色背景上选错。
        ///
        /// ⚠️ 不含 alpha：alpha 不参与亮度（Unity 同）。
        ZFUNCTION()
        static float GetGrayscale(const ::Color& c);

        /// `max(r, g, b)`（Unity 的 `Color.maxColorComponent`）。
        ///
        /// ⚠️ 同样**不含** alpha —— 名字里的 "color" 指 RGB 三通道。
        ZFUNCTION()
        static float GetMaxColorComponent(const ::Color& c);

        /// gamma 空间 -> 线性空间，逐 RGB 通道，**alpha 不变**（Unity 的
        /// `Color.linear`）。底下就是 `Mathf.GammaToLinearSpace`。
        ///
        /// 用来把"美术在 sRGB 里调出来的颜色"喂给**线性**计算（光照、叠加）。
        /// 混用的表现是"颜色看着比预期亮/暗一截"，而且只在中低亮度上明显 ——
        /// 两端（0 和 1）是不动的，所以纯白纯黑怎么测都对。
        ZFUNCTION()
        static ::Color ToLinear(const ::Color& c);

        /// 线性空间 -> gamma 空间（Unity 的 `Color.gamma`）。与上面互为逆运算。
        ZFUNCTION()
        static ::Color ToGamma(const ::Color& c);

        /// `"RGBA(1.000, 0.000, 0.000, 1.000)"`：三位小数（Unity 的
        /// `Color.ToString()`）。
        ///
        /// ⚠️ 与 `Color32.ToString()` 的差别不只是小数位数，而是**量纲**：那边是
        /// 0..255 的整数，这边是 0..1 的浮点。
        ZFUNCTION()
        static std::string ToString(const ::Color& c);

        /// 色温（开尔文）转颜色。Unity 的
        /// `Mathf.CorrelatedColorTemperatureToRGB`。
        ///
        /// ⚠️ **这是近似值**，不是可验证的精确换算：Unity 只说"把色温转成 RGB"，
        /// 没有公布它用的曲线。这里用的是常见的 Planckian-locus 近似
        /// （Tanner Helland 那套分段幂/对数拟合，1000K..40000K），
        /// 所以**不要指望和 Unity 逐位一致** —— 要一致只能照抄 Unity 的实现，
        /// 而那不在公开文档里。
        ///
        /// 用途是"给灯光一个可信的暖/冷色"（6500K 是白、2000K 是烛光橙、
        /// 10000K 是冷蓝）。alpha 恒为 1。
        ZFUNCTION()
        static ::Color CorrelatedColorTemperatureToRGB(float kelvin);
    };

    /// Unity 的 `UnityEngine.Color32`：四个 0..255 的字节版 `Color`。
    ///
    /// 与 `Color` 一样是"值类型 + 门面"的分成两半（见文件开头的说明）：
    /// `::Color32` 是跨边界的值（.d.ts 里是 `declare interface Color32`），这个
    /// `Runtime::Color32` 只挂静态方法。
    ///
    /// Unity 的 `Color32` **没有** `Color32.red` 那批静态常量（那些在 `Color`
    /// 上），这里同样不补 —— 补了就是 ZEngine 发明的 API。
    ZCLASS()
    class Color32 : public Object
    {
        ZGENERATED_BODY(Color32)

    public:
        /// `Color32.Lerp`：`t` 被夹到 [0,1]，逐通道插值后**截断**成字节。
        ZFUNCTION()
        static ::Color32 Lerp(const ::Color32& a, const ::Color32& b, float t);

        /// `Color32.LerpUnclamped`：`t` 不夹（Unity 2018.1+）。
        ZFUNCTION()
        static ::Color32 LerpUnclamped(const ::Color32& a, const ::Color32& b, float t);

        /// Unity 的 `implicit operator Color32(Color)` 的显式版本。
        ///
        /// ⚠️ 转换是**截断**不是四舍五入（Unity 用 `(byte)(Clamp01(v) * 255f)`）：
        /// `Color(0.5f)` 得到 127 而不是 128。所以
        /// `ToColor(FromColor(c)) != c` 是**正常**的，不是 bug。
        ZFUNCTION()
        static ::Color32 FromColor(const ::Color& c);

        /// Unity 的 `implicit operator Color(Color32)` 的显式版本。
        ZFUNCTION()
        static ::Color ToColor(const ::Color32& c);

        /// Unity 的 `Color32.ToString()`：`"RGBA(255, 0, 0, 255)"`。
        ZFUNCTION()
        static std::string ToString(const ::Color32& c);
    };

    /// Unity 的 `UnityEngine.ColorUtility`：颜色与 **HTML 十六进制串**互转。
    ///
    /// 只有三个成员（`ToHtmlStringRGB` / `ToHtmlStringRGBA` /
    /// `TryParseHtmlString`），但它是"配色表 / 皮肤配置 / 调试输出"这类需求唯一
    /// 的正规入口 —— 没有它，脚本只能自己拼 `%02X`，而自己拼的那一版迟早在
    /// alpha 上和 Unity 对不上（Unity 的 RGB 版**丢掉** alpha）。
    ZCLASS()
    class ColorUtility : public Object
    {
        ZGENERATED_BODY(ColorUtility)

    public:
        /// `"RRGGBB"`（**不带** `#`，**不含** alpha）。Unity 的
        /// `ColorUtility.ToHtmlStringRGB`。
        ///
        /// ⚠️ 与 `Color32` 那边的取整规则**不同**：这里用的是
        /// `RoundToInt(Clamp01(v) * 255)`（四舍五入），而 `Color32(Color)` 是
        /// **截断**。所以 `Color(0.5)` 在 HTML 串里是 `80`，走 `Color32` 是 127。
        /// 这是 Unity 的原样行为 —— 抄成截断的话，同一个颜色在两条路上会得到
        /// 两个串。
        ZFUNCTION()
        static std::string ToHtmlStringRGB(const ::Color& c);

        /// `"RRGGBBAA"`（带 alpha）。同上，四舍五入。
        ZFUNCTION()
        static std::string ToHtmlStringRGBA(const ::Color& c);

        /// 解析 HTML 颜色串，返回 `{valid, color}`。
        ///
        /// 接受的形式（`'#'` 可有可无，大小写不敏感）：
        ///
        ///     "#RGB"      3 位 -> 每位复制成两位（"#F0A" == "#FF00AA"）
        ///     "#RGBA"     4 位
        ///     "#RRGGBB"   6 位 -> alpha 取 1
        ///     "#RRGGBBAA" 8 位
        ///
        /// 其它长度、含非十六进制字符、空串一律 `valid == false`。
        ///
        /// ⚠️ 与 Unity 的差别只在"怎么把失败告诉调用方"：Unity 是
        /// `bool TryParseHtmlString(string, out Color)`，这里是返回一个带
        /// `valid` 的结果对象（TS 没有 out 参数）。**解析失败必须看 `valid`** ——
        /// 失败时 `color` 是全 0（透明黑），不看 valid 直接用的话，配置表里一个
        /// 拼错的色值会变成"这个 UI 不见了"。
        ZFUNCTION()
        static ColorParseResult TryParseHtmlString(const eastl::string& html);
    };

    /// Unity 的 `UnityEngine.Gradient` 门面。
    ///
    /// 与 `Bounds` / `AnimationCurve` 同一套分工：`::Gradient` 是跨边界的**值**
    /// （d.ts 里是 `declare interface Gradient`），这里挂"值类型上的实例方法"。
    ///
    /// ⚠️ 所有"改"返回**新值**，不原地改：Gradient 跨边界时是快照，原地改没
    /// 有意义（同 `Rect.SetXMin` / `AnimationCurve.AddKey`）。**不接住返回值就
    /// 等于没改**，而且不报错。
    ///
    /// ⚠️ 因此这里**没有** `colorKeys` / `alphaKeys` 的 ZSCRIPTPROPERTY：属性
    /// setter 的返回值会被丢弃，装上就是个永久空操作。用 `WithColorKeys` /
    /// `SetKeys` 并接住返回值。
    ZCLASS()
    class Gradient : public Object
    {
        ZGENERATED_BODY(Gradient)

    public:
        /// Unity 的 `new Gradient()`：**两端都是不透明白**，不是空渐变。
        ZFUNCTION()
        static ::Gradient Create();

        /// 按 `time`（0..1）求值（Unity 的 `gradient.Evaluate`）。
        ///
        /// 两端之外取端点值（不循环）；键未排序也能用（内部先排序）。
        /// ⚠️ 返回的 alpha 由 **alphaKeys** 决定，颜色键里自带的 a **不参与**。
        ZFUNCTION()
        static ::Color Evaluate(const ::Gradient& g, float time);

        /// 一次设完两条键序列（Unity 的 `gradient.SetKeys`）。返回新值。
        ZFUNCTION()
        static ::Gradient SetKeys(const ::Gradient& g, const std::vector<GradientColorKey>& color_keys,
                                  const std::vector<GradientAlphaKey>& alpha_keys);

        ZFUNCTION()
        static std::vector<GradientColorKey> GetColorKeys(const ::Gradient& g);

        ZFUNCTION()
        static std::vector<GradientAlphaKey> GetAlphaKeys(const ::Gradient& g);

        /// 非破坏地换掉颜色键 / 透明度键。
        ZFUNCTION()
        static ::Gradient WithColorKeys(const ::Gradient& g, const std::vector<GradientColorKey>& color_keys);

        ZFUNCTION()
        static ::Gradient WithAlphaKeys(const ::Gradient& g, const std::vector<GradientAlphaKey>& alpha_keys);

        /// `GradientMode.Blend`（0，键之间插值）/ `Fixed`（1，阶梯）。
        ///
        /// ⚠️ `Fixed` 只影响**颜色**键 —— alpha 键永远插值（Unity 同）。
        ZFUNCTION()
        static int GetMode(const ::Gradient& g);

        ZFUNCTION()
        static ::Gradient WithMode(const ::Gradient& g, int mode);
    };
}  // namespace Runtime
