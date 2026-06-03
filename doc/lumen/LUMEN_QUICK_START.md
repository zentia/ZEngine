# ZEngine Lumen系统快速开始指南

## 概述

ZEngine Lumen系统是基于Unreal Engine 5的Lumen技术设计的实时全局光照和反射系统。本指南将帮助您快速开始使用Lumen系统。

## 系统架构

Lumen系统由以下核心组件组成：

1. **距离场系统（Distance Field）**: 用于软件光线追踪
2. **表面缓存系统（Surface Cache）**: 存储场景表面信息
3. **光线追踪系统（Ray Tracing）**: 支持硬件和软件光线追踪
4. **全局光照系统（Global Illumination）**: 计算间接光照
5. **反射系统（Reflections）**: 计算反射效果

## 基本使用

### 1. 初始化

Lumen系统已经在渲染管线中自动初始化。您可以通过渲染管线访问Lumen系统：

```cpp
#include "runtime/function/render/lumen/lumen_render_pass.h"

// 获取Lumen渲染Pass
auto render_pipeline = std::static_pointer_cast<RenderPipeline>(m_render_pipeline);
auto lumen_pass = std::static_pointer_cast<LumenRenderPass>(render_pipeline->m_lumen_pass);

// 获取Lumen渲染器
auto lumen_renderer = lumen_pass->getLumenRenderer();
```

### 2. 配置Lumen

您可以通过修改Lumen配置来调整系统行为：

```cpp
auto& config = lumen_renderer->getConfig();

// 启用/禁用功能
config.enabled = true;
config.enable_global_illumination = true;
config.enable_reflections = true;

// 设置光线追踪类型
config.use_hardware_ray_tracing = false; // 默认使用软件追踪
config.use_software_ray_tracing = true;
config.ray_tracing_type = RayTracingType::Software;

// 设置质量
config.quality = LumenQuality::High;

// 调整追踪参数
config.max_trace_distance = 10000.0f;
config.max_reflection_bounces = 4;
```

### 3. 系统更新

Lumen系统会在每帧自动更新。您不需要手动调用更新函数，系统会在渲染管线的`preparePassData`阶段自动更新。

### 4. 获取结果

Lumen系统计算的结果（全局光照和反射）会自动应用到渲染管线中。您可以通过以下方式访问：

```cpp
// 获取全局光照
auto gi_system = lumen_renderer->getGlobalIllumination();
Vector3 indirect_lighting = gi_system->getIndirectLighting(world_position, normal);

// 获取反射
auto reflection_system = lumen_renderer->getReflections();
LumenReflectionData reflection = reflection_system->computeReflection(
    world_position, normal, view_direction, roughness, ray_tracing);
```

## 配置选项

### LumenConfig结构

```cpp
struct LumenConfig
{
    // 基本设置
    bool enabled = true;
    bool enable_global_illumination = true;
    bool enable_reflections = true;
    
    // 光线追踪设置
    bool use_hardware_ray_tracing = false;
    bool use_software_ray_tracing = true;
    RayTracingType ray_tracing_type = RayTracingType::Software;
    
    // 追踪参数
    float max_trace_distance = 10000.0f;
    uint32_t max_reflection_bounces = 4;
    
    // 表面缓存设置
    uint32_t surface_cache_resolution = 512;
    uint32_t surface_cache_page_size = 64;
    
    // 距离场设置
    uint32_t distance_field_resolution = 256;
    bool use_hierarchical_distance_field = true;
    
    // 质量设置
    LumenQuality quality = LumenQuality::High;
    
    // 性能设置
    bool enable_temporal_accumulation = true;
    bool enable_spatial_reuse = true;
    uint32_t gi_sample_count = 1;
    uint32_t reflection_sample_count = 1;
};
```

## 性能优化建议

1. **根据硬件调整质量**: 如果硬件性能较低，可以降低质量等级或减少采样次数
2. **使用软件光线追踪**: 如果硬件不支持光线追踪，系统会自动使用软件追踪
3. **调整追踪距离**: 根据场景大小调整`max_trace_distance`以减少计算量
4. **启用时间累积**: 启用`enable_temporal_accumulation`可以减少噪声并提高性能
5. **启用空间复用**: 启用`enable_spatial_reuse`可以复用附近的光照计算结果

## 调试功能

Lumen系统提供了调试可视化功能：

```cpp
auto& config = lumen_renderer->getConfig();

// 可视化距离场
config.debug_visualize_distance_field = true;

// 可视化表面缓存
config.debug_visualize_surface_cache = true;

// 可视化光线追踪
config.debug_visualize_ray_tracing = true;
```

## 统计信息

您可以获取Lumen系统的性能统计信息：

```cpp
const auto& stats = lumen_renderer->getStats();

// 距离场统计
uint32_t df_update_count = stats.distance_field_update_count;
float df_update_time = stats.distance_field_update_time_ms;

// 光线追踪统计
uint32_t rt_count = stats.ray_tracing_count;
float rt_time = stats.ray_tracing_time_ms;

// 内存使用
uint64_t total_memory = stats.total_memory_bytes;
```

## 注意事项

1. **首次运行**: 首次运行时，距离场和表面缓存的生成可能需要一些时间
2. **动态场景**: Lumen系统支持动态场景，但频繁的几何变化可能会影响性能
3. **内存使用**: 距离场和表面缓存会占用一定的内存，请根据场景大小调整分辨率
4. **硬件要求**: 硬件光线追踪需要支持Vulkan光线追踪扩展的GPU

## 下一步

- 查看[Lumen系统设计文档](LUMEN_SYSTEM_DESIGN.md)了解详细的技术细节
- 参考示例代码学习如何使用Lumen系统
- 根据您的需求调整配置参数以获得最佳性能和效果

## 常见问题

**Q: Lumen系统会影响性能吗？**
A: 是的，Lumen系统会增加一定的计算开销。您可以通过调整质量设置和采样次数来平衡性能和效果。

**Q: 如何禁用Lumen系统？**
A: 设置`config.enabled = false`即可禁用Lumen系统。

**Q: 支持硬件光线追踪吗？**
A: 系统支持硬件光线追踪，但需要GPU支持Vulkan光线追踪扩展。如果不支持，系统会自动使用软件光线追踪。

**Q: 如何优化Lumen性能？**
A: 可以降低质量等级、减少采样次数、调整追踪距离、启用时间累积和空间复用等。

