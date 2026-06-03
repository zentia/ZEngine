# ZEngine 预载系统使用指南

## 概述

ZEngine 的预载系统参考了 Unity Addressables 和 Unreal Engine 的 Streaming 系统设计，提供了高效的异步资源预载功能。该系统支持：

- **异步加载**：在后台线程加载资源，不阻塞主线程
- **优先级管理**：支持多级优先级，确保关键资源优先加载
- **依赖管理**：自动处理资源依赖关系
- **预载组**：类似 Unity Addressables Group，可以批量管理资源
- **进度追踪**：实时查询加载进度和状态

---

## 核心概念

### 1. 预载优先级

预载系统支持 5 个优先级级别：

```cpp
enum class PreloadPriority : uint8_t
{
    Critical = 0,    // 关键资源（必须立即加载）
    High     = 1,    // 高优先级（重要资源）
    Normal   = 2,    // 普通优先级（常规资源）
    Low      = 3,    // 低优先级（后台预载）
    Background = 4   // 后台加载（最低优先级）
};
```

### 2. 预载状态

每个资源都有以下状态之一：

- `Pending`：等待加载
- `Loading`：正在加载
- `Loaded`：已加载
- `Failed`：加载失败
- `Unloaded`：已卸载

### 3. 预载组（PreloadGroup）

预载组类似于 Unity Addressables 的 Group，用于批量管理一组相关资源。可以：
- 统一设置优先级
- 批量开始/停止预载
- 查询整体加载进度

---

## 基本使用

### 1. 初始化

预载系统在引擎初始化时自动启动，无需手动初始化：

```cpp
// 系统已在 RuntimeGlobalContext::startSystems() 中初始化
auto* preload_manager = g_runtime_global_context.m_preload_manager;
```

### 2. 预载单个资源

#### 异步预载（推荐）

```cpp
#include "runtime/resource/preload/preload_manager.h"

// 获取预载管理器
auto* preload_manager = PreloadManager::getInstancePtr();

// 预载资源（默认 Normal 优先级）
uint64_t request_id = preload_manager->preloadAsset("asset/texture/hero.zasset");

// 带回调的预载
request_id = preload_manager->preloadAsset(
    "asset/mesh/character.zasset",
    PreloadPriority::High,
    [](bool success) {
        if (success) {
            LOG_INFO("Character mesh loaded!");
        } else {
            LOG_ERROR("Failed to load character mesh");
        }
    }
);
```

#### 同步加载（阻塞）

```cpp
// 同步加载资源（会阻塞直到完成）
bool success = preload_manager->loadAssetSync("asset/texture/icon.zasset");
if (success) {
    // 资源已加载，可以直接使用
}
```

### 3. 预载带依赖的资源

```cpp
// 定义依赖关系
std::vector<string> dependencies = {
    "asset/material/base_material.zasset",
    "asset/texture/diffuse.zasset"
};

// 预载资源及其依赖
preload_manager->preloadAssetWithDependencies(
    "asset/mesh/model.zasset",
    dependencies,
    PreloadPriority::High,
    [](bool success) {
        // 所有依赖加载完成后，才会加载主资源
    }
);
```

### 4. 批量预载

```cpp
std::vector<string> assets = {
    "asset/texture/tex1.zasset",
    "asset/texture/tex2.zasset",
    "asset/texture/tex3.zasset"
};

// 批量预载，带进度回调
preload_manager->preloadAssets(
    assets,
    PreloadPriority::Normal,
    [](size_t loaded, size_t total) {
        float progress = static_cast<float>(loaded) / static_cast<float>(total);
        LOG_INFO("Loading progress: {:.1f}%", progress * 100.0f);
    }
);
```

---

## 预载组使用

### 1. 创建预载组

```cpp
// 创建预载组
auto group = preload_manager->createGroup("MainMenuAssets");

// 添加资源到组
group->addAsset("asset/ui/main_menu.zasset", PreloadPriority::High);
group->addAsset("asset/texture/menu_bg.zasset", PreloadPriority::Normal);
group->addAsset("asset/audio/menu_music.zasset", PreloadPriority::Low);

// 添加带依赖的资源
std::vector<string> deps = {"asset/material/ui_material.zasset"};
group->addAssetWithDependencies("asset/ui/button.zasset", deps, PreloadPriority::High);
```

### 2. 开始预载组

```cpp
// 开始预载组中的所有资源
group->startPreload();

// 检查加载进度
while (!group->isComplete()) {
    float progress = group->getProgress();
    LOG_INFO("Group progress: {:.1f}%", progress * 100.0f);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}
```

### 3. 卸载预载组

```cpp
// 卸载组中的所有资源
group->unload();

// 或删除整个组
preload_manager->removeGroup("MainMenuAssets");
```

---

## 状态查询

### 1. 查询资源状态

```cpp
// 检查资源是否已加载
if (preload_manager->isAssetLoaded("asset/texture/hero.zasset")) {
    // 资源已加载，可以使用
}

// 获取详细状态
PreloadStatus status = preload_manager->getAssetStatus("asset/texture/hero.zasset");
switch (status) {
    case PreloadStatus::Loaded:
        // 已加载
        break;
    case PreloadStatus::Loading:
        // 正在加载
        break;
    case PreloadStatus::Failed:
        // 加载失败
        break;
    // ...
}

// 获取加载进度 [0.0, 1.0]
float progress = preload_manager->getLoadProgress("asset/texture/hero.zasset");
```

### 2. 获取统计信息

```cpp
auto stats = preload_manager->getStatistics();
LOG_INFO("Total requests: {}", stats.total_requests);
LOG_INFO("Pending: {}", stats.pending_requests);
LOG_INFO("Loading: {}", stats.loading_requests);
LOG_INFO("Loaded: {}", stats.loaded_requests);
LOG_INFO("Failed: {}", stats.failed_requests);
LOG_INFO("Active loads: {}", stats.active_loads);
```

---

## 高级功能

### 1. 取消预载

```cpp
// 取消预载请求
uint64_t request_id = preload_manager->preloadAsset("asset/texture/test.zasset");
bool cancelled = preload_manager->cancelPreload(request_id);
```

### 2. 暂停/恢复预载

```cpp
// 暂停所有预载
preload_manager->pausePreload();

// 恢复预载
preload_manager->resumePreload();
```

### 3. 设置最大并发加载数

```cpp
// 限制同时加载的资源数量（默认 4）
preload_manager->setMaxConcurrentLoads(8);
```

### 4. 卸载资源

```cpp
// 卸载单个资源
preload_manager->unloadAsset("asset/texture/hero.zasset");

// 卸载所有资源
preload_manager->unloadAll();
```

---

## 实际应用场景

### 场景 1：关卡加载前预载

```cpp
void preloadLevelAssets(const string& level_url) {
    auto* preload_manager = PreloadManager::getInstancePtr();
    
    // 创建关卡预载组
    auto level_group = preload_manager->createGroup("Level_" + level_url);
    
    // 预载关卡资源
    level_group->addAsset(level_url, PreloadPriority::Critical);
    
    // 预载关卡依赖的资源
    level_group->addAsset("asset/texture/level_terrain.zasset", PreloadPriority::High);
    level_group->addAsset("asset/mesh/level_geometry.zasset", PreloadPriority::High);
    level_group->addAsset("asset/audio/level_ambient.zasset", PreloadPriority::Normal);
    
    // 开始预载
    level_group->startPreload();
    
    // 等待加载完成
    while (!level_group->isComplete()) {
        float progress = level_group->getProgress();
        updateLoadingScreen(progress);
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    
    // 加载完成，可以进入关卡
    LOG_INFO("Level assets preloaded!");
}
```

### 场景 2：按需预载（流式加载）

```cpp
void onPlayerEnterRegion(const Vector3& position) {
    auto* preload_manager = PreloadManager::getInstancePtr();
    
    // 根据玩家位置预载附近区域资源
    string region_id = getRegionId(position);
    
    // 低优先级后台预载
    preload_manager->preloadAsset(
        "asset/region/" + region_id + ".zasset",
        PreloadPriority::Low
    );
}
```

### 场景 3：UI 资源预载

```cpp
void preloadUIAssets() {
    auto* preload_manager = PreloadManager::getInstancePtr();
    auto ui_group = preload_manager->createGroup("UIAssets");
    
    // UI 资源通常需要快速加载
    ui_group->addAsset("asset/ui/main_menu.zasset", PreloadPriority::High);
    ui_group->addAsset("asset/ui/settings.zasset", PreloadPriority::Normal);
    ui_group->addAsset("asset/ui/inventory.zasset", PreloadPriority::Normal);
    
    ui_group->startPreload();
}
```

---

## 与现有系统集成

### 与 AssetManager 集成

预载系统内部使用 `AssetManager` 加载资源。预载完成后，资源可以通过 `AssetManager` 正常访问：

```cpp
// 预载资源
preload_manager->preloadAsset("asset/texture/hero.zasset", PreloadPriority::High);

// 等待加载完成
while (!preload_manager->isAssetLoaded("asset/texture/hero.zasset")) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

// 使用 AssetManager 加载（此时应该很快，因为已预载）
TextureRes texture;
g_runtime_global_context.m_asset_manager->loadAsset("asset/texture/hero.zasset", texture);
```

### 与 WorldManager 集成

可以在加载关卡前预载关卡资源：

```cpp
void loadLevelWithPreload(const string& level_url) {
    auto* preload_manager = PreloadManager::getInstancePtr();
    
    // 预载关卡资源
    preload_manager->preloadAsset(level_url, PreloadPriority::Critical);
    
    // 等待预载完成
    while (!preload_manager->isAssetLoaded(level_url)) {
        // 显示加载进度
        float progress = preload_manager->getLoadProgress(level_url);
        updateLoadingUI(progress);
    }
    
    // 加载关卡（此时资源已在内存中）
    g_runtime_global_context.m_world_manager->loadLevel(level_url);
}
```

---

## 性能优化建议

1. **合理设置优先级**：关键资源使用 `Critical` 或 `High`，非关键资源使用 `Low` 或 `Background`
2. **使用预载组**：批量管理相关资源，便于统一控制
3. **控制并发数**：根据硬件性能调整 `setMaxConcurrentLoads()`
4. **及时卸载**：不再使用的资源及时卸载，释放内存
5. **依赖管理**：正确设置依赖关系，避免重复加载

---

## 注意事项

1. **资源路径**：使用相对路径，与 `AssetManager` 的路径格式一致
2. **线程安全**：预载系统是线程安全的，可以在任何线程调用
3. **回调执行**：回调函数在加载线程执行，注意线程安全
4. **内存管理**：预载的资源会占用内存，注意及时卸载不需要的资源
5. **错误处理**：检查加载状态，处理加载失败的情况

---

## API 参考

### PreloadManager 主要接口

| 方法 | 说明 |
|------|------|
| `preloadAsset()` | 异步预载单个资源 |
| `preloadAssetWithDependencies()` | 预载资源及其依赖 |
| `loadAssetSync()` | 同步加载资源 |
| `createGroup()` | 创建预载组 |
| `getGroup()` | 获取预载组 |
| `preloadAssets()` | 批量预载资源 |
| `isAssetLoaded()` | 检查资源是否已加载 |
| `getAssetStatus()` | 获取资源状态 |
| `getLoadProgress()` | 获取加载进度 |
| `unloadAsset()` | 卸载资源 |
| `cancelPreload()` | 取消预载请求 |
| `pausePreload()` / `resumePreload()` | 暂停/恢复预载 |
| `setMaxConcurrentLoads()` | 设置最大并发加载数 |
| `tick()` | 每帧更新（系统自动调用） |
| `getStatistics()` | 获取统计信息 |

### PreloadGroup 主要接口

| 方法 | 说明 |
|------|------|
| `addAsset()` | 添加资源到组 |
| `addAssetWithDependencies()` | 添加带依赖的资源 |
| `startPreload()` | 开始预载组中的所有资源 |
| `unload()` | 卸载组中的所有资源 |
| `getProgress()` | 获取加载进度 |
| `isComplete()` | 检查是否全部加载完成 |

---

## 参考

- Unity Addressables: https://docs.unity3d.com/Packages/com.unity.addressables@latest
- Unreal Engine Streaming: https://docs.unrealengine.com/en-US/StreamingLevels/index.html

