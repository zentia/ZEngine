# ZEngine 预载系统设计文档

## 概述

ZEngine 预载系统参考了 Unity Addressables 和 Unreal Engine Streaming 系统的设计理念，提供了高效的异步资源预载功能。该系统旨在优化资源加载性能，减少运行时卡顿，提升用户体验。

---

## 设计目标

1. **异步加载**：在后台线程加载资源，不阻塞主线程
2. **优先级管理**：支持多级优先级，确保关键资源优先加载
3. **依赖管理**：自动处理资源依赖关系，确保加载顺序正确
4. **批量管理**：通过预载组批量管理相关资源
5. **进度追踪**：实时查询加载进度和状态
6. **灵活控制**：支持暂停、恢复、取消等操作

---

## 架构设计

### 核心组件

```
┌─────────────────────────────────────────┐
│         PreloadManager                  │
│  (Singleton, 全局预载管理器)              │
├─────────────────────────────────────────┤
│  - 请求管理 (PreloadRequest)            │
│  - 预载组管理 (PreloadGroup)            │
│  - 加载队列 (TaskQueue)                 │
│  - 依赖关系处理                          │
│  - 状态追踪                              │
└─────────────────────────────────────────┘
           │                    │
           │                    │
    ┌──────▼──────┐      ┌──────▼──────┐
    │PreloadGroup │      │LoadingThread │
    │  (预载组)    │      │ (加载线程)   │
    └─────────────┘      └─────────────┘
           │                    │
           │                    │
    ┌──────▼────────────────────▼──────┐
    │      AssetManager                 │
    │  (实际资源加载)                     │
    └───────────────────────────────────┘
```

### 类层次结构

```
PreloadManager (Singleton)
├── PreloadRequest (请求)
│   ├── asset_url
│   ├── priority
│   ├── status
│   ├── dependencies
│   └── callback
│
└── PreloadGroup (预载组)
    ├── group_name
    ├── asset_urls
    ├── priorities
    └── dependencies
```

---

## 核心特性

### 1. 优先级系统

参考 Unity Addressables 和 UE Streaming 的优先级设计：

- **Critical**：关键资源，必须立即加载（如启动必需资源）
- **High**：高优先级资源（如当前关卡资源）
- **Normal**：普通优先级（常规资源）
- **Low**：低优先级（后台预载）
- **Background**：后台加载（最低优先级，不影响游戏体验）

优先级映射到 `TaskQueue` 的优先级：
- Critical/High → TaskPriority::high
- Normal → TaskPriority::normal
- Low/Background → TaskPriority::low

### 2. 依赖管理

支持资源依赖关系：

```cpp
// 资源 A 依赖资源 B 和 C
// 系统会确保 B 和 C 加载完成后，才加载 A
preload_manager->preloadAssetWithDependencies(
    "asset/mesh/model.zasset",
    {"asset/material/base.zasset", "asset/texture/diffuse.zasset"},
    PreloadPriority::High
);
```

依赖检查机制：
- 加载前检查所有依赖是否已加载
- 如果依赖未就绪，请求重新入队
- 依赖加载完成后，自动触发依赖它的资源加载

### 3. 预载组（PreloadGroup）

类似 Unity Addressables Group，用于批量管理资源：

```cpp
auto group = preload_manager->createGroup("Level1Assets");
group->addAsset("asset/level/level1.zasset", PreloadPriority::High);
group->addAsset("asset/texture/level1_tex.zasset", PreloadPriority::Normal);
group->startPreload(); // 批量开始预载
```

优势：
- 统一管理相关资源
- 统一控制加载/卸载
- 统一查询进度

### 4. 异步加载队列

使用 `TaskQueue` 实现异步加载：

- 后台线程处理加载任务
- 支持优先级排序
- 支持并发控制（最大并发加载数）
- 线程安全

### 5. 状态管理

每个资源都有明确的状态：

- **Pending**：等待加载（在队列中）
- **Loading**：正在加载（正在处理）
- **Loaded**：已加载（可以使用）
- **Failed**：加载失败（需要重试或处理错误）
- **Unloaded**：已卸载（已从内存移除）

---

## 实现细节

### 1. 线程模型

```
主线程 (Game Thread)
  │
  ├─> PreloadManager::tick()  // 更新统计信息
  │
  └─> 用户调用预载接口
       │
       └─> 添加到加载队列
            │
            └─> 加载线程 (Loading Thread)
                 │
                 └─> 处理加载任务
                      │
                      └─> AssetManager::loadAsset()
```

### 2. 加载流程

```
1. 用户调用 preloadAsset()
   │
2. 创建 PreloadRequest
   │
3. 添加到 TaskQueue（按优先级）
   │
4. 加载线程取出任务
   │
5. 检查依赖是否就绪
   │  ├─> 否：重新入队
   │  └─> 是：继续
   │
6. 调用 loadAssetInternal()
   │
7. 更新状态为 Loaded/Failed
   │
8. 调用回调函数
   │
9. 通知等待此资源的其他请求
```

### 3. 依赖处理

```cpp
// 依赖检查
bool checkDependenciesReady(const PreloadRequest& request) {
    for (const auto& dep : request.dependencies) {
        if (!isAssetLoaded(dep)) {
            return false; // 依赖未就绪
        }
    }
    return true; // 所有依赖已就绪
}

// 依赖加载完成后，重新处理队列
void onDependencyLoaded(const string& dep_url) {
    // 查找所有等待此依赖的请求
    for (auto& request : m_requests) {
        if (request->status == PreloadStatus::Pending) {
            if (hasDependency(request, dep_url)) {
                // 重新入队处理
                m_loading_queue.enqueue(...);
            }
        }
    }
}
```

### 4. 并发控制

```cpp
// 限制同时加载的资源数量
std::atomic<size_t> m_active_load_count;
std::atomic<size_t> m_max_concurrent_loads;

void processLoadingQueue() {
    if (m_active_load_count >= m_max_concurrent_loads) {
        return; // 达到并发限制，等待
    }
    
    Task task;
    if (m_loading_queue.dequeue(task)) {
        m_active_load_count++;
        task.function(); // 执行加载
        m_active_load_count--;
    }
}
```

---

## 与现有系统集成

### 1. 与 AssetManager 集成

预载系统使用 `AssetManager` 进行实际资源加载：

```cpp
bool PreloadManager::loadAssetInternal(const string& asset_url) {
    // 检查资源文件是否存在
    auto asset_path = m_asset_manager->getFullPath(asset_url);
    if (!std::filesystem::exists(asset_path)) {
        return false;
    }
    
    // 可以扩展为实际加载逻辑
    // 由于 AssetManager 使用模板，需要根据资源类型加载
    return true;
}
```

**注意**：由于 `AssetManager` 使用模板方法，实际资源加载需要知道资源类型。预载系统的主要作用是：
- 管理加载队列和优先级
- 处理依赖关系
- 提供加载状态查询

实际资源加载仍然由 `AssetManager` 在需要时完成。

### 2. 与 WorldManager 集成

可以在加载关卡前预载关卡资源：

```cpp
void WorldManager::loadLevel(const string& level_url) {
    // 预载关卡资源
    if (m_preload_manager) {
        m_preload_manager->preloadAsset(level_url, PreloadPriority::Critical);
    }
    
    // 加载关卡
    // ...
}
```

### 3. 与全局上下文集成

预载系统在 `RuntimeGlobalContext` 中初始化：

```cpp
void RuntimeGlobalContext::startSystems() {
    // ...
    m_preload_manager = PreloadManager::getInstancePtr();
    m_preload_manager->initialize();
    // ...
}
```

在引擎主循环中更新：

```cpp
void Engine::logicalTick(float delta_time) {
    if (g_runtime_global_context.m_preload_manager) {
        g_runtime_global_context.m_preload_manager->tick(delta_time);
    }
    // ...
}
```

---

## 性能优化

### 1. 优先级调度

- 关键资源优先加载，减少等待时间
- 低优先级资源在后台加载，不影响游戏体验

### 2. 并发控制

- 限制同时加载的资源数量，避免内存峰值
- 可根据硬件性能调整并发数

### 3. 依赖优化

- 自动处理依赖关系，避免重复加载
- 依赖加载完成后立即触发依赖它的资源加载

### 4. 内存管理

- 支持资源卸载，释放内存
- 预载组统一管理，便于批量卸载

---

## 扩展性

### 1. 资源类型扩展

可以通过扩展 `loadAssetInternal()` 支持不同类型的资源：

```cpp
bool PreloadManager::loadAssetInternal(const string& asset_url) {
    // 根据文件扩展名或元数据判断资源类型
    if (isTexture(asset_url)) {
        return loadTextureInternal(asset_url);
    } else if (isMesh(asset_url)) {
        return loadMeshInternal(asset_url);
    }
    // ...
}
```

### 2. 缓存机制

可以添加资源缓存：

```cpp
class PreloadManager {
    // 资源缓存
    std::unordered_map<string, std::shared_ptr<ResourceCache>> m_cache;
    
    // 预读文件到内存
    bool preloadToCache(const string& asset_url) {
        // 读取文件内容到内存缓存
        // 后续加载时直接从缓存读取
    }
};
```

### 3. 进度追踪

可以扩展为更详细的进度追踪：

```cpp
struct LoadProgress {
    size_t bytes_loaded;
    size_t bytes_total;
    float progress; // [0.0, 1.0]
};
```

---

## 参考设计

### Unity Addressables

- **Addressable Group**：类似我们的 `PreloadGroup`
- **优先级系统**：Critical、High、Normal、Low
- **依赖管理**：自动处理资源依赖
- **异步加载**：使用 `AsyncOperation`

### Unreal Engine Streaming

- **Level Streaming**：类似我们的预载组
- **优先级系统**：Always Loaded、Load on Demand
- **依赖管理**：自动处理关卡依赖
- **异步加载**：使用 `FStreamableManager`

---

## 总结

ZEngine 预载系统提供了：

1. ✅ **异步加载**：后台线程加载，不阻塞主线程
2. ✅ **优先级管理**：5 级优先级，灵活控制
3. ✅ **依赖管理**：自动处理资源依赖关系
4. ✅ **预载组**：批量管理相关资源
5. ✅ **进度追踪**：实时查询加载状态和进度
6. ✅ **灵活控制**：暂停、恢复、取消等操作
7. ✅ **线程安全**：多线程环境下安全使用
8. ✅ **易于扩展**：可扩展支持不同类型资源

该系统为 ZEngine 提供了强大的资源预载能力，可以有效优化游戏加载性能，提升用户体验。

