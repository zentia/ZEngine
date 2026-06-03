# 游戏开发高性能容器库推荐

## 概述

标准库（STL）容器在游戏开发中可能存在性能瓶颈，特别是在需要：
- 高性能内存分配
- 缓存友好性
- 确定性性能
- 更少的内存碎片
- 自定义分配器支持

的场景中。本文档介绍几个适合游戏开发的高性能容器库。

## 推荐库对比

### 1. EASTL (Electronic Arts STL) ⭐ 强烈推荐

**GitHub**: https://github.com/electronicarts/EASTL

**特点**:
- ✅ API与STL高度兼容，迁移成本低
- ✅ 为游戏性能优化设计
- ✅ 支持自定义分配器（可与mimalloc集成）
- ✅ 提供固定大小容器（`fixed_vector`, `fixed_string`等）
- ✅ 更少的堆分配，更好的缓存局部性
- ✅ 包含性能分析工具
- ✅ 被多个AAA游戏引擎使用（EA自家游戏，部分Unity组件等）

**集成方式**:
```cmake
# 在 engine/3rdparty/CMakeLists.txt 中添加
if(NOT TARGET EASTL)
    add_subdirectory(EASTL)
    set_target_properties(EASTL PROPERTIES FOLDER ${third_party_folder}/EASTL)
endif()
```

**使用示例**:
```cpp
#include <EASTL/vector.h>
#include <EASTL/unordered_map.h>
#include <EASTL/string.h>

// 使用方式与STL几乎相同
eastl::vector<int> vec;
eastl::unordered_map<std::string, int> map;
eastl::string str;
```

**性能优势**:
- `eastl::vector` 通常比 `std::vector` 快 10-20%
- `eastl::string` 有更好的SSO（Small String Optimization）
- `eastl::hash_map` 针对游戏场景优化

---

### 2. robin_hood::unordered_map ⭐ 推荐（针对哈希表）

**GitHub**: https://github.com/martinus/robin-hood-hashing

**特点**:
- ✅ Header-only，集成简单
- ✅ 通常比 `std::unordered_map` 快 2-3 倍
- ✅ 内存占用更小
- ✅ 适合频繁查找和插入的场景

**集成方式**:
```cmake
# Header-only库，只需添加include目录
target_include_directories(${TARGET_NAME} PUBLIC
    $<BUILD_INTERFACE:${THIRD_PARTY_DIR}/robin-hood-hashing/src/include>
)
```

**使用示例**:
```cpp
#include <robin_hood.h>

robin_hood::unordered_map<std::string, int> map;
map["key"] = 42;
```

---

### 3. emhash (emilib) ⭐ 轻量级选择

**GitHub**: https://github.com/ktprime/emhash

**特点**:
- ✅ Header-only
- ✅ 针对小键值对优化
- ✅ 比标准库快，内存效率高
- ✅ 支持多种哈希策略

**集成方式**:
```cmake
target_include_directories(${TARGET_NAME} PUBLIC
    $<BUILD_INTERFACE:${THIRD_PARTY_DIR}/emhash>
)
```

---

### 4. plf::colony - 向量替代方案

**GitHub**: https://github.com/mattreecebentley/plf_colony

**特点**:
- ✅ 类似 `std::vector` 但删除元素为 O(1)
- ✅ 适合需要频繁增删的场景
- ✅ 迭代器稳定（元素移动后仍有效）

**使用场景**:
- 实体组件系统（ECS）中的组件存储
- 需要稳定指针的游戏对象管理

---

### 5. stb_ds.h (已包含) ⭐ 轻量级

**位置**: `engine/3rdparty/stb/stb_ds.h`

**特点**:
- ✅ 项目已包含
- ✅ 单文件库，零依赖
- ✅ 提供动态数组和哈希表
- ✅ C/C++ 兼容

**使用示例**:
```cpp
#define STB_DS_IMPLEMENTATION
#include "stb_ds.h"

int* arr = NULL;
arrput(arr, 42);
int len = arrlen(arr);
arrfree(arr);
```

---

## 性能对比参考

### 哈希表性能（相对std::unordered_map）
- `robin_hood::unordered_map`: ~2.5x 更快
- `eastl::hash_map`: ~1.5-2x 更快
- `emhash::hash_map`: ~2x 更快
- `std::unordered_map`: 基准

### 向量性能（相对std::vector）
- `eastl::vector`: ~10-20% 更快
- `plf::colony`: 删除操作快很多
- `std::vector`: 基准

## 集成建议

### 方案A：完整迁移到EASTL（推荐大型项目）
- 优势：统一的API，完整的容器生态
- 成本：需要替换所有STL容器，但API兼容性高
- 适用：大型引擎开发

### 方案B：混合使用（推荐当前项目）
- 核心性能路径：使用EASTL
- 哈希表优化：使用robin_hood
- 工具代码：保留STL（兼容性）
- 适用：渐进式优化

### 方案C：针对性替换（最小改动）
- 只替换性能热点容器
- 哈希表 → robin_hood
- 向量 → eastl::vector（仅热点路径）
- 适用：快速优化

## 集成步骤示例（EASTL）

### 1. 下载EASTL
```bash
cd engine/3rdparty
git submodule add https://github.com/electronicarts/EASTL.git
# 或直接克隆
git clone https://github.com/electronicarts/EASTL.git
```

### 2. 修改CMakeLists.txt
在 `engine/3rdparty/CMakeLists.txt` 中添加：
```cmake
if(NOT TARGET EASTL)
    add_subdirectory(EASTL)
    set_target_properties(EASTL PROPERTIES FOLDER ${third_party_folder}/EASTL)
endif()
```

### 3. 链接库
在 `engine/source/runtime/CMakeLists.txt` 中：
```cmake
target_link_libraries(${TARGET_NAME} PUBLIC EASTL)
```

### 4. 配置自定义分配器（可选，推荐与mimalloc集成）
```cpp
// 创建EASTL配置头文件
#define EASTL_USER_CONFIG_HEADER "eastl_config.h"
```

### 5. 逐步迁移
```cpp
// 旧代码
#include <vector>
#include <unordered_map>
std::vector<int> vec;

// 新代码
#include <EASTL/vector.h>
#include <EASTL/unordered_map.h>
eastl::vector<int> vec;
```

## 注意事项

1. **类型兼容性**: EASTL与STL类型不完全兼容，不能直接混用
2. **编译时间**: Header-only库可能增加编译时间
3. **调试支持**: 确保调试器能识别新容器类型
4. **第三方库兼容**: 某些第三方库可能依赖STL
5. **渐进迁移**: 建议先在新代码中使用，逐步迁移旧代码

## 性能测试建议

在集成前建议进行基准测试：
1. 识别性能关键路径
2. 对比新旧容器性能
3. 测试内存占用差异
4. 验证缓存性能

## 当前项目建议

基于你的项目结构，推荐：
1. **短期**: 使用 `robin_hood::unordered_map` 替换热点路径的 `std::unordered_map`
2. **中期**: 集成EASTL，在新代码中使用
3. **长期**: 逐步迁移核心系统到EASTL

## 参考资料

- EASTL文档: https://github.com/electronicarts/EASTL
- robin_hood基准测试: https://github.com/martinus/robin-hood-hashing#benchmark
- 游戏引擎容器设计: 《游戏引擎架构》第5章

