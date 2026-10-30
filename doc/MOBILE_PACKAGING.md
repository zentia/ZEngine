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

### 3.5 iOS arm64 交叉编译（2026-10-29 首次编通）

不需要设备就能验的一部分，现在可以验了：**把引擎编到 iphoneos/arm64**。CI 里从来没有
iOS job（只有 macos / linux / windows），所以这条路径此前从未被编译过，第一次跑就炸出
5 处问题。

```bash
# 工程根是**仓库根**，不是 engine/。（ios.cmake 顶部与 Player/IOS/CMakeLists.txt
# 里原先写的 `-S engine` 已于 2026-10-30 改成 `-S .`：那样配会把路径解析成
# /engine/Libraries/... 并报"GPUSpine submodule missing"。）
cmake -S . -B build/ios -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=$PWD/engine/toolchain/ios.cmake
ninja -C build/ios ZGameIOS
# 产物：build/ios/engine/Source/Player/IOS/ZEngine.app/ZEngine（Mach-O arm64, platform 2）
```

前提是 Xcode 且**许可已同意**：没同意时 `xcodebuild` / `xcrun --sdk` / `simctl` /
`codesign` 全部返回同一句 *You have not agreed to the Xcode license agreements*，很容易
误判成"SDK 没装"。同意要管理员权限（`sudo xcodebuild -license accept`），也可以首次启动
Xcode.app 在弹窗里点。

修掉的 5 处（按踩到的顺序）：

1. **`-fno-stack-check` 必须 iOS 全局，不能只加在 ZRuntime**。ZEnginePCH 是共享预编译头，
   AppleClang 拒绝 PCH 与 TU 的 stack-check 设置不一致。只加 ZRuntime → PCH 报
   *was enabled ... but is currently disabled*；只加 PCH → ZPlayer 反向报一次。落点：
   `engine/CMakeLists.txt` 在 `add_subdirectory(Source/CommonPCH)` **之前** `add_compile_options`。
   （arm64 iOS 没有 `___chkstk_darwin`，栈探测必须关，配合 `Platform/IOS/ZChkstkDarwin.c`。）
2. **`if(APPLE)` 的 `.mm` glob 对 iOS 也成立**，于是把 macOS 的 AppKit 源
   （`Render/Platform/MacOS/*.mm`）编进了 iOS —— iPhoneOS SDK 里没有 `Cocoa/Cocoa.h`。
   iOS 下排除它们；ZRuntime 里没人 include 它们（只有 Editor 用，而 iOS 跳过了 Editor），
   所以不会断链。iOS 的窗口本来就是宿主给的（UIKit UIView，
   `WindowSystem::CreatePlatformApplication()` 对 iOS 返回 nullptr）。
3. **MetalRHI 的 6 处 macOS 专属 API**：
   * `MTLPixelFormatDepth24Unorm_Stencil8` —— Apple GPU 没有 24 位深度，iOS SDK 根本没声明；改 `Depth32Float_Stencil8`。
   * `MTLStorageModeManaged` ×2 —— iOS 是统一内存，改 `Shared`（全文没有 `synchronizeResource` / `didModifyRange`，无需补同步）。
   * `NSRect` + `convertRectToBacking:` —— `UIView` 没有这个方法；改 `bounds × contentsScale`，且 scale 来源必须与 `SyncLayerMetrics` **同一个**，否则 `resized` 每帧为真、swapchain 无限重建。
   * `view.layer = nil` —— `UIView.layer` 只读，只有 `NSView` 能置。
4. **CrashHandler.h 的 POSIX 分支：`WriteSnapshot()` 里那两个块作用域函数声明把名字放进了
   `zcrash_posix`**（C++ 规定块作用域的函数声明落在最近的 enclosing namespace），而定义在
   全局作用域 → 链接找 `zcrash_posix::ZEngineForceFlushLogs`，永远没有。Windows 分支不受
   影响（它的 handler 本身在全局作用域）。iOS 是第一个把这条路径真正链起来的平台。
   修法：在 `namespace zcrash_posix` 之前于全局作用域声明。

⚠️ **两个构建树共用 `bin/`（已根治）**：宿主的 `build/` 与 `build/ios` 都往仓库根的 `bin/`
输出同名 `.a`，谁后编谁覆盖，被覆盖的一方随后以完全不相关的症状炸掉：

* 编完 iOS　→ 宿主测试链上 iOS 归档，跑起来 SIGSEGV，栈指向 `MemoryManager::Malloc` 这类
  地方，**看不出是库被换了**；
* 编完宿主 → iOS 链接报 *building for 'iOS', but linking in object file
  (bin/libz.a[2](adler32.c.o)) built for 'macOS'*。

现在交叉构建（iOS / Android / OHOS）输出到自己的构建树 `<build tree>/bin`
（根 `CMakeLists.txt` 里按 `IOS/ANDROID/OHOS` 覆盖 `ZENGINE_UNIFIED_OUTPUT_DIR`，
`FORCE` 是故意的——交叉构建的缓存里可能残留宿主的值）。想指定别处传
`-DZENGINE_CROSS_UNIFIED_OUTPUT_DIR=<path>`。判别某归档是哪个平台的：
`otool -l <x.a> | grep -A3 LC_BUILD_VERSION`（platform 1 = macOS，2 = iOS）。

⚠️ 这条链路上还牵出两个**静默失败**的测试脚本问题（都不是 iOS 专属，但只有在这时被撞出来）：

* `scripts/run_smoke_test.py` 的 `LINK_LIBS` 只认 `lib<name>.a`，而本机 cmake 是 Debug 配置、
  产物叫 `lib<name>-debug.a`；认不到就**静默跳过**。偏偏 Apple 这条链接带了
  `-Wl,-undefined,dynamic_lookup`，于是 `mi_malloc` 之类被推迟到运行时解析、**绑成 0**，
  调用即 `EXC_BAD_ACCESS at 0x0000000000000000` —— 链接期零报错，崩溃栈还指向调用方
  （`MemoryManager::Malloc`）而不是缺失的库。已改为按实际存在的归档名解析（含 `-debug` /
  `-d` 变体），并把缺失的库打到 stderr。
* 归档里混着"用旧头文件编的 `.o`"时会冒出老签名的未定义符号（例如
  `ImmediatePtrResolveRead(int,long)` 两参版）。识别信号是 `nm -m <bin> | grep "dynamically
  looked up"`。这种只能**全量重编**，不要去补声明。

### 3.6 iOS 交叉编译进 CI（2026-10-30）

§3.5 是"第一次编通"，但一次性成果会腐烂：下次谁改一行 MetalRHI 或 `if(APPLE)`，
没有 CI 就没人知道又坏了。所以补了 `.github/workflows/build_ios.yml`
（`macos-latest`，Release + Ninja，跑的就是 §3.5 那两条命令）。

它的定位是**编译闸门**，不是出包：不签名、不打包、不运行。不加
`-DZENGINE_IOS_DEVELOPMENT_TEAM`，因为公开 runner 没有证书；而 Ninja 生成器本身不做
codesign，所以缺证书也能编出 `.app`（只是装不上设备）。同理只编 Release——Debug 只会让
runner 分钟数翻倍，却不会多覆盖任何一类"编译错误"。

**必须装 `glslang` / `spirv-tools`**（和 `build_macos.yml` 一样）。iOS 虽然走 Metal，
但内置 shader 是"先在宿主上编成 SPIR-V、再用 SPIRV-Cross 转 MSL"：

```
-- Engine: built-in shaders available as SPIR-V (Vulkan path + Metal via SPIRV-Cross)
-- ShaderCompile: glslangValidator is unavailable; skipping SPIR-V generation
```

没有 `glslangValidator` 时 `compile_shader()` **整体不跑** —— `engine/shader/CMakeLists.txt:31-41`
的 else 分支只有一句 `add_custom_target(... SOURCES ...)`，**没有任何生成规则**，于是
`engine/shader/generated/cpp/*.h` 不会被生成。

⚠️ **"不生成"不等于"构建会失败"**：那批 `.h` 是 gitignore 的构建产物
（`engine/.gitignore` 的 `generated/`），**磁盘上残留的那份会一直被用下去**。所以缺
glslang 的表现不是报错，而是——**你改了 shader 源码，构建却悄悄用着旧的那份**。
这比"真机一片黑"更难发现：构建绿、能跑、只有 shader 行为对不上。
（2026-10-30 之前本机正是这样：从没装过 glslang，所有验证用的都是磁盘残留的 `.h`。）

所以 job 里除了装它，还加了一步断言：
configure 日志里出现 `glslangValidator is unavailable` 就直接判红。

⚠️ 这一条最初写反过：当时写的是"只装 ninja，iOS 路径 0 次提及 glslang"。那是**假阴性**——
本机 bash 的 `grep` 在这个环境里会静默返回空，看着像"没有匹配"。教训：
**"grep 没匹配到"不等于"不存在"**，尤其在这个环境里一律用工具而不是 bash grep 下结论。

✅ **本机已于 2026-10-30 装好**（`brew install glslang spirv-tools`，glslang 16.6.0 /
SPIRV-Tools v2026.4）。装上后 configure 变成
`-- Found glslangValidator on PATH: /opt/homebrew/bin/glslangValidator`，不再出现
`skipping SPIR-V generation`，构建日志里会多出整批
`Generating .../engine/shader/generated/cpp/*.h`。

⚠️ **宿主也一样，而且没有 job 拦它**：`engine/shader/CMakeLists.txt` 不区分平台，缺
glslang 时宿主的 `generated/cpp/*.h` 同样不重新生成，用的还是磁盘残留那份 —— 但
`build_macos/linux/windows` 里只有 macos 和 linux 装了它。装好之后两条树都要**重新
configure**（CMake 缓存里存着"没找到 glslang"的探测结果，不重配不会重探）并重编，
否则你以为验证过了，其实验证的是"旧 `.h`"那个构建。

装完后用真产物重跑过四条断言，全绿：`Mach-O 64-bit executable arm64` /
`platform 2` / `dynamically looked up` 0 条 / `Info.plist` 存在。

**光"编译成功"不算证据**，所以最后一步是四条断言 —— 都做过正向 + 负向实跑：

| 断言 | 拦的是什么 | 负向构造 |
| --- | --- | --- |
| `file` 含 `Mach-O 64-bit executable arm64` | toolchain 悄悄退回宿主 SDK，照样报成功 | 用非 arm64 二进制 |
| `LC_BUILD_VERSION platform == 2`（1 = macOS） | 宿主 SDK 漏进来 | 拿 `/bin/ls` 冒充产物 → 报 `platform is '1'` |
| `nm -m \| grep -c "dynamically looked up"` == 0 | 缺库符号被 `-Wl,-undefined,dynamic_lookup` **绑成 0**，链接期零报错、首调崩在 `0x0`（§3.5 末尾） | 用 iphoneos clang 真编一个带未定义符号 + 该 flag 的二进制（在 iOS 上是 *deprecated* 不是禁止，所以这条不是多余的） |
| `Info.plist` 存在 | `MACOSX_BUNDLE_INFO_PLIST` 没生效 | 删掉真 `.app` 里的 plist |

产物路径 `build/ios/engine/Source/Player/IOS/ZEngine.app/ZEngine` 是**故意写死**的：
target 名是 `ZGameIOS` 但 `OUTPUT_NAME` 是 `ZEngine`，谁改了 OUTPUT_NAME 就让这个 job 红，
而不是静默跳过（同 `smoke_tests.yml` 的哲学：没跑起来的测试等于没测试）。

### 3.7 真机验证 fixture：`projects/DeviceSmoke`

真机验证需要一个能装到设备上、又能跑满帧循环的项目。仓库里原本没有候选：
`engine/` 没有 `Build/iOS`，`zbuild_ios.py` 第一步就断言 `project/Build/iOS` 存在。

所以加了 `projects/DeviceSmoke/`，它**故意不写 `startup_level`**：

* `PlayerShell::OpenStartupLevel()` 在 `startup_level` 为空时返回 `true` 并记
  `launching with an empty world` —— 引擎照样完整 boot：所有系统起来、RHI(Metal)
  初始化、CADisplayLink 驱动真实帧循环，只是没有关卡内容要加载。
* 真机要验的是 Metal swapchain、帧率与热节流、多指触摸 id 分配、bundle 资源完整性，
  **空世界里全能测**。带上内容反而引入"这份内容有没有为 iOS 打好包"这个无关的失败源。

⚠️ **不要给它补 `startup_level`**，也不要拿它去替掉 `engine/ZGameSmoke.zproject`：

| | `projects/DeviceSmoke` | `engine/ZGameSmoke.zproject` |
| --- | --- | --- |
| startup_level | **无**（空世界） | `asset/level/1-1.scene` |
| 用途 | iOS 真机：Metal / 帧率 / 触摸 / bundle | 编辑器与桌面端验收：关卡、内容、Spine 等 |
| 被谁依赖 | `zbuild_ios.py --project projects/DeviceSmoke` | `scripts/run_editor_macos.sh`（`cp -f` 硬依赖，删了脚本直接挂）；大量 doc 里的验收步骤 |

两者**互补不重复**。ZGameSmoke 是仓库里唯一带真关卡的工程，删了 macOS 编辑器就起不来。
（`ProjectInfoPathsSmokeTest` 不读它 —— 那个测试内嵌自己的 `.zproject` 文本，所以不会因为删除而红。）

⚠️ **`build/ios` 与 `build/ios-xcode` 不能合并**：CMake 缓存里写着 `CMAKE_GENERATOR`，
Ninja 与 Xcode 不一致时 cmake 直接报 *generator does not match the generator used
previously*。`build/ios` 是 CI 编译闸门（Ninja）用的，`zbuild_ios.py` 走 Xcode
（要签名 / Archive），两者各用一棵树。

### 3.8 iOS 模拟器：没有证书 / 没有设备时唯一能真跑的路径（2026-10-30 首次跑通）

真机验证卡在"没有 Team ID / 没有 Apple ID / 设备不被识别"时，**模拟器是唯一能把
iOS 代码真正跑起来的地方**：它不需要签名，也不需要设备。

```bash
cmake -S . -B build/iossim -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_TOOLCHAIN_FILE=$PWD/engine/toolchain/ios-simulator.cmake
ninja -C build/iossim ZGameIOS

UDID=$(xcrun simctl list devices booted | grep -m1 -oE '[0-9A-F-]{36}')
xcrun simctl install "$UDID" build/iossim/engine/Source/Player/IOS/ZEngine.app
xcrun simctl launch  "$UDID" com.zengine.player
```

* `ios-simulator.cmake` 先 `include(ios.cmake)` 再把 `CMAKE_OSX_SYSROOT` 换成
  `iphonesimulator`。**必须走这个文件**而不能 `-DCMAKE_OSX_SYSROOT=...` 覆盖：
  `ios.cmake` 里是普通 `set()`，`-D` 传进去的值会被它自己覆盖掉。
* 引擎日志在
  `~/Library/Developer/CoreSimulator/Devices/<UDID>/data/Containers/Data/Application/<容器>/Library/Logs/ZEngine/engine_*.log`。
  ⚠️ **容器 UUID 每次 `install` 都可能变**，用
  `ls -td .../Application/*/Library/Logs/ZEngine/*.log | head -1` 定位最新那份。
* ⚠️ 沙箱里 **`ps` 不可用**（`operation not permitted`）。判断进程还活着改用
  **有没有新的崩溃报告**：跑之前记下 `ls -t ~/Library/Logs/DiagnosticReports/ | head -1`，
  跑完再比一次 —— 没变就是没崩。

模拟器能验 / 不能验：

| | 模拟器 | 真机 |
| --- | --- | --- |
| boot 全链路、工程与资源加载 | ✅ | ✅ |
| Metal swapchain 建立、CADisplayLink 帧循环 | ✅ | ✅ |
| 帧率与热节流 | ❌（跑在宿主机 GPU 上，数字没意义） | ✅ |
| 多指触摸 / 手势 | ❌（鼠标只能单点） | ✅ |
| 签名、包体、启动耗时 | ❌ | ✅ |

### 3.9 本轮在 iOS 上修掉的四个运行时阻塞（2026-10-30）

四个都是"编译期零报错、链接期零报错，装上才炸"，且**都不会出现在 iOS CI 的编译闸门里**
—— 这正是 §3.6 那句"没有 job 去编的配置 = 根本没编译过"的运行时版本：
**编译得过 ≠ 起得来**。

**1) UIScene 生命周期（iOS 26+ SDK）—— 启动即退**

```
UIKitCore: Application failed to launch: 'UIScene life cycle is required for apps
built with this SDK'
```
老式的 `UIApplicationDelegate` + 自己持有 `window` 在新 SDK 上直接被拒。已把
`ZEngineAppDelegate` 拆成 AppDelegate（进程级）+ `ZEngineSceneDelegate`（窗口级，
持有 window / renderView / displayLink），并在 `Info.plist` 补
`UIApplicationSceneManifest`。**只在运行时发作，编译期完全看不出来。**

**2) 缺 `ZEditor.ini` → `ConfigManager` 失败 → 整个引擎 rollback**

移动端 `.app` bundle 里从来没有这个文件。`ConfigManager::Initialize()` 原来把它当
致命错误，于是 39 个系统全部回滚。已改成"文件**不存在**时用默认值继续"（打 WARNING），
只有"存在但打不开/解析不了"才算失败。

**3) `ProjectInfo` 拿不到工程路径 —— 两个独立原因叠在一起**

```
[I] Initializing system: 11ProjectInfo
[F] Failed to initialize system 'ProjectInfo', rolling back      ← 前面一行解释都没有
```

* 桌面端工程路径由命令行 `-p` 进 `CommandSystem`，**移动端没有命令行**。所以
  `PlayerShell.cpp` 必须自己把工程路径写回 `CommandSystem`。
* 坑 a：那段 write-back 一开始放在 `RegisterGameRuntime()` **之前** —— 而
  `CommandSystem` 本身就是 Runtime 系统，`RegisterRuntime()` 才注册它，所以
  `GET_SYSTEM(CommandSystem)` 返回 `nullptr`，**整个块被静默跳过**。
* 坑 b：`CommandSystem::Initialize()` 里是**无条件** `project_path = project_path_str`，
  没有 `-p` 时 `project_path_str` 是空串，把刚写进来的值又冲掉了。已改成
  "命令行 > 宿主预设 > 空"。

> 教训：两处都是**静默失败**（一个 nullptr 分支没日志，一个无条件覆盖没日志）。
> 症状和"根本没写这段代码"一模一样，白等了一轮。现在 nullptr 分支会打 ERROR。

**4) 粒子纹理没打进包 → 首帧空指针崩溃**

```
EXC_BAD_ACCESS / SIGSEGV, KERN_INVALID_ADDRESS at 0x0000000000000000
ParticlePass::EnsureAttachmentsInitialized()  ← 每帧第一件事，首帧就崩
```
`RenderResourceBase::LoadTextureHDR()` 在 `stbi_loadf` 读不到文件时**返回 `nullptr`**，
原代码照常 `->m_Width`。空工程（如 `DeviceSmoke`）里 `GlobalParticleRes` 两张图的路径
是空的，于是必然崩。已改成取不到就早退并**只打一次** ERROR
（`m_AttachmentLoadFailedLogged`，照 `m_DrawSkippedLogged` 的先例），粒子不显示但不崩。

**修复后的 iOS boot 证据**（`DeviceSmoke`，模拟器 iPhone 18 Pro）：

```
[W] ZPlayer: no startup_level configured in .zproject; launching with an empty world
[I] ZPlayer: booted (project = .../ZEngine.app/zengine_project)
[I] MetalRHI: swapchain ready 874x402
[E] ParticlePass: attachments not initialized (particle billboard texture could not be
    loaded) -- particles stay invisible
```
跑 14 秒无新崩溃报告（修复前同一位置必崩，见 `ZEngine-2026-10-30-183037.ips`）。

### 3.10 BqLog：交叉编译会污染宿主的验证入口（2026-10-30）

`bqlog` 的 CMakeLists 把产物**硬编码写进源码树**：

```cmake
# engine/3rdparty/bqlog/src/CMakeLists.txt:135
set(ARTIFACTS_OUTPUT_BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../artifacts/${BUILD_LIB_TYPE}")
```

那是它"分发产物"的设计，一台机器只编一个平台时没问题。但单机交叉编译时，
宿主(macOS) 与 iOS 的 `ARTIFACTS_SUB_DIR` **都是** `lib/$<CONFIG>`（android/ohos 反倒
没事，它们的子目录带 ABI），先编的会被后编的静默覆盖。

后果很难懂 —— 跑完 `ninja -C build/iossim` 之后，`verify_prefab_native.sh` 三个用例
**全部变红**，而输出只有一句：

```
clang++: error: linker command failed with exit code 1
```

看着像 prefab 测试坏了，其实跟 prefab 毫无关系。真错误被脚本的 grep 过滤掉了，得手动
跑 `run_smoke_test.py` 才看得到：

```
ld: building for 'macOS', but linking in object file
(engine/3rdparty/bqlog/artifacts/static_lib/lib/Debug/BqLog.framework/BqLog[2](aes.cpp.o))
built for 'iOS-simulator'
```

已在 `engine/3rdparty/CMakeLists.txt` 里让交叉编译（`IOS/ANDROID/OHOS`）的 BqLog 产物
落进 build tree（`build/iossim/3rdparty/bqlog/`），源码树那份只由宿主写。
判别平台：`otool -l <BqLog> | grep -A3 LC_BUILD_VERSION`（`1`=macOS，`7`=iOS-simulator）。
被污染后要**删掉那个二进制**再让 ninja 重建（产物时间戳比源新，ninja 不会自己重编）。

> 这是记忆里"宿主与 iOS 共用输出目录"那条坑的新变体：上一轮只把 `bin/` 隔离了，
> 漏了第三方库自己写源码树的那一半。

### 3.11 iOS boot 闸门：把"能编译"升级成"能启动"（2026-10-30）

§3.6 的 `build` job 只证明 iOS 源码**编得过**。§3.9 那四个缺陷证明这远远不够 ——
它们没有一个会在编译或链接期报错。所以补了 `scripts/verify_ios_boot.sh`，CI 里是
`build_ios.yml` 的第二个 job `boot-simulator`。

```bash
scripts/verify_ios_boot.sh [app_bundle]     # 默认 build/iossim/.../ZEngine.app
IOS_SIM_UDID=<udid> IOS_BOOT_WAIT=30 scripts/verify_ios_boot.sh
```

选模拟器（已 Booted 优先，否则起一个 iPhone）→ uninstall → install → launch →
跑 `IOS_BOOT_WAIT` 秒（默认 15）→ 断言：

| 断言 | 拦住的是 | 负向是怎么构造的 |
| --- | --- | --- |
| 运行期间没有新的崩溃报告 `.ips` | 空指针崩溃（§3.9 缺陷 4） | 把 `ParticlePass` 的 nullptr 检查改回不检查 → 首帧崩 → 报 `app crashed during the run: ZEngine-2026-10-30-194110.ips` |
| 日志有 `ZPlayer: booted` | UIScene 缺失导致启动即退（§3.9 缺陷 1） | 把 `application:configurationForConnectingSceneSession:` 改名 → 报 `no engine log ... the app never got far enough to open it` |
| 日志有 `MetalRHI: swapchain ready` | RHI 没起来 | 同上（一起红） |
| 日志没有 `[F]`（fatal）行 | 系统初始化失败（§3.9 缺陷 2、3） | 同上 |

实现上两个坑：

* ⚠️ **必须先 `uninstall` 再 `install`**：容器 UUID 每次 install 都可能变，而日志是
  **累积**的 —— 不清掉就会读到上一次运行留下的 `[F]` 行，那条断言等于没做。
* ⚠️ 用 `simctl list devices -j` + python 解析，不要 grep 文本：本机沙箱里 bash 的
  `grep` 会静默返回空，脚本会退化成"永远找不到设备"（§3.6 那条假阴性是同一个坑）。

**顺带发现：`Info.plist` 的 `UIApplicationSceneManifest` 其实是冗余的。**
把整个 manifest 删掉、重新构建（确认 bundle 里那份也确实没有 manifest 了），app
**照样 boot 成功**。真正必需的是代码里的
`application:configurationForConnectingSceneSession:options:` 返回一个带
`delegateClass = [ZEngineSceneDelegate class]` 的 `UISceneConfiguration` —— UIKit 有
这个回调就不需要 plist 声明。当时两处一起加，很容易误以为是 plist 那处在起作用。
留着无害（声明式、更明确），但**别指望只靠 plist 能救回删掉的代码那处**。

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
| 所有 Apple 工具都说 *not agreed to the Xcode license* | 许可未同意（不是 SDK 缺失） | `sudo xcodebuild -license accept`（要管理员权限）或首次启动 Xcode.app 点同意 |
| iOS 配置报 `GPUSpine submodule missing at /engine/Libraries/...` | 用了 `-S engine`；工程根是**仓库根** | §3.5 的命令（`cmake -S . -B build/ios`） |
| PCH 报 *stack checking was enabled ... but is currently disabled* | `-fno-stack-check` 没同时覆盖 PCH 与所有消费方 | §3.5 第 1 条 |
| 宿主测试崩在 `EXC_BAD_ACCESS at 0x0` 且链接期无报错 | 某个库没链上 + `dynamic_lookup` 绑成 0（或刚被 iOS 构建覆盖 `bin/`） | §3.5 末尾两条；`nm -m <bin> \| grep "dynamically looked up"` |
| iOS CI 报 `ZEngine not found` | target 名是 `ZGameIOS` 而 `OUTPUT_NAME` 是 `ZEngine`；后者被改了 | §3.6；`.github/workflows/build_ios.yml` 写死路径是故意的 |
| iOS CI 报 `platform is '1'` | 宿主 SDK 漏进来了（toolchain 没生效，`CMAKE_SYSTEM_NAME` 不是 iOS） | §3.6 断言表；`otool -l <bin> \| grep -A3 LC_BUILD_VERSION` |
| 带项目内容构建报 `mkdir(.../ZEngine.app/zengine/Build): Not a directory` | bundle 内的项目目录原本叫 `zengine`，而可执行文件 `OUTPUT_NAME` 是 `ZEngine` —— macOS 默认卷宗**大小写不敏感**，两者被当成同一条路径 | 已改名 `zengine_project`（`IOSMain.mm` 的 `k_bundled_project_dir` / `Player/IOS/CMakeLists.txt` 的 glob / `zbuild_ios.py` 的 `STAGING_PROJECT` 三处必须同步）。**设备上是大小写敏感的，所以只在构建宿主机发作，真机永不复现** |
| 链接期 undefined symbol | 宿主用了没有 `ZPLAYER_API` 的符号 | 自检"宿主符号"两项 |
| iOS 模拟器启动即退：*UIScene life cycle is required for apps built with this SDK* | iOS 26+ SDK 不再接受纯 `UIApplicationDelegate` + 自建 window | §3.9 第 1 条 |
| iOS 上 `[F] Failed to initialize system 'ConfigManager'` | `.app` 里没有 `ZEditor.ini`（已改成非致命） | §3.9 第 2 条 |
| iOS 上 `[F] Failed to initialize system 'ProjectInfo'`，前面一行解释都没有 | 工程路径没写回 `CommandSystem`（时序 + 无条件覆盖两个原因） | §3.9 第 3 条 |
| iOS 首帧 `EXC_BAD_ACCESS at 0x0`，栈在 `ParticlePass::EnsureAttachmentsInitialized` | 纹理没读到，`LoadTextureHDR` 返回 `nullptr` 却照样解引用 | §3.9 第 4 条 |
| `verify_prefab_native.sh` 三个用例全红，只有一句 `linker command failed` | 刚跑过 iOS 构建，源码树里的 `BqLog.framework` 被覆盖成 iOS-simulator 版 | §3.10 |
| 沙箱里 `ps` 报 *operation not permitted* | 正常限制；判断进程存活改用崩溃报告比对 | §3.8 |
