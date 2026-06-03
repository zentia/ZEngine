# ZEngine 平台宏使用指南

## 概述

ZEngine 提供了完整的跨平台宏系统，用于在编译时检测当前平台、架构和编译器。这些宏在 CMake 配置阶段自动定义，确保在所有支持的平台上都能正确工作。

## 宏定义位置

- **CMake 定义**：`CMakeLists.txt` (根目录)
- **C++ 头文件**：`engine/source/runtime/core/base/platform.h`

## 平台检测宏

### 平台标识符

| 宏名 | 值 | 说明 |
|------|-----|------|
| `Z_PLATFORM_WINDOWS` | 1 | Windows 平台 |
| `Z_PLATFORM_ANDROID` | 1 | Android 平台 |
| `Z_PLATFORM_MACOS` | 1 | macOS 平台 |
| `Z_PLATFORM_LINUX` | 1 | Linux 平台 |
| `Z_PLATFORM_IOS` | 1 | iOS 平台 |

### 架构标识符

| 宏名 | 值 | 说明 |
|------|-----|------|
| `Z_ARCH_X64` | 1 | x64 架构 |
| `Z_ARCH_X86` | 1 | x86 架构 |
| `Z_ARCH_ARM64` | 1 | ARM64 架构 |
| `Z_ARCH_ARM32` | 1 | ARM32 架构 |

### 编译器标识符

| 宏名 | 值 | 说明 |
|------|-----|------|
| `Z_COMPILER_MSVC` | 1 | Microsoft Visual C++ |
| `Z_COMPILER_GCC` | 1 | GNU Compiler Collection |
| `Z_COMPILER_CLANG` | 1 | Clang |
| `Z_COMPILER_APPLE_CLANG` | 1 | Apple Clang |

## 使用方式

### 1. 包含头文件

```cpp
#include "engine/source/runtime/core/base/platform.h"
```

### 2. 平台检测

```cpp
// 使用宏进行条件编译
#ifdef Z_PLATFORM_WINDOWS
    // Windows 特定代码
    #include <windows.h>
#elif defined(Z_PLATFORM_ANDROID)
    // Android 特定代码
    #include <android/log.h>
#endif

// 使用函数进行运行时检测
if (Z_IS_WINDOWS()) {
    // Windows 特定逻辑
}
else if (Z_IS_ANDROID()) {
    // Android 特定逻辑
}
```

### 3. 架构检测

```cpp
#ifdef Z_ARCH_X64
    // 64位特定代码
    typedef long long int64_t;
#elif defined(Z_ARCH_X86)
    // 32位特定代码
    typedef long int32_t;
#endif
```

### 4. 编译器检测

```cpp
#ifdef Z_COMPILER_MSVC
    // MSVC 特定代码
    #pragma warning(disable: 4996)
#elif defined(Z_COMPILER_GCC)
    // GCC 特定代码
    #pragma GCC diagnostic ignored "-Wunused-variable"
#endif
```

## 便利函数

### 运行时检测函数

```cpp
// 平台检测
bool Z_IS_WINDOWS();
bool Z_IS_ANDROID();
bool Z_IS_MACOS();
bool Z_IS_LINUX();
bool Z_IS_IOS();

// 架构检测
bool Z_IS_X64();
bool Z_IS_X86();
bool Z_IS_ARM64();
bool Z_IS_ARM32();

// 编译器检测
bool Z_IS_MSVC();
bool Z_IS_GCC();
bool Z_IS_CLANG();
bool Z_IS_APPLE_CLANG();
```

### 信息获取函数

```cpp
namespace Z::Platform {
    constexpr const char* GetPlatformName();    // 返回平台名称
    constexpr const char* GetArchitectureName(); // 返回架构名称
    constexpr const char* GetCompilerName();    // 返回编译器名称
}
```

## 平台特定常量

```cpp
// 路径分隔符
char Z_PATH_SEPARATOR;           // '\\' (Windows) 或 '/' (Unix)
const char* Z_PATH_SEPARATOR_STR; // "\\" (Windows) 或 "/" (Unix)
```

## 实际使用示例

### 文件路径处理

```cpp
#include "platform.h"
#include <string>

std::string BuildPath(const std::string& dir, const std::string& file) {
    return dir + Z_PATH_SEPARATOR_STR + file;
}
```

### 平台特定 API 调用

```cpp
#include "platform.h"

void ShowMessage(const std::string& message) {
#ifdef Z_PLATFORM_WINDOWS
    MessageBoxA(NULL, message.c_str(), "ZEngine", MB_OK);
#elif defined(Z_PLATFORM_ANDROID)
    __android_log_write(ANDROID_LOG_INFO, "ZEngine", message.c_str());
#elif defined(Z_PLATFORM_MACOS) || defined(Z_PLATFORM_LINUX)
    std::cout << message << std::endl;
#endif
}
```

### 内存对齐

```cpp
#include "platform.h"

#ifdef Z_ARCH_X64
    constexpr size_t CACHE_LINE_SIZE = 64;
#elif defined(Z_ARCH_X86)
    constexpr size_t CACHE_LINE_SIZE = 32;
#else
    constexpr size_t CACHE_LINE_SIZE = 64; // 默认值
#endif
```

## 最佳实践

1. **优先使用编译时检测**：使用 `#ifdef` 而不是运行时 `if` 语句
2. **统一命名规范**：所有宏都以 `Z_` 开头
3. **提供默认值**：为不支持的平台提供合理的默认行为
4. **文档化**：在代码中注释平台特定的逻辑
5. **测试**：在所有支持的平台上测试代码

## 添加新平台

要添加新平台支持，需要修改：

1. `CMakeLists.txt` 中的平台检测逻辑
2. `platform.h` 中的宏定义
3. 添加相应的测试用例

## 注意事项

- 这些宏在 CMake 配置阶段定义，确保在编译时可用
- 所有宏都有明确的数值（1），便于条件判断
- 提供了多种检测方式，适应不同的使用场景
- 与标准库的宏（如 `WIN32`、`__linux__`）兼容但不依赖
