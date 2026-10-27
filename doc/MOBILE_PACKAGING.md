# ZEngine 移动端出包链

P0-1。目标是两条命令拿到能装到真机上的包：

```bash
python3 zbuild_android.py --project <项目目录> --config release [--aab] [--install]
python3 zbuild_ios.py     --project <项目目录> --config Release [--export] [--team TEAMID]
```

本文记录链路拓扑、静态自检覆盖范围、**仍然只能真机验**的部分，以及本轮修掉的
四个坑。

---

## 1. 拓扑

```
engine/Source/Player/
├── PlayerShell.h/.cpp          ← 与平台无关的启动壳（静态库 ZPlayer）
│                                  ZPLAYER_* 是宿主唯一可调用的面
├── Android/
│   ├── AndroidMain.cpp         ← android_main()：NativeActivity 入口
│   ├── CMakeLists.txt          ← TARGET_NAME = ZGameAndroid（产物 libZGameAndroid.so）
│   ├── build.gradle / settings.gradle / gradle.properties
│   ├── gradle/wrapper/gradle-wrapper.properties
│   └── src/main/{AndroidManifest.xml, assets/, res/values/strings.xml}
└── IOS/
    ├── IOSMain.mm              ← UIKit App 入口
    ├── CMakeLists.txt          ← target ZGameIOS（MACOSX_BUNDLE，产物 ZEngine.app）
    ├── Info.plist
    └── staging/                ← 项目内容在这里摆好再打进 bundle

engine/toolchain/ios.cmake      ← CMAKE_TOOLCHAIN_FILE
scripts/check_mobile_packaging.py  ← 静态自检（61 项，不需要 SDK / Xcode / 设备）
```

接入点全部在 `engine/CMakeLists.txt`：

| 行 | 分支 | 动作 |
| --- | --- | --- |
| 336 | 无条件 | `add_subdirectory(Source/Player)` —— ZPlayer 静态库全平台都建 |
| 349 | `elseif(ANDROID)` | `add_subdirectory(Source/Player/Android)` |
| 355 | `elseif(IOS)` | `add_subdirectory(Source/Player/IOS)` |

桌面 `ZGame.cpp` 链同一个 `ZPlayer`，所以"启动引擎的七个步骤"全仓库只有一份
实现，移动端不会出现只有移动端有的加载顺序 bug。

---

## 2. 静态自检

```bash
python3 scripts/check_mobile_packaging.py
```

不需要 Android SDK / NDK / Xcode / 真机，纯读文件。设计意图是**把"只能真机验"
里属于静态一致性的那部分全部拿掉**，让真机验只负责"能不能起来、画面对不对"。

| 分组 | 项数 | 抓的是哪类错 |
| --- | --- | --- |
| Android Manifest / res | 15 | `lib_name` 不等于 CMake `TARGET_NAME` → logcat 只有一句 *Unable to load native library*，永远指不到根因；Activity 不是 NativeActivity / 没 exported / 缺 MAIN+LAUNCHER → 装上了点不开；`@string/app_name` 引用了不存在的 string |
| Android Gradle | 11 | `externalNativeBuild.path` 指到不存在的文件；minSdk 低于 Vulkan 下限；AGP 与 Gradle wrapper 版本不匹配（AGP 版本两处都找，不会因为声明挪到 settings.gradle 就整项消失） |
| Android 引擎根接入 + assets | 4 | `add_subdirectory(Source/Player/Android)` 不在 `ANDROID` 分支里 → 配出来没有宿主 `.so`，且不报错 |
| Android 宿主符号 | 3 | 用了 `PlayerShell.h` 里没有 `ZPLAYER_API` 的符号 → 移动端开 `-fvisibility=hidden`，链接期才炸 |
| iOS bundle | 6 | `Info.plist` 里 `${MACOSX_BUNDLE_XXX}` 没有对应 CMake 属性 → 原样输出字面量，App 起不来且 Xcode 报错指不到这里；`UIRequiredDeviceCapabilities` 里没有 metal |
| iOS toolchain | 4 | `IOS` 没显式设 → target 被静默跳过 |
| iOS 引擎根接入 | 2 | `add_subdirectory(Source/Player/IOS)` 不在 `IOS` 分支里 |
| iOS deployment target | 5 | plist / `XCODE_ATTRIBUTE` / toolchain 三处不一致；任一处缺失单独报，不会整组静默消失 |
| iOS scheme + 宿主符号 | 5 | `zbuild_ios.py` 的 `-scheme` 与 CMake target 名不符；宿主符号同 Android |
| 出包脚本 | 6 | 语法不完整（真的 `ast.parse` 一遍）；`--help` 跑不通 = argparse 表坏了 |

当前 61 / 61 通过。

### 自检脚本自己也别变成"假通过"

写这个脚本的过程里，先在自己身上踩到 8 处判定不严谨 —— 全是**假通过**（该报错
的时候报 `[ ok ]`），比报错更危险。都已修，并且每一处都用负向样本验证过"真的会
炸"：

| 症状 | 根因 | 修法 |
| --- | --- | --- |
| "可以用当前 python 解析"这一项永远通过 | 跑的是 `[sys.executable, "--help"]`，测的是 python 解释器自己，跟目标脚本一点关系没有 | 改成 `ast.parse` 目标文件（用 ast 而非 py_compile，免得落一地 `__pycache__`）。负向验证：往 `zbuild_android.py` 注入语法错误 → 报 `第 230 行: invalid syntax` |
| 15 个枚举成员被当成"声明了但没实现的函数" | 用"名字第 9 个字符是否大写"区分函数与枚举，`ZPlayer_OK` 的 `O` 也是大写 | 函数只从 `ZPlayer_xxx(` 取，枚举成员从 `\bZPlayer_\w+\b` 取，两组分开用 |
| 跨行调用被当成函数实现 | 只看"不以 `;` 结尾"，而 `ZPlayer_Touch(` 那行也不以 `;` 结尾 | 追加"行首无缩进"条件 —— `ZPlayer_*` 是 `extern "C"` 顶层符号，定义必然顶格，函数体里的调用都在缩进里 |
| `add_subdirectory` 在不在分支里都通过 | 两个 `in` 判定各自独立，落在 ANDROID 分支里也算过 | 真去切分支正文再查；且收集**所有**同名分支（`engine/CMakeLists.txt` 里 150 行那个 `if(ANDROID)` 装的是 `add_link_options`，344 行那个才是 `add_subdirectory`，只取第一个会假失败） |
| metal 写在注释里也算通过 | 只查 `"metal" in plist_text` | 解析 plist，查 `UIRequiredDeviceCapabilities` 数组 |
| `XCODE_ATTRIBUTE` 缺失时整组检查静默消失 | `if m_xcode and check(...)` 短路 | 三项各自独立报 |
| 占位符用子串匹配，`..._PREFIX` 能顶替 `...` | `p not in text` | 整词 `\b<p>\b` |
| AGP 版本挪到 `settings.gradle` 后整项消失 | 只在 `build.gradle` 里找 | 两处都找，报告里说明读自哪个文件 |
| `minSdkVersion 26` 旧写法不认 | 正则只认 `minSdk 26` | `minSdk(?:Version)?\s*[=:]?\s*(\d+)` |

---

## 3. 本轮修掉的四个坑

按"在真机上暴露时的严重程度"排序。

### 3.1 旋转 / 分屏后画面被拉伸 —— 严重

**症状**：竖屏转横屏，画面拉伸且不恢复；杀进程重启才正常。

**根因**：Manifest 声明了 `configChanges=orientation|screenSize|...`，于是
Activity **不重建**，系统也不会发 `APP_CMD_TERM_WINDOW` + `APP_CMD_INIT_WINDOW`
那对回调。窗口尺寸变了却没有任何人通知引擎，swapchain 不重建，旧尺寸的
swapchain 被拉伸到新尺寸上。

**修法**（`AndroidMain.cpp:401`）：自己处理 `APP_CMD_CONFIG_CHANGED`。

```cpp
case APP_CMD_CONFIG_CHANGED:
    if (app->window != nullptr && g_window_ready)
    {
        ZPlayer_Resize(ANativeWindow_getWidth(app->window),
                       ANativeWindow_getHeight(app->window));
    }
    break;
```

`ZPlayer_Resize` 幂等，如果系统随后还是发了 TERM/INIT 那一对，多调一次没有
额外开销也不会错乱。

### 3.2 出包脚本猜产物名 —— 高（每次都踩）

**症状**：Gradle 明明 `BUILD SUCCESSFUL`，脚本报"没找到产物"。

**根因**：`locate_output()` 硬编码 `app-<config>.apk`。但这个 Gradle 工程是
**单模块根工程**（`rootProject.name = "ZEngineAndroid"`，没有 `:app` 子目录），
AGP 产物名跟 module name 走，实际是 `ZEngineAndroid-<config>.apk`。

**修法**：改成 glob `build/outputs/apk/<config>/*.apk`，多个取最新的，一个都
没有时把目录内容列出来再 fail。AAB 同理走 `bundle/<config>/*.aab`。

iOS 侧同病：`zbuild_ios.py` 硬编码 `bin/<config>/ZEngine.app`（Makefile 生成器
布局），Xcode 生成器用的是 `<Config>-iphoneos/ZEngine.app`。改成先试两个已知
位置，再 `rglob("ZEngine.app")` 兜底。

### 3.3 iOS target 被静默跳过 —— 高（无报错）

**症状**：配出来的工程里没有 `ZGameIOS`，或者 `IOS/CMakeLists.txt` 直接
`FATAL_ERROR`，但不告诉你为什么不成立。

**根因**：`IOS` 是工程侧两个判据的唯一开关 —— `engine/CMakeLists.txt:350` 的
`elseif(IOS)` 和 `IOS/CMakeLists.txt:17` 的 `if(NOT IOS)`。CMake 自己的 Darwin
平台模块在 `project()` 时也会设 `IOS`，但那属于内部行为且时机在 toolchain 之后。

**修法**（`engine/toolchain/ios.cmake:17`）：toolchain 里显式设一次，把"依赖
CMake 内部行为"变成"自己保证"，两者值相同不冲突。同时显式设
`CMAKE_OSX_DEPLOYMENT_TARGET = 16.0`，与 `XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET`
对齐（不设的话编译器检测走 SDK 默认版本，可能与 Xcode 属性不一致）。

### 3.4 宿主符号与 visibility —— 中

移动端开 `-fvisibility=hidden`（`ZENGINE_MOBILE_HIDE_INTERNAL_SYMBOLS`，
`engine/CMakeLists.txt:76`），`PlayerShell.h` 里没有 `ZPLAYER_API`（即
`__attribute__((visibility("default")))`）的符号在宿主里根本看不见。这类错误
要等链接完整个引擎才暴露，一次几十分钟。自检脚本把它挪到静态阶段。

---

## 4. 仍然只能真机验的

静态自检**覆盖不到**的部分，出包前必须在设备上确认：

1. **Vulkan surface 生命周期**：`INIT_WINDOW` / `TERM_WINDOW` / `CONFIG_CHANGED`
   三者的实际时序，尤其从后台切回来时有没有 `surface lost`。
2. **NativeActivity 冷启动耗时**：`android_main()` 到首帧的时间，以及有没有被
   ANR watchdog 杀掉（> 5s 无响应）。
3. **iOS 帧率与热节流**：Metal 后端的 present 间隔、掉帧后能不能恢复。
4. **触摸输入的多指与手势**：`ZPlayer_Touch_*` 在真机上的坐标与时序。
5. **assets / staging 内容完整性**：静态只查目录存在，不查内容是否齐全。
6. **签名与 Provisioning Profile**：`--team` 与 profile 的匹配只有 Xcode 能判。
7. **包体大小与 so 的 strip 结果**：取决于 `-fvisibility=hidden` 与
   `ZENGINE_MOBILE_SIZE_OPTIMIZE` 的实际效果。

---

## 5. 常见失败对照

| 现象 | 原因 | 查哪儿 |
| --- | --- | --- |
| logcat: *Unable to load native library* | `android.app.lib_name` ≠ CMake `TARGET_NAME` | 自检第 5/6 项 |
| 装上了，图标点了没反应 | Activity 没 `exported` 或缺 MAIN/LAUNCHER | 自检第 8/9/10 项 |
| Gradle sync 失败 | AGP 与 Gradle wrapper 版本不匹配 | 自检最后一项（AGP 8.0→8.4、8.5→8.7、8.7→8.9、8.9→8.11） |
| 旋转后画面拉伸 | 没处理 `APP_CMD_CONFIG_CHANGED` | `AndroidMain.cpp:401` |
| Gradle BUILD SUCCESSFUL 但报没产物 | 产物名猜错 | `zbuild_android.py::locate_output` |
| iOS 配出来没有 target | `IOS` 变量不成立 | `engine/toolchain/ios.cmake:17` |
| iOS 装上闪退，Xcode 报错指不到 | `Info.plist` 占位符没被替换 | 自检第 5 项（占位符 → CMake 属性） |
| 链接期 undefined symbol | 宿主用了没有 `ZPLAYER_API` 的符号 | 自检"宿主符号"两项 |
