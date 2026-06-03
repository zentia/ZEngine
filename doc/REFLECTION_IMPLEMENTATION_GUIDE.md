# C++ 反射系统实现指南（类似 Unreal Engine）

## 概述

本文档详细说明如何在 C++ 中实现类似 Unreal Engine 的反射系统。ZEngine 项目已经有一个基础的反射系统，本文将解释其工作原理以及如何进一步改进。

## 目录

1. [反射系统架构](#反射系统架构)
2. [实现方案对比](#实现方案对比)
3. [ZEngine 当前实现](#zengine-当前实现)
4. [Unreal Engine 反射原理](#unreal-engine-反射原理)
5. [改进建议](#改进建议)
6. [完整示例](#完整示例)

---

## 反射系统架构

### 核心组件

1. **类型元数据（Type Metadata）**
   - 存储类的名称、字段、方法、基类等信息
   - 运行时可通过类型名查找类型信息

2. **字段访问器（Field Accessor）**
   - 提供字段的读写访问
   - 支持类型安全的访问

3. **方法访问器（Method Accessor）**
   - 支持方法的动态调用
   - 参数传递和返回值处理

4. **代码生成器（Code Generator）**
   - 解析源代码中的反射标记
   - 生成反射元数据代码

---

## 实现方案对比

### 方案 1: 宏 + 模板 + 代码生成（ZEngine 当前方案）

**优点：**
- 零运行时开销（编译时生成）
- 类型安全
- 支持复杂的类型系统
- IDE 友好（代码补全）

**缺点：**
- 需要预处理步骤
- 依赖代码生成工具（如 Clang）
- 编译时间较长

**实现方式：**
```cpp
// 声明时使用宏
REFLECTION_TYPE(MyClass)
CLASS(MyClass, Fields)
{
    REFLECTION_BODY(MyClass)
public:
    int m_value;
    std::string m_name;
};
```

### 方案 2: 纯宏展开（简单方案）

**优点：**
- 无需代码生成
- 实现简单
- 编译快速

**缺点：**
- 功能受限
- 不支持复杂类型
- 代码冗长

**示例：**
```cpp
#define REGISTER_CLASS(Class, ...) \
    static ClassRegistry<Class> _reg_##Class(#Class, __VA_ARGS__)
```

### 方案 3: 预编译头 + 模板特化（中等方案）

**优点：**
- 无需外部工具
- 完全在编译时完成
- 支持类型擦除

**缺点：**
- 模板代码复杂
- 编译时间增加
- 需要手动注册

### 方案 4: 运行时注册（最灵活但性能最低）

**优点：**
- 完全动态
- 支持热重载
- 易于扩展

**缺点：**
- 运行时开销
- 类型安全性降低
- 内存占用较高

---

## ZEngine 当前实现

### 1. 声明阶段

```cpp
// 使用宏标记需要反射的类
REFLECTION_TYPE(Component)
CLASS(Component, WhiteListFields)
{
    REFLECTION_BODY(Component)
protected:
    AActor* m_parent_object;
    bool m_is_dirty {false};
};
```

**宏定义：**
- `REFLECTION_TYPE(class_name)`: 声明反射类型
- `CLASS(class_name, ...)`: 定义反射类（使用 Clang attribute）
- `REFLECTION_BODY(class_name)`: 添加友元声明，允许反射访问私有成员
- `META(...)`: 添加元数据属性（仅在解析时可见）

### 2. 代码生成阶段

**流程：**
1. Meta Parser 使用 Clang 解析源代码
2. 查找 `__attribute__((annotate(...)))` 标记
3. 提取类、字段、方法信息
4. 使用 Mustache 模板生成反射代码
5. 生成的文件包含：
   - 类型操作器（Type Operator）
   - 字段访问函数
   - 方法调用函数
   - 注册函数

**生成的代码示例：**
```cpp
namespace Reflection {
namespace TypeFieldReflectionOparator {
    class TypeComponentOperator {
    public:
        static const char* getClassName() { return "Component"; }
        static void* get_m_is_dirty(void* instance) {
            return static_cast<void*>(&(static_cast<Component*>(instance)->m_is_dirty));
        }
        static void set_m_is_dirty(void* instance, void* field_value) {
            static_cast<Component*>(instance)->m_is_dirty = 
                *static_cast<bool*>(field_value);
        }
        // ...
    };
}
}
```

### 3. 运行时使用

```cpp
// 获取类型元数据
auto typeMeta = Reflection::TypeMeta::newMetaFromName("Component");

// 获取字段
auto field = typeMeta.getFieldByName("m_is_dirty");
void* value = field.get(instance);
field.set(instance, new_value);

// 创建对象
auto instance = Reflection::TypeMeta::newFromNameAndJson("Component", json_data);

// 序列化
Json json = Reflection::TypeMeta::writeByName("Component", instance);
```

---

## Unreal Engine 反射原理

### UE 反射系统特点

1. **UCLASS/USTRUCT/UPROPERTY 宏**
   - 编译时生成反射数据
   - 使用 UnrealHeaderTool 预处理
   - 生成 .generated.h 文件

2. **GENERATED_BODY() 宏**
   - 展开为大量反射代码
   - 包含类型信息表
   - 实现虚函数表

3. **反射数据结构**
   ```cpp
   // UE 反射核心结构（简化版）
   struct UClass {
       FName ClassName;
       UClass* SuperClass;
       TArray<UProperty*> Properties;
       TMap<FName, UFunction*> Functions;
   };
   
   struct UProperty {
       FName PropertyName;
       FName PropertyType;
       size_t Offset;
       EPropertyFlags Flags;
   };
   ```

4. **属性标志（Property Flags）**
   - `EditAnywhere`: 可在编辑器中编辑
   - `BlueprintReadOnly`: 蓝图只读
   - `SaveGame`: 保存到存档
   - `VisibleAnywhere`: 可见但不可编辑

### UE 反射使用示例

```cpp
UCLASS(BlueprintType, Blueprintable)
class MYGAME_API AMyActor : public AActor
{
    GENERATED_BODY()

public:
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Stats")
    float Health;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly)
    int32 Score;

    UFUNCTION(BlueprintCallable)
    void DoSomething();
};
```

---

## 改进建议

### 1. 添加属性标志系统

**当前问题：** 无法标记字段的访问权限、编辑器可见性等

**改进方案：**
```cpp
// 定义标志枚举
enum class PropertyFlags {
    None = 0,
    Editable = 1 << 0,        // 可编辑
    Visible = 1 << 1,         // 可见
    ReadOnly = 1 << 2,        // 只读
    SaveGame = 1 << 3,        // 保存到存档
    BlueprintReadWrite = 1 << 4,
    Category = 1 << 5,
};

// 使用方式
REFLECTION_TYPE(MyComponent)
CLASS(MyComponent, Fields)
{
    REFLECTION_BODY(MyComponent)
public:
    META(Editable, Visible, Category="Stats")
    float m_health = 100.0f;
    
    META(ReadOnly, Visible, Category="Debug")
    int m_kill_count = 0;
};
```

### 2. 改进序列化系统

**支持更多类型：**
```cpp
// 添加嵌套对象支持
template<typename T>
Json Serializer::write(const T& instance) {
    if constexpr (std::is_base_of_v<ReflectionObject, T>) {
        // 使用反射序列化
        return Reflection::TypeMeta::writeByName(...);
    } else if constexpr (std::is_arithmetic_v<T>) {
        // 基础类型
        return Json(instance);
    } else if constexpr (is_std_vector_v<T>) {
        // vector 类型
        return serialize_vector(instance);
    }
    // ...
}
```

### 3. 添加蓝图式函数调用

**支持带参数的函数反射：**
```cpp
// 声明
UFUNCTION(BlueprintCallable)
void SetHealth(float new_health);

// 运行时调用
auto method = typeMeta.getMethodByName("SetHealth");
MethodInvoker invoker(method);
invoker.addArgument(75.5f);
invoker.invoke(instance);
```

### 4. 添加属性变更通知

**支持观察者模式：**
```cpp
class Component {
    // 属性变更回调
    std::function<void(const std::string&, void*)> m_onPropertyChanged;
    
public:
    template<typename T>
    void setProperty(const std::string& name, const T& value) {
        auto field = m_typeMeta.getFieldByName(name.c_str());
        field.set(this, &value);
        if (m_onPropertyChanged) {
            m_onPropertyChanged(name, this);
        }
    }
};
```

### 5. 类型转换和验证

**添加安全的类型转换：**
```cpp
template<typename TargetType>
bool TypeMeta::canCastTo() const {
    // 检查继承关系
    // 检查接口实现
    return checkCastability<TargetType>(*this);
}

template<typename TargetType>
TargetType* TypeMeta::cast(void* instance) const {
    if (!canCastTo<TargetType>()) {
        return nullptr;
    }
    return static_cast<TargetType*>(instance);
}
```

### 6. 代码生成优化

**减少生成的代码量：**
- 使用模板减少重复代码
- 合并相似的访问器
- 使用更紧凑的数据结构

**改进模板：**
```cpp
// 当前：每个字段生成单独的函数
static void* get_field1(void* instance) { ... }
static void* get_field2(void* instance) { ... }

// 改进：使用统一的访问器
template<auto MemberPtr>
void* getField(void* instance) {
    return &(instance->*MemberPtr);
}
```

---

## 完整示例

### 定义反射类

```cpp
// MyActor.h
#pragma once
#include "runtime/core/meta/reflection/reflection.h"

namespace Z {
    REFLECTION_TYPE(MyActor)
    CLASS(MyActor, Fields)
    {
        REFLECTION_BODY(MyActor)
        
    public:
        MyActor() = default;
        virtual ~MyActor() = default;
        
        // 可编辑属性
        META(Editable, Category="Stats")
        float m_health = 100.0f;
        
        META(Editable, Category="Stats")
        float m_max_health = 100.0f;
        
        // 只读属性
        META(ReadOnly, Category="Debug")
        int m_kill_count = 0;
        
        // 向量属性
        META(Editable, Category="Transform")
        std::vector<float> m_position {0, 0, 0};
        
        // 方法
        void TakeDamage(float damage);
        void Heal(float amount);
    };
}
```

### 使用反射系统

```cpp
// 示例代码
void ExampleUsage() {
    // 1. 创建对象
    auto actor = Reflection::TypeMeta::newFromNameAndJson(
        "MyActor", 
        Json::object {
            {"m_health", 80.0},
            {"m_max_health", 100.0}
        }
    );
    
    // 2. 获取类型信息
    auto typeMeta = Reflection::TypeMeta::newMetaFromName("MyActor");
    
    // 3. 遍历所有字段
    FieldAccessor* fields = nullptr;
    int fieldCount = typeMeta.getFieldsList(fields);
    for (int i = 0; i < fieldCount; ++i) {
        std::cout << "Field: " << fields[i].getFieldName() 
                  << ", Type: " << fields[i].getFieldTypeName() << std::endl;
    }
    delete[] fields;
    
    // 4. 访问特定字段
    auto healthField = typeMeta.getFieldByName("m_health");
    float* health = static_cast<float*>(healthField.get(actor.m_instance));
    std::cout << "Current health: " << *health << std::endl;
    
    // 5. 修改字段值
    float newHealth = 50.0f;
    healthField.set(actor.m_instance, &newHealth);
    
    // 6. 序列化
    Json json = Reflection::TypeMeta::writeByName("MyActor", actor.m_instance);
    std::cout << json.dump() << std::endl;
    
    // 7. 调用方法
    auto takeDamageMethod = typeMeta.getMethodByName("TakeDamage");
    if (takeDamageMethod.isValid()) {
        // 注意：当前实现只支持无参数方法，需要改进
        takeDamageMethod.invoke(actor.m_instance);
    }
}
```

### 编辑器集成示例

```cpp
// 在编辑器中显示和编辑属性
void ShowPropertiesInEditor(Reflection::ReflectionInstance& instance) {
    auto typeMeta = instance.m_meta;
    FieldAccessor* fields = nullptr;
    int count = typeMeta.getFieldsList(fields);
    
    for (int i = 0; i < count; ++i) {
        auto& field = fields[i];
        
        // 检查属性标志
        // TODO: 需要添加标志检查功能
        
        // 显示字段名
        ImGui::Text("Field: %s", field.getFieldName());
        
        // 根据类型显示不同的控件
        const char* typeName = field.getFieldTypeName();
        void* value = field.get(instance.m_instance);
        
        if (strcmp(typeName, "float") == 0) {
            float* f = static_cast<float*>(value);
            ImGui::DragFloat(field.getFieldName(), f, 0.1f);
        } else if (strcmp(typeName, "int") == 0) {
            int* i = static_cast<int*>(value);
            ImGui::DragInt(field.getFieldName(), i, 1);
        }
        // ... 更多类型
        
        // 如果值改变，调用 set
        // ImGui 会标记值是否改变
    }
    
    delete[] fields;
}
```

---

## 总结

### ZEngine 反射系统的优势

1. ✅ 已实现基础反射功能
2. ✅ 支持字段访问和序列化
3. ✅ 代码生成自动化
4. ✅ 类型安全

### 需要改进的地方

1. ⚠️ 添加属性标志系统
2. ⚠️ 支持带参数的函数调用
3. ⚠️ 改进类型转换和验证
4. ⚠️ 优化代码生成性能
5. ⚠️ 添加属性变更通知

### 下一步建议

1. 实现属性标志系统
2. 扩展方法调用支持参数
3. 添加类型转换功能
4. 优化代码生成模板
5. 实现属性变更回调

---

## 参考资料

- [Unreal Engine Reflection System](https://docs.unrealengine.com/5.0/en-US/reflection-system-in-unreal-engine/)
- [Clang AST Introduction](https://clang.llvm.org/docs/IntroductionToTheClangAST.html)
- [C++ Reflection 实现方案对比](https://github.com/Manu343726/reflection)

---

*最后更新: 2024*

