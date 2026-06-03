# Global Context 实现分析与改进建议

## 当前实现的问题分析

### 1. **所有权管理不一致**
- **问题**：混合使用单例模式、`shared_ptr` 和原始指针
  - 单例：`ConfigManager`, `AssetManager`, `WindowSystem`, `PreloadManager`, `ModuleManager`
  - `shared_ptr`：`FileSystem`, `WorldManager`, `PhysicsManager`, `InputSystem`, `ParticleManager`, `DebugDrawManager`, `RenderDebugConfig`, `RHI`, `TaskGraph`
  - 原始指针：`RenderSystem`, `CanvasManager`
  - 全局变量：`g_thread_manager`

- **影响**：
  - 难以追踪资源生命周期
  - 可能导致内存泄漏或悬空指针
  - 测试困难（单例难以 mock）

### 2. **初始化顺序硬编码**
- **问题**：所有初始化逻辑都在 `startSystems()` 中硬编码
- **影响**：
  - 难以维护和扩展
  - 添加新系统需要修改核心代码
  - 依赖关系不明确

### 3. **错误处理不足**
- **问题**：如果某个系统初始化失败，没有清理已初始化的系统
- **影响**：可能导致资源泄漏或程序崩溃

### 4. **缺乏依赖关系管理**
- **问题**：系统之间的依赖关系通过初始化顺序隐式表达
- **影响**：
  - 难以理解系统依赖
  - 容易出错（顺序错误导致崩溃）
  - 难以并行初始化独立系统

### 5. **Shutdown 顺序可能不精确**
- **问题**：虽然大致是反向顺序，但可能不够精确
- **影响**：可能导致资源清理顺序错误

## Unity 和 Unreal 的做法参考

### Unity 的做法
1. **模块化系统**：每个系统都是独立的模块
2. **生命周期管理**：明确的初始化阶段（Awake, Start, Update, etc.）
3. **依赖注入**：通过构造函数或属性注入依赖
4. **ScriptableObject**：用于配置和全局数据

### Unreal Engine 的做法
1. **Subsystem 系统**：
   - 每个子系统继承自 `UEngineSubsystem` 或 `UGameInstanceSubsystem`
   - 自动管理初始化和关闭顺序
   - 使用依赖关系图来管理初始化顺序

2. **生命周期管理**：
   ```cpp
   virtual void Initialize(FSubsystemCollectionBase& Collection) override;
   virtual void Deinitialize() override;
   ```

3. **依赖声明**：
   - 通过模板参数声明依赖
   - 引擎自动解析依赖关系

## 改进建议

### 方案 1：统一系统接口（推荐）

创建一个统一的系统接口，所有系统都实现这个接口：

```cpp
// engine/source/runtime/function/global/engine_system.h
#pragma once

#include <string>
#include <vector>
#include <memory>

namespace Z
{
    enum class SystemInitPhase
    {
        PreInit,      // 最早初始化（配置、文件系统等）
        Core,         // 核心系统（线程、内存等）
        Resource,     // 资源系统（AssetManager, PreloadManager）
        Platform,     // 平台系统（Window, Input）
        Rendering,    // 渲染系统（RHI, RenderSystem）
        Gameplay,     // 游戏系统（World, Physics, Particle）
        PostInit      // 最后初始化（ModuleManager）
    };

    class IEngineSystem
    {
    public:
        virtual ~IEngineSystem() = default;
        
        // 获取系统名称
        virtual std::string getName() const = 0;
        
        // 获取初始化阶段
        virtual SystemInitPhase getInitPhase() const = 0;
        
        // 获取依赖的系统名称列表
        virtual std::vector<std::string> getDependencies() const { return {}; }
        
        // 初始化系统
        virtual bool initialize() = 0;
        
        // 关闭系统
        virtual void shutdown() = 0;
        
        // 获取系统是否已初始化
        bool isInitialized() const { return m_initialized; }
        
    protected:
        bool m_initialized = false;
    };
}
```

### 方案 2：系统注册机制

使用注册机制来管理所有系统：

```cpp
// engine/source/runtime/function/global/system_registry.h
#pragma once

#include "engine_system.h"
#include <unordered_map>
#include <vector>
#include <memory>

namespace Z
{
    class SystemRegistry
    {
    public:
        static SystemRegistry& getInstance();
        
        // 注册系统
        void registerSystem(std::shared_ptr<IEngineSystem> system);
        
        // 初始化所有系统（自动处理依赖关系）
        bool initializeAll();
        
        // 关闭所有系统（按相反顺序）
        void shutdownAll();
        
        // 获取系统
        template<typename T>
        std::shared_ptr<T> getSystem() const;
        
    private:
        // 拓扑排序，确定初始化顺序
        std::vector<std::shared_ptr<IEngineSystem>> topologicalSort();
        
        std::vector<std::shared_ptr<IEngineSystem>> m_systems;
        std::unordered_map<std::string, std::shared_ptr<IEngineSystem>> m_system_map;
    };
}
```

### 方案 3：改进的 GlobalContext

```cpp
// engine/source/runtime/function/global/global_context.h (改进版)
#pragma once

#include "system_registry.h"
#include <memory>

namespace Z
{
    class RuntimeGlobalContext
    {
    public:
        RuntimeGlobalContext();
        ~RuntimeGlobalContext();
        
        // 初始化所有系统
        bool startSystems();
        
        // 关闭所有系统
        void shutdownSystems();
        
        // 获取系统（类型安全）
        template<typename T>
        std::shared_ptr<T> getSystem() const
        {
            return m_registry.getSystem<T>();
        }
        
        // 向后兼容的访问器
        std::shared_ptr<InputSystem> getInputSystem() const;
        std::shared_ptr<FileSystem> getFileSystem() const;
        // ... 其他系统
        
    private:
        SystemRegistry m_registry;
        bool m_initialized = false;
    };
}
```

## 具体改进步骤

### 步骤 1：统一所有权管理
- 所有系统都使用 `shared_ptr` 或 `unique_ptr`
- 移除单例模式，改为通过 GlobalContext 访问
- 统一资源管理方式

### 步骤 2：实现系统接口
- 为每个系统实现 `IEngineSystem` 接口
- 明确声明依赖关系
- 实现错误处理

### 步骤 3：实现依赖关系管理
- 使用拓扑排序确定初始化顺序
- 支持并行初始化独立系统
- 验证依赖关系完整性

### 步骤 4：改进错误处理
- 初始化失败时自动清理
- 提供详细的错误信息
- 支持部分初始化回滚

### 步骤 5：添加测试支持
- 支持系统 mock
- 支持独立测试单个系统
- 支持依赖注入

## 迁移建议

1. **渐进式迁移**：先实现新接口，保持旧接口兼容
2. **逐个系统迁移**：一次迁移一个系统，确保稳定性
3. **添加单元测试**：为每个系统添加测试
4. **文档更新**：更新系统依赖关系文档

## 总结

当前实现虽然功能完整，但在可维护性、扩展性和错误处理方面有改进空间。参考 Unity 和 Unreal 的做法，建议：

1. **统一系统接口**：所有系统实现统一接口
2. **依赖关系管理**：显式声明依赖，自动解析顺序
3. **统一所有权**：使用智能指针统一管理
4. **错误处理**：完善的错误处理和回滚机制
5. **可测试性**：支持依赖注入和 mock

这些改进将使系统更加健壮、可维护和可扩展。

