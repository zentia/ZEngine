# ZEngine Lumen系统设计文档

## 概述

ZEngine Lumen系统是基于Unreal Engine 5的Lumen全局光照技术设计的实时全局光照和反射系统。该系统能够提供高质量的动态全局光照、反射和间接光照效果，通过硬件加速光线追踪和软件光线追踪（基于距离场）实现实时渲染。

## 核心特性

### 1. 全局光照（Global Illumination）
- 实时动态间接光照
- 多反射间接光照
- 支持动态光源和几何体
- 高质量的光照传播

### 2. 反射（Reflections）
- 屏幕空间反射（Screen Space Reflections）
- 距离场反射（Distance Field Reflections）
- 硬件光线追踪反射（Hardware Ray Tracing Reflections）
- 混合反射策略

### 3. 距离场（Distance Fields）
- 全局距离场（Global Distance Field）
- 层次距离场（Hierarchical Distance Fields）
- 距离场生成和更新
- 软件光线追踪支持

### 4. 表面缓存（Surface Cache）
- 场景表面信息缓存
- 光照信息存储
- 动态更新机制
- 多级细节（LOD）支持

### 5. 光线追踪
- 硬件加速光线追踪（如果支持）
- 软件光线追踪（基于距离场）
- 混合追踪策略
- 性能自适应

## 架构设计

### 核心组件

#### 1. LumenTypes (lumen_types.h)
定义Lumen系统的核心数据结构：
- `LumenConfig`: Lumen配置参数
- `LumenSurfaceCache`: 表面缓存数据
- `LumenDistanceField`: 距离场数据
- `LumenRayTracingResult`: 光线追踪结果
- `LumenGlobalIllumination`: 全局光照数据

#### 2. LumenDistanceField (lumen_distance_field.h/cpp)
距离场系统：
- 生成全局距离场
- 构建层次距离场
- 距离场更新和流式加载
- 距离场查询接口

#### 3. LumenSurfaceCache (lumen_surface_cache.h/cpp)
表面缓存系统：
- 构建表面缓存
- 更新表面信息
- 管理缓存生命周期
- 多级细节支持

#### 4. LumenRayTracing (lumen_ray_tracing.h/cpp)
光线追踪系统：
- 硬件光线追踪（如果支持）
- 软件光线追踪（基于距离场）
- 混合追踪策略
- 性能优化

#### 5. LumenGlobalIllumination (lumen_global_illumination.h/cpp)
全局光照计算：
- 间接光照计算
- 多反射传播
- 光照缓存
- 时间累积

#### 6. LumenReflections (lumen_reflections.h/cpp)
反射系统：
- 屏幕空间反射
- 距离场反射
- 硬件光线追踪反射
- 反射混合

#### 7. LumenRenderer (lumen_renderer.h/cpp)
Lumen渲染器主类：
- 管理所有Lumen子系统
- 执行渲染管线
- 与渲染系统集成
- 性能监控

#### 8. LumenRenderPass (lumen_render_pass.h/cpp)
渲染通道集成：
- 集成到ZEngine渲染管线
- 与现有渲染系统协同工作
- 渲染目标管理

## 数据流

```
场景几何数据
    ↓
LumenDistanceField (生成距离场)
    ↓
LumenSurfaceCache (构建表面缓存)
    ↓
LumenRayTracing (光线追踪)
    ↓
LumenGlobalIllumination (计算全局光照)
    ↓
LumenReflections (计算反射)
    ↓
LumenRenderPass (渲染输出)
    ↓
最终渲染结果
```

## 使用流程

### 1. 初始化
```cpp
auto& lumen_system = LumenSystem::getInstance();
LumenConfig config;
config.enable_global_illumination = true;
config.enable_reflections = true;
config.use_hardware_ray_tracing = true;
lumen_system.initialize(rhi, render_resource, config);
```

### 2. 更新距离场
```cpp
lumen_system.updateDistanceField(render_scene, camera);
```

### 3. 更新表面缓存
```cpp
lumen_system.updateSurfaceCache(render_scene, camera);
```

### 4. 计算全局光照
```cpp
lumen_system.computeGlobalIllumination(render_scene, camera, delta_time);
```

### 5. 计算反射
```cpp
lumen_system.computeReflections(render_scene, camera, gbuffer);
```

### 6. 渲染
```cpp
lumen_system.render(rhi, render_scene, camera);
```

## 配置参数

### LumenConfig
- `enabled`: 是否启用Lumen
- `enable_global_illumination`: 是否启用全局光照
- `enable_reflections`: 是否启用反射
- `use_hardware_ray_tracing`: 是否使用硬件光线追踪
- `use_software_ray_tracing`: 是否使用软件光线追踪
- `max_trace_distance`: 最大追踪距离
- `max_reflection_bounces`: 最大反射次数
- `surface_cache_resolution`: 表面缓存分辨率
- `distance_field_resolution`: 距离场分辨率
- `quality`: 质量等级（Low/Medium/High/Epic）

## 性能优化

### 1. 距离场优化
- 层次化距离场
- 按需更新
- 流式加载
- 压缩存储

### 2. 表面缓存优化
- 多级细节
- 动态更新
- 智能缓存管理
- 内存优化

### 3. 光线追踪优化
- 自适应采样
- 早期终止
- 层次加速结构
- 性能自适应

### 4. 全局光照优化
- 时间累积
- 空间复用
- 降采样计算
- 缓存复用

## 技术细节

### 距离场生成
- 使用有符号距离场（SDF）表示场景几何
- 支持层次化表示以提高查询效率
- 动态更新机制支持动态几何

### 表面缓存
- 存储场景表面的材质和几何信息
- 支持多级细节以提高性能
- 动态更新以支持动态场景

### 软件光线追踪
- 使用距离场进行快速光线追踪
- 支持层次遍历以提高效率
- 自适应步长优化

### 硬件光线追踪
- 利用Vulkan光线追踪扩展
- 构建加速结构（BLAS/TLAS）
- 高效的光线追踪管线

### 全局光照
- 使用光线追踪计算间接光照
- 多反射传播
- 时间累积以减少噪声
- 空间复用以提高性能

### 反射
- 混合多种反射技术
- 屏幕空间反射用于近距离
- 距离场反射用于中距离
- 硬件光线追踪用于远距离

## 未来扩展

1. **动态全局光照**: 支持完全动态的场景
2. **体积光照**: 支持体积雾和体积光照
3. **半透明支持**: 支持半透明物体的全局光照
4. **性能优化**: 进一步优化性能和内存使用
5. **质量提升**: 提高光照质量和反射精度

## 参考资料

- Unreal Engine 5 Lumen技术文档
- Real-Time Global Illumination技术论文
- Signed Distance Fields技术
- Hardware Ray Tracing技术

