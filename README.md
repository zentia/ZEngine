# ZEngine 

<p align="center">
  <a href="https://zentia.github.io">
    <img src="engine/resource/ZEngine.png" width="400" alt="Z Engine logo">
  </a>
</p>

**Z Engine** is a tiny game engine.

## Recent Updates (October 2026)

- **ZSlate**: the engine's UI framework now lives in its own submodule
  (`engine/Libraries/ZSlate`), and the editor has been migrated onto it — the
  ImGui backend is retired, with a few ImGui widget islands (scene grid, shader
  editor, preview controls, modals) still being converted.
- **Standalone GUI tools** under `engine/tools/`: `ZEnvironmentCollector`
  (Windows compatibility report + crash/log collection), `LuaMemPreview` (Lua
  memory snapshots, `-batchmode` CSV export), `TexPreview` (texture viewer),
  `ClientProjBranches` (branch / SVN / git manager). All four build to
  `bin/Tools/<Config>/`.
- **ZInsights**: a standalone `.ztrace` profiler viewer (Win32 + Direct2D),
  rendering the same timeline widget the in-editor panel paints.
- **ZAppShell**: header-only startup helpers — one command-line reader and one
  console attach, shared by every ZEngine executable regardless of its runtime
  library (`/MT` or `/MD`).
- **D3D12 backend** alongside Vulkan behind the multi-RHI abstraction; Metal on
  macOS.
- **agentkit**: Python kits for crash attribution (`zagent` / `zfault` / `zlab`),
  wired into CI behind an explicit platform gate.
- **RenderDoc integration**: in-engine capture module plus an MCP server and
  analysis scripts under `tools/`.

See [CHANGELOG.md](CHANGELOG.md) for detailed version history.

## Key Features

### Core Engine
- **Multi-platform Support**: Windows, Linux, and macOS (web via Emscripten)
- **Multi-RHI Renderer**: Vulkan and D3D12 backends behind one abstraction,
  Metal on macOS, with validation layers
- **Physics**: Integrated Jolt Physics engine
- **Audio**: Software mixer (voices, buses, master), WAV decode, 3D attenuation
  and panning, WASAPI output on Windows
- **Asset Pipeline**: Complete asset management and bundle system

### Scripting
- **TypeScript / JavaScript**: the supported scripting path, via Puerts
  (V8 on desktop, QuickJS on web/mobile) with `tsc --watch`, hot reload and
  Inspector-editable per-instance fields
- **Lua / C#**: not scripting backends. Lua survives as a loader stub and C#
  as memory-profiler hooks only; see `doc/TYPESCRIPT_SCRIPTING_DESIGN.md`

### Editor
- **Visual Editor**: ZSlate-based editor interface (see above — the ImGui
  backend has been retired, a few widget islands remain mid-conversion)
- **Console Window**: Built-in console for debugging
- **Project Management**: Asset browser and hierarchy viewer
- **Timeline Editor**: Animation and sequencing tools
- **Inspector**: Property editing and component management

### Developer Tools
- **Profiling**: Memory and performance profilers for all scripting languages
- **ZInsights**: Standalone flame-chart viewer for `.ztrace` captures
- **Serialization**: Binary and JSON serialization with reflection
- **Virtual File System**: Flexible file system abstraction
- **Hot Reload**: Asset and script hot reloading support

## Repository Layout

| Path | What |
|---|---|
| `engine/Source/Runtime/` | The runtime: `Core`, `Function` (Render, Physics, Audio, Animation, GAS, Input, ShaderLab…), `Resource`, `Scripting`, `Profiler`, `VirtualFileSystem`, `ZEngineApi` |
| `engine/Source/Editor/` | ZEditor — ZSlate windows, editor UI, property drawers, docking layout |
| `engine/Source/Insights/` | ZInsights — standalone `.ztrace` viewer (Windows + Direct2D) |
| `engine/Source/Modules/` | Optional engine modules (currently RenderDoc) |
| `engine/Source/WebLauncher/` | Emscripten entry point + `shell.html` |
| `engine/Libraries/ZSlate/` | ZSlate UI framework — **git submodule** |
| `engine/Libraries/ZAppShell/` | Header-only startup helpers (wide command line, console attach) |
| `engine/tools/` | The four standalone GUI tools (see below) |
| `engine/3rdparty/` | Third-party sources |
| `tools/` | RenderDoc integration, its MCP server, and capture analysis scripts |
| `agentkit/` | Python kits for crash attribution: `zagent`, `zfault`, `zlab` |
| `doc/` | Design documents and guides |
| `zbuild.py` | Unified build driver (configure / build / tool / test / install) |

## Standalone Tools

Built by `zbuild.py tool <name>` (or `--all`), output to `bin/Tools/<Config>/`.

| Tool | Executable | What |
|---|---|---|
| WindowsEnvironmentCheck | `ZEnvironmentCollector.exe` | Windows compatibility report: hardware/firmware/driver probe, consented crash capture, game-log collection, ZIP64 report. GUI, or `-batchmode` for CI |
| LuaMemPreview | `LuaMemPreview.exe` | Viewer for Lua memory snapshots (`.zasset` or CSV); `-batchmode` exports CSV for one snapshot or a whole folder |
| TexPreview | `TexPreview.exe` | Texture viewer |
| ClientProjBranches | `ClientProjBranches.exe` | Branch / SVN / git manager |

`ZEditor.exe` and `ZInsights.exe` are built with the main tree, not by
`zbuild.py tool`.

## Continuous build status

|    Build Type     |                                                                                      Status                                                                                      |
| :---------------: | :------------------------------------------------------------------------------------------------------------------------------------------------------------------------------: |
| **Build Windows** | [![Build Windows](https://github.com/zentia/ZEngine/actions/workflows/build_windows.yml/badge.svg)](https://github.com/zentia/ZEngine/workflows/build_windows.yml) |
|  **Build Linux**  |    [![Build Linux](https://github.com/zentia/ZEngine/actions/workflows/build_linux.yml/badge.svg)](https://github.com/zentia/ZEngine/workflows/build_linux.yml)    |
|  **Build macOS**  |    [![Build macOS](https://github.com/zentia/ZEngine/actions/workflows/build_macos.yml/badge.svg)](https://github.com/zentia/ZEngine/workflows/build_macos.yml)    |

## Prerequisites

To build Z, you must first install the following tools.

### Windows 10/11
- Visual Studio 2022 (or more recent)
- CMake 3.19 (or more recent)
- Git 2.1 (or more recent)

### macOS >= 10.15 (x86_64)
- Xcode 12.3 (or more recent)
- CMake 3.19 (or more recent)
- Git 2.1 (or more recent)

### Ubuntu 20.04
 - apt install the following packages
```
sudo apt install libxrandr-dev
sudo apt install libxrender-dev
sudo apt install libxinerama-dev
sudo apt install libxcursor-dev
sudo apt install libxi-dev
sudo apt install libglvnd-dev
sudo apt install libvulkan-dev
sudo apt install cmake
sudo apt install clang
sudo apt install libc++-dev
sudo apt install libglew-dev
sudo apt install libglfw3-dev
sudo apt install vulkan-validationlayers
sudo apt install mesa-vulkan-drivers
```
- [NVIDIA driver](https://docs.nvidia.com/cuda/cuda-installation-guide-linux/index.html#runfile) (The AMD and Intel driver is open-source, and thus is installed automatically by mesa-vulkan-drivers)

## Build ZEngine

### Build on Windows
You may execute the **build_windows.bat**. This batch file will generate the projects, and build the **Release** config of **ZEngine** automatically. After successful build, you can find the ZEditor.exe at the **bin** directory.

Or you can use the following command to generate the **Visual Studio** project firstly, then open the solution in the build directory and build it manually.
```
cmake -S . -B build
```

### Build on macOS

> The following build instructions only tested on specific hardware of x86_64, and do not support M1 chips. For M1 compatible, we will release later.

To compile Z Engine, you must have the most recent version of Xcode installed.
Then run 'cmake' from the project's root directory, to generate a project of Xcode.

```
cmake -S . -B build -G "Xcode"
```
and you can build the project with
```
cmake --build build --config Release
```

Or you can execute the **build_macos.sh** to build the binaries.

### Build on Ubuntu 20.04
You can execute the **build_linux.sh** to build the binaries.

### zbuild.py

`zbuild.py` is a unified driver wrapping the steps above, and the only way to
build the standalone tools:

```
python zbuild.py configure --config release   # debug | release | relwithdebinfo
python zbuild.py build --target ZEditor
python zbuild.py tools                        # list the standalone GUI tools
python zbuild.py tool TexPreview              # one -> bin/Tools/Release/
python zbuild.py tool --all                   # all four
python zbuild.py test
python zbuild.py clean
```

Note that `--config` also selects the scripting backend: debug builds QuickJS,
release and relwithdebinfo build V8.

## Documentation
For documentation, please refer to the Wiki section. Design documents also live
under [`doc/`](doc/) — notably `doc/BUILD_SYSTEM.md`, `doc/CODING_STYLE.md`, and
`doc/MULTI_API_RHI_GUIDE.md`.

## Extra

### Vulkan Validation Layer: Validation Error
We have noticed some developers on Windows encounted ZEditor.exe could run normally but reported an exception Vulkan Validation Layer: Validation Error
when debugging. You can solve this problem by installing Vulkan SDK (official newest version will do).

### Generate Compilation Database

You can build `compile_commands.json` with the following commands when `Unix Makefiles` generaters are avaliable. `compile_commands.json` is the file
required by `clangd` language server, which is a backend for cpp lsp-mode in Emacs.

For Windows:

``` powershell
cmake -DCMAKE_TRY_COMPILE_TARGET_TYPE="STATIC_LIBRARY" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -S . -B compile_db_temp -G "Unix Makefiles"
copy compile_db_temp\compile_commands.json .
```

### Using Physics Debug Renderer
Currently Physics Debug Renderer is only available on Windows. You can use the following command to generate the solution with the debugger project.

``` powershell
cmake -S . -B build -DENABLE_PHYSICS_DEBUG_RENDERER=ON
```

Note:
1. Please clean the build directory before regenerating the solution. We've encountered building problems in regenerating directly with previous CMakeCache.
2. Physics Debug Renderer will run when you start ZEditor. We've synced the camera position between both scenes. But the initial camera mode in Physics Debug Renderer is wrong. Scrolling down the mouse wheel once will change the camera of Physics Debug Renderer to the correct mode.

## Coding Style Guide

> 📖 **详细文档**: 完整的命名规范请参考 [doc/CODING_STYLE.md](doc/CODING_STYLE.md)

The summary below must match that document — it is the authority. Note that the
engine uses **CamelCase for functions**, not camelCase; several older summaries
in this repository got that backwards.

#### Namespaces
- PascalCase, keep names short
- Example: `namespace Engine`, `namespace Engine::Render`

#### Classes and Structs
- CamelCase, nouns or noun phrases
- Example: `class WindowSystem`, `class RenderConfig`

#### Functions and Methods
- **CamelCase**, begin with verbs or verb phrases
- Example: `Initialize()`, `IsQuit()`, `CreateObject()`

#### Member Variables
- `m_` prefix followed by **camelCase**
- Example: `m_window`, `m_isQuit`

#### Static Members
- `s_` prefix followed by camelCase
- Example: `s_instance`, `s_count`

#### Global Variables
- `g_` prefix followed by camelCase
- Example: `g_runtimeContext`

#### Parameters
- snake_case; use an `in_` prefix to avoid shadowing
- Example: `float in_delta_time`, `WindowCreateInfo in_create_info`

#### Local Variables
- snake_case
- Example: `float delta_time`, `int frame_count`

#### Constants
- All uppercase with underscores
- Example: `MAX_BUFFER_SIZE`, `DEFAULT_FPS`

#### Enums
- Type names CamelCase, **values CamelCase**
- Example:
```cpp
enum class LogLevel {
    Debug,
    Info,
    Warn,
    Error,
    Fatal
};
```

#### Interface Classes
- `I` prefix followed by CamelCase
- Example: `class IRenderer`, `class ISystem`

#### File Names
- **PascalCase**, matching the type they declare
- Headers use .h extension, source files use .cpp
- Example: `WindowSystem.h`, `RenderSystem.cpp`

#### Macros
- All uppercase with underscores, `Z_` prefix for project-specific ones
- Example: `Z_ASSERT`, `Z_PLATFORM_WINDOWS`

#### Template Parameters
- Simple capital letters, or meaningful CamelCase names
- Example:
```cpp
template<typename T>
template<typename KeyType, typename ValueType>
```

#### Boolean Variables
- Prefix with is, has, can, should, etc.
- Example: `isVisible`, `hasChildren`, `canUpdate`

### General Principles
1. Maintain consistency throughout the project
2. New code should follow existing conventions
3. Refactor inconsistent naming during major updates
4. Prioritize clarity and readability over brevity
