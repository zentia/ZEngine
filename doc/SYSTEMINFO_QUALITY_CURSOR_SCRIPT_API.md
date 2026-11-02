# `SystemInfo` / `QualitySettings` / `Cursor` 脚本门面

TS 侧：`SystemInfo.processorCount`、`QualitySettings.SetQualityLevel(3)`、
`Cursor.lockState = CursorLockMode.Locked`。对位 Unity 的
`UnityEngine.SystemInfo` / `UnityEngine.QualitySettings` / `UnityEngine.Cursor`，
**拼写与枚举取值一致**。

代码位置：

| 文件 | 内容 |
| --- | --- |
| `Runtime/Scripting/ScriptSystemInfo/ScriptSystemInfo.{h,cpp}` | `SystemInfo` + `DeviceType` / `OperatingSystemFamily` / `BatteryStatus` |
| `Runtime/Scripting/ScriptQuality/ScriptQuality.{h,cpp}` | `QualitySettings` + `ShadowResolution` / `ShadowProjection` / `AnisotropicFiltering` / `SkinWeights` |
| `Runtime/Scripting/ScriptCursor/ScriptCursor.{h,cpp}` | `Cursor` + `CursorLockMode` |
| `Runtime/Function/Render/Platform/Generic/GenericWindow.h` | `GenericWindow::CursorLockMode` + 两个光标虚函数的默认实现 |
| `Runtime/Function/Render/Platform/Windows/WindowsWindow.cpp` | 光标在 Windows 上的真实实现（`ShowCursor` / `ClipCursor`） |

---

## 1. 三个门面属于**两类不同的数据源**，不要混着看

| 门面 | 数据源 | 拿不到时 |
| --- | --- | --- |
| `SystemInfo` | **机器 / 驱动**（OS 系统调用、RHI 设备属性） | 返回哨兵值：`"Unknown"` / `0` / `-1` / `false` |
| `QualitySettings` | **它自己**（用户设定，没有上游可问） | 不存在"拿不到"这回事 |
| `Cursor` | **它自己 + 窗口后端** | 状态永远读得回，但 OS 效果在非 Windows 上不保证 |

判据很简单：`SystemInfo` 是"问"，`QualitySettings` / `Cursor` 是"说"。

---

## 2. `SystemInfo`：为什么"拿不到"是合法答案

ZEngine 的 Runtime 还要能在 headless（CI、cooker、专用服务器）里跑 —— 那里
`GET_SYSTEM(RenderSystem)` 返回 `nullptr`，设备属性根本无从问起。

取舍是**一律返回哨兵值，不编造 plausible 的假值**：

* 字符串 → `"Unknown"`
* 尺寸 / 条数 → `0`（不是 1 —— 1 会被当成"单核机器""1MB 显存"）
* `batteryLevel` → `-1`（Unity 在不支持时也是 -1）
* `supportsXxx` → `false`

理由：脚本最常见的用法是 `if (SystemInfo.supportsInstancing)`、
`if (SystemInfo.graphicsMemorySize > 512)`。给假值会让这类判断在真机上静默走错
分支 —— 那比"这个 API 在这台机器上不可用"难查得多。

### 明确拿不到的两项（不是漏实现）

* **`graphicsMemorySize` 恒 0**。RHI 抽象层只有 `GetPhysicalDeviceProperties()`，
  **没有** `GetPhysicalDeviceMemoryProperties()`（Vulkan 底层本来有，只是没往上
  抬）。要补的时候先给 `RHI` 加这个虚函数，而不是在这里靠 `limits` 猜。
* **`deviceModel` 在桌面恒 `"Unknown"`**。Unity 这一项给的是主板 / 机型标识
  （`"MacBookPro16,1"`），要读它需要 SMBIOS（Windows）或 `hw.model` sysctl
  （macOS）。宁可留空也不拿 hostname 冒充 —— 后者会让"按机型做画质分档"的代码
  静默错判。

### `deviceUniqueIdentifier` 不是 Unity 语义上的"设备唯一 ID"

Unity 手上的是平台给的硬件标识（IDFV / ANDROID_ID）。这里给的是
`<平台名>-<机器名>-<逻辑处理器数>`：跨进程稳定、跨重启稳定，但**同型号同名的
两台机器会得到同一个值**。给崩溃上报做个分组键是它正当的用途；用在"存档绑机器"
这类防伪场景之前先看清楚这一点。

---

## 3. `QualitySettings`：接线状态（读这段再决定要不要用）

⚠️ **目前没有渲染代码消费这些开关**：`vSyncCount` 不会真的去改交换链，
`shadowDistance` 也不会真的去改阴影级联。它们是"能写能读、语义与 Unity 一致、
但还不生效"的状态。

这与 `Screen.SetFullScreen` 的处理不同（那里是干脆不导出）。区别在：

* 全屏切不了是**窗口后端的缺口**，导出它等于承诺一个做不到的事；
* 画质档位是**上层策略**，脚本侧本来就该有地方表达"我要中等画质"，渲染侧什么
  时候来读是实现进度问题。缺一个"用户意图"会让"画质设置面板"这种需求无处下手。

每接一项进来，就在 `ScriptQuality.cpp` 的 `PresetTable` 注释里记一笔。

### 档位与手动改值的关系

`SetQualityLevel(i)` 把预设表里第 i 档的值**整体覆盖**到当前开关上（与 Unity 一致）。
所以：

* "先 `SetQualityLevel(3)` 再单独改 `antiAliasing`" 是合法的 —— 后者只改那一项；
* 反过来 `SetQualityLevel` 会覆盖你手动改过的项 —— 这是 Unity 的行为，有意保留。

### 越界不夹紧

`SetQualityLevel(99)` **什么也不做**（Unity 也忽略越界调用）。夹紧会让它悄悄变成
"最高档" —— 那种"我传错了但看起来成功了"比没反应更难查。

### 离散合法集一律"向下取"

`antiAliasing` 只接受 0/2/4/8，`shadowCascades` 只接受 0/2/4。写了别的值会被夹到
**不高于它的最近合法值**（3 → 2，9 → 8）。向上取会让"我要 3x MSAA"变成"我要 4x"
—— 那是**升级**，会让画质悄悄超出用户选的预算。

---

## 4. `Cursor`：跨平台生效情况

| 平台 | `visible` | `lockState` |
| --- | --- | --- |
| Windows | 真改（`ShowCursor`） | 真改（`ClipCursor`） |
| 其余后端 | 只记状态 | 只记状态 |

**读回来永远是你设的值**（`GenericWindow` 的默认实现记状态），但"屏幕上光标是否
真的消失了"在非 Windows 上不保证。要接某个后端，override
`GenericWindow::SetCursorVisible` / `SetCursorLockMode` 即可，脚本侧不用动。

### `ShowCursor` 是引用计数，不是开关

Windows 的 `ShowCursor(TRUE/FALSE)` 加减一个计数器，计数 < 0 时光标隐藏。所以
"绝对设置"必须循环到越过 0：

```cpp
while (::ShowCursor(FALSE) >= 0) { }   // 隐藏
while (::ShowCursor(TRUE)  <  0) { }   // 显示
```

直接调一次 `ShowCursor(FALSE)` 在嵌套调用（编辑器面板 + 游戏各调一次）下会把计数
推到 -2，之后调一次 `ShowCursor(TRUE)` 仍然不显示 —— 经典的"光标不见了" bug。

### `Locked` 的实现

钉在客户区正中：`ClipCursor` 到中心一个 1x1 的矩形 + `SetCursorPos` 到中心。不需要
raw input 也能拿到 Unity 的行为（移动量仍然从光标位置差算得出）。

### 没有 `Cursor.SetCursor`

Unity 的 `SetCursor` 吃一张 `Texture2D` 当位图。ZEngine 的 `Texture2D` 是**资产**
（`.zasset` 里的像素 blob），身上没有 OS 光标句柄，也没有"从资产造 HCURSOR /
NSCursor"这一层。要补得先有"运行时把 Texture2D 上传成 GUI 资源"的能力。

---

## 5. TS 用法

```ts
// 按机器能力选起点
if (SystemInfo.supportsComputeShaders && SystemInfo.processorCount >= 8) {
    QualitySettings.SetQualityLevel(4);
} else {
    QualitySettings.SetQualityLevel(2);
}
// 再单独覆盖一项（会保留，直到下次切档）
QualitySettings.vSyncCount = 0;

// 进玩法：隐藏 + 锁定
Cursor.SetCursorState(false, CursorLockMode.Locked);
// 退出玩法：还回去
Cursor.ResetToDefaults();
```
