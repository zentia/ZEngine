# ZEngine Nanite系统设计文档

## 概述

ZEngine Nanite系统是基于Unreal Engine 5的Nanite虚拟几何技术设计的虚拟化几何渲染系统。该系统能够高效渲染包含数百万三角形的复杂场景，通过自动LOD、GPU驱动的裁剪和流式加载等技术实现高性能渲染。

## 核心特性

### 1. 虚拟几何（Virtualized Geometry）
- 将传统网格数据转换为集群化的表示形式
- 支持自动层次LOD生成
- 几何数据压缩和流式传输

### 2. GPU驱动的渲染管线
- GPU执行视锥裁剪和遮挡剔除
- 减少CPU-GPU数据传输
- 提高渲染效率

### 3. 自动LOD系统
- 基于屏幕空间误差的LOD选择
- 层次化的集群结构
- 平滑的LOD过渡

### 4. 流式加载
- 根据相机位置动态加载/卸载几何数据
- 支持异步资源加载
- 内存高效管理

## 架构设计

### 核心组件

#### 1. NaniteTypes (nanite_types.h)
定义Nanite系统的核心数据结构：
- `NaniteCluster`: 集群数据结构
- `NaniteMeshResource`: Nanite网格资源
- `NaniteInstance`: 实例数据
- `NaniteVisibilityResult`: 可见性结果
- `NaniteConfig`: 配置参数

#### 2. NaniteClusterBuilder (nanite_cluster.h/cpp)
负责将传统网格转换为Nanite集群：
- 递归分割网格为集群
- 构建层次结构
- 计算集群边界和错误度量
- 优化集群组织

#### 3. NaniteClusterManager (nanite_cluster.h/cpp)
管理集群的加载和流式传输：
- 注册/注销网格资源
- 根据相机位置更新流式加载
- 管理集群的可见性状态

#### 4. NaniteCullingSystem (nanite_culling.h/cpp)
实现裁剪和可见性系统：
- CPU/GPU视锥裁剪
- 遮挡剔除（基于层次Z缓冲）
- 距离裁剪
- LOD选择算法

#### 5. NaniteRenderer (nanite_renderer.h/cpp)
Nanite渲染器主类：
- 管理Nanite实例
- 执行渲染管线
- 组织渲染批次
- 更新GPU缓冲区

#### 6. NaniteRenderPass (nanite_renderer.h/cpp)
渲染通道集成：
- 集成到ZEngine渲染管线
- 与现有渲染系统协同工作

## 数据流

```
传统网格数据
    ↓
NaniteClusterBuilder (构建集群)
    ↓
NaniteMeshResource (存储)
    ↓
NaniteClusterManager (流式加载)
    ↓
NaniteCullingSystem (裁剪和LOD选择)
    ↓
NaniteRenderer (渲染)
    ↓
GPU渲染管线
```

## 使用流程

### 1. 初始化
```cpp
auto& nanite_system = NaniteSystem::getInstance();
nanite_system.initialize(rhi, render_resource);
```

### 2. 转换网格
```cpp
NaniteClusterBuilder builder;
NaniteMeshResource mesh_resource;
builder.buildClustersFromMesh(positions, normals, uvs, indices, mesh_resource, config);
```

### 3. 注册实例
```cpp
NaniteInstance instance;
instance.instance_id = generateId();
instance.mesh_id = mesh_resource.mesh_id;
instance.transform = transform_matrix;
nanite_renderer->registerInstance(instance);
```

### 4. 渲染
```cpp
nanite_renderer->render(rhi, render_scene, camera, config);
```

## 配置参数

### NaniteConfig
- `enabled`: 是否启用Nanite
- `enable_lod`: 是否启用LOD
- `enable_culling`: 是否启用裁剪
- `enable_streaming`: 是否启用流式加载
- `lod_bias`: LOD偏移
- `max_screen_error`: 最大屏幕空间误差
- `max_visible_clusters`: 最大可见集群数
- `max_streaming_distance`: 最大流式加载距离
- `use_gpu_culling`: 是否使用GPU裁剪

## 性能优化

### 1. 集群大小优化
- 每个集群包含64-128个三角形
- 平衡渲染效率和内存使用

### 2. 层次结构优化
- 最大深度16级
- 基于空间位置的层次构建

### 3. 裁剪优化
- GPU并行裁剪
- 早期剔除不可见几何

### 4. 内存优化
- 几何数据压缩
- 按需加载
- 智能缓存管理

## 未来扩展

1. **几何压缩**: 实现更高效的几何数据压缩算法
2. **动态几何**: 支持动态Nanite几何
3. **实例化优化**: 改进实例化渲染
4. **多线程优化**: 进一步优化多线程处理
5. **硬件加速**: 利用硬件特性（如Mesh Shader）

## 参考资料

- Unreal Engine 5 Nanite技术文档
- GPU-Driven Rendering Pipelines
- Virtualized Geometry技术论文

