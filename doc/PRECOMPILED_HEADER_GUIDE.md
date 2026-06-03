# ZEngine 预编译头文件使用指南

## 📋 概述

预编译头文件（Precompiled Headers, PCH）可以显著加快编译速度，特别是对于包含大量头文件的大型项目。ZEngine 已为 `ZRuntime` 和 `ZEditor` 配置了预编译头支持。

## 🎯 工作原理

预编译头文件将常用的头文件预先编译并缓存，后续编译时直接使用缓存结果，避免重复编译相同的头文件。

## 📁 文件结构

```
engine/source/runtime/
├── pch.h          # 预编译头文件（包含常用头文件）
└── pch.cpp        # 预编译头源文件（用于编译 pch.h）

engine/source/editor/
├── pch.h          # 编辑器专用预编译头文件
└── pch.cpp        # 编辑器专用预编译头源文件
```

## 🔧 配置说明

### CMakeLists.txt 配置

预编译头文件已在 `CMakeLists.txt` 中配置：

```cmake
# 配置预编译头
if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.16")
    target_sources(${TARGET_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/pch.cpp")
    target_precompile_headers(${TARGET_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/pch.h")
endif()
```

## 📝 使用方式

### 自动使用（推荐）

CMake 3.16+ 的 `target_precompile_headers` 会自动处理预编译头，**你不需要在源代码中手动包含 `pch.h`**。编译器会自动在每个 `.cpp` 文件开头注入预编译头。

### 手动使用（不推荐）

如果你需要在某些特定情况下手动包含，可以这样做：

```cpp
// 在 .cpp 文件的第一行
#include "pch.h"  // 或 #include "runtime/pch.h"

// 然后是你的其他头文件
#include "your_header.h"
```

**注意**：手动包含可能导致重复包含，建议使用自动方式。

## ✏️ 自定义预编译头文件

### 添加常用头文件

编辑 `pch.h`，添加你项目中经常使用的头文件：

```cpp
#pragma once

// 标准库
#include <memory>
#include <string>
// ... 其他常用头文件

// EASTL
#include "EASTL/string.h"
#include "EASTL/shared_ptr.h"
// ... 其他常用 EASTL 头文件

// 引擎核心
#include "runtime/export_runtime.h"
#include "runtime/core/base/macro.h"
// ... 其他常用引擎头文件
```

### 添加原则

1. ✅ **应该包含**：
   - 频繁使用的标准库头文件（如 `<memory>`, `<string>`, `<vector>`）
   - 几乎所有源文件都使用的第三方库头文件（如 EASTL）
   - 项目核心头文件（如 `global_context.h`）

2. ❌ **不应该包含**：
   - 只在少数文件中使用的头文件
   - 经常变化的头文件
   - 可能导致命名冲突的头文件
   - 包含实现细节的头文件（如 `.inl` 文件）

## 🚀 性能优化建议

### 1. 选择合适的头文件

只包含真正频繁使用的头文件。过多的头文件会：
- 增加预编译头文件大小
- 延长首次编译时间
- 降低编译缓存效率

### 2. 避免频繁修改 pch.h

每次修改 `pch.h` 都会导致所有源文件重新编译。建议：
- 在项目初期确定常用头文件
- 后续谨慎添加新的头文件

### 3. 使用条件编译

对于平台特定的头文件，可以使用条件编译：

```cpp
#ifdef Z_PLATFORM_WINDOWS
#include <windows.h>
#endif

#ifdef Z_PLATFORM_LINUX
#include <unistd.h>
#endif
```

## 🔍 验证预编译头是否生效

### MSVC

在 Visual Studio 中：
1. 打开项目属性
2. 查看 C/C++ → 预编译头
3. 应该看到 "使用预编译头 (/Yu)"

### 编译输出

启用预编译头后，编译日志中应该看到：
- MSVC: `/Yu"pch.h"` 标志
- GCC/Clang: 使用预编译头文件的提示

### 性能测试

对比启用/禁用预编译头的编译时间：
- 首次编译：预编译头可能稍慢（需要编译 PCH）
- 增量编译：预编译头应该明显更快

## ⚠️ 注意事项

1. **CMake 版本要求**：需要 CMake 3.16 或更高版本
2. **编译器支持**：
   - MSVC：完全支持
   - GCC：支持（需要 GCC 3.4+）
   - Clang：支持
3. **不要手动管理**：让 CMake 自动处理，不要手动设置编译选项
4. **不要包含实现**：`pch.h` 应该只包含头文件，不包含实现代码

## 🐛 常见问题

### Q: 编译时出现 "找不到 pch.h" 错误

A: 检查：
1. `pch.h` 文件是否存在
2. CMake 配置是否正确
3. 包含路径是否正确

### Q: 编译速度没有明显提升

A: 可能原因：
1. 预编译头文件包含的头文件不够常用
2. 项目规模较小，预编译头优势不明显
3. 磁盘 I/O 成为瓶颈（使用 SSD 可以改善）

### Q: 修改 pch.h 后所有文件都重新编译

A: 这是正常的。预编译头文件改变会影响所有依赖它的源文件。建议：
- 谨慎修改 `pch.h`
- 在项目稳定后再添加新头文件

## 📚 参考资料

- [CMake target_precompile_headers 文档](https://cmake.org/cmake/help/latest/command/target_precompile_headers.html)
- [MSVC 预编译头文件](https://docs.microsoft.com/en-us/cpp/build/creating-precompiled-header-files)
- [GCC 预编译头文件](https://gcc.gnu.org/onlinedocs/gcc/Precompiled-Headers.html)

