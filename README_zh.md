# ZEngine（中文说明）

<p align="center">
  <a href="https://zentia.github.io">
    <img src="engine/resource/ZEngine.png" width="400" alt="Z Engine logo">
  </a>
</p>

**Z Engine** 是一个轻量级游戏引擎。

## 最新更新（2026年10月）

- **ZSlate**：引擎 UI 框架已独立为子模块（`engine/Libraries/ZSlate`），编辑器已迁移到它——ImGui 后端已退役，仍有少量 ImGui 控件孤岛（场景网格、shader 编辑器、预览控件、弹窗）在转换中
- **独立 GUI 工具**（`engine/tools/`）：`ZEnvironmentCollector`（Windows 兼容性报告 + 崩溃/日志采集）、`LuaMemPreview`（Lua 内存快照，`-batchmode` 导出 CSV）、`TexPreview`（纹理查看器）、`ClientProjBranches`（分支/SVN/git 管理）。四个都输出到 `bin/Tools/<Config>/`
- **ZInsights**：独立的 `.ztrace` 性能分析查看器（Win32 + Direct2D），渲染的火焰图控件与编辑器内面板是同一个
- **ZAppShell**：header-only 启动工具——统一的命令行读取与控制台附着，供所有 ZEngine 可执行文件共用，且不受运行时库（`/MT` / `/MD`）差异影响
- **D3D12 后端**：与 Vulkan 并列，共用一套多 RHI 抽象；macOS 使用 Metal
- **agentkit**：崩溃归因的 Python 工具包（`zagent` / `zfault` / `zlab`），已接入 CI（带显式平台开关）
- **RenderDoc 集成**：引擎内截帧模块，以及 `tools/` 下的 MCP 服务与截帧分析脚本

详细版本历史请查看 [CHANGELOG.md](CHANGELOG.md)。

## 核心特性

### 引擎核心
- **多平台支持**：Windows、Linux 和 macOS（Web 通过 Emscripten）
- **多 RHI 渲染器**：Vulkan 与 D3D12 后端共用一套抽象，macOS 使用 Metal，支持验证层
- **物理引擎**：集成 Jolt Physics 物理引擎
- **资源管线**：完整的资源管理和打包系统

### AI（第一阶段）
- **行为树**：Selector / Sequence / Parallel 组合节点，Inverter / Repeater / Cooldown 装饰节点，Condition / Action 叶子节点
- **黑板**：按代理（per-agent）的键值槽位与感知快照，不参与序列化
- **感知组件**：视锥 + 射线视线检测 + 听觉半径，基于 `PhysicsScene::Raycast`，可按间隔扫描
- **控制器组件**：`chase` / `chase_and_retreat` / `guard` 三种内置模板，输出为 `AIMoveIntent` 与 `AIActionRequest`
- **执行层接线（框架层）**：`AIControllerComponent` 每帧把 `AIMoveIntent` 直接推给同对象的 `MotorComponent`，Motor 加了一条与玩家输入并行的 `TickAIMotor` 路径；脚本只负责配置（profile、范围、行为树），不参与驱动移动
- **Navigation（二期）**：`NavMesh` 用网格采样可行走表面，A* 寻路 + string pulling 平滑；`NavigationSystem` 持有网格并桥接 `PhysicsScene`，`NavMeshVolumeComponent` 在编辑器里配置体积。追击/撤退/回岗三个动作优先走寻路，无网格时回退直线
- **AISystem**：全局开关、每帧预算与统计；AI 层从不直接移动刚体，执行层负责消费意图

### 脚本
- **TypeScript / JavaScript**：唯一支持的脚本路径，经 Puerts（桌面 V8，Web / 移动端 QuickJS），
  配 `tsc --watch`、热重载、以及可在 Inspector 编辑的逐实例字段
- **Lua / C#**：**不是**脚本后端。Lua 只留了一个加载器桩，C# 只留了内存分析钩子；
  详见 `doc/TYPESCRIPT_SCRIPTING_DESIGN.md`

### 编辑器
- **可视化编辑器**：基于 ZSlate 的编辑器界面（ImGui 后端已退役，少量控件孤岛仍在转换）
- **控制台窗口**：内置调试控制台
- **项目管理**：资源浏览器和层级视图
- **时间轴编辑器**：动画和序列编辑工具
- **Inspector**：属性编辑和组件管理

### 开发工具
- **性能分析**：支持所有脚本语言的内存和性能分析
- **ZInsights**：独立的 `.ztrace` 火焰图查看器
- **序列化**：支持反射的二进制和 JSON 序列化
- **虚拟文件系统**：灵活的文件系统抽象
- **热重载**：资源和脚本热重载支持

## 仓库结构

| 路径 | 说明 |
|---|---|
| `engine/Source/Runtime/` | 运行时：`Core`、`Function`（Render、Physics、Animation、GAS、AI、Input、ShaderLab 等）、`Resource`、`Scripting`、`Profiler`、`VirtualFileSystem`、`ZEngineApi` |
| `engine/Source/Editor/` | ZEditor——ZSlate 窗口、编辑器 UI、属性绘制器、停靠布局 |
| `engine/Source/Insights/` | ZInsights——独立的 `.ztrace` 查看器（Windows + Direct2D） |
| `engine/Source/Modules/` | 可选引擎模块（当前为 RenderDoc） |
| `engine/Source/WebLauncher/` | Emscripten 入口 + `shell.html` |
| `engine/Libraries/ZSlate/` | ZSlate UI 框架——**git 子模块** |
| `engine/Libraries/ZAppShell/` | header-only 启动工具（宽字符命令行、控制台附着） |
| `engine/tools/` | 四个独立 GUI 工具（见下） |
| `engine/3rdparty/` | 第三方源码 |
| `tools/` | RenderDoc 集成、其 MCP 服务，以及截帧分析脚本 |
| `agentkit/` | 崩溃归因的 Python 工具包：`zagent`、`zfault`、`zlab` |
| `doc/` | 设计文档与指南 |
| `zbuild.py` | 统一构建入口（configure / build / tool / test / install） |

## 独立工具

用 `zbuild.py tool <名称>`（或 `--all`）构建，产物在 `bin/Tools/<Config>/`。

| 工具 | 可执行文件 | 说明 |
|---|---|---|
| ZEnvironmentCollector | `ZEnvironmentCollector.exe` | Windows 兼容性报告：硬件/固件/驱动探测、经同意的崩溃采集、游戏日志收集、ZIP64 报告。GUI 或 `-batchmode`（供 CI 用） |
| LuaMemPreview | `LuaMemPreview.exe` | Lua 内存快照查看器（`.zasset` 或 CSV）；`-batchmode` 可导出单个快照或整个目录的 CSV |
| TexPreview | `TexPreview.exe` | 纹理查看器 |
| ClientProjBranches | `ClientProjBranches.exe` | 分支 / SVN / git 管理 |

`ZEditor.exe` 和 `ZInsights.exe` 随主工程一起构建，不走 `zbuild.py tool`。

## 持续集成状态

| 构建类型         | 状态                                                                                   |
| :--------------: | :------------------------------------------------------------------------------------: |
| **Windows 构建** | [![Build Windows](https://github.com/zentia/ZEngine/actions/workflows/build_windows.yml/badge.svg)](https://github.com/zentia/ZEngine/workflows/build_windows.yml) |
| **Linux 构建**   | [![Build Linux](https://github.com/zentia/ZEngine/actions/workflows/build_linux.yml/badge.svg)](https://github.com/zentia/ZEngine/workflows/build_linux.yml) |
| **macOS 构建**   | [![Build macOS](https://github.com/zentia/ZEngine/actions/workflows/build_macos.yml/badge.svg)](https://github.com/zentia/ZEngine/workflows/build_macos.yml) |

## 依赖环境

构建 ZEngine 需要先安装以下工具：

### Windows 10/11
- Visual Studio 2022（或更新版本）
- CMake 3.19（或更新版本）
- Git 2.1（或更新版本）
- Vulkan SDK

### macOS >= 10.15 (x86_64)
- Xcode 12.3（或更新版本）
- CMake 3.19（或更新版本）
- Git 2.1（或更新版本）
- Vulkan SDK

### Ubuntu 20.04
请安装以下依赖包：
```bash
sudo apt install libxrandr-dev libxrender-dev libxinerama-dev libxcursor-dev libxi-dev libglvnd-dev libvulkan-dev cmake clang libc++-dev libglew-dev libglfw3-dev vulkan-validationlayers mesa-vulkan-drivers
```
- [NVIDIA 驱动](https://docs.nvidia.com/cuda/cuda-installation-guide-linux/index.html#runfile)（AMD/Intel 驱动已由 mesa-vulkan-drivers 自动安装）

## 构建指南

### Windows 构建
直接运行 **build_windows.bat**，会自动生成工程并编译 Release 配置，生成的 ZEditor.exe 位于 **bin** 目录。

也可以手动生成 Visual Studio 工程：
```powershell
cmake -S . -B build
```
然后用 Visual Studio 打开 build 目录下的解决方案进行编译。

### macOS 构建
> 仅支持 x86_64，M1 芯片支持后续发布。

确保已安装最新版 Xcode，然后在项目根目录运行：
```bash
cmake -S . -B build -G "Xcode"
cmake --build build --config Release
```
或直接执行 **build_macos.sh**。

### Ubuntu 构建
执行 **build_linux.sh** 即可自动编译。

### zbuild.py

`zbuild.py` 是上述步骤的统一入口，也是构建独立工具的唯一方式：

```bash
python zbuild.py configure --config release   # debug | release | relwithdebinfo
python zbuild.py build --target ZEditor
python zbuild.py tools                        # 列出独立 GUI 工具
python zbuild.py tool TexPreview              # 构建单个 -> bin/Tools/Release/
python zbuild.py tool --all                   # 构建全部四个
python zbuild.py test
python zbuild.py clean
```

### AI 冒烟测试
默认关闭。它不启动世界、物理与 RHI，只验证黑板与行为树语义：

```bash
cmake -B build -DZENGINE_BUILD_AI_SMOKE_TEST=ON
cmake --build build --target AISmokeTest
```

注意 `--config` 同时决定脚本后端：debug 用 QuickJS，release 和 relwithdebinfo 用 V8。

## 文档
详细文档请参见 Wiki。设计文档另见 [`doc/`](doc/)，其中较常用的有 `doc/BUILD_SYSTEM.md`、`doc/CODING_STYLE.md`、`doc/MULTI_API_RHI_GUIDE.md`。

## 常见问题

### Vulkan Validation Layer 报错
如遇到 Vulkan Validation Layer: Validation Error，请安装最新版 Vulkan SDK。

### 生成编译数据库
如需生成 `compile_commands.json`（供 clangd 等工具使用）：
```powershell
cmake -DCMAKE_TRY_COMPILE_TARGET_TYPE="STATIC_LIBRARY" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -S . -B compile_db_temp -G "Unix Makefiles"
copy compile_db_temp\compile_commands.json .
```

### 启用物理调试渲染器
仅支持 Windows，生成带调试器的解决方案：
```powershell
cmake -S . -B build -DENABLE_PHYSICS_DEBUG_RENDERER=ON
```
注意：
1. 重新生成前请清理 build 目录，避免 CMakeCache 导致的问题。
2. 启动 ZEditor 后物理调试渲染器会自动运行，初始相机模式可能不正确，滚动鼠标滚轮可切换到正确模式。

## 代码风格规范

> 📖 **详细文档**：完整命名规范请参考 [doc/CODING_STYLE.md](doc/CODING_STYLE.md)，该文档为准

### C++ 命名规范

- **命名空间**：PascalCase，简短（如 `Engine`、`Engine::Render`）
- **类名**：CamelCase（如 `WindowSystem`、`RenderConfig`）
- **接口/抽象类**：`I` 前缀 + CamelCase（如 `IRenderer`、`ISystem`）
- **函数名**：**CamelCase**，动词开头（如 `Initialize()`、`IsQuit()`）
- **成员变量**：`m_` 前缀 + camelCase（如 `m_window`、`m_isQuit`）
- **静态成员**：`s_` 前缀 + camelCase（如 `s_instance`、`s_count`）
- **全局变量**：`g_` 前缀 + camelCase（如 `g_runtimeContext`）
- **常量**：全大写 + 下划线（如 `MAX_BUFFER_SIZE`）
- **枚举**：类型 CamelCase，**枚举值 CamelCase**（如 `Debug`、`Forward`、`Deferred`）
- **参数**：snake_case，用 `in_` 前缀避免 shadow（如 `in_delta_time`、`in_create_info`）
- **局部变量**：snake_case（如 `delta_time`、`frame_count`）
- **文件名**：**PascalCase**，与其中声明的类型一致，头文件 `.h`、源文件 `.cpp`（如 `WindowSystem.h`）
- **宏定义**：全大写 + 下划线，项目相关加 `Z_` 前缀（如 `Z_ASSERT`、`Z_PLATFORM_WINDOWS`）
- **模板参数**：大写字母或有含义的 CamelCase（如 `T`、`KeyType`）
- **布尔变量**：is/has/can/should 前缀（如 `isVisible`、`hasChildren`）
- **私有方法**：遵循普通函数规则，可选下划线后缀（如 `calculateBounds_`）

注意：**函数是 CamelCase 而非 camelCase**，文件名是 PascalCase 而非小写下划线——本仓库里较早的几处摘要把这两条写反了。

#### 总体原则
1. 保持风格一致
2. 新代码遵循现有规范
3. 重大重构时统一命名
4. 以可读性和清晰性为首要

---

如需补充更多内容或有其他问题，欢迎提交 Issue 或 PR！
