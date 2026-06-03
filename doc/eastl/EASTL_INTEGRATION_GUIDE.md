# EASTL 集成指南

## 概述

EASTL (Electronic Arts STL) 已经集成到项目中，并配置为使用 mimalloc 作为内存分配器。

## 目录结构

```
engine/3rdparty/
├── EASTL/                      # EASTL 源代码（需要从 GitHub 获取）
│   └── EASTLUserConfig.h       # EASTL 用户配置文件（已创建）
└── CMakeLists.txt              # 已添加 EASTL 集成配置
```

## 安装 EASTL

在集成完成后，需要将 EASTL 源代码下载到 `engine/3rdparty/EASTL/` 目录：

### 方法1：使用 Git Submodule（推荐）

```bash
cd engine/3rdparty
git submodule add https://github.com/electronicarts/EASTL.git
git submodule update --init --recursive
```

### 方法2：直接克隆

```bash
cd engine/3rdparty
git clone https://github.com/electronicarts/EASTL.git
```

### 方法3：手动下载

从 [EASTL GitHub Releases](https://github.com/electronicarts/EASTL/releases) 下载最新版本，解压到 `engine/3rdparty/EASTL/` 目录。

## 配置说明

### EASTLUserConfig.h

已创建的配置文件位于 `engine/3rdparty/EASTL/EASTLUserConfig.h`，主要配置包括：

- **内存分配器**：使用 mimalloc (`mi_malloc`, `mi_free`, `mi_realloc`)
- **断言**：使用标准 `assert`（Debug 模式）
- **异常**：已禁用异常（`EASTL_EXCEPTIONS_ENABLED 0`）
- **RTTI**：已启用（`EASTL_RTTI_ENABLED 1`）
- **命名空间**：使用 `eastl` 命名空间

## 使用方法

### 基本使用

EASTL 的 API 与标准库高度兼容，主要区别是命名空间：

```cpp
// 标准库
#include <vector>
#include <unordered_map>
#include <string>

std::vector<int> vec;
std::unordered_map<std::string, int> map;
std::string str;

// EASTL 替换
#include <EASTL/vector.h>
#include <EASTL/hash_map.h>
#include <EASTL/string.h>

eastl::vector<int> vec;
eastl::hash_map<eastl::string, int> map;
eastl::string str;
```

### 常用容器对应表

| 标准库 | EASTL | 说明 |
|--------|-------|------|
| `std::vector` | `eastl::vector` | 动态数组 |
| `std::string` | `eastl::string` | 字符串 |
| `std::unordered_map` | `eastl::hash_map` | 哈希映射 |
| `std::unordered_set` | `eastl::hash_set` | 哈希集合 |
| `std::map` | `eastl::map` | 有序映射 |
| `std::set` | `eastl::set` | 有序集合 |
| `std::deque` | `eastl::deque` | 双端队列 |
| `std::list` | `eastl::list` | 双向链表 |
| `std::array` | `eastl::array` | 固定大小数组 |
| `std::pair` | `eastl::pair` | 键值对 |
| `std::tuple` | `eastl::tuple` | 元组 |

### EASTL 特有容器

EASTL 提供了一些标准库没有的容器：

```cpp
#include <EASTL/fixed_vector.h>
#include <EASTL/fixed_string.h>
#include <EASTL/intrusive_list.h>

// 固定大小容器（栈分配，减少堆分配）
eastl::fixed_vector<int, 32> fixedVec;  // 最多32个元素在栈上

// 侵入式链表（节点包含链表指针，避免额外分配）
eastl::intrusive_list<MyNode> list;
```

### 使用示例

#### 示例1：基本容器操作

```cpp
#include <EASTL/vector.h>
#include <EASTL/hash_map.h>
#include <EASTL/string.h>

void example_basic_containers()
{
    // Vector
    eastl::vector<int> vec;
    vec.push_back(1);
    vec.push_back(2);
    vec.push_back(3);
    
    for (size_t i = 0; i < vec.size(); ++i)
    {
        // 访问元素
    }
    
    // Hash Map
    eastl::hash_map<eastl::string, int> map;
    map["key1"] = 100;
    map["key2"] = 200;
    
    auto it = map.find("key1");
    if (it != map.end())
    {
        // 找到元素
    }
    
    // String
    eastl::string str = "Hello EASTL";
    str += "!";
}
```

#### 示例2：固定大小容器

```cpp
#include <EASTL/fixed_vector.h>
#include <EASTL/fixed_string.h>

void example_fixed_containers()
{
    // 固定大小 vector（最多64个元素在栈上）
    eastl::fixed_vector<int, 64> fixedVec;
    fixedVec.push_back(42);
    // 如果超过64个元素，会自动切换到堆分配
    
    // 固定大小 string（最多31个字符在栈上）
    eastl::fixed_string<char, 32> fixedStr;
    fixedStr = "Hello";
}
```

#### 示例3：替换现有代码

```cpp
// 旧代码（使用 std::vector）
#include <vector>
std::vector<float> imageData(m_bitmap_w * m_bitmap_h);

// 新代码（使用 eastl::vector）
#include <EASTL/vector.h>
eastl::vector<float> imageData(m_bitmap_w * m_bitmap_h);
```

## 性能优势

EASTL 相比标准库的主要优势：

1. **更快的性能**：通常比 STL 快 10-20%
2. **更好的缓存局部性**：优化的内存布局
3. **自定义分配器支持**：已配置为使用 mimalloc
4. **固定大小容器**：减少堆分配
5. **更少的模板实例化**：减少编译时间

## 迁移策略

### 渐进式迁移

建议采用渐进式迁移策略：

1. **阶段1**：新代码直接使用 EASTL
2. **阶段2**：识别性能关键路径，替换为 EASTL
3. **阶段3**：逐步替换其他代码

### 迁移检查清单

- [ ] 替换 `#include <vector>` → `#include <EASTL/vector.h>`
- [ ] 替换 `std::` → `eastl::`
- [ ] 检查第三方库兼容性（某些库可能依赖 STL）
- [ ] 更新单元测试
- [ ] 验证性能改进

## 注意事项

### 1. 类型不兼容

EASTL 容器与 STL 容器类型不完全兼容，不能直接混用：

```cpp
// ❌ 错误：不能混用
std::vector<int> stdVec;
eastl::vector<int> eastlVec;
stdVec = std::vector<int>(eastlVec.begin(), eastlVec.end());  // 需要转换

// ✅ 正确：统一使用 EASTL
eastl::vector<int> vec1;
eastl::vector<int> vec2 = vec1;  // 可以复制
```

### 2. 迭代器兼容性

EASTL 迭代器与 STL 迭代器兼容，可以在算法中使用：

```cpp
#include <EASTL/vector.h>
#include <algorithm>

eastl::vector<int> vec = {3, 1, 4, 1, 5};
std::sort(vec.begin(), vec.end());  // STL 算法可以使用 EASTL 迭代器
```

### 3. 第三方库

某些第三方库可能依赖 STL，需要保留部分 STL 使用：

```cpp
// 第三方库可能需要 STL
some_third_party_function(std::vector<int>());  // 仍需要使用 STL

// 内部代码使用 EASTL
eastl::vector<int> myData;
```

### 4. 调试支持

在 Visual Studio 等 IDE 中，EASTL 容器的调试支持可能不如 STL 完善，但基本功能可用。

## 构建验证

集成完成后，可以通过以下方式验证：

1. **编译测试**：
   ```bash
   cmake -B build
   cmake --build build
   ```

2. **代码示例**：
   在某个源文件中添加测试代码：
   ```cpp
   #include <EASTL/vector.h>
   void test_eastl()
   {
       eastl::vector<int> vec;
       vec.push_back(42);
   }
   ```

## 故障排除

### 问题1：找不到 EASTL 头文件

**解决方案**：确保 EASTL 源代码在 `engine/3rdparty/EASTL/` 目录下。

### 问题2：编译错误（缺少 EAStdC）

**解决方案**：EASTL 的某些版本可能需要 EAStdC，可以：
1. 下载 EAStdC 并添加到项目
2. 使用 EASTL 的独立版本（不需要 EAStdC）
3. 禁用需要 EAStdC 的功能

### 问题3：链接错误

**解决方案**：确保在 `engine/source/runtime/CMakeLists.txt` 中已添加：
```cmake
target_link_libraries(${TARGET_NAME} PUBLIC EASTL)
```

## 相关资源

- **EASTL GitHub**: https://github.com/electronicarts/EASTL
- **EASTL 文档**: https://github.com/electronicarts/EASTL/wiki
- **性能对比**: 参见 `GAME_CONTAINER_LIBRARIES.md`

## 下一步

1. 下载 EASTL 源代码到 `engine/3rdparty/EASTL/`
2. 运行 CMake 配置
3. 编译项目验证集成
4. 在新代码中开始使用 EASTL
5. 逐步迁移现有代码

