# ZEngine 工厂框架使用指南

## 概述

ZEngine工厂框架提供了一套灵活、类型安全的对象创建机制，支持通过字符串ID创建对象实例。该框架采用模板设计，支持自动注册和手动注册两种方式。

## 核心组件

### 1. IFactory<BaseType>

工厂接口基类，所有工厂类都应该继承此接口。

```cpp
template<typename BaseType>
class IFactory
{
public:
    virtual std::shared_ptr<BaseType> create() = 0;
    virtual std::string getName() const = 0;
};
```

### 2. FactoryRegistry<BaseType>

工厂注册表，单例模式，用于管理和注册所有工厂。

```cpp
template<typename BaseType>
class FactoryRegistry : public Singleton<FactoryRegistry<BaseType>>
{
public:
    bool registerFactory(const std::string& name, std::shared_ptr<IFactory<BaseType>> factory);
    bool unregisterFactory(const std::string& name);
    std::shared_ptr<BaseType> create(const std::string& name);
    bool isRegistered(const std::string& name) const;
    std::vector<std::string> getRegisteredNames() const;
    void clear();
    size_t size() const;
};
```

### 3. FactoryCreator<BaseType, DerivedType>

工厂创建器模板类，用于创建具体产品类型的工厂实现。

```cpp
template<typename BaseType, typename DerivedType>
class FactoryCreator : public IFactory<BaseType>
{
public:
    explicit FactoryCreator(const std::string& name);
    std::shared_ptr<BaseType> create() override;
    std::string getName() const override;
};
```

### 4. FactoryCreatorWithInit<BaseType, DerivedType, InitInfo>

带初始化参数的工厂创建器，用于创建需要初始化参数的产品类型。

```cpp
template<typename BaseType, typename DerivedType, typename InitInfo>
class FactoryCreatorWithInit : public IFactory<BaseType>
{
public:
    explicit FactoryCreatorWithInit(const std::string& name, 
                                    std::function<void(DerivedType*)> init_func = nullptr);
    std::shared_ptr<BaseType> create() override;
    std::string getName() const override;
};
```

## 使用方法

### 方法1: 自动注册（推荐）

在派生类的.cpp文件中使用`REGISTER_FACTORY`宏：

```cpp
// MyRenderPass.h
namespace Z
{
    class IRenderPass
    {
    public:
        virtual ~IRenderPass() = default;
        virtual void render() = 0;
    };

    class MyRenderPass : public IRenderPass
    {
    public:
        void render() override;
    };
}

// MyRenderPass.cpp
#include "MyRenderPass.h"
#include "runtime/core/base/factory.h"

REGISTER_FACTORY(IRenderPass, MyRenderPass, "MyRenderPass");
```

### 方法2: 手动注册

在初始化函数中手动注册工厂：

```cpp
void initializeFactories()
{
    auto factory = std::make_shared<FactoryCreator<IRenderPass, MyRenderPass>>("MyRenderPass");
    FactoryRegistry<IRenderPass>::getInstance().registerFactory("MyRenderPass", factory);
}
```

### 方法3: 带初始化参数的注册

如果需要传递初始化参数：

```cpp
struct RenderPassInitInfo
{
    int width;
    int height;
};

class AdvancedRenderPass : public IRenderPass
{
public:
    void initialize(const RenderPassInitInfo& info)
    {
        m_width = info.width;
        m_height = info.height;
    }
private:
    int m_width;
    int m_height;
};

// 注册带初始化的工厂
auto factory = std::make_shared<FactoryCreatorWithInit<IRenderPass, AdvancedRenderPass, RenderPassInitInfo>>(
    "AdvancedRenderPass",
    [](AdvancedRenderPass* pass) {
        RenderPassInitInfo info{1920, 1080};
        pass->initialize(info);
    }
);
FactoryRegistry<IRenderPass>::getInstance().registerFactory("AdvancedRenderPass", factory);
```

### 方法4: 自定义工厂

实现自定义的工厂类：

```cpp
class CustomFactory : public IFactory<IRenderPass>
{
public:
    std::shared_ptr<IRenderPass> create() override
    {
        auto pass = std::make_shared<MyRenderPass>();
        // 自定义初始化逻辑
        return pass;
    }

    std::string getName() const override
    {
        return "CustomRenderPass";
    }
};

// 注册自定义工厂
auto factory = std::make_shared<CustomFactory>();
FactoryRegistry<IRenderPass>::getInstance().registerFactory("CustomRenderPass", factory);
```

## 创建对象

### 使用宏创建（推荐）

```cpp
auto pass = CREATE_FROM_FACTORY(IRenderPass, "MyRenderPass");
if (pass)
{
    pass->render();
}
```

### 直接使用注册表

```cpp
auto pass = FactoryRegistry<IRenderPass>::getInstance().create("MyRenderPass");
```

## 查询和管理

### 检查工厂是否已注册

```cpp
if (FactoryRegistry<IRenderPass>::getInstance().isRegistered("MyRenderPass"))
{
    // 工厂已注册
}
```

### 获取所有已注册的工厂名称

```cpp
auto names = FactoryRegistry<IRenderPass>::getInstance().getRegisteredNames();
for (const auto& name : names)
{
    std::cout << "Registered factory: " << name << std::endl;
}
```

### 注销工厂

```cpp
FactoryRegistry<IRenderPass>::getInstance().unregisterFactory("MyRenderPass");
```

### 清空所有工厂

```cpp
FactoryRegistry<IRenderPass>::getInstance().clear();
```

## 实际应用示例：RenderPass工厂

以下示例展示如何将工厂框架应用到RenderPipeline中：

```cpp
// render_pass_factory.h
namespace Z
{
    class RenderPassBase;
    
    // 使用工厂创建RenderPass
    std::shared_ptr<RenderPassBase> createRenderPass(const std::string& pass_name);
}

// render_pass_factory.cpp
#include "render_pass_factory.h"
#include "runtime/core/base/factory.h"
#include "runtime/function/render/render_pass_base.h"
#include "runtime/function/render/passes/main_camera_pass.h"
#include "runtime/function/render/passes/directional_light_pass.h"

// 注册所有RenderPass
REGISTER_FACTORY(RenderPassBase, MainCameraPass, "MainCameraPass");
REGISTER_FACTORY(RenderPassBase, DirectionalLightShadowPass, "DirectionalLightShadowPass");

std::shared_ptr<RenderPassBase> createRenderPass(const std::string& pass_name)
{
    return FactoryRegistry<RenderPassBase>::getInstance().create(pass_name);
}

// 使用示例
void RenderPipeline::initialize(RenderPipelineInitInfo init_info)
{
    // 使用工厂创建RenderPass，而不是直接使用make_shared
    m_main_camera_pass = createRenderPass("MainCameraPass");
    m_directional_light_pass = createRenderPass("DirectionalLightShadowPass");
    
    // ... 后续初始化代码
}
```

## 线程安全

工厂注册表是线程安全的，所有操作都使用互斥锁保护。可以在多线程环境中安全使用。

## 注意事项

1. **避免在头文件中使用REGISTER_FACTORY宏**：该宏会创建静态对象，如果在头文件中使用会导致多重定义错误。应该在对应的.cpp文件中使用。

2. **工厂名称唯一性**：每个工厂名称必须是唯一的，重复注册同名工厂会失败（返回false）。

3. **生命周期管理**：使用`REGISTER_FACTORY`宏注册的工厂会在程序结束时自动注销。手动注册的工厂需要手动注销。

4. **类型安全**：工厂框架是类型安全的，编译时会检查类型匹配。

5. **性能考虑**：工厂创建操作是O(1)时间复杂度（哈希表查找），性能开销很小。

## 最佳实践

1. **使用有意义的工厂名称**：工厂名称应该清晰、唯一，建议使用类名作为工厂名称。

2. **集中管理注册**：将所有工厂注册代码放在一个专门的初始化函数中，便于管理和维护。

3. **错误处理**：创建对象后应该检查返回值是否为nullptr，处理工厂不存在的情况。

4. **文档化**：为每个工厂添加注释，说明其用途和创建的对象类型。

## 扩展性

工厂框架设计为可扩展的，可以轻松添加新的功能：

- 支持创建参数：可以扩展`FactoryCreator`支持传递构造参数
- 支持单例模式：可以创建单例工厂，确保每个名称只创建一个实例
- 支持对象池：可以扩展工厂支持对象池，复用对象实例

## 总结

ZEngine工厂框架提供了一个强大、灵活的对象创建机制，可以显著简化代码，提高可维护性和可扩展性。通过使用工厂框架，可以：

- 解耦对象创建和使用
- 支持运行时动态创建对象
- 简化对象管理
- 提高代码的可测试性

更多示例代码请参考 `engine/source/runtime/core/base/factory_example.h` 和 `factory_example.cpp`。

