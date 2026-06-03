# 确保 MemoryManager 的 operator new/delete 优先于标准库

## 问题
当自定义 `operator new`/`operator delete` 与标准库同时存在时，链接器可能选择标准库版本，导致 allocator 混用（如 CRT malloc + mimalloc free）引发崩溃。

## 方案一：使用 mimalloc 官方头文件（推荐）

mimalloc 提供 `mimalloc-new-delete.h`，在**单一源文件**中 include 即可全局覆盖。

在 `MemoryManager.cpp` 顶部添加：
```cpp
#include <mimalloc-new-delete.h>  // 必须在仅此一个 .cpp 中包含
```

并移除 MemoryManager.cpp 中自定义的 `operator new`/`operator new[]`/`operator delete`/`operator delete[]`（保留 EASTL 的 placement new 重载）。

优势：官方实现，覆盖完整（含 aligned、nothrow 等重载），无链接顺序问题。

---

## 方案二：Object Library 强制优先链接

将 MemoryManager 单独做成 object library，作为**第一个**链接依赖：

```cmake
# 在 engine/Source/Runtime/CMakeLists.txt 中
add_library(MemoryManagerOverrides OBJECT
    Core/Memory/MemoryManager.cpp
)
target_include_directories(MemoryManagerOverrides PRIVATE ${ENGINE_ROOT_DIR}/Source/Runtime)
target_link_libraries(MemoryManagerOverrides PRIVATE mimalloc-static)
# 从 ZRuntime 的 SOURCE_FILES 中排除 MemoryManager.cpp，或创建新 target

# 在 Editor CMakeLists.txt 中，将 object 放在最前面：
target_link_libraries(${TARGET_NAME} PRIVATE 
    $<TARGET_OBJECTS:MemoryManagerOverrides>  # 第一
    ZRuntime
    # ... 其他
)
```

Object 文件会被完整链接，其符号会先于静态库中的符号被解析。

---

## 方案三：MSVC /WHOLEARCHIVE 强制包含

确保 ZRuntime 中的所有 .obj（含 MemoryManager）都被链接进去：

```cmake
if(MSVC)
    target_link_options(${TARGET_NAME} PRIVATE 
        "/WHOLEARCHIVE:$<TARGET_FILE:ZRuntime>"
    )
endif()
```

注意：可能导致重复符号（若 ZRuntime 已通过 target_link_libraries 链接）。通常只需保证 ZRuntime 在 target_link_libraries 中**排在前面**即可。

---

## 方案四：调整 target_link_libraries 顺序

链接器按从左到右顺序解析，**先出现的库优先**。确保 ZRuntime（含 MemoryManager）在标准库之前：

```cmake
# ZRuntime 已包含 mimalloc-static，确保放在较前位置
target_link_libraries(${TARGET_NAME} PUBLIC 
    ZRuntime      # 放前面
    stb
    glfw
    # ...
)
```

MSVC 默认会追加 libcmt.lib 等，通常在用户库之后，理论上 ZRuntime 会先被搜索。若仍有问题，可尝试方案一或二。

---

## 方案五：显式排除标准库的 operator（高级）

```cmake
if(MSVC)
    # 排除包含 operator new 的标准库（需根据实际库名调整）
    target_link_options(${TARGET_NAME} PRIVATE /NODEFAULTLIB:libcpmt.lib)
    target_link_libraries(${TARGET_NAME} PRIVATE libcmt.lib)  # 或保留其他需要的
endif()
```

风险较高，可能破坏 C++ 运行时，需谨慎测试。
