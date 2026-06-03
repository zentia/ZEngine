# Shader头文件包含和变体功能实现

## 概述

ZEngine现在支持类似Unity和Unreal的shader头文件包含和变体功能，无需第三方库，完全基于glslang实现。

## 第三方库评估

### 为什么不使用第三方库？

经过评估，**不需要额外的第三方库**。原因如下：

1. **glslang已经足够强大**
   - glslang（Vulkan SDK自带）提供了完整的GLSL编译功能
   - 支持自定义include处理器（`TShader::Includer`接口）
   - 支持预处理器宏定义

2. **Unity/Unreal的实现方式**
   - Unity和Unreal也是基于glslang/DXC等标准编译器
   - 它们主要是在编译器基础上添加了变体管理和缓存系统
   - 这些功能我们可以在应用层实现，无需额外库

3. **轻量级实现**
   - 我们的实现直接使用glslang的API
   - 代码简洁，易于维护
   - 不增加额外的依赖

## 实现的功能

### 1. 头文件包含（Include）

**实现方式：**
- 实现了glslang的`TShader::Includer`接口
- 自定义`ShaderIncluder`类处理include请求
- 支持多个include搜索路径
- 支持相对路径（相对于包含该文件的shader）

**使用示例：**
```cpp
// shader.vert
#extension GL_GOOGLE_include_directive : enable
#include "common.h"
#include "lighting.h"

// C++代码
rhi->createShaderModuleFromFile(
    "shaders/shader.vert",
    static_cast<int>(ShaderStage::Vertex),
    {"shaders/include"}  // include搜索路径
);
```

### 2. Shader变体（Variants）

**实现方式：**
- 通过预处理器宏定义实现
- 在编译前将宏定义插入到shader源码开头
- 不同的宏定义组合生成不同的shader变体

**使用示例：**
```cpp
// 定义变体宏
RHI::ShaderMacros macros = {
    {"ENABLE_SHADOWS", "1"},
    {"MAX_LIGHTS", "8"}
};

// 编译变体
rhi->createShaderModuleFromFile(
    "shaders/shader.frag",
    static_cast<int>(ShaderStage::Fragment),
    {},
    macros
);
```

## 技术细节

### Include处理器实现

```cpp
class ShaderIncluder : public glslang::TShader::Includer {
    // 实现includeSystem和includeLocal方法
    // 在指定路径中搜索include文件
    // 支持相对路径解析
};
```

### 宏定义处理

```cpp
// 将宏定义转换为预处理器指令字符串
std::string buildDefinesString(const ShaderMacros& macros) {
    // 生成: #define MACRO1 value1\n#define MACRO2 value2\n
}
```

### 编译流程

1. 构建宏定义字符串
2. 将宏定义插入到shader源码开头
3. 创建include处理器
4. 调用glslang编译（传入include处理器）
5. 生成SPIR-V字节码

## 与Unity/Unreal的对比

| 功能 | Unity | Unreal | ZEngine |
|------|-------|--------|---------|
| Include支持 | ✅ | ✅ | ✅ |
| Shader变体 | ✅ | ✅ | ✅ |
| 变体管理工具 | ✅ (ShaderLab) | ✅ (Material Editor) | ⚠️ (代码层) |
| 变体缓存 | ✅ | ✅ | ⚠️ (需自行实现) |
| 变体剔除 | ✅ | ✅ | ⚠️ (需自行实现) |

**说明：**
- ✅ 已实现
- ⚠️ 可在应用层实现，不属于编译器功能

## 最佳实践

### 1. 变体管理

建议在应用层实现变体管理系统：

```cpp
class ShaderVariantManager {
    std::map<std::pair<std::string, RHI::ShaderMacros>, RHIShader*> cache;
    
    RHIShader* getVariant(const std::string& path, const RHI::ShaderMacros& macros) {
        auto key = std::make_pair(path, macros);
        if (cache.find(key) == cache.end()) {
            cache[key] = rhi->createShaderModuleFromFile(path, stage, {}, macros);
        }
        return cache[key];
    }
};
```

### 2. Include路径管理

建议统一管理include路径：

```cpp
class ShaderManager {
    std::vector<std::string> include_paths = {
        "shaders/include",
        "shaders/common"
    };
    
    RHIShader* compileShader(const std::string& path, const RHI::ShaderMacros& macros) {
        return rhi->createShaderModuleFromFile(path, stage, include_paths, macros);
    }
};
```

### 3. 变体定义

建议使用枚举或配置定义变体：

```cpp
enum class ShaderQuality {
    Low, Medium, High
};

RHI::ShaderMacros getQualityMacros(ShaderQuality quality) {
    switch (quality) {
        case ShaderQuality::Low:
            return {{"QUALITY_LOW", "1"}, {"MAX_LIGHTS", "2"}};
        case ShaderQuality::Medium:
            return {{"QUALITY_MEDIUM", "1"}, {"MAX_LIGHTS", "4"}};
        case ShaderQuality::High:
            return {{"QUALITY_HIGH", "1"}, {"MAX_LIGHTS", "8"}};
    }
}
```

## 性能考虑

1. **编译缓存**: 建议缓存编译好的shader变体，避免重复编译
2. **预编译**: 对于生产环境，仍建议使用预编译的shader
3. **变体数量**: 控制变体数量，避免组合爆炸
4. **延迟编译**: 在需要时才编译shader变体

## 总结

ZEngine通过glslang实现了完整的shader头文件包含和变体功能，无需额外的第三方库。这些功能与Unity和Unreal的实现方式类似，都是基于标准编译器的扩展。变体管理和缓存可以在应用层实现，提供更大的灵活性。

