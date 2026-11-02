# `Color` 补齐面 + `ColorUtility`

对应 Unity：`Color.grayscale` / `Color.maxColorComponent` / `Color.linear` /
`Color.gamma` / `Color.ToString()`、`UnityEngine.ColorUtility`、
`Mathf.CorrelatedColorTemperatureToRGB`。

| 文件 | 内容 |
|---|---|
| `Runtime/Scripting/ScriptColor/ScriptColor.{h,cpp}` | `Color` 补齐 + `ColorUtility`（新类） |
| `Runtime/Core/Color/Color.h` | `ColorParseResult`（新值类型） |
| `Runtime/Scripting/Binding/ScriptBinding.{h,cpp}` | `Converter<ColorParseResult>` + d.ts 声明 |

```ts
Color.GetGrayscale(c);                          // 0.299r + 0.587g + 0.114b
Color.GetMaxColorComponent(c);                  // max(r, g, b)，不含 alpha
Color.ToLinear(c);                              // gamma -> 线性，alpha 不动
Color.ToGamma(c);
Color.ToString(c);                              // "RGBA(1.000, 0.000, 0.000, 1.000)"

ColorUtility.ToHtmlStringRGB(c);                // "FF0000"（无 '#'，无 alpha）
ColorUtility.ToHtmlStringRGBA(c);               // "FF000040"
const r: ColorParseResult = ColorUtility.TryParseHtmlString("#F0A");
if (r.valid) { paint(r.color); }

Color.CorrelatedColorTemperatureToRGB(6500);    // 近似，见第 5 节
```

## 1. Unity 是实例属性，ZEngine 是静态工具

Unity 里 `c.grayscale` / `c.maxColorComponent` / `c.linear` / `c.gamma` 是
**实例属性**。`::Color` 在 ZEngine 里是跨边界的**值类型**（d.ts 中的
`declare interface Color` 只有 r/g/b/a 四个字段，加不了属性），所以这一批只能
像 `Bounds.Contains(bounds, p)` 那样把值当第一个参数传进来：

```
Unity:    c.grayscale
ZEngine:  Color.GetGrayscale(c)
```

与 `Bounds` / `Plane` / `AnimationCurve` 同一套路，不是新发明。

## 2. ⚠️ `ToHtmlString*` 用**四舍五入**，`Color32` 用**截断**

同一件事的两条路，取整规则**不一样**，而且都是 Unity 的原样行为：

| 路径 | 规则 | `0.5` 的结果 |
|---|---|---|
| `ColorUtility.ToHtmlStringRGB` | `RoundToInt(Clamp01(v) * 255)` | `128` = `0x80` |
| `::Color32(Color)` | `(byte)(Clamp01(v) * 255)` | `127` = `0x7F` |

差一位，截图上完全看不出来，但**配色表和 Unity 对不上**。所以
`ScriptColor.cpp` 里有独立的 `ChannelToByte()`，没有复用 `::Color32(c)` ——
源码门 CXW9 就是钉这一条的。

## 3. ⚠️ 缩写串每位 `*17`，不是 `*16`

`#RGB` / `#RGBA` 的每一位要复制到两位：

```
"#FFF"  ->  FF FF FF  （纯白）
"#F0A"  ->  FF 00 AA
```

复制是 `n * 17`（`0xF * 17 == 0xFF`），不是 `n * 16`（那会得到 `0xF0`）。
`*16` 的版本每个通道差 15/255 —— 肉眼几乎看不出，但和 Unity 对不上，判据是
CX23（`#FFF` 必须**正好是**纯白）。

`'#'` 可有可无，大小写不敏感。6 位时 alpha 补 **1**（不是 0 —— 补 0 的话解析
出来的颜色全是透明的）。

## 4. ⚠️ 解析失败返回**全 0**（含 alpha），不是保留原值

Unity 的签名是 `bool TryParseHtmlString(string, out Color)`；TS 没有 out 参数，
`Color` 又是值类型（跨边界是快照），所以"成功与否"和"颜色"只能一起回来 ——
与 `RaycastHit.valid` 同一套路（`ColorParseResult { valid, color }`）。

失败时 `color` 是全 0（透明黑）。这是**故意**的：调用方拿到一个"看着像旧值"
的结果会当成解析成功继续用，而全 0 至少在屏幕上什么都没有 —— 配置表里一个
拼错的色值应该表现为"这个 UI 不见了"，而不是"悄悄用了上一条"。

## 5. ⚠️ `CorrelatedColorTemperatureToRGB` 是**近似值**

Unity 的文档只说"把色温转成 RGB"，**没有公布它用的曲线**。这里用的是常见的
Planckian-locus 近似（Tanner Helland 的分段幂/对数拟合，适用
1000 K..40000 K）：

| 色温 | 结果 | 用途 |
|---|---|---|
| 1500 K | `(1.000, 0.425, 0.000)` | 烛光橙 |
| 6500 K | `(1.000, 0.997, 0.980)` | 近白，略暖 |
| 10000 K | `(0.791, 0.855, 1.000)` | 冷蓝 |

**不要指望和 Unity 逐位一致** —— 要一致只能照抄 Unity 的实现，而那不在公开
文档里。所以测试判的是"定性 + 单调性"（暖 = 红拉满蓝为 0、冷 = 蓝拉满红被压、
色温升高则蓝升红降、极端值不出 `[0,1]`），不是黄金数。

两个分支（`log(t)` 在 t 很小时为负、`pow` 在 t 很大时溢出 255）都必须夹住，
否则 500 K 会给出负的绿通道（CX35）。

## 6. `Color.linear` / `Color.gamma` 的两个坑

* **alpha 不过曲线**：只有 RGB 参与。把 alpha 也转了的话，"同一个颜色不同
  透明度"会变出两种不同的观感。
* **0 和 1 是不动点**：任何 sRGB 曲线都满足 `f(0)=0`、`f(1)=1`，所以"纯黑纯白
  对得上"证明不了任何事。真正的判据是中灰：`ToLinear(0.5) == 0.2140`。
  用 `pow(v, 2.2)` 顶替是 `0.2176` —— 差 0.0036，肉眼看不见，但线性空间里
  做叠加时误差会积累。

实现复用 `Mathf.GammaToLinearSpace` / `LinearToGammaSpace`，**不抄第二份曲线**
（源码门 CXW8）。

## 7. 判据与变异

| 段 | 位置 | 覆盖 |
|---|---|---|
| CX0–CX35 | `UnityParitySmokeTest.cpp` | 亮度 / 最大分量 / linear-gamma / ToString / HTML 串 / 解析 / 色温 |
| CXW0–CXW13 | 同上 | 源码门（ZFUNCTION 带括号、复用 Mathf、*17、d.ts 声明） |
| Z8–Z9 | `ScriptComponentSmokeTest.cpp` + `Mover.ts` | TypeScript 端到端 |

`.mutate_co.py` 的变异体全是"看起来合理的邻居算法"：平均亮度代替 Rec.601
权重、`pow(v,2.2)` 代替分段 sRGB 曲线、截断代替四舍五入、`*16` 代替 `*17`、
失败时保留旧值。17/17 被抓到。

第一轮有两个漏了：

| 变异 | 为什么漏 | 修法 |
|---|---|---|
| COM14 去掉色温的**上夹** | 范围检查只挑了 500/1000/20000/40000 K，而上夹真正生效只在 t 刚过 66 那一段（约 6650 K 时红通道算出来是 256.9）；6500 K 走的是"红恒为 255"那一支、10000 K 已衰减到 201，两个都测不到 | CX35 改成**扫** 500..40000 K、步长 50 K，不再挑点 |
| COM17 把 `ColorParseResult` 改名成 `..._RENAMED` | **无效变异**：`"declare interface ColorParseResult"` 仍是它的前缀，子串门照样命中 | 改成整块摘掉 |

另外 `ScriptColor.cpp` 的函数都在 `namespace Runtime` 里，函数尾是 4 空格的
`}`（默认锚点正好），而命名空间收尾带注释 —— 顶格的 `"\n}\n"` 在文件里**不存在**，
第一轮 COM10–COM14 因此报 "END OF FUNC NOT FOUND"。
