# 运行时Shader编译功能

ZEngine现在支持像Unity和Unreal一样在运行时编译和加载shader，无需预先编译。

## 功能概述

新增了两个运行时shader编译接口：
1. `createShaderModuleFromFile` - 从文件路径编译GLSL shader
2. `createShaderModuleFromSource` - 从源码字符串编译GLSL shader

### 核心特性

- ✅ **头文件包含支持** - 支持`#include`指令，自动解析include文件
- ✅ **Shader变体支持** - 通过预处理器宏定义实现shader变体（类似Unity的shader variants）
- ✅ **运行时编译** - 无需预编译，支持动态加载和编译shader

## 使用方法

### 1. 从文件编译Shader

```cpp
#include "runtime/function/render/interface/rhi.h"

// 获取RHI实例（假设你已经有了）
RHI* rhi = ...;

// 编译顶点shader
RHIShader* vertex_shader = rhi->createShaderModuleFromFile(
    "path/to/shader.vert",
    static_cast<int>(ShaderStage::Vertex),
    {"path/to/shader/includes"},  // 可选的include路径
    {}  // 可选的宏定义（shader变体）
);

// 编译片段shader
RHIShader* fragment_shader = rhi->createShaderModuleFromFile(
    "path/to/shader.frag",
    static_cast<int>(ShaderStage::Fragment)
);

if (vertex_shader && fragment_shader) {
    // 使用编译好的shader创建pipeline
    // ...
}
```

### 2. 从源码字符串编译Shader

```cpp
// GLSL顶点shader源码
const char* vertex_source = R"(
#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec2 inTexCoord;

layout(location = 0) out vec2 fragTexCoord;

void main() {
    gl_Position = vec4(inPosition, 1.0);
    fragTexCoord = inTexCoord;
}
)";

// 编译shader
RHIShader* vertex_shader = rhi->createShaderModuleFromSource(
    vertex_source,
    static_cast<int>(ShaderStage::Vertex),
    "MyVertexShader",  // shader名称（用于错误报告）
    {},  // include路径
    {}   // 宏定义
);

if (vertex_shader) {
    // 使用编译好的shader
    // ...
}
```

### 3. 使用头文件包含（Include）

ZEngine支持GLSL的`#include`指令，可以像Unity或Unreal一样组织shader代码：

**shader.vert:**
```glsl
#version 450

#extension GL_GOOGLE_include_directive : enable

#include "common.h"
#include "lighting.h"

void main() {
    // ...
}
```

**编译时指定include路径：**
```cpp
RHIShader* shader = rhi->createShaderModuleFromFile(
    "shaders/shader.vert",
    static_cast<int>(ShaderStage::Vertex),
    {"shaders/include", "shaders/common"}  // include搜索路径
);
```

Include处理器会：
- 在指定的include路径中搜索头文件
- 支持相对路径（相对于包含该文件的shader位置）
- 自动处理嵌套的include

### 4. Shader变体（Variants）

通过预处理器宏定义实现shader变体，类似Unity的shader variants：

**shader.frag:**
```glsl
#version 450

#ifdef ENABLE_SHADOWS
    // 阴影相关代码
    vec3 calculateShadow(vec3 position) {
        // ...
    }
#endif

#ifdef MAX_LIGHTS
    #define LIGHT_COUNT MAX_LIGHTS
#else
    #define LIGHT_COUNT 4
#endif

void main() {
    #ifdef ENABLE_SHADOWS
        vec3 shadow = calculateShadow(worldPos);
    #endif
    
    // 使用LIGHT_COUNT...
}
```

**编译不同变体：**
```cpp
// 变体1: 启用阴影，最大8个光源
RHI::ShaderMacros macros1 = {
    {"ENABLE_SHADOWS", "1"},
    {"MAX_LIGHTS", "8"}
};
RHIShader* shader_variant1 = rhi->createShaderModuleFromFile(
    "shaders/shader.frag",
    static_cast<int>(ShaderStage::Fragment),
    {},
    macros1
);

// 变体2: 禁用阴影，默认光源数量
RHI::ShaderMacros macros2 = {
    {"ENABLE_SHADOWS", "0"}
};
RHIShader* shader_variant2 = rhi->createShaderModuleFromFile(
    "shaders/shader.frag",
    static_cast<int>(ShaderStage::Fragment),
    {},
    macros2
);

// 变体3: 无宏定义（使用默认值）
RHIShader* shader_default = rhi->createShaderModuleFromFile(
    "shaders/shader.frag",
    static_cast<int>(ShaderStage::Fragment)
);
```

**宏定义格式：**
- `{"MACRO_NAME", "value"}` - 定义宏并赋值
- `{"MACRO_NAME", ""}` - 定义宏但不赋值（相当于`#define MACRO_NAME`）

### 5. Shader Stage枚举

支持的shader stage类型（定义在`ShaderStage`枚举中）：

- `ShaderStage::Vertex` - 顶点shader
- `ShaderStage::Fragment` - 片段shader
- `ShaderStage::Geometry` - 几何shader
- `ShaderStage::TessellationControl` - 曲面细分控制shader
- `ShaderStage::TessellationEvaluation` - 曲面细分计算shader
- `ShaderStage::Compute` - 计算shader
- `ShaderStage::Mesh` - Mesh shader
- `ShaderStage::Task` - Task shader
- `ShaderStage::RayGen` - 光线追踪生成shader
- `ShaderStage::RayClosestHit` - 光线追踪最近命中shader
- `ShaderStage::RayMiss` - 光线追踪未命中shader
- `ShaderStage::RayCallable` - 光线追踪可调用shader

## 实现细节

### 依赖

运行时shader编译功能依赖于：
- **glslang库** - Vulkan SDK通常包含此库，提供GLSL到SPIR-V的编译
- **SPIRV-Tools** - 用于SPIR-V生成和优化
- **C++17 filesystem库** - 用于include文件路径解析（`std::filesystem`）

### 实现原理

1. **Include处理**: 实现了glslang的`Includer`接口，自定义include处理器会在编译时自动解析和加载include文件
2. **宏定义**: 在编译前将宏定义字符串（`#define MACRO value`）插入到shader源码的开头
3. **变体支持**: 通过不同的宏定义组合生成不同的shader变体，每个变体都是独立的SPIR-V模块

### CMake配置

CMake会自动尝试从Vulkan SDK中找到并链接glslang库。如果找不到，会显示警告，但不会阻止编译。

### 错误处理

如果shader编译失败：
- 函数返回`nullptr`
- 错误信息会输出到`std::cerr`
- 你可以通过日志系统查看详细错误信息

### 性能考虑

- 运行时编译会有性能开销，建议：
  - 在初始化时编译shader，而不是每帧编译
  - 缓存编译好的shader模块
  - 对于生产环境，仍建议使用预编译的shader

### 与现有系统的兼容性

- 现有的预编译shader系统仍然可用
- 两种方式可以混合使用
- 运行时编译的shader与预编译的shader使用相同的接口

## 示例：完整的Pipeline创建

```cpp
// 编译shader
RHIShader* vert_shader = rhi->createShaderModuleFromFile("shaders/mesh.vert", 
                                                          static_cast<int>(ShaderStage::Vertex));
RHIShader* frag_shader = rhi->createShaderModuleFromFile("shaders/mesh.frag", 
                                                          static_cast<int>(ShaderStage::Fragment));

if (!vert_shader || !frag_shader) {
    // 处理错误
    return;
}

// 创建pipeline shader stages
RHIPipelineShaderStageCreateInfo vert_stage{};
vert_stage.stage = RHIShaderStageFlagBits::RHI_SHADER_STAGE_VERTEX_BIT;
vert_stage.module = vert_shader;
vert_stage.pName = "main";

RHIPipelineShaderStageCreateInfo frag_stage{};
frag_stage.stage = RHIShaderStageFlagBits::RHI_SHADER_STAGE_FRAGMENT_BIT;
frag_stage.module = frag_shader;
frag_stage.pName = "main";

std::vector<RHIPipelineShaderStageCreateInfo> stages = {vert_stage, frag_stage};

// 创建pipeline（使用现有的pipeline创建接口）
// ...
```

## 高级用法

### Shader变体管理最佳实践

1. **定义变体枚举**：
```cpp
enum class ShaderVariant {
    Default,
    WithShadows,
    WithShadowsAndSSAO,
    Mobile
};

RHI::ShaderMacros getVariantMacros(ShaderVariant variant) {
    switch (variant) {
        case ShaderVariant::WithShadows:
            return {{"ENABLE_SHADOWS", "1"}};
        case ShaderVariant::WithShadowsAndSSAO:
            return {{"ENABLE_SHADOWS", "1"}, {"ENABLE_SSAO", "1"}};
        case ShaderVariant::Mobile:
            return {{"MOBILE", "1"}, {"MAX_LIGHTS", "2"}};
        default:
            return {};
    }
}
```

2. **缓存编译结果**：
```cpp
std::map<std::pair<std::string, RHI::ShaderMacros>, RHIShader*> shader_cache;

RHIShader* getOrCompileShader(const std::string& path, 
                              const RHI::ShaderMacros& macros) {
    auto key = std::make_pair(path, macros);
    if (shader_cache.find(key) == shader_cache.end()) {
        shader_cache[key] = rhi->createShaderModuleFromFile(
            path, static_cast<int>(ShaderStage::Fragment), {}, macros);
    }
    return shader_cache[key];
}
```

## 注意事项

1. **Include路径**: 如果shader使用了`#include`指令，需要提供include路径。系统会自动在指定路径中搜索头文件。
2. **GLSL版本**: 确保shader使用兼容的GLSL版本（推荐#version 450或更高）
3. **Vulkan特性**: 确保shader使用Vulkan兼容的GLSL语法
4. **内存管理**: 编译好的shader模块需要手动释放（使用`destroyShaderModule`）
5. **Include扩展**: 使用`#include`时需要在shader开头添加：`#extension GL_GOOGLE_include_directive : enable`
6. **宏定义顺序**: 宏定义会在shader源码之前插入，确保宏定义在使用前生效
7. **变体缓存**: 建议缓存编译好的shader变体，避免重复编译

## 故障排除

如果运行时编译不工作：

1. **检查Vulkan SDK**: 确保Vulkan SDK正确安装并设置了`VULKAN_SDK`环境变量
2. **检查glslang库**: 确认Vulkan SDK的Lib目录包含glslang库文件
3. **查看编译输出**: 检查CMake配置输出，看是否找到了glslang库
4. **检查错误信息**: 查看`std::cerr`输出的详细错误信息

