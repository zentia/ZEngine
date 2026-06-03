# C++ 反射系统快速入门

## 核心原理

C++ 不像 C# 或 Java 那样有内置的反射系统，所以需要自己实现。主要有以下几种方案：

### 1. 代码生成方案（推荐，类似 UE）

**工作原理：**
```
源代码 → 元解析器 → 生成反射代码 → 编译
```

**优点：**
- ✅ 零运行时开销
- ✅ 类型安全
- ✅ 功能强大

**实现步骤：**

1. **标记需要反射的类**
   ```cpp
   REFLECTION_TYPE(MyClass)
   CLASS(MyClass, Fields)
   {
       REFLECTION_BODY(MyClass)
   public:
       int m_value;
   };
   ```

2. **元解析器解析源代码**
   - 使用 Clang 解析 AST
   - 查找 `__attribute__((annotate(...)))` 标记
   - 提取类、字段、方法信息

3. **生成反射代码**
   - 生成字段访问器函数
   - 生成类型注册代码
   - 生成序列化代码

4. **运行时使用**
   ```cpp
   // 获取类型
   auto typeMeta = TypeMeta::newMetaFromName("MyClass");
   
   // 访问字段
   auto field = typeMeta.getFieldByName("m_value");
   void* value = field.get(instance);
   
   // 序列化
   Json json = TypeMeta::writeByName("MyClass", instance);
   ```

### 2. 宏展开方案（简单但受限）

**工作原理：**
```cpp
#define REGISTER_CLASS(Class) \
    static ClassInfo<Class> _reg_##Class(#Class)
```

**适用场景：**
- 简单的类型注册
- 不需要复杂的元数据

### 3. 模板特化方案（无需外部工具）

**工作原理：**
```cpp
template<typename T>
struct TypeInfo {
    static constexpr const char* name() { return "Unknown"; }
};

template<>
struct TypeInfo<int> {
    static constexpr const char* name() { return "int"; }
};
```

**适用场景：**
- 基础类型注册
- 有限的反射需求

## 关键实现点

### 1. 字段访问

**问题：** 如何通过字段名访问私有成员？

**解决：**
- 使用友元类（`REFLECTION_BODY` 宏）
- 生成专门的访问器函数

```cpp
// 生成的代码
class TypeMyClassOperator {
    static void* get_m_value(void* instance) {
        return &(static_cast<MyClass*>(instance)->m_value);
    }
    static void set_m_value(void* instance, void* value) {
        static_cast<MyClass*>(instance)->m_value = 
            *static_cast<int*>(value);
    }
};
```

### 2. 类型注册

**问题：** 如何将类型信息存储到全局注册表？

**解决：**
- 使用静态初始化
- 在程序启动时注册

```cpp
// 生成的注册代码
void TypeWrapperRegister_MyClass() {
    FieldFunctionTuple* tuple = new FieldFunctionTuple(
        &TypeMyClassOperator::set_m_value,
        &TypeMyClassOperator::get_m_value,
        // ...
    );
    REGISTER_FIELD_TO_MAP("MyClass", tuple);
}

// 程序启动时调用
TypeWrappersRegister::InitializeAll();
```

### 3. 序列化

**问题：** 如何将对象转换为 JSON？

**解决：**
- 遍历所有字段
- 根据类型序列化值
- 处理嵌套对象

```cpp
template<typename T>
Json Serializer::write(const T& instance) {
    auto typeMeta = TypeMeta::newMetaFromName("MyClass");
    FieldAccessor* fields = nullptr;
    int count = typeMeta.getFieldsList(fields);
    
    Json result = Json::object{};
    for (int i = 0; i < count; ++i) {
        const char* name = fields[i].getFieldName();
        void* value = fields[i].get((void*)&instance);
        // 根据字段类型序列化...
    }
    return result;
}
```

## 与 Unreal Engine 对比

| 特性 | ZEngine | Unreal Engine |
|------|---------|---------------|
| 宏标记 | `REFLECTION_TYPE`, `CLASS` | `UCLASS`, `USTRUCT` |
| 字段标记 | `META(...)` | `UPROPERTY(...)` |
| 代码生成 | Clang + Mustache | UnrealHeaderTool |
| 生成文件 | `.reflection.gen.h` | `.generated.h` |
| 属性标志 | 基础支持 | 完整支持 |
| 蓝图集成 | 无 | 完整支持 |

## 使用建议

### 1. 定义反射类

```cpp
REFLECTION_TYPE(MyComponent)
CLASS(MyComponent, Fields)
{
    REFLECTION_BODY(MyComponent)

public:
    // 可编辑属性
    META(Editable, Visible, Category="Stats")
    float m_health;

    // 只读属性
    META(ReadOnly, Visible, Category="Debug")
    int m_kill_count;

    // 保存到存档
    META(Editable, SaveGame, Category="Progress")
    int m_level;
};
```

### 2. 运行时访问

```cpp
// 创建对象
auto instance = TypeMeta::newFromNameAndJson("MyComponent", json);

// 访问字段
auto typeMeta = TypeMeta::newMetaFromName("MyComponent");
auto healthField = typeMeta.getFieldByName("m_health");
float* health = static_cast<float*>(healthField.get(instance.m_instance));

// 修改字段
float newHealth = 50.0f;
healthField.set(instance.m_instance, &newHealth);
```

### 3. 编辑器集成

```cpp
// 在 ImGui 中显示属性
void ShowProperties(TypeMeta& typeMeta, void* instance) {
    FieldAccessor* fields = nullptr;
    int count = typeMeta.getFieldsList(fields);

    for (int i = 0; i < count; ++i) {
        const char* name = fields[i].getFieldName();
        const char* type = fields[i].getFieldTypeName();
        
        // 根据类型显示不同的控件
        if (strcmp(type, "float") == 0) {
            float* value = static_cast<float*>(fields[i].get(instance));
            ImGui::DragFloat(name, value);
        }
        // ...
    }
}
```

## 常见问题

### Q: 为什么需要代码生成？

**A:** C++ 没有运行时类型信息（RTTI 功能有限），无法在运行时获取字段信息。代码生成可以在编译时生成必要的访问代码。

### Q: 性能开销大吗？

**A:** 代码生成方案几乎没有运行时开销，所有反射代码都是编译时生成的普通函数调用。

### Q: 如何支持私有成员？

**A:** 使用友元类。`REFLECTION_BODY` 宏会声明反射操作器为友元，从而可以访问私有成员。

### Q: 如何扩展功能？

**A:** 
1. 扩展 META 宏支持更多属性
2. 改进代码生成模板
3. 增强运行时类型系统

## 参考资料

- `REFLECTION_IMPLEMENTATION_GUIDE.md` - 详细实现指南
- `engine/source/runtime/core/meta/reflection/` - 反射系统源码
- `engine/source/meta_parser/` - 元解析器源码

