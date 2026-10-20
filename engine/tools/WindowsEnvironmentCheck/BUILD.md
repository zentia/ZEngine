# Windows Environment Check —— 构建与分发

面向要构建或打包本工具的人。**用法见 [README.md](README.md)。**

## 两种构建配置

| 配置方式 | 后端 | 说明 |
|---|---|---|
| 全树 `cmake -S . -B build` | 引擎后端（链接 ZRuntimeShared） | **分发用的构建**，配合 `-DZENGINE_STATIC_CRT=ON` 让 DLL 也是 `/MT`，产物仍能发到没有 VC redist 的机器（已验证：exe 与 DLL 都不再 import `MSVCP140`/`VCRUNTIME140`）。此前 Jolt 固定 `/MD`，会把 `ZRuntimeShared.dll` 的链接打断；现在它跟随该开关 |
| 独立 `cmake -S engine/tools/WindowsEnvironmentCheck -B <dir>` | 自包含（静态 bqlog + 内置表） | 没有 ZRuntimeShared 目标（那要拉起整个 Runtime 依赖树），退化为旧路径 |

## 与 ZRuntimeShared 的关系

全树构建时本工具链接 `ZRuntimeShared.dll`，日志和多语言表都来自引擎——原因是这两件事
引擎本来就有，而本工具正好都需要：

- **日志**：引擎的 `LogSystem`（bqlog）。工具是 WINDOWS 子系统进程，双击启动时 stderr
  没人看得见，而它要报告的失败（缺字体、WER 恢复失败、ProcDump 没产出 dump）恰恰是玩家
  描述不出来的那些。日志落在 `%USERPROFILE%\AppData\LocalLow\ZEngine\Logs\ZEnvironmentCollector_*.log`。
- **多语言表**：硬编码在代码里不好维护，走表；而表本身是引擎能力（DataTable +
  AssetManager + SerializedFile），所以一并从引擎取。

两者都只通过 `extern "C"` 入口（`ZRuntime_*`）跨模块，不传 `std::string`、不跨模块
`new/delete`，因此 `/MT` 的 exe 也能安全消费 CRT 选择不同的 DLL。

只起引擎的**最小切片**（`RegisterCore()` + `RegisterPlatform()`）：反射、内存、FileSystem、
ObjectManager、AssetManager，加上 LogSystem。**不注册** Application / WindowSystem / 任何 RHI /
ScriptingManager / ProjectInfo——一个被派去诊断机器的工具，不该在那台机器上开窗口或建 GPU 设备。

注意：引擎后端下 `ZRuntimeShared.dll` 必须和 exe 同目录（POST_BUILD 已拷贝），
否则进程起不来。

## 产物

Release 构建后需要一起分发：

```text
ZEnvironmentCollector.exe
ZDllLoadProbe.exe
ZRuntimeShared.dll          <- 仅引擎后端（全树构建）
resource\ui_strings_zh-CN.zasset   <- 仅引擎后端，构建期由 DataTableCook 生成
profiles\default.ini
profiles\osgame.ini
LICENSE-Zydis.txt
LICENSE-Zycore.txt
```

- `ZEnvironmentCollector.exe`：主程序（GUI + `-batchmode` CLI）。
- `ZDllLoadProbe.exe`：无 C/C++ CRT、无 AVX/AVX2 的隔离 DLL 加载探针。
- `ZRuntimeShared.dll`：引擎切片（日志 + 表）。缺了它进程起不来，不只是少个功能。
  用 `-DZENGINE_STATIC_CRT=ON` 构建时它也是 `/MT`，所以目录照样是自包含的。
- `resource\ui_strings_zh-CN.zasset`：简体中文界面表。缺失时界面降级为英文，工具仍然可用
  （引擎日志里会写明原因）——诊断工具不能因为自己的资源目录坏了就跑不起来。
- `profiles\*.ini`：项目配置，GUI 默认读取同目录 `profiles/default.ini`。
- `LICENSE-*.txt`：静态链接的 Zydis/Zycore MIT 许可证。

不要单独分发主程序，否则动态探针会报告 `DLL_PROBE_INFRASTRUCTURE_FAILED`，`--collect` 的分阶段子进程也会报 `HELPER_MISSING`。

## 构建目录布局

产物不落在 build 目录里：`ZEnvironmentCollector.exe` 与 `ZDllLoadProbe.exe` 输出到
`bin\Tools\<Config>\`（子目录名与源码目录同名），profiles 与 LICENSE 也复制过去，所以该目录
就是可直接打包的分发单元——而不会在共享的 `bin\Tools\<Config>\` 里和另外三个工具的 exe 混在一起。

**构建目录按配置带后缀**，因为 `bin\Tools\<Config>\` 是全仓库每一棵构建树共用的：

| 配置 | 构建目录 |
|---|---|
| 独立构建 | `WindowsEnvironmentCheck\` |
| 引擎后端 `/MD` | `WindowsEnvironmentCheck-zruntime-md\` |
| 引擎后端 `/MT`（分发用） | `WindowsEnvironmentCheck-zruntime-mt\` |

两个维度都需要：只按后端分的话，两棵全树（`/MD` 与 `/MT`）仍然会互相覆盖同一个 exe，
而 `cmake --install` 只会忠实地把最后落地的那个装走——包里的 exe 就悄悄变成了另一个构建。
**安装目录不受影响**，始终是 `WindowsEnvironmentCheck\`（`--prefix dist` 装出
`dist\WindowsEnvironmentCheck\`），那是包内的契约目录，不随配置变。

## 独立构建与分发

独立构建默认包含GUI；从引擎父项目嵌入时默认关闭GUI（`ENVIRONMENT_CHECK_BUILD_GUI=OFF`），因为整树里 ZSlate 已经是 `/MD`，与本工具的 `/MT` 链接会 LNK2038。**关闭GUI不会少一个exe**：同一个 `ZEnvironmentCollector.exe` 仍然会构建，只是换成 `EnvironmentCheckGuiFallback.cpp`，双击会提示改用 `-batchmode`，命令行能力完全不变。不同运行库配置须使用独立build目录。

```powershell
cmake -S engine\tools\WindowsEnvironmentCheck -B build\tools\ZEnvironmentCollector -G "Visual Studio 17 2022" -A x64 -DENVIRONMENT_CHECK_BUILD_GUI=ON -DBUILD_TESTING=ON
cmake --build build\tools\ZEnvironmentCollector --config Release --target ZEnvironmentCollector EnvironmentCollectorTests EnvironmentCollectorUiTests EnvironmentCaptureTests EnvironmentPreflightTests
ctest --test-dir build\tools\ZEnvironmentCollector -C Release --output-on-failure
cmake --install build\tools\ZEnvironmentCollector --config Release --component EnvironmentCollector --prefix dist
```

## 独立构建（CLI only）

只想出命令行包时关掉GUI，产物仍是同一个 `ZEnvironmentCollector.exe`：

```powershell
cmake -S engine\tools\WindowsEnvironmentCheck `
  -B build\tools\ZEnvironmentCollectorCli `
  -G "Visual Studio 17 2022" `
  -A x64 `
  -DENVIRONMENT_CHECK_BUILD_GUI=OFF

cmake --build build\tools\ZEnvironmentCollectorCli `
  --config Release `
  --target ZEnvironmentCollector
```

构建使用固定版本及 SHA-256 的 Zydis 4.1.1 和匹配的 Zycore。首次配置需要网络下载依赖，之后可复用 CMake FetchContent 缓存。

## 打包分发（zip + SHA256）

参考扁鹊（`E:\BianqueCCS\pack`）的做法：打包是一个**独立的、不编译任何东西的 CMake 工程**
`engine\tools\WindowsEnvironmentCheck\pack\`，只把已经 staging 好的安装树用 CPack 打成一个包。
之所以不放进工具自己的 `CMakeLists.txt`：那份文件也会被整树 `add_subdirectory()`，
CPack 会在一个从没打算发东西的构建里被配置一遍。

```powershell
cmake --install build\tools\ZEnvironmentCollector --config Release --component EnvironmentCollector --prefix dist\staging
cmake -S engine\tools\WindowsEnvironmentCheck\pack -B build\tools\ZEnvironmentCollector\pack
cmake --build build\tools\ZEnvironmentCollector\pack --target package
```

产出 `dist\<名字>-windows-x86_64-<版本>.zip` 与同名 `.zip.sha256`，名字区分两种构建：

- 自包含构建 → `zenvironment-collector-windows-x86_64-<版本>.zip`
- 引擎后端构建 → `zenvironment-collector-zruntime-windows-x86_64-<版本>.zip`

多出的 `zruntime` 由 pack 读 exe 导入表自动加上，不用手填——两种构建曾产出**完全同名**的两个包，
内容却不同，只能打开才知道是哪个。

- 版本号从 `ToolVersion.h` 解析（与报告里的 `toolVersion` 同一个来源，不可能对不上）；可用
  `-DPACK_VERSION_OVERRIDE=x.y.z` 覆盖。
- 架构从生成器探测，可用环境变量 `USER_DEF_ARCH` 覆盖。
- 包内是一个以包名命名的顶层目录，展开后 `README.md` + `WindowsEnvironmentCheck\`
  （exe、`ZDllLoadProbe.exe`、`profiles\`、LICENSE）——就是能直接跑的那个目录，没有多余的层。
- ZIP 用 CMake 自带的归档能力生成，不需要装 7-Zip。
- **两种构建打出的包同名但内容不同**：独立构建自包含（无 DLL）；引擎后端（全树构建）多出
  `ZRuntimeShared.dll` 与 `resource\ui_strings_zh-CN.zasset`。pack 会读 exe 的导入表判断是哪一种：
  引擎后端但 staging 里没有 DLL 时**直接配置失败**（缺 DLL 是 `0xc0000135`、进程根本起不到
  `main()`，而包看起来完全正常）；缺 zasset 只告警，因为 UI 会退回英文而不是死。
- staging 目录里没有 `ZEnvironmentCollector.exe` 时**直接配置失败**并打印该跑的那条命令：
  把一个空的/过期的 staging 目录打成一个看起来很像样的包，是这里唯一一种"看起来成功了"的失败。
- README.md 会与源码树逐字节比对，不一致时告警——staging 复用会装进旧 README，而 launcher
  契约写在里面（二进制错了会当场报错，README 错了不会）。

## 开发/验证记录

以下几段是开发期的验证流水账，不是用法也不是构建步骤，留档备查：

分发 `ZEnvironmentCollector.exe`、`ZDllLoadProbe.exe`、profiles及LICENSE文件，不只发主 exe。无需ZRuntime或引擎资源；Release采用静态CRT。GUI和ZIP模式未改变旧环境报告schema5。GUI动态依赖Windows的 `d3d11.dll`、`D3DCOMPILER_47.dll` 及基础系统DLL。

1.6.1 将问题类型改为ZSlate下拉框，支持鼠标选择、方向键/回车、Esc取消、外点关闭且不穿透底层控件。启用PerMonitorV2，窗口/字体按物理DPI更新而不重建控件；修正字形对齐时额外拉伸1像素的错误，测量与绘制使用相同整数像素字号。

验证：Release构建成功，CTest核心与UI共2/2通过；UI覆盖100/125/150/200%下下拉选择/关闭/键盘/边界翻转、字形与UV的1:1映射（含裁剪）、指针重置保留编辑焦点及失焦清理。实机150%：旧版窗口DPI=96且awareness=0，新版DPI=144且awareness=2；本轮成功取得中文及下拉展开截图，并通过真实鼠标输入验证外点按住移动不穿透、Esc后第一次点击有效、选择卡住生成hang报告及ZIP校验。截图位于独立build的 `validation-1.6.1-final`。这些测试不是与ImGui/UE同字体同字号的视觉等价证明。

仍为内部试用：实际跨显示器DPI切换、WARP故障回退、各厂商BIOS/WHEA字段和受限权限需进一步实机验证。GUI使用Windows 10 1607及以上的DPI API。最后打包阶段不可取消。尚未数字签名，不作为已完成现网发布验收的产物。
