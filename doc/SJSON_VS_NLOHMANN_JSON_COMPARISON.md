# sjson-cpp vs nlohmann/json 对比分析

## 一、概述

ZEngine 项目中同时使用了两个 JSON 库，它们服务于不同的用途：

- **sjson-cpp**: 用于 ACL（动画压缩库）的 SJSON 格式文件读写
- **nlohmann/json**: 用于引擎的主要 JSON 序列化系统

## 二、基本信息对比

| 特性 | sjson-cpp | nlohmann/json |
|------|-----------|---------------|
| **版本** | v0.9.0 | v3.12.0+ |
| **许可证** | MIT | MIT |
| **格式支持** | SJSON（Simplified JSON） | 标准 JSON |
| **集成方式** | 100% 头文件 | 100% 头文件 |
| **C++ 标准** | C++11 | C++11 |
| **项目位置** | `engine/3rdparty/acl/external/sjson-cpp/` | `engine/3rdparty/json/` |
| **主要用途** | ACL 动画文件格式 | 引擎序列化系统 |

## 三、设计理念对比

### 3.1 sjson-cpp

**设计目标**：
- ✅ **零内存分配** - 设计上不进行任何内存分配
- ✅ **极简快速** - 最小化、快速，不干扰程序员
- ✅ **轻量级** - 专注于 SJSON 格式解析
- ✅ **零依赖** - 完全自包含

**核心特点**：
- 使用 `StringView` 返回原始缓冲区引用，不复制数据
- 解析器只接受纯 SJSON，不接受标准 JSON
- 所有操作都是零开销抽象

### 3.2 nlohmann/json

**设计目标**：
- ✅ **直观语法** - 像 Python 一样，JSON 作为一等数据类型
- ✅ **易于集成** - 单头文件，无需复杂构建系统
- ✅ **功能完整** - 支持完整的 JSON 标准
- ✅ **STL 兼容** - 与标准库容器无缝集成

**核心特点**：
- 使用操作符重载实现直观的 API
- 支持序列化/反序列化任意类型
- 支持 JSON Pointer、JSON Patch、JSON Merge Patch
- 支持二进制格式（BSON、CBOR、MessagePack 等）

## 四、格式差异：SJSON vs JSON

### 4.1 SJSON（Simplified JSON）

SJSON 是 Autodesk Stingray 引擎使用的简化 JSON 格式，主要特点：

- ✅ **支持注释** - 使用 `//` 或 `/* */`
- ✅ **键名无需引号** - 如果键名是有效的标识符
- ✅ **尾随逗号** - 允许对象和数组中的尾随逗号
- ✅ **更宽松的语法** - 更接近配置文件格式

**示例**：
```sjson
{
    // 这是注释
    position: {
        x: 10.5,
        y: 20.0,
        z: 30.0,  // 尾随逗号允许
    },
    name: "Player",  // 键名无需引号
}
```

### 4.2 标准 JSON

标准 JSON 格式（RFC 8259）：

- ❌ **不支持注释**
- ✅ **键名必须引号**
- ❌ **不允许尾随逗号**
- ✅ **严格语法**

**示例**：
```json
{
    "position": {
        "x": 10.5,
        "y": 20.0,
        "z": 30.0
    },
    "name": "Player"
}
```

## 五、API 使用对比

### 5.1 sjson-cpp API

**读取示例**：
```cpp
#include "sjson/parser.h"

// 解析 SJSON 文件
sjson::parser parser(buffer, buffer_size);
auto root = parser.root();

// 访问值（返回 StringView，零拷贝）
auto name = root["name"].as_string();
int x = root["position"]["x"].as_int();
double y = root["position"]["y"].as_double();
```

**写入示例**：
```cpp
#include "sjson/writer.h"

sjson::writer writer;
writer.insert("name", "Player");
writer.insert("x", 10.5);
writer.insert("y", 20.0);
auto json_string = writer.c_str();  // 获取生成的字符串
```

**特点**：
- 使用 `StringView` 避免内存分配
- API 相对底层，需要手动管理缓冲区
- 专注于读写操作，功能单一

### 5.2 nlohmann/json API

**读取示例**：
```cpp
#include "nlohmann/json.hpp"
using json = nlohmann::json;

// 从文件读取
std::ifstream file("config.json");
json j;
file >> j;

// 从字符串解析
auto j2 = json::parse(json_string);

// 直观的访问方式
std::string name = j["name"];
int x = j["position"]["x"];
double y = j["position"]["y"];

// 支持异常或可选值
try {
    int value = j.at("key");
} catch (json::exception& e) {
    // 处理错误
}
```

**写入示例**：
```cpp
json j;
j["name"] = "Player";
j["position"]["x"] = 10.5;
j["position"]["y"] = 20.0;

// 序列化为字符串
std::string json_string = j.dump();
std::string pretty = j.dump(4);  // 格式化输出

// 写入文件
std::ofstream file("output.json");
file << j;
```

**特点**：
- 使用操作符重载，语法直观
- 自动内存管理
- 支持类型转换和验证
- 丰富的辅助功能

## 六、性能对比

| 特性 | sjson-cpp | nlohmann/json |
|------|-----------|---------------|
| **内存分配** | ⭐⭐⭐⭐⭐ 零分配 | ⭐⭐⭐ 自动管理 |
| **解析速度** | ⭐⭐⭐⭐ 快速（简单格式） | ⭐⭐⭐ 中等 |
| **内存占用** | ⭐⭐⭐⭐⭐ 极小（仅视图） | ⭐⭐⭐ 中等 |
| **序列化速度** | ⭐⭐⭐⭐ 快速 | ⭐⭐⭐ 中等 |
| **适用场景** | 高性能、零分配需求 | 通用 JSON 处理 |

**性能分析**：

1. **sjson-cpp**：
   - ✅ 零内存分配，适合性能敏感场景
   - ✅ 使用 `StringView`，避免数据复制
   - ✅ 解析器简单，速度快
   - ⚠️ 但只支持 SJSON 格式，不是标准 JSON

2. **nlohmann/json**：
   - ✅ 功能完整，API 友好
   - ⚠️ 需要内存分配来存储 JSON 树
   - ⚠️ 解析和序列化速度中等
   - ✅ 但对于大多数应用场景性能足够

## 七、功能特性对比

| 功能 | sjson-cpp | nlohmann/json |
|------|-----------|---------------|
| **标准 JSON 支持** | ❌ | ✅ |
| **SJSON 支持** | ✅ | ❌ |
| **注释支持** | ✅（SJSON 格式） | ❌ |
| **类型安全** | ⭐⭐ 基础 | ⭐⭐⭐⭐⭐ 完整 |
| **STL 集成** | ❌ | ✅ |
| **自定义类型序列化** | ❌ | ✅ |
| **JSON Pointer** | ❌ | ✅ |
| **JSON Patch** | ❌ | ✅ |
| **二进制格式** | ❌ | ✅（BSON、CBOR 等） |
| **错误处理** | ⭐⭐⭐ 基础 | ⭐⭐⭐⭐⭐ 完善 |
| **验证功能** | ⭐⭐ 基础 | ⭐⭐⭐⭐⭐ 完整 |

## 八、在 ZEngine 中的使用场景

### 8.1 sjson-cpp 使用场景

**位置**: `engine/3rdparty/acl/`

**用途**：
- ✅ ACL 动画压缩库的文件格式读写
- ✅ 动画剪辑数据（`.acl.sjson` 文件）
- ✅ 轨道列表数据
- ✅ 工具链（`acl_compressor`, `acl_decompressor`）

**原因**：
- ACL 库使用 SJSON 作为其原生格式
- 需要零内存分配的高性能解析
- 文件格式是 SJSON，不是标准 JSON

### 8.2 nlohmann/json 使用场景

**位置**: `engine/source/runtime/core/meta/json.h`

**用途**：
- ✅ 引擎序列化系统（`Serializer` 类）
- ✅ 资产文件序列化（`AssetManager`）
  - 游戏对象配置（`*.object.json`）
  - 关卡配置（`*.level.json`）
  - 材质配置（`*.material.json`）
  - 动画数据（`*.animation_clip.json`）
  - 骨架数据（`*.skeleton.json`）
- ✅ 运行时资产加载
- ✅ 编辑器资产保存

**原因**：
- 需要标准 JSON 格式（与外部工具兼容）
- 需要丰富的 API 和类型转换
- 需要序列化任意 C++ 类型
- 开发效率优先于极致性能

## 九、选择建议

### 9.1 何时使用 sjson-cpp

✅ **推荐使用** 如果：
- 需要处理 **SJSON 格式**文件
- 需要**零内存分配**（嵌入式、实时系统）
- 性能是**绝对优先**考虑
- 文件格式已经固定为 SJSON
- 只需要简单的读写操作

❌ **不推荐使用** 如果：
- 需要处理标准 JSON
- 需要丰富的功能和 API
- 需要序列化复杂类型
- 需要与外部 JSON 工具兼容

### 9.2 何时使用 nlohmann/json

✅ **推荐使用** 如果：
- 需要处理**标准 JSON**格式
- 需要**直观的 API**和易用性
- 需要**序列化任意类型**
- 需要**丰富的功能**（Pointer、Patch 等）
- 需要与**外部工具兼容**
- 开发效率优先

❌ **不推荐使用** 如果：
- 需要**零内存分配**
- 需要**极致性能**（考虑 rapidjson）
- 只需要处理 SJSON 格式

## 十、迁移考虑

### 10.1 从 sjson-cpp 迁移到 nlohmann/json

**适用场景**：
- 需要从 SJSON 迁移到标准 JSON
- 需要更多功能

**挑战**：
- ⚠️ 格式不兼容（SJSON vs JSON）
- ⚠️ API 完全不同
- ⚠️ 性能特性不同（内存分配）

**工作量**：
- 中等（需要格式转换和 API 重写）

### 10.2 从 nlohmann/json 迁移到 sjson-cpp

**适用场景**：
- 需要零内存分配
- 性能是瓶颈

**挑战**：
- ⚠️ 格式不兼容（JSON vs SJSON）
- ⚠️ 功能大幅减少
- ⚠️ API 更底层

**工作量**：
- 较大（需要重写大量代码，失去很多功能）

## 十一、总结

### 11.1 核心差异

| 维度 | sjson-cpp | nlohmann/json |
|------|-----------|---------------|
| **定位** | 高性能、零分配、SJSON 专用 | 通用、功能完整、标准 JSON |
| **优势** | 性能、零分配、轻量 | 易用、功能丰富、标准 |
| **劣势** | 格式限制、功能单一 | 性能开销、内存分配 |

### 11.2 在 ZEngine 中的角色

两个库在项目中**各司其职**，不存在冲突：

- **sjson-cpp**: 专门用于 ACL 动画库的 SJSON 格式处理
- **nlohmann/json**: 用于引擎的通用 JSON 序列化系统

这种设计是合理的，因为：
1. 它们服务于不同的子系统
2. 它们处理不同的格式（SJSON vs JSON）
3. 它们有不同的性能需求

### 11.3 建议

**保持现状** ✅

当前的双库设计是合理的：
- sjson-cpp 用于 ACL 库的特定需求
- nlohmann/json 用于引擎的通用序列化

**不需要统一**，因为：
- 它们服务于不同的目的
- 格式不兼容（SJSON vs JSON）
- 性能需求不同

---

**文档日期**: 2024  
**分析范围**: ZEngine 项目中的 JSON 库使用情况  
**版本**: sjson-cpp v0.9.0, nlohmann/json v3.12.0+

