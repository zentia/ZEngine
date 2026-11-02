# `Material` / `Light` 脚本门面（slice 52）

TS 侧：`renderer.material.color = red`、`light.intensity = 3`、`light.spotAngle = 60`。
对位是 Unity 的 `UnityEngine.Material` 与 `UnityEngine.Light`。

代码位置：

| 文件 | 内容 |
| --- | --- |
| `Runtime/Scripting/ScriptMaterial/ScriptMaterial.h` | `MaterialInstance`（= 脚本里的 `Material`） |
| `Runtime/Function/Framework/Component/Mesh/BaseRenderer.h/.cpp` | `renderer.material` / `GetMaterial(i)` / `HasMaterial*` 查询 |
| `Runtime/Function/Framework/Component/Light/LightComponent.h` | `Light` 的 Unity 面 |
| `Runtime/Scripting/Binding/ScriptBinding.cpp` | d.ts 里那行 `declare type Material = MaterialInstance;` |

---

## 1. 为什么 C++ 侧叫 `MaterialInstance` 而脚本侧叫 `Material`

引擎里已经有一个全局 `class Material`（`Resource/ResType/Data/Material.h`）—— 那是
**材质资产**：shader 引用 + 导入期保存下来的属性。ZHeaderTool 要求反射类名**全局
唯一**（生成的 `.generated.h` 落在一个扁平目录里，同名的第二份会覆盖第一份），而
资产那个名字是序列化类型、写进了磁盘上的 `.zasset`，改不动。

于是：

* C++ 类叫 **`MaterialInstance`**（"实例化出来的那份材质"，与 Unity 的语义对得上）；
* d.ts 里补一行 **`declare type Material = MaterialInstance;`**，把 Unity 的拼写还给
  脚本 —— 类型标注、`renderer.material` 的返回类型都能写 `Material`。

⚠️ 只是**类型**别名，不是新类：`new Material()` 不存在（Unity 那边是
`new Material(shader)`，本引擎没有"凭 shader 造一份材质"的入口）。材质只能从
`renderer.material` 拿。

### 1.1 句柄里没有数据

`MaterialInstance` 只有两个字段：`PPtr<BaseRenderer> m_Renderer` 与 `int m_Slot`，
全部方法转发到 `BaseRenderer` 的对应槽位。它派生 `Object` 只是为了能被 ZCLASS 发布
并跨脚本边界传递。

这么做的原因是引擎里真正与 Unity `renderer.material` 对位的，是 `BaseRenderer` 上那份
**per-sub-mesh 的运行时材质实例**（`MaterialInstanceOverride`）：改基础色、`SetFloat`、
换贴图、开关 keyword 都真的生效、真的只影响这一个 renderer。缺的只是"脚本世界里有一个
对象代表它"。

**状态在 renderer 上** ⇒ 不用缓存也能对：`renderer.material` 每次 new 一个句柄，读到的
值仍然是对的。`GetMaterial(slot)` 还是按槽位缓存了一份，免得每帧
`renderer.material.color = x` 都在造对象。

### 1.2 悬空安全

用 `PPtr<BaseRenderer>`：renderer 被销毁之后它解析成 `nullptr`，所有 setter 变成空操作、
所有 getter 返回默认值。不崩、不抛。脚本可以用 `IsValid()` 自查（这不是 Unity 的成员，
留着是因为"SetFloat 一直返回 false"最难查的一种原因就是 renderer 已经被 Destroy 了）。

---

## 2. 发布面

### `Material`（19 个方法 + 1 个属性）

```
color                                    （= renderer 的 base color，w 是 alpha）
IsValid
SetFloat(name, v) : bool      GetFloat(name) : number
SetColor(name, c) : bool      GetColor(name) : Color
SetVector(name, v) : bool     GetVector(name) : Vector4
SetTexture(name, path) : bool GetTexture(name) : string
HasProperty(name)  HasFloat  HasVector  HasColor  HasTexture
EnableKeyword(k)   DisableKeyword(k)   IsKeywordEnabled(k)
```

⚠️ setter 返回 **bool = 名字认不认识**（Unity 返回 void，这里故意不一样）：打错属性名
静默失败是最难查的一类 bug。

⚠️ `GetColor` 在名字不认识时返回**不透明白 (1,1,1,1)**，与 `GetFloat` 的 `0` 不同是有意
的 —— Unity 里"没有这个颜色属性"通常被当白色，读成 `(0,0,0,0)` 会让物体当场消失。

### `Renderer` 新增（6 个成员）

```
material                （槽位 0 那份）
GetMaterial(i)          （其余槽位；越界返回 null）
GetMaterialCount()
HasMaterialProperty / HasMaterialFloat / HasMaterialVector / HasMaterialTexture
```

⚠️ `Has*` 的判据与 setter **共用同一张表**（`ShaderLab::FindSlot` /
`MaterialCustomParams::Find*`），不是另写一份"认识的名字"清单 —— 两套判据会在某次加
属性名之后悄悄分叉，于是 `HasFloat` 说有而 `SetFloat` 说没有。

⚠️ `HasMaterialVector` 同时回答 `HasColor`：颜色在这个引擎里就是一个 vec4 属性，没有单独
的"颜色槽"概念。

⚠️ `renderer.materials`（数组）**没有**发布：ZFUNCTION 返回不了
`std::vector<MaterialInstance*>`（没有那个 Converter），只能按下标取。

⚠️ `renderer.sharedMaterial` 没有发布：今天没有对应的脚本对象（材质资产进不了脚本，见
`Resource/ResType/Data/Material.h` 的注释）。发一个返回路径字符串的假 Material 比不发更糟。

### 刻意没做的 Unity 成员

| Unity | 为什么不做 |
| --- | --- |
| `mainTexture` / `mainTextureOffset` / `mainTextureScale` | 贴图槽存的是**资产路径**，没有 Texture 对象身份；UV 变换引擎里不存在（`GameObjectMaterialDesc` 没有 tiling/offset）。发一个恒为 (0,0)/(1,1) 的假属性比不发更糟 |
| `shader` / `renderQueue` / `enableInstancing` / `globalIlluminationFlags` / `passCount` / `SetPass` / `CopyPropertiesFromMaterial` | 都没有后端 |
| `SetInt` / `GetInt` / `SetMatrix` / `GetMatrix` | 属性块里没有整型和矩阵的落点（`ShaderPropertySlot::FloatCount` 直接拒掉矩阵）。用 SetFloat 冒充 SetInt 会在回读时给出 "1.0" 这种数字 |
| `shaderKeywords`（string[]） | 今天只知道**这个槽位显式覆盖过**的 keyword，资产自己开的那份没有 per-renderer 回读路径。列不全就不发 |

---

## 3. `Light`：`color` × `intensity` 与 `flux` 的换算

引擎的物理量是 `m_Flux`（**每通道辐射通量**），Unity 那个面是 `color`（归一化 RGB）×
`intensity`（标量）。两者是同一份数据的两种写法：

```
flux = color.rgb * intensity
```

拆法必须**可逆**，否则脚本 `light.color = red` 读回来变成粉色 —— 那种 bug 看上去像
"颜色空间不对"，查起来全是冤枉路。取

```
intensity = max(flux.r, flux.g, flux.b)
color     = flux / intensity
```

这是精确的（一次除法一次乘法，误差在 1e-7），并且满足两条 Unity 行为：

* 默认 `color = white`：flux 三个通道相等时拆出来正好是 (1,1,1)；
* 改 `intensity` 不改色调。

**边界**：flux 全 0 时 intensity 是 0，除法无意义 —— 这时 color 报**白**。灯"没有颜色"
在 Unity 里是白 + intensity 0，报黑会让脚本以为这是一盏黑灯而不是一盏关掉的灯。之后
`SetIntensity` 从白起步，于是"把 intensity 从 0 拧到 100"得到一盏白灯。

**`color.a` 不参与**：辐射通量只有三个通道。恒报 1，写进来的值被丢掉 —— 明确丢掉比
"存着但不起作用"好查。

### 角度：全锥角 / 度 与 半锥角 / 弧度

| Unity | 引擎存量 | 换算 |
| --- | --- | --- |
| `spotAngle`（**全**锥角，**度**） | `m_OuterConeAngle`（**半**锥角，**弧度**） | `outer = (deg * 0.5) / Rad2Deg` |
| `innerSpotAngle`（全锥角，度） | `m_InnerConeAngle`（半锥角，弧度） | 同上 |
| `range` | `m_Radius` | 恒等 |

⚠️ 差一个 2 和一个 57.3，写错时画面"看起来像聚光灯但范围不对"，是最难发现的一类单位
bug，所以换算写死在一处（`LightComponent.h`）。

### 没做的 Light 成员

`shadows` / `shadowStrength` / `shadowBias` / `cookie` / `cullingMask` /
`colorTemperature` / `bounceIntensity` / `flare` / `lightmapBakeType` —— 都没有后端
（阴影是全局按 shadow map 尺寸开的，没有 per-light 开关；没有 per-light 的层过滤）。

`type` 保持引擎的编码（0 = Point，1 = Spot），**没有**改成 Unity `LightType` 的
（Spot=0, Directional=1, Point=2, Area=3）—— 它已经被序列化进场景资产，换编码会静默
改变所有已存灯的类型。

---

## 4. 已知与 Unity 的差别

| Unity | ZEngine | 原因 |
| --- | --- | --- |
| `renderer.materials` 是数组 | `GetMaterial(i)` + `GetMaterialCount()` | 没有 vector-of-pointer 的 Converter |
| `new Material(shader)` | 无（材质只能从 renderer 拿） | 没有"凭 shader 造材质实例"的入口 |
| `material.mainTexture` 是 `Texture` | 无（`GetTexture(name)` 返回资产路径） | 贴图槽存的是路径 |
| `Light.type` 用 `LightType` 枚举 | `int`（0=Point，1=Spot） | 序列化兼容 |
| `Light.color` 参与 alpha | alpha 恒 1 | 辐射通量只有三个通道 |
