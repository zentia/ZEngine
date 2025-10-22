# ZEngine 

<p align="center">
  <a href="https://zentia.github.io">
    <img src="engine/source/editor/resource/ZentiaEngine.png" width="400" alt="Z Engine logo">
  </a>
</p>

**Z Engine** is a tiny game engine.

## Continuous build status

|    Build Type     |                                                                                      Status                                                                                      |
| :---------------: | :------------------------------------------------------------------------------------------------------------------------------------------------------------------------------: |
| **Build Windows** | [![Build Windows](https://github.com/zentia/ZEngine/actions/workflows/build_windows.yml/badge.svg)](https://github.com/zentia/ZEngine/workflows/build_windows.yml) |
|  **Build Linux**  |    [![Build Linux](https://github.com/zentia/ZEngine/actions/workflows/build_linux.yml/badge.svg)](https://github.com/zentia/ZEngine/actions/workflows/build_linux.yml)    |
|  **Build macOS**  |    [![Build macOS](https://github.com/zentia/ZEngine/actions/workflows/build_macos.yml/badge.svg)](https://github.com/zentia/ZEngine/actions/workflows/build_macos.yml)    |

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

## Build MiniUnrealEngine

### Build on Windows
You may execute the **build_windows.bat**. This batch file will generate the projects, and build the **Release** config of **Zentia Engine** automatically. After successful build, you can find the ZentiaEditor.exe at the **bin** directory.

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

## Documentation
For documentation, please refer to the Wiki section.

## Extra

### Vulkan Validation Layer: Validation Error
We have noticed some developers on Windows encounted ZentiaEditor.exe could run normally but reported an exception Vulkan Validation Layer: Validation Error
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
2. Physics Debug Renderer will run when you start ZentiaEditor. We've synced the camera position between both scenes. But the initial camera mode in Physics Debug Renderer is wrong. Scrolling down the mouse wheel once will change the camera of Physics Debug Renderer to the correct mode.

## Coding Style Guide

### C++ Naming Conventions

#### Namespaces
- Use uppercase initials, keep names short
- Example: `namespace Z`

#### Classes
- Use PascalCase (capitalize first letter of each word)
- Use nouns or noun phrases
- Example: `class MemoryManager`, `class RuntimeGlobalContext`

#### Functions
- Use camelCase (first word lowercase, capitalize first letter of subsequent words)
- Begin with verbs or verb phrases
- Example: `initialize()`, `createObject()`, `destroyObject()`

#### Member Variables
- Prefix with `m_` followed by camelCase
- Example: `m_windowSystem`, `m_configManager`

#### Static Members
- Prefix with `s_` followed by camelCase
- Example: `s_instance`

#### Constants
- Use all uppercase with underscores
- Example: `MAX_BUFFER_SIZE`, `DEFAULT_WINDOW_WIDTH`

#### Enums
- Enum type names use PascalCase
- Enum values use UPPER_CASE
- Example:
```cpp
enum class RenderType {
    FORWARD,
    DEFERRED,
    HYBRID
};
```

#### Parameters and Local Variables
- Use camelCase
- Be descriptive but concise
- Example: `float deltaTime`, `int frameCount`

#### Interface Classes
- Prefix with 'I' followed by PascalCase
- Example: `class IRenderer`, `class ISystem`

#### File Names
- Use lowercase with underscores
- Headers use .h extension
- Source files use .cpp extension
- Example: `memory_manager.h`, `render_system.cpp`

#### Macros
- Use all uppercase with underscores
- Add project prefix for project-specific macros
- Example: `Z_ASSERT`, `Z_ENABLE_LOGGING`

#### Template Parameters
- Use simple capital letters or meaningful PascalCase names
- Example:
```cpp
template<typename T>
template<typename KeyType, typename ValueType>
```

#### Boolean Variables
- Prefix with is, has, can, should, etc.
- Example: `isVisible`, `hasChildren`, `canUpdate`

#### Private Methods
- Follow normal function naming rules
- Optionally use underscore suffix for private
- Example: `calculateBounds_`, `updateInternal_`

### General Principles
1. Maintain consistency throughout the project
2. New code should follow existing conventions
3. Refactor inconsistent naming during major updates
4. Prioritize clarity and readability over brevity
