# ZEngine VirtualTexture系统设计文档

## 概述

ZEngine VirtualTexture系统是基于Unreal Engine的VirtualTexture技术设计的虚拟化纹理渲染系统。该系统能够高效管理大规模纹理数据，通过将纹理分割成固定大小的页（Pages），并根据视口需求动态加载和渲染这些页，从而有效管理内存并提高渲染性能。

## 核心特性

### 1. 纹理分页（Texture Paging）
- 将高分辨率纹理分割成固定大小的页（通常128x128或256x256像素）
- 支持多级Mipmap，每个Mip级别都进行分页
- 页表管理，记录每个页在物理内存中的位置

### 2. 物理纹理池（Physical Texture Pool）
- 维护一个物理纹理池，存储实际加载的纹理页
- 支持多个物理纹理池（用于不同格式或用途）
- 动态分配和释放物理纹理槽位

### 3. 需求驱动加载（Demand-Driven Loading）
- 根据相机视口和对象可见性，动态加载所需的纹理页
- 基于屏幕空间误差计算所需Mip级别
- 支持异步加载，不阻塞渲染线程

### 4. 缓存和替换策略（Cache and Replacement）
- LRU（Least Recently Used）缓存策略
- 优先级驱动的页加载和卸载
- 内存预算管理，自动驱逐低优先级页

### 5. 页表查找（Page Table Lookup）
- 高效的页表查找机制
- 支持硬件加速的页表查找（如果可用）
- 页表压缩和优化

## 架构设计

### 核心组件

#### 1. VirtualTextureTypes (virtual_texture_types.h/cpp)
定义VirtualTexture系统的核心数据结构：
- `VirtualTexturePage`: 虚拟纹理页数据结构
- `VirtualTextureResource`: 虚拟纹理资源
- `PhysicalTexturePool`: 物理纹理池
- `PageTableEntry`: 页表项
- `VirtualTextureConfig`: 配置参数

#### 2. VirtualTextureResource (virtual_texture_resource.h/cpp)
表示一个虚拟纹理资源：
- 管理虚拟纹理的页表
- 计算所需页和Mip级别
- 处理页的加载请求
- 管理虚拟纹理的元数据

#### 3. PhysicalTexturePool (virtual_texture_manager.h/cpp)
管理物理纹理池：
- 分配和释放物理纹理槽位
- 管理物理纹理的内存
- 处理页的上传和更新
- 实现LRU缓存策略

#### 4. VirtualTextureManager (virtual_texture_manager.h/cpp)
VirtualTexture系统的主管理器：
- 注册/注销虚拟纹理资源
- 根据相机位置更新页加载
- 管理物理纹理池
- 处理异步加载任务
- 更新页表

#### 5. VirtualTexturePageLoader (virtual_texture_manager.h/cpp)
处理页的异步加载：
- 从磁盘加载纹理页数据
- 生成Mip级别
- 准备上传到GPU的数据

## 数据结构

### VirtualTexturePage
```cpp
struct VirtualTexturePage
{
    uint32_t virtual_texture_id;  // 虚拟纹理ID
    uint32_t mip_level;           // Mip级别
    uint32_t page_x;              // 页的X坐标
    uint32_t page_y;              // 页的Y坐标
    uint32_t physical_slot;       // 物理槽位索引（如果已加载）
    PageState state;              // 页的状态
    float priority;               // 优先级
    uint64_t last_access_time;    // 最后访问时间（用于LRU）
};
```

### PageTableEntry
```cpp
struct PageTableEntry
{
    uint32_t physical_slot;       // 物理槽位索引
    uint32_t mip_level;           // Mip级别
    bool is_valid;                // 是否有效
};
```

### PhysicalTexturePool
```cpp
class PhysicalTexturePool
{
    RHIImage* physical_texture;        // 物理纹理
    RHIImageView* physical_texture_view; // 物理纹理视图
    uint32_t pool_width;                // 池宽度（页数）
    uint32_t pool_height;               // 池高度（页数）
    uint32_t page_size;                // 页大小（像素）
    std::vector<PhysicalSlot> slots;    // 物理槽位
};
```

## 工作流程

### 1. 初始化
```cpp
// 创建VirtualTexture管理器
m_virtual_texture_manager = std::make_shared<VirtualTextureManager>();
m_virtual_texture_manager->initialize(rhi, render_resource);

// 创建物理纹理池
m_virtual_texture_manager->createPhysicalTexturePool(
    2048, 2048,  // 池大小（页数）
    128,         // 页大小（像素）
    RHIFormat::RHI_FORMAT_R8G8B8A8_UNORM
);
```

### 2. 注册虚拟纹理
```cpp
// 注册一个虚拟纹理
uint64_t vt_handle = m_virtual_texture_manager->registerVirtualTexture(
    "path/to/large_texture.png",
    4096, 4096,  // 虚拟纹理尺寸
    128          // 页大小
);
```

### 3. 请求页加载
```cpp
// 根据相机位置和对象位置，请求加载所需的页
m_virtual_texture_manager->requestPages(
    vt_handle,
    camera_position,
    object_position,
    object_bounds,
    screen_size
);
```

### 4. 更新系统
```cpp
// 每帧更新
m_virtual_texture_manager->update(
    camera_position,
    camera_forward,
    fov,
    aspect_ratio,
    screen_width,
    screen_height,
    delta_time
);
```

### 5. 获取页表
```cpp
// 获取页表用于渲染
RHIImageView* page_table = m_virtual_texture_manager->getPageTable(vt_handle);
RHIImageView* physical_pool = m_virtual_texture_manager->getPhysicalTexturePool();
```

## 与UE VirtualTexture的对比

### 相似之处
- 基于页的纹理虚拟化
- 物理纹理池管理
- 需求驱动的页加载
- LRU缓存策略
- 页表查找机制

### 实现差异
- ZEngine使用更简化的页表结构（UE有更复杂的层次页表）
- ZEngine的页大小固定为128或256（UE支持可变页大小）
- ZEngine使用CPU驱动的页加载（UE支持GPU驱动的页加载）
- 支持自定义配置，但默认值针对ZEngine优化

## 配置参数

```cpp
struct VirtualTextureConfig
{
    uint32_t page_size;              // 页大小（像素），默认128
    uint32_t physical_pool_width;   // 物理池宽度（页数），默认2048
    uint32_t physical_pool_height;   // 物理池高度（页数），默认2048
    size_t memory_budget_mb;         // 内存预算（MB），默认512
    uint32_t max_concurrent_loads;   // 最大并发加载数，默认4
    float min_screen_size;           // 最小屏幕空间，默认0.01
    float max_distance;              // 最大距离，默认2000.0
    bool enable_async_loading;       // 启用异步加载，默认true
    bool enable_mip_streaming;       // 启用Mip流式传输，默认true
};
```

## 性能优化

### 1. 页表压缩
- 使用稀疏页表，只存储已加载的页
- 页表数据压缩

### 2. 批量加载
- 批量加载相邻的页
- 预加载可能需要的页

### 3. 异步处理
- 页加载在后台线程进行
- 页上传在渲染线程进行

### 4. 内存管理
- 智能的内存预算管理
- 及时释放不再需要的页

## 未来改进

1. **GPU驱动的页加载**
   - 使用计算着色器进行页需求分析
   - GPU驱动的页表更新

2. **层次页表**
   - 实现多级页表以支持更大的虚拟纹理
   - 页表压缩和优化

3. **硬件加速**
   - 利用硬件虚拟纹理支持（如果可用）
   - 硬件页表查找

4. **自适应页大小**
   - 根据纹理内容动态调整页大小
   - 优化内存使用

5. **压缩格式支持**
   - 支持BC/DXT压缩格式
   - 支持ASTC压缩格式

## 文件结构

```
engine/source/runtime/function/render/texture/
├── virtual_texture_types.h          # 类型定义
├── virtual_texture_types.cpp        # 类型实现
├── virtual_texture_resource.h       # 虚拟纹理资源
├── virtual_texture_resource.cpp     # 虚拟纹理资源实现
├── virtual_texture_manager.h        # 管理器接口
└── virtual_texture_manager.cpp      # 管理器实现
```

## 集成点

- `RenderResource`: 管理VirtualTexture管理器
- `RenderSystem`: 每帧更新VirtualTexture状态
- `RenderResourceBase`: 提供纹理加载接口
- `MaterialSystem`: 使用VirtualTexture进行材质渲染

## 注意事项

1. VirtualTexture管理器在`RenderResource`析构时自动关闭
2. 确保在渲染线程中访问页表和物理纹理池
3. 页加载是异步的，需要检查加载状态
4. 内存预算超出时会自动驱逐低优先级页
5. 页表需要定期更新以反映最新的页状态

