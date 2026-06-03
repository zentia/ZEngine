# ZEngine 枚举序列化使用指南

## 概述

ZEngine 支持两种枚举序列化方式：
1. **整数序列化**（默认）：将枚举序列化为底层整数类型
2. **字符串序列化**（推荐）：使用 `NLOHMANN_JSON_SERIALIZE_ENUM` 宏将枚举序列化为字符串

## 自动枚举序列化支持

ZEngine 的 `Serializer` 和 `BinarySerializer` 已经内置了对枚举类型的支持，无需额外配置即可序列化任何枚举类型。

### JSON 序列化（Serializer）

枚举会被自动序列化为底层整数类型。例如：

```cpp
enum class LogLevel : uint8_t {
    verbose,
    debug,
    info,
    warning,
    error,
    fatal
};

LogLevel level = LogLevel::info;
Json json = Serializer::write(level);
// json 值为 2（整数）

LogLevel restored;
Serializer::read(json, restored);
// restored == LogLevel::info
```

### 二进制序列化（BinarySerializer）

枚举会被序列化为底层整数类型：

```cpp
LogLevel level = LogLevel::error;
BinaryStream stream(BinaryStream::Mode::Write);
BinarySerializer::write(stream, level);
// 写入 uint8_t 值 4

BinaryStream read_stream(BinaryStream::Mode::Read);
read_stream.setBuffer(stream.getBuffer());
LogLevel restored;
BinarySerializer::read(read_stream, restored);
// restored == LogLevel::error
```

## 字符串序列化（推荐）

为了更好的可读性和稳定性，建议使用 `NLOHMANN_JSON_SERIALIZE_ENUM` 宏将枚举序列化为字符串。这样可以避免因枚举值顺序改变而导致的数据不兼容问题。

### 使用方法

1. **定义枚举类型**

```cpp
namespace Z {
    enum class AssetFormat {
        JSON,
        Binary
    };
}
```

2. **在枚举所在的命名空间中声明序列化映射**

```cpp
#include "runtime/core/meta/json.h"

namespace Z {
    // 在枚举定义之后，同一命名空间中声明
    NLOHMANN_JSON_SERIALIZE_ENUM(AssetFormat, {
        {AssetFormat::JSON, "json"},
        {AssetFormat::Binary, "binary"},
    })
}
```

3. **使用序列化**

```cpp
AssetFormat format = AssetFormat::JSON;
Json json = Serializer::write(format);
// json 值为 "json"（字符串）

AssetFormat restored;
Serializer::read(json, restored);
// restored == AssetFormat::JSON
```

### 完整示例

```cpp
#include "runtime/core/meta/serializer/serializer.h"
#include "runtime/core/meta/json.h"

namespace Z {
    enum class LogLevel : uint8_t {
        verbose,
        debug,
        info,
        warning,
        error,
        fatal
    };

    // 声明字符串序列化映射
    NLOHMANN_JSON_SERIALIZE_ENUM(LogLevel, {
        {LogLevel::verbose, "verbose"},
        {LogLevel::debug, "debug"},
        {LogLevel::info, "info"},
        {LogLevel::warning, "warning"},
        {LogLevel::error, "error"},
        {LogLevel::fatal, "fatal"},
    })
}

// 使用示例
void example() {
    Z::LogLevel level = Z::LogLevel::info;
    
    // JSON 序列化（字符串格式）
    Json json = Z::Serializer::write(level);
    // json == "info"
    
    // JSON 反序列化
    Z::LogLevel restored;
    Z::Serializer::read(json, restored);
    // restored == Z::LogLevel::info
    
    // 二进制序列化（仍然使用整数）
    Z::BinaryStream stream(Z::BinaryStream::Mode::Write);
    Z::BinarySerializer::write(stream, level);
    // 写入 uint8_t 值 2
}
```

### 注意事项

1. **命名空间要求**：`NLOHMANN_JSON_SERIALIZE_ENUM` 宏必须在枚举所在的命名空间中声明（可以是全局命名空间）

2. **头文件包含**：使用该宏的地方需要包含 `runtime/core/meta/json.h`

3. **默认值**：如果 JSON 中的值无法匹配任何枚举值，会使用映射中的第一个值作为默认值

4. **二进制序列化**：即使使用了字符串序列化，二进制序列化仍然使用整数类型（更高效）

5. **向后兼容**：如果 JSON 中已经是整数格式，反序列化时仍然可以正常工作（会先尝试字符串匹配，失败后使用整数）

## 在反射类中使用枚举

如果枚举类型作为反射类的成员，序列化会自动处理：

```cpp
#include "runtime/core/meta/reflection/reflection.h"

namespace Z {
    REFLECTION_TYPE(MyComponent)
    CLASS(MyComponent, Fields)
    {
        REFLECTION_BODY(MyComponent)
        
    public:
        META(Editable, Visible, Category = "Config")
        LogLevel m_log_level = LogLevel::info;
    };
}

// 使用
MyComponent component;
component.m_log_level = LogLevel::error;

// 序列化整个对象
Json json = Serializer::write(component);
// json["m_log_level"] == "error"（如果使用了字符串序列化）

// 反序列化
MyComponent restored;
Serializer::read(json, restored);
// restored.m_log_level == LogLevel::error
```

## 最佳实践

1. **优先使用字符串序列化**：对于需要长期存储或跨版本兼容的枚举，使用 `NLOHMANN_JSON_SERIALIZE_ENUM` 宏

2. **保持命名一致性**：字符串值应该与枚举值名称保持一致（小写）

3. **处理无效值**：在映射的第一个位置放置一个"无效"或"未知"值作为默认值

```cpp
enum class TaskState {
    Invalid = -1,
    Stopped,
    Running,
    Completed,
};

NLOHMANN_JSON_SERIALIZE_ENUM(TaskState, {
    {TaskState::Invalid, nullptr},  // 第一个值作为默认值
    {TaskState::Stopped, "stopped"},
    {TaskState::Running, "running"},
    {TaskState::Completed, "completed"},
})
```

4. **文档化枚举**：在枚举定义处添加注释说明序列化方式

## 技术细节

### 实现原理

- **Serializer**：使用 `std::is_enum` 检测枚举类型，转换为底层整数类型进行序列化。如果使用了 `NLOHMANN_JSON_SERIALIZE_ENUM`，nlohmann/json 库会自动使用字符串序列化。

- **BinarySerializer**：始终使用底层整数类型进行序列化，无论是否使用了字符串序列化宏。

### 底层类型支持

支持所有标准枚举底层类型：
- `uint8_t`, `int8_t`
- `uint16_t`, `int16_t`
- `uint32_t`, `int32_t`
- `uint64_t`, `int64_t`
- `int`, `unsigned int`

## 相关文档

- [二进制序列化指南](BINARY_SERIALIZATION_GUIDE.md)
- [反射系统文档](../engine/source/runtime/core/meta/reflection/README.md)

