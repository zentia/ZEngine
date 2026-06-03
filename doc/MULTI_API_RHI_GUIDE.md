# ZEngine 多图形API支持指南

## 概述

ZEngine现在支持多个图形API后端：
- **Vulkan** (跨平台，默认)
- **DirectX 12** (Windows)
- **Metal** (macOS/iOS)

## 架构设计

### RHI抽象层

RHI (Render Hardware Interface) 是ZEngine的图形API抽象层，提供了统一的接口来访问不同的底层图形API。

```
┌─────────────────────────────────────┐
│         RHI (抽象接口)               │
│  - 统一的资源管理接口                │
│  - 统一的命令提交接口                │
│  - 统一的着色器编译接口               │
└──────────────┬──────────────────────┘
               │
    ┌──────────┼──────────┐
    │          │          │
┌───▼───┐ ┌───▼───┐ ┌───▼───┐
│Vulkan │ │ DX12  │ │ Metal │
│  RHI  │ │  RHI  │ │  RHI  │
└───────┘ └───────┘ └───────┘
```

### 关键组件

1. **GraphicsAPI枚举** (`render_type.h`)
   ```cpp
   enum class GraphicsAPI : uint8_t
   {
       Vulkan = 0,
       DirectX12,
       Metal,
       API_COUNT
   };
   ```

2. **RHI工厂** (`rhi_factory.h/cpp`)
   - 根据平台和配置创建合适的RHI实现
   - 提供API可用性检查

3. **RHI实现**
   - `VulkanRHI`: Vulkan实现
   - `DX12RHI`: DirectX 12实现 (Windows)
   - `MetalRHI`: Metal实现 (macOS/iOS)

## 使用方法

### 1. 选择图形API

在初始化时指定要使用的图形API：

```cpp
#include "runtime/function/render/interface/rhi_factory.h"

// 获取默认API（根据平台自动选择）
GraphicsAPI api = RHIFactory::getDefaultAPI();

// 或者手动指定
GraphicsAPI api = GraphicsAPI::Vulkan; // 或 DirectX12, Metal

// 检查API是否可用
if (RHIFactory::isAPIAvailable(api))
{
    auto rhi = RHIFactory::createRHI(api, window_system);
}
```

### 2. 系统注册

RHI系统会在引擎启动时自动注册。在`register_runtime.cpp`中：

```cpp
// 根据平台注册相应的RHI实现
REGISTER_SYSTEM(VulkanRHI);
#ifdef _WIN32
REGISTER_SYSTEM(DX12RHI);
#endif
#ifdef __APPLE__
REGISTER_SYSTEM(MetalRHI);
#endif
```

### 3. 着色器编译

不同API使用不同的着色器语言：
- **Vulkan**: GLSL (编译为SPIR-V)
- **DirectX 12**: HLSL
- **Metal**: Metal Shading Language (MSL)

RHI接口提供了统一的着色器编译接口：

```cpp
// 从文件编译着色器
RHIShader* shader = rhi->createShaderModuleFromFile(
    "shaders/main.vert",
    RHI_SHADER_STAGE_VERTEX_BIT,
    include_paths,
    macros
);

// 从源代码编译着色器
RHIShader* shader = rhi->createShaderModuleFromSource(
    shader_source_code,
    RHI_SHADER_STAGE_VERTEX_BIT,
    "main",
    include_paths,
    macros
);
```

底层实现会自动处理不同着色器语言的转换。

## API差异处理

### 内存管理

- **Vulkan**: 使用VMA (Vulkan Memory Allocator)
- **DirectX 12**: 使用D3D12内存管理
- **Metal**: 使用Metal资源管理

RHI接口抽象了这些差异，提供统一的`createBuffer`、`createImage`等接口。

### 命令提交

不同API的命令提交方式不同：
- **Vulkan**: Command Buffer + Queue Submit
- **DirectX 12**: Command List + Command Queue
- **Metal**: Command Encoder + Command Buffer

RHI接口提供了统一的命令提交接口，隐藏了这些差异。

### 同步原语

- **Vulkan**: Semaphore, Fence
- **DirectX 12**: Fence
- **Metal**: MTLFence, Dispatch Semaphore

RHI接口统一了同步机制。

## 平台支持

| API | Windows | Linux | macOS | iOS |
|-----|---------|-------|-------|-----|
| Vulkan | ✅ | ✅ | ✅ | ❌ |
| DirectX 12 | ✅ | ❌ | ❌ | ❌ |
| Metal | ❌ | ❌ | ✅ | ✅ |

## 迁移指南

### 从单一Vulkan实现迁移

1. **更新RHI接口调用**
   - 所有RHI接口调用保持不变
   - VMA相关函数已抽象化

2. **着色器处理**
   - 需要为不同API提供对应的着色器文件
   - 或使用着色器编译工具链自动转换

3. **平台特定代码**
   - 使用`#ifdef`条件编译处理平台差异
   - 或通过RHI接口统一处理

## 最佳实践

1. **优先使用RHI抽象接口**
   - 避免直接调用底层API
   - 保持代码的可移植性

2. **着色器管理**
   - 使用统一的着色器编译接口
   - 考虑使用着色器变体系统

3. **性能优化**
   - 不同API有不同的优化策略
   - 在RHI实现中处理API特定的优化

4. **错误处理**
   - 检查API可用性
   - 提供合理的回退机制

## 未来扩展

- 支持更多图形API（如OpenGL、WebGPU等）
- 运行时API切换
- 多API混合渲染（用于调试和对比）

## 相关文档

- [RENDER_SYSTEM_ARCHITECTURE.md](./RENDER_SYSTEM_ARCHITECTURE.md)
- [THREADING_GUIDE.md](./THREADING_GUIDE.md)

