# EASTL 快速开始指南

## ✅ 已完成的集成工作

### 1. CMake 集成
- ✅ 在 `engine/3rdparty/CMakeLists.txt` 中添加了 EASTL 子目录配置
- ✅ 在 `engine/source/runtime/CMakeLists.txt` 中链接了 EASTL 库

### 2. 配置文件
- ✅ 创建了 `engine/3rdparty/EASTL/EASTLUserConfig.h`
- ✅ 配置为使用 mimalloc 作为内存分配器
- ✅ 设置了断言、异常、RTTI 等选项

## 📋 下一步操作（必须执行）

### 步骤1：下载 EASTL 源代码

选择以下方法之一：

**方法A：Git Submodule（推荐）**
```bash
cd engine/3rdparty
git submodule add https://github.com/electronicarts/EASTL.git
git submodule update --init --recursive
```

**方法B：直接克隆**
```bash
cd engine/3rdparty
git clone https://github.com/electronicarts/EASTL.git
```

### 步骤2：验证目录结构

确保目录结构如下：
```
engine/3rdparty/
├── EASTL/
│   ├── include/           # EASTL 头文件
│   ├── source/            # EASTL 源文件
│   ├── CMakeLists.txt     # EASTL 的 CMake 文件
│   └── EASTLUserConfig.h  # 我们的配置文件（已创建）
└── CMakeLists.txt         # 已更新
```

### 步骤3：重新配置 CMake

```bash
# Windows
cd build
cmake ..

# Linux/Mac
cmake -B build
```

### 步骤4：编译测试

```bash
# Windows
cmake --build build

# Linux/Mac  
cmake --build build
```

### 步骤5：验证集成

在代码中添加测试：

```cpp
#include <EASTL/vector.h>
#include <EASTL/string.h>

void test_eastl()
{
    eastl::vector<int> vec;
    vec.push_back(42);
    
    eastl::string str = "EASTL works!";
}
```

## 🚀 开始使用

### 基本替换

```cpp
// 旧代码
#include <vector>
#include <string>
#include <unordered_map>

std::vector<int> vec;
std::string str;
std::unordered_map<std::string, int> map;

// 新代码
#include <EASTL/vector.h>
#include <EASTL/string.h>
#include <EASTL/hash_map.h>

eastl::vector<int> vec;
eastl::string str;
eastl::hash_map<eastl::string, int> map;
```

### 示例：替换当前文件

可以尝试在 `debug_draw_font.cpp` 中替换：

```cpp
// 原来
#include <vector>
std::vector<float> imageData(m_bitmap_w * m_bitmap_h);

// 改为
#include <EASTL/vector.h>
eastl::vector<float> imageData(m_bitmap_w * m_bitmap_h);
```

## 📚 更多信息

- 详细文档：参见 `EASTL_INTEGRATION_GUIDE.md`
- 性能对比：参见 `GAME_CONTAINER_LIBRARIES.md`

## ⚠️ 注意事项

1. **EASTL 可能需要 EAStdC**：某些 EASTL 功能可能需要 EAStdC 库。如果遇到编译错误，可能需要：
   - 下载 EAStdC 并添加到项目
   - 或使用 EASTL 的独立版本
   - 或禁用需要 EAStdC 的功能

2. **CMake 配置可能需要调整**：如果 EASTL 的 CMakeLists.txt 需要特殊配置，可能需要调整 `engine/3rdparty/CMakeLists.txt` 中的设置。

3. **类型不兼容**：EASTL 容器与 STL 容器不能直接混用，需要统一使用 EASTL。

## 🔧 故障排除

### 问题：找不到 EASTL 头文件
**解决**：确保 EASTL 源代码在正确位置，重新运行 CMake 配置。

### 问题：链接错误
**解决**：检查 `engine/source/runtime/CMakeLists.txt` 中是否已添加：
```cmake
target_link_libraries(${TARGET_NAME} PUBLIC EASTL)
```

### 问题：编译错误（缺少 EAStdC）
**解决**：可以从 [EAStdC GitHub](https://github.com/electronicarts/EAStdC) 下载并集成，或使用不需要 EAStdC 的 EASTL 版本。

