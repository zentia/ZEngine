# `Application` / `Screen` 脚本门面

TS 侧：`Application.Quit()`、`Application.dataPath`、`Screen.width`。对位是
Unity 的 `UnityEngine.Application` / `UnityEngine.Screen`，**拼写与 Unity 一致**。

代码位置：

| 文件 | 内容 |
| --- | --- |
| `Runtime/Application/Application.h/.cpp` | `Application` 门面 + `RuntimePlatform` 枚举 |
| `Runtime/Scripting/ScriptScreen/ScriptScreen.h` | `Screen` 门面（header-only） |
| `Runtime/Function/Render/WindowSystem.h/.cpp` | `IsWindowFocused()`（runInBackground 的数据源） |
| `Runtime/Resource/UserPreferences/UserPreferences.h/.cpp` | `GetUserPreferencesDirectory()`（persistentDataPath 的数据源） |

---

## 1. 为什么 `Application` 是"给已有类挂 ZCLASS"，而 `Screen` 是新类

脚本侧要的名字是 `Application`。C++ 里**已经有一个** `class Application : public
Object, public IEngineSystem`（引擎主循环）。

再新建一个同名的绑定类会让两个 `Application` 定义进同一个二进制 —— ODR 违规：
链接器挑一个，另一处静默用错布局，能编译、能运行、然后在某个成员访问上读错
内存。所以 ZCLASS 直接挂在已有的那个类上。

`ZHeaderTool` 取基类列表的**第一项**（`parser.py` 在顶层逗号处断开），所以
`public Object, public IEngineSystem` 里的第二个基类不影响反射：脚本看到的父类
是 `Object`。

`Screen` 没有这个约束，于是按 `ScriptTime.h` / `ScriptInput.h` 的先例做成
header-only 的纯转发类。

---

## 2. 静态属性：`Application.dataPath` / `Screen.width`（同 `Time.deltaTime`）

以前 ZSCRIPTPROPERTY 的 getter 不允许 static（`zht/parser.py` 直接报
"getter must not be static"），Unity 的静态属性只能落成 `Get<UnityName>()` /
`Set<UnityName>(v)` 方法对。`Color.red` 那一档把静态属性做出来了（静态属性走
pesapi 的 `Variables` 表、装到**构造器**上），于是这里换回 Unity 的拼写。

Get* 方法在 C++ 里照旧保留（ZSCRIPTPROPERTY 的 getter 本来就得是个
ZFUNCTION），但**不在 .d.ts 里** —— 静态访问器会被 `codegen.py` 从方法表摘掉，
否则 MSVC 把同一个 `MethodCallback` 编进两个 COMDAT 段（LNK1179）。

⚠️ **一个静态访问器只能挂一个属性**。`realtimeSinceStartup` 与 `unscaledTime`
是同一个量，但必须各有一个 getter（`GetRealtimeSinceStartup()` 转发一层）：
两个 `ZSCRIPTPROPERTY` 写同一个 `Getter=` 会让 `PropertyCallback<同一个指针>`
进两个 COMDAT 段，链接器拒收整个 obj。

⚠️ **`targetFrameRate` / `runInBackground` 的写是系统状态**：这两个量存在
`Application` 的实例成员上（`m_TargetFrameRate` / `m_RunInBackground`），没有
Application 系统时读的是降级值 `-1` / `true`，写是空操作（不崩、不抛）。

### 名字冲突：静态与非静态不能同签名重载

C++ 禁止"静态成员函数与同参数列表的非静态成员函数"重载。Application 上已经
有实例方法 `GetFps() const` / `GetName() const`，所以门面里**没有**
`GetFps()` / `GetName()` —— 要帧率用 `Time.GetFps()`。

---

## 3. API

### `Application`

| 方法 | Unity 对位 | 数据源 |
| --- | --- | --- |
| `Quit()` | `Application.Quit` | 置 `m_Isquit` + `WindowSystem::RequestClose()` |
| `GetIsPlaying()` | `isPlaying` | `g_isPlaying` |
| `GetIsEditor()` | `isEditor` | `g_isEditor` |
| `GetDataPath()` | `dataPath` | `ConfigManager::GetAssetFolder()` |
| `GetProjectPath()` | —（ZEngine 扩展） | `ProjectInfo::GetProjectRoot()` |
| `GetStreamingAssetsPath()` | `streamingAssetsPath` | `<dataPath>/StreamingAssets` |
| `GetPersistentDataPath()` | `persistentDataPath` | `UserPreferences::GetUserPreferencesDirectory(product)` |
| `GetProductName()` / `GetCompanyName()` / `GetVersion()` | 同名 | `PlayerSettings` |
| `GetEngineVersion()` | `unityVersion` | `ProjectInfo::engine_version` |
| `GetPlatform()` | `platform` | `Z_PLATFORM_IS_*` + `g_isEditor` |
| `GetPlatformName()` | —（ZEngine 扩展） | 上者的打印名 |
| `GetTargetFrameRate()` / `SetTargetFrameRate(int)` | `targetFrameRate` | `Application::m_TargetFrameRate` |
| `GetRunInBackground()` / `SetRunInBackground(bool)` | `runInBackground` | `Application::m_RunInBackground` |

`RuntimePlatform` 是 `ZENUM`，取值与 Unity 对齐（1/2 Windows，3/4 OSX，5/6
Linux，7 Android，8 iPhonePlayer，9 WebGLPlayer），ZEngine 独有的鸿蒙排在
10 —— 不占 Unity 的号段。桌面平台上 **Editor 是奇数、Player 是偶数**，测试用
这条不变量，避免硬编码 `WindowsEditor`（CI 上还有 Linux / macOS）。

### `Screen`

| 方法 | Unity 对位 | 数据源 |
| --- | --- | --- |
| `GetWidth()` / `GetHeight()` | `Screen.width/height` | `WindowSystem::GetWindowSize()` |
| `GetFramebufferWidth()` / `GetFramebufferHeight()` | —（ZEngine 扩展） | `WindowSystem::GetFramebufferSize()` |
| `GetDpi()` | `Screen.dpi`（语义见下） | `GenericWindow::GetDpiScale()` |
| `GetMonitorCount()` | `Display.displays.Length` | `GenericApplication::GetMonitorCount()` |
| `GetPrimaryMonitorWidth/Height/X/Y()` | —（ZEngine 扩展） | `GenericApplication::GetPrimaryMonitorWorkArea()` |
| `SetResolution(w, h)` | `Screen.SetResolution` | `GenericWindow::SetSize()` |

---

## 4. 语义陷阱

### 屏幕坐标 ≠ 像素

`GetWidth()` 是**窗口客户区**尺寸（屏幕坐标），`GetFramebufferWidth()` 是
**实际渲染像素**。200% 缩放的 Windows 上前者 1280、后者 2560。
布局用前者，render target 尺寸用后者。

无头测试里两者恰好同值，所以 `UnityParitySmokeTest` 的 N19/N20 是**源码级**
门禁（精确计数 `GetWindowSize()` / `GetFramebufferSize()` 各 3 处），不是行为
断言 —— 行为断言抓不到把 `GetWidth()` 改成读帧缓冲的那种改动。

### `GetDpi()` 返回的是缩放比，不是绝对 DPI

Unity 的 `Screen.dpi` 是绝对 DPI（如 96 / 220），这里返回的是**缩放系数**
（1.0 / 1.5 / 2.0）。后者才是 UI 缩放真正要用的量，也是各后端唯一能可靠给出
的：Windows 有 `GetDpiForWindow`，macOS 是 `backingScaleFactor`，Linux/X11 只
能自己算。没有主窗口时由 `framebuffer / window` 反推。

### `Quit()` 不是立即退出

只置位 + 请求关窗，当前帧照常跑完（Unity 同样在帧末退出）。主循环看的是
`WindowSystem::ShouldClose()`，所以**必须**同时调 `RequestClose()` —— 只置
`m_Isquit` 的话是个"看起来生效了"的死门（门禁 N5）。

### `SetResolution` 在没有主窗口时是空操作

移动端 / Web / 外部宿主那一支由 `SetHostWindowSize()` 推入尺寸，引擎改不动
—— 硬改会在下一次 `surfaceChanged` 被覆盖回去。`0` / 负值被夹到 1（0 宽的
HWND 会让之后所有基于尺寸的除法变成 0 除）。

### 没有 `SetFullScreen` / `GetFullScreen` / `Screen.resolutions`

**故意不导出。** 引擎的窗口后端（`WindowsWindow` / `MacOSWindow` / Linux）
目前没有全屏切换 —— `GenericWindow` 里连个虚函数都没有，枚举显示器模式也需
要 GLFW `glfwGetVideoModes` 一类的后端支持。

与其返回一个永远 `false` 的假值，不如让脚本在编译期就撞上"没有这个 API"。
要补时从给 `GenericWindow` 加 `SetFullScreen` 虚函数开始，三个后端各自实现。

---

## 5. `targetFrameRate` 与 `runInBackground` 的落地

两块状态都存在 `Application` 实例上（不是文件静态量：Application 是系统，
只有一个实例，而实例成员让"退出 Play 时重置"有明确归属）。

### 限帧

`Application::Run()` 每帧末调 `LimitFrameRate(frame_start)`：按 `1/rate` 算
预算，减掉本帧已花掉的时间，剩余为正才 `sleep_for`。**整帧成本**（含渲染），
不只是逻辑。`rate <= 0` 直接返回（不限帧）。

只在 `Run()` 生效 —— 编辑器有自己的驱动方式，限帧是"出货行为"不是"编辑行为"。

### 后台运行

`runInBackground == false` 时失焦就跳过这一帧，但**必须继续
`PollEvents()`**：不泵消息的后果不是"卡住"而是 OS 判"程序未响应"。

时间也必须照常推进：`CalculateDeltaTime()` 在 `continue` 之前调用一次，把
"失焦期间"那一大段时间吃掉。否则恢复焦点那一帧的 delta 是"失焦了多久"，
逻辑侧表现为角色瞬移。

焦点状态存在 `WindowSystem::m_IsWindowFocused`，初值 `true` —— 无引擎窗口的
那一支（移动端 / Web / 外部宿主）根本没有失焦这回事，恒 `false` 会让世界一
上来就不推进。

---

## 6. 顺带修掉的一个真 bug

`UserPreferences::GetUserPreferencesDirectory()` 的 **Windows 分支完全忽略
`app_name`**，直接以 `.../LocalLow/ZEngine` 结尾。后果：

* 每个工程共用同一个 `ZEngine` 目录，互相覆盖 `preferences.json`；
* `Application.GetPersistentDataPath()` 给出的"存档目录"也是共享的。

macOS / Linux 分支是对的（用了 `folder_name`）。这正是 `PlayerSettings.h` 开头
记的那个坑 —— 那边已经靠 `m_ProductName` 修好了产品名，这边却没用它。

现在三端统一为 `<company>/<product>`（Windows 是
`%USERPROFILE%/AppData/LocalLow/ZEngine/<Product>`），与 Unity 的
`persistentDataPath` 形状一致。

---

## 7. 测试

`UnityParitySmokeTest`（一个共用 exe，见文件头的说明）：

* **L 组**（L0–L26）：用临时 ini + 手工填充的 `ProjectInfo` 搭一个"最小工
  程"，断言每个 getter 的**确定值**（不是"取决于谁在跑测试"）。含 Editor /
  Player 的奇偶不变量。
* **M 组**（M0–M14）：`Screen`。M0 断言"根本没注册 WindowSystem"返回 0，
  M1 起断言"注册了但无窗口"返回 1280×720 兜底 —— 两个不同的失败模式必须分开。
* **N 组**（N0–N21）：源码级门禁。`Quit()` 调 `RequestClose`、限帧被声明
  **且**被调用、`m_IsWindowFocused` 被**存储**（回调只在焦点变化那一帧触发
  一次，事后问不到）、`g_isEditor` 在 `Editor::Initialize` 读任何东西**之前**
  被置位（顺序断言，不是计数）。

### 变异验证（两组互补）

| 变异 | 红灯 |
| --- | --- |
| `GetDataPath()` 改读 `GetRootFolder()` | L4 |
| `Quit()` 去掉 `RequestClose()` | N5 |
| `GetWidth()` 改读 `GetFramebufferSize()` | N19 / N20 |
| `GetPlatform()` 忽略 `g_isEditor` | L17 |
| `SetTargetFrameRate()` 改成空实现 | L21 / L22 |
| `OnWindowFocus` 不存 `m_IsWindowFocused` | N11 |
