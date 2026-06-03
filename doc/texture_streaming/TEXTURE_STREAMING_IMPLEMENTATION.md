# ZEngine Texture Streaming Implementation

## 概述

本文档描述了基于Unreal Engine的TextureStreaming系统实现的ZEngine纹理流式传输系统。

## 系统架构

### 核心组件

1. **StreamableTexture** (`texture_streaming_types.h/cpp`)
   - 表示可流式传输的纹理资源
   - 管理纹理的mip级别和GPU资源
   - 计算基于距离和屏幕空间的LOD级别

2. **TextureStreamingManager** (`texture_streaming_manager.h/cpp`)
   - 纹理流式传输的主管理器
   - 处理纹理注册、加载、卸载
   - 管理内存预算和优先级
   - 支持异步加载

3. **TextureStreamingTypes** (`texture_streaming_types.h/cpp`)
   - 定义流式传输相关的数据结构和枚举
   - 包含配置、统计信息等

## 主要特性

### 1. Mip级别流式传输
- 根据距离和屏幕空间大小自动选择合适的mip级别
- 支持动态调整纹理分辨率以节省内存

### 2. 优先级系统
- **Critical**: 屏幕空间大于50%
- **High**: 屏幕空间10%-50%
- **Normal**: 屏幕空间1%-10%
- **Low**: 屏幕空间小于1%

### 3. 内存管理
- 可配置的内存预算（默认512MB）
- 自动驱逐低优先级纹理以释放内存
- 实时内存使用统计

### 4. 异步加载
- 支持多线程异步纹理加载
- 可配置的最大并发加载数（默认4个）
- 非阻塞的纹理加载流程

## 使用方法

### 初始化

纹理流式传输管理器在`RenderResource::uploadGlobalRenderResource`中自动初始化：

```cpp
// 在RenderResource中自动初始化
m_texture_streaming_manager = std::make_shared<TextureStreamingManager>();
m_texture_streaming_manager->initialize(rhi, this);
```

### 注册纹理

```cpp
// 注册一个纹理用于流式传输
uint64_t texture_handle = texture_streaming_manager->registerTexture("path/to/texture.png", true);

// 请求流式传输特定mip级别
texture_streaming_manager->requestTextureStreaming(
    texture_handle,
    0,  // mip level
    TextureStreamingPriority::Normal,
    10.0f,  // distance
    0.5f,   // screen size
    true    // is visible
);
```

### 获取纹理资源

```cpp
RHIImage* image = nullptr;
RHIImageView* image_view = nullptr;
VmaAllocation allocation = nullptr;

if (texture_streaming_manager->getTextureResources(texture_handle, image, image_view, allocation))
{
    // 使用纹理资源进行渲染
}
```

### 更新流式传输

系统在`RenderSystem::tick`中自动更新：

```cpp
// 每帧自动调用
streaming_manager->updateStreaming(camera_pos, camera_forward, fov, aspect, width, height);
streaming_manager->tick(delta_time);
```

## 配置

可以通过`TextureStreamingConfig`进行配置：

```cpp
TextureStreamingConfig config;
config.memory_budget_mb = 1024;           // 内存预算（MB）
config.max_concurrent_loads = 8;          // 最大并发加载数
config.min_screen_size = 0.01f;           // 最小屏幕空间
config.max_distance = 2000.0f;            // 最大距离
config.enable_async_loading = true;       // 启用异步加载
config.enable_mip_streaming = true;       // 启用mip流式传输

texture_streaming_manager->setConfig(config);
```

## 统计信息

可以获取流式传输统计信息：

```cpp
TextureStreamingStats stats = texture_streaming_manager->getStats();
// stats.total_textures - 总纹理数
// stats.loaded_textures - 已加载纹理数
// stats.total_memory_used - 已使用内存（字节）
// stats.memory_usage_ratio - 内存使用率（0.0-1.0）
```

## 与UE TextureStreaming的对比

### 相似之处
- 基于距离和屏幕空间的LOD计算
- 优先级驱动的加载策略
- 内存预算管理
- 异步加载支持

### 实现差异
- ZEngine使用更简化的LOD计算（UE有更复杂的屏幕空间误差计算）
- ZEngine的mip级别生成需要进一步实现（当前使用基础纹理）
- 支持自定义配置，但默认值针对ZEngine优化

## 未来改进

1. **Mip级别生成**
   - 实现完整的mip级别链生成
   - 支持从文件加载预生成的mip级别

2. **更精确的LOD计算**
   - 实现基于屏幕空间误差的LOD计算
   - 考虑纹理在屏幕上的实际投影大小

3. **纹理池优化**
   - 实现纹理池以减少分配开销
   - 支持纹理压缩格式

4. **性能优化**
   - GPU驱动的纹理流式传输
   - 更智能的预加载策略

## 文件结构

```
engine/source/runtime/function/render/texture/
├── texture_streaming_types.h          # 类型定义
├── texture_streaming_types.cpp        # 类型实现
├── texture_streaming_manager.h        # 管理器接口
└── texture_streaming_manager.cpp      # 管理器实现
```

## 集成点

- `RenderResource`: 管理纹理流式传输管理器
- `RenderSystem`: 每帧更新流式传输状态
- `RenderResourceBase`: 提供纹理加载接口

## 注意事项

1. 纹理流式传输管理器在`RenderResource`析构时自动关闭
2. 确保在渲染线程中访问纹理资源
3. 纹理加载是异步的，需要检查加载状态
4. 内存预算超出时会自动驱逐低优先级纹理

