# DX12 Shader编译器实现说明

## 概述

已成功实现DX12的shader编译器，使用DXC (DirectX Shader Compiler) 来编译HLSL shader为DXIL字节码。

## 实现文件

### 1. `dx12_rhi_resource.h`
- 定义了`DX12Shader`类，继承自`RHIShader`
- 封装了`ID3DBlob`，用于存储编译后的shader字节码

### 2. `dx12_shader_compiler.h` 和 `dx12_shader_compiler.cpp`
- `DX12ShaderCompiler`类：主要的shader编译器
- `DX12ShaderIncluder`类：自定义的include处理器，支持`#include`指令
- 支持从文件或源码字符串编译HLSL shader
- 支持shader变体（通过宏定义）
- 支持include路径

### 3. `dx12_rhi.h` 和 `dx12_rhi.cpp`
- 更新了`DX12RHI`类，添加了shader编译器成员
- 实现了`createShaderModule`、`createShaderModuleFromFile`和`createShaderModuleFromSource`方法

## 功能特性

### ✅ 已实现功能

1. **HLSL编译**
   - 使用DXC将HLSL编译为DXIL
   - 支持所有shader阶段（Vertex, Fragment, Geometry, Compute, Mesh, Task等）

2. **Include支持**
   - 支持`#include`指令
   - 支持多个include路径
   - 自动搜索include文件

3. **Shader变体**
   - 通过宏定义实现shader变体
   - 支持预处理器定义

4. **错误处理**
   - 详细的错误信息
   - 编译失败时返回错误消息

## 使用方法

### 从文件编译Shader

```cpp
RHI* rhi = ...; // 获取DX12RHI实例

// 编译顶点shader
RHIShader* vertex_shader = rhi->createShaderModuleFromFile(
    "shaders/mesh.hlsl",
    static_cast<int>(ShaderStage::Vertex),
    {"shaders/include"},  // include路径
    {{"ENABLE_SHADOWS", "1"}, {"MAX_LIGHTS", "4"}}  // 宏定义
);
```

### 从源码编译Shader

```cpp
const char* hlsl_source = R"(
struct VSInput {
    float3 position : POSITION;
    float2 texcoord : TEXCOORD0;
};

struct VSOutput {
    float4 position : SV_POSITION;
    float2 texcoord : TEXCOORD0;
};

VSOutput main(VSInput input) {
    VSOutput output;
    output.position = float4(input.position, 1.0);
    output.texcoord = input.texcoord;
    return output;
}
)";

RHIShader* shader = rhi->createShaderModuleFromSource(
    hlsl_source,
    static_cast<int>(ShaderStage::Vertex),
    "MyVertexShader",
    {"shaders/include"},
    {}
);
```

## Shader Profile映射

| Shader Stage | DXC Profile |
|-------------|-------------|
| Vertex | `vs_6_0` |
| Fragment | `ps_6_0` |
| Geometry | `gs_6_0` |
| TessellationControl | `hs_6_0` |
| TessellationEvaluation | `ds_6_0` |
| Compute | `cs_6_0` |
| Mesh | `ms_6_5` |
| Task | `as_6_5` |

## 依赖项

### 必需的库
- `dxcompiler.lib` - DXC编译器库
- `d3dcompiler.lib` - D3D编译器库（用于创建blob）
- `d3d12.lib` - DirectX 12库
- `dxgi.lib` - DXGI库

### 头文件
- `dxcapi.h` - DXC API头文件
- `d3d12.h` - DirectX 12头文件
- `d3dcompiler.h` - D3D编译器头文件

## 注意事项

1. **DXC库位置**
   - DXC通常随Windows SDK一起安装
   - 确保`dxcompiler.lib`在链接路径中
   - 运行时需要`dxcompiler.dll`在可执行文件路径中

2. **Shader语言**
   - 当前实现只支持HLSL
   - 不支持GLSL（GLSL需要先转换为HLSL）

3. **编译选项**
   - 默认使用`-O3`优化级别
   - Debug模式下启用`-WX`（将警告视为错误）

## 后续改进

1. **Shader缓存**
   - 实现shader编译结果缓存
   - 避免重复编译相同shader

2. **错误报告**
   - 改进错误信息格式
   - 支持错误行号定位

3. **更多编译选项**
   - 支持自定义编译选项
   - 支持不同的优化级别

4. **SPIR-V支持**
   - 使用DXC将HLSL编译为SPIR-V（用于Vulkan）
   - 实现统一的shader编译接口

## 与Vulkan实现的对比

| 特性 | Vulkan | DX12 |
|------|--------|------|
| Shader语言 | GLSL | HLSL |
| 编译器 | glslang | DXC |
| 输出格式 | SPIR-V | DXIL |
| Include支持 | ✅ | ✅ |
| Shader变体 | ✅ | ✅ |
| 运行时编译 | ✅ | ✅ |

## 总结

DX12 shader编译器已成功实现，提供了与Vulkan类似的运行时shader编译功能。现在ZEngine可以在运行时编译HLSL shader，支持include和shader变体，为多API支持奠定了基础。

