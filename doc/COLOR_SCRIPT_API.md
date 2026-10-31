# `Color`：值类型 + Unity 的静态面

配套源码：

| 文件 | 作用 |
| --- | --- |
| `engine/Source/Runtime/Scripting/ScriptColor/ScriptColor.h/.cpp` | 脚本门面（`Runtime::Color`，发布成 `Color`） |
| `engine/Source/Runtime/Core/Color/Color.h` | 值类型本身（`class Color { r,g,b,a; }`，POD） |
| `engine/Source/Runtime/Scripting/Binding/ScriptBinding.h` | `ZScript::Converter<Color>`、`PropertyCallback` |
| `engine/Source/Runtime/Scripting/Binding/ScriptBinding.cpp` | 静态属性的注册（`Variables` 表）与 `.d.ts` 声明 |
| `engine/tools/ZHeaderTool/zht/{parser,codegen}.py` | `ZSCRIPTPROPERTY` 放开静态访问器 |

## 0. 这次补的是什么

`Color` 一直存在于 C++（`Core/Color/Color.h`），但**脚本根本拿不到它**：没有
`ZScript::Converter<Color>`，`.d.ts` 里也没有对应的类型。于是"改一个材质的颜色"
"把血条从绿 tween 到红"这类操作在 TS 里无路可走 —— 而这几乎是每个 Unity 项目
第一天就要写的东西。

更麻烦的是 Unity 的 `Color` 一半是值类型、一半是静态命名空间：

```ts
const c: Color = { r: 1, g: 0, b: 0, a: 1 };   // 值类型
const t = Color.Lerp(c, Color.white, 0.5);     // 静态工具
const r = Color.red;                           // 静态常量
```

这三件事在 ZEngine 里分属三套机制，逐一补上：

1. **值类型** —— `Converter<Color>` + `declare interface Color { r,g,b,a }`；
2. **静态工具** —— `ZCLASS` 的静态 `ZFUNCTION`（`Lerp` / `HSVToRGB` / `RGBToHSV`）；
3. **静态常量** —— 新增的**静态脚本属性**（`Color.red`），这是本次唯一的工具链改动。

## 1. 为什么门面叫 `Runtime::Color` 而不是 `Color`

C++ 里已经有一个全局的 `class Color`（值类型），同名类在全局作用域是编译错误，
而 `.d.ts` 的类名直接取 C++ 类名 —— 想让脚本看到 `Color`，就得放进命名空间。

代价是 `namespace Runtime` 内部裸写 `Color` 指的是门面，所以门面里所有值类型都写
全称 `::Color`。漏掉 `::` 会拿到门面类，编译器立刻报错，不会静默。

脚本侧看不到这个分裂：`declare class Color`（门面，带静态成员）与
`declare interface Color`（值类型，带实例成员）在 TS 里**同名合并**，合起来正是
Unity 的形状。合并是否合法由 `ScriptTsChainSmokeTest --dump-dts` 产出的文件交给
真正的 `tsc` 验过。

## 2. 静态脚本属性（本次的工具链改动）

`ZSCRIPTPROPERTY` 此前硬性拒绝静态访问器：

```
ZSCRIPTPROPERTY '%s': getter '%s' must not be static
```

所以 Unity 里所有"其实是静态属性"的东西在 ZEngine 只能导成方法
（`Time.GetDeltaTime()` 对应 `Time.deltaTime`，见 `ScriptTime.h`）。这次放开：

* `parser.py` —— 静态 getter 合法；getter 与 setter 的静态性必须一致（否则
  `Color.red = x` 写到另一个东西上）；
* `codegen.py` —— 属性表带 `.isStatic`；
* `ScriptBinding.cpp` —— `pesapi_set_property_info(..., is_static=1)` 把条目放进
  `Variables` 表，后端（`papi-quickjs` 的 `FindOrCreateClass`）把它装到**构造器**
  上；实例属性走 `Properties`、装到 prototype。两张表各自从 0 编号，个数也要分开传。

### 两个必须知道的坑

**（a）静态访问器不能再出现在方法表里。** 一个 `ZSCRIPTPROPERTY` 的 getter 本身也
是 `ZFUNCTION`，如果两边都发，MSVC 会把同一个 `MethodCallback<F>::Invoke` 编进
**两个 COMDAT 段**，链接器直接拒收整个 obj：

```
LNK1179: invalid or corrupt file: duplicate COMDAT '?Invoke@?$MethodCallback@...'
```

（实例访问器不受影响 —— `Transform.GetPosition()` 一直都发，也没事。）所以
`codegen.py` 会把静态访问器从方法表里摘掉；这同时是更好的 API：`Color.red` 旁边
不该再挂一个 `Color.GetRed()`。

**（b）属性访问器走独立的 trampoline。** `PropertyCallback<F>` 而不是
`MethodCallback<F>`：共用会重新制造上面那个"同一个符号被两处引用"的形状。

## 3. 与 Unity 逐位对齐的常量

| 常量 | 值 | 备注 |
| --- | --- | --- |
| `clear` | `(0,0,0,0)` | 全透明，**不是**黑 |
| `white` | `(1,1,1,1)` | |
| `black` | `(0,0,0,1)` | alpha 是 1；写成 0 就画不出来 |
| `red` / `green` / `blue` | 单位通道 | |
| `yellow` | `(1, 235/255, 4/255, 1)` | Unity 的黄是 RGB(255,235,4)，不是纯 `(1,1,0)` |
| `cyan` / `magenta` | | |
| `gray` / `grey` | `(0.5,0.5,0.5,1)` | 两种拼写同一个值 |

## 4. 方法

```ts
Color.Lerp(a: Color, b: Color, t: number): Color          // t 夹到 [0,1]
Color.LerpUnclamped(a: Color, b: Color, t: number): Color // t 不夹，可外推
Color.HSVToRGB(h: number, s: number, v: number): Color    // alpha 恒为 1
Color.RGBToHSV(c: Color): Vector3                         // 返回 (h, s, v)
```

* `Lerp` **夹** `t`。不夹的话 `t = 2` 得到 2 —— 那不是插值是外推，接下去写进材质
  就溢出。想做"过冲再回弹"的闪光用 `LerpUnclamped`。alpha 同样参与插值。
* `RGBToHSV` 返回 `Vector3(h,s,v)`：Unity 的签名是 `out H, out S, out V`，TS 没有
  out 参数，只能打包返回。
* HSV 的两个"看着像 bug"的地方是 Unity 的原样行为，抄错会让从 Unity 搬过来的取色
  代码对不上：
  * **灰色的 H 是 `1/3`，不是 0**（走 else 分支，offset = 2，`diff == 0` 时
    `H = offset`，再 `/6`）；
  * 只有**纯黑**（`V == 0`）的 H 才是 0，那是单独的 `V != 0` 判据。
* 色相环绕：`HSVToRGB(1.25, …)` 与 `HSVToRGB(0.25, …)` 同色，负数同理。

## 5. 值类型的往返细节

* 脚本字面量常只写 `{r,g,b}` —— 缺 `a` 时按 **1** 处理（不是 0，那会变成全透明）。
  判据必须是 `is_undefined`：缺失的属性返回的是 JS 的 `undefined`，直接
  `get_value_double` 会得到 NaN，而 NaN 会一路活到"颜色合成成 nothing"。
* 静态常量是**真 getter**：每次取到新对象，`Color.red.r = 0.25` 不会污染下一次读取。

## 6. 测试

| 测试 | 钉的是什么 |
| --- | --- |
| `UnityParitySmokeTest` AZ 组 | C++ 实现：常量值、Lerp 是否夹、HSV 往返闭合、Unity 的灰/黑约定 |
| `UnityParitySmokeTest` BA 组 | 接线门：`Converter<Color>`、`Variables` 表、`.d.ts` 的 `static` 前缀、LNK1179 回归门 |
| `ScriptVMSmokeTest` I 组 | 端到端：真起 QuickJS，`Color.red` 在 JS 里真的存在且值对 |
| `ReflectionCodegenSmokeTest` | 每个属性的 `.d.ts` 行（含 `static` 前缀）与绑定表一致 |

变异脚本 `.mutate_cl.py`（21 个变异）覆盖上述全部路径。
