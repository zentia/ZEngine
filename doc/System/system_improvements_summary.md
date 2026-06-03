# Global Context 改进总结

## 已完成的改进

### 1. 统一的系统接口 (`engine_system.h`)
- 创建了 `IEngineSystem` 接口，所有系统都实现此接口
- 定义了 `SystemInitPhase` 枚举，明确系统的初始化阶段
- 支持依赖关系声明和自动解析

### 2. 系统注册机制 (`system_registry.h`)
- 实现了 `SystemRegistry` 类，自动管理系统的注册、初始化和关闭
- 使用拓扑排序自动确定初始化顺序
- 支持依赖关系验证和循环依赖检测
- 初始化失败时自动回滚

### 3. 系统适配器 (`system_adapters.h`)
- 为所有现有系统创建了适配器类
- 适配器包装现有系统，使其符合新接口
- 支持单例、智能指针和原始指针的统一管理

### 4. 重构的 GlobalContext
- `RuntimeGlobalContext` 现在使用 `SystemRegistry` 管理所有系统
- 保持向后兼容性，原有的成员变量仍然可用
- 添加了新的 `getSystem<T>()` 模板方法，类型安全地获取系统
- 改进了错误处理，初始化失败时自动回滚

## 主要改进点

### 1. 统一的所有权管理
- **之前**：混合使用单例、`shared_ptr` 和原始指针
- **现在**：通过适配器统一管理，所有系统都通过注册表访问

### 2. 自动依赖管理
- **之前**：依赖关系通过初始化顺序隐式表达
- **现在**：显式声明依赖，自动拓扑排序确定顺序

### 3. 错误处理和回滚
- **之前**：初始化失败时没有清理机制
- **现在**：初始化失败时自动回滚已初始化的系统

### 4. 可扩展性
- **之前**：添加新系统需要修改 `startSystems()` 方法
- **现在**：只需创建适配器并注册，依赖关系自动处理

## 使用方法

### 获取系统（新方法，推荐）

```cpp
// 类型安全的方式获取系统
auto input_system = g_runtime_global_context.getSystem<InputSystemAdapter>();
if (input_system)
{
    auto input = input_system->get();
    // 使用 input system
}
```

### 获取系统（向后兼容）

```cpp
// 仍然可以使用原有的成员变量
auto input = g_runtime_global_context.m_input_system;
```

### 添加新系统

1. 创建系统适配器（如果系统已有接口，直接实现 `IEngineSystem`）

```cpp
class MySystemAdapter : public IEngineSystem
{
public:
    std::string getName() const override { return "MySystem"; }
    SystemInitPhase getInitPhase() const override { return SystemInitPhase::Gameplay; }
    std::vector<std::string> getDependencies() const override { return {"InputSystem"}; }
    
    bool initialize() override
    {
        m_system = std::make_shared<MySystem>();
        m_system->initialize();
        setInitialized(true);
        return true;
    }
    
    void shutdown() override
    {
        if (m_system)
            m_system->shutdown();
        m_system.reset();
        setShutdown(true);
    }
    
private:
    std::shared_ptr<MySystem> m_system;
};
```

2. 在 `RuntimeGlobalContext::registerAllSystems()` 中注册

```cpp
void RuntimeGlobalContext::registerAllSystems()
{
    // ... 其他系统
    m_registry.registerSystem(std::make_shared<MySystemAdapter>());
}
```

## 系统初始化阶段

系统按以下阶段初始化：

1. **PreInit** (0): 配置、文件系统、日志等基础系统
2. **Core** (1): 线程、内存管理等核心系统
3. **Resource** (2): 资源管理（AssetManager, PreloadManager）
4. **Platform** (3): 平台相关（Window, Input）
5. **Rendering** (4): 渲染系统（RHI, RenderSystem）
6. **Gameplay** (5): 游戏系统（World, Physics, Particle）
7. **PostInit** (6): 最后初始化（ModuleManager, Canvas）

## 注意事项

1. **特殊初始化**：某些系统（如 WindowSystem、RHI、RenderSystem）需要特殊参数初始化，这些在 `startSystems()` 中单独处理
2. **向后兼容**：原有的成员变量访问方式仍然可用，但建议逐步迁移到新的 `getSystem<T>()` 方法
3. **错误处理**：`startSystems()` 现在返回 `bool`，调用者应该检查返回值

## 未来改进方向

1. **并行初始化**：独立系统可以并行初始化以提高启动速度
2. **热重载**：支持运行时重新加载某些系统
3. **配置驱动**：通过配置文件控制系统的初始化和依赖关系
4. **性能分析**：添加系统初始化时间统计

## 文件结构

```
engine/source/runtime/function/global/
├── engine_system.h          # 系统接口定义
├── system_registry.h        # 系统注册表实现
├── system_adapters.h        # 系统适配器实现
├── global_context.h         # GlobalContext 头文件
└── global_context.cpp       # GlobalContext 实现
```

## 测试建议

1. 测试系统初始化顺序是否正确
2. 测试依赖关系验证
3. 测试初始化失败时的回滚机制
4. 测试系统关闭顺序
5. 测试向后兼容性

