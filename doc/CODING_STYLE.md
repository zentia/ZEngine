# ZEngine 代码命名规范 (Game Engine Hybrid Style)

本文档详细描述了 ZEngine 代码库中的命名规范，采用**游戏引擎混合风格**，融合了 Unreal、Unity 和 Google 的最佳实践。

## 目录

1. [风格总览](#风格总览)
2. [类与结构体](#类与结构体)
3. [接口](#接口)
4. [函数与方法](#函数与方法)
5. [变量命名](#变量命名)
6. [枚举](#枚举)
7. [常量](#常量)
8. [命名空间](#命名空间)
9. [类型别名](#类型别名)
10. [宏定义](#宏定义)
11. [文件名](#文件名)
12. [模板参数](#模板参数)
13. [STL 兼容接口](#stl-兼容接口)
14. [代码示例](#代码示例)

---

## 风格总览

| 类型 | 命名风格 | 前缀/后缀 | 示例 |
|------|---------|----------|------|
| 类/结构体 | CamelCase | - | `WindowSystem`, `RenderConfig` |
| 接口/抽象类 | CamelCase | `I` 前缀 | `IRenderer`, `ISystem` |
| 函数/方法 | CamelCase | - | `Initialize()`, `IsQuit()` |
| 成员变量 | camelCase | `m_` 前缀 | `m_window`, `m_isQuit` |
| 静态成员 | camelCase | `s_` 前缀 | `s_instance`, `s_count` |
| 全局变量 | camelCase | `g_` 前缀 | `g_runtimeContext` |
| 局部变量 | snake_case | - | `delta_time`, `frame_count` |
| 参数 | snake_case | `in_` 前缀避免 shadow | `in_delta_time`, `create_info` |
| 枚举类型 | CamelCase | - | `LogLevel`, `RenderPass` |
| 枚举值 | CamelCase | - | `Debug`, `Forward`, `Deferred` |
| 常量 | UPPER_CASE | - | `MAX_BUFFER_SIZE`, `DEFAULT_FPS` |
| 命名空间 | PascalCase | - | `Engine`, `Engine::Render` |
| 类型别名 | CamelCase | - | `OnResetFunc`, `FieldTuple` |
| 宏定义 | UPPER_CASE | `Z_` (项目特定) | `Z_PLATFORM_WINDOWS` |
| 文件名 | PascalCase | - | `WindowSystem.h` |
| 文件夹 | PascalCase | - | `Runtime`, `Core`, `Math` |
| 模板参数 | CamelCase | - | `T`, `KeyType`, `ValueType` |

---

## 类与结构体

### 规则
- **命名风格**：CamelCase（每个单词首字母大写）
- **后缀**：根据需要使用描述性后缀（如 `Component`, `Manager`, `System`）

### 示例
```cpp
// 类
class WindowSystem {};
class RenderSystem {};
class RuntimeGlobalContext {};

// 结构体
struct WindowCreateInfo {};
struct EngineInitParams {};
struct RenderConfig {};
```

---

## 接口

### 规则
- **命名风格**：CamelCase
- **前缀**：使用 `I` 前缀标识接口/抽象类

### 示例
```cpp
// 接口类
class IRenderer {
public:
    virtual ~IRenderer() = default;
    virtual void Render() = 0;
};

class IEngineSystem {
public:
    virtual bool Initialize() = 0;
    virtual void Shutdown() = 0;
};

class IInputHandler {
public:
    virtual void OnKeyDown(int keyCode) = 0;
    virtual void OnKeyUp(int keyCode) = 0;
};
```

---

## 函数与方法

### 规则
- **命名风格**：CamelCase（每个单词首字母大写）
- **命名原则**：
  - 以动词或动词短语开头（如 `Initialize`, `CreateObject`, `PollEvents`）
  - 布尔返回值的函数使用 `Is`, `Has`, `Can`, `Should` 等前缀
  - Getter 函数使用 `Get` 前缀
  - Setter 函数使用 `Set` 前缀

### 示例
```cpp
// 普通方法
void Initialize();
void PollEvents();
void CalculateDeltaTime();

// 布尔返回
bool IsQuit() const;
bool ShouldClose() const;
bool HasChildren() const;
bool CanUpdate() const;

// Getter/Setter
int GetFPS() const;
void SetPosition(const Vector3& pos);
Vector3 GetPosition() const;

// 运行和更新
void Run();
void Tick(float deltaTime);
bool TickOneFrame(float deltaTime);
```

---

## 变量命名

### 成员变量 (Member Variables)

#### 规则
- **前缀**：`m_`（member）
- **命名风格**：`m_` + camelCase
- **作用**：一眼区分成员变量、局部变量和参数

#### 示例
```cpp
class WindowSystem {
private:
    GLFWwindow* m_window{nullptr};
    int m_width{0};
    int m_height{0};
    bool m_isFocusMode{false};
    float m_averageDuration{0.f};
    int m_frameCount{0};
};
```

### 静态成员变量 (Static Member Variables)

#### 规则
- **前缀**：`s_`（static）
- **命名风格**：`s_` + camelCase

#### 示例
```cpp
class Engine {
private:
    static Engine* s_instance;
    static int s_instanceCount;
    static float s_fpsAlpha;
};
```

### 全局变量 (Global Variables)

#### 规则
- **前缀**：`g_`（global）
- **命名风格**：`g_` + camelCase

#### 示例
```cpp
extern RuntimeGlobalContext g_runtimeGlobalContext;
extern EditorGlobalContext g_editorGlobalContext;
extern bool g_isEditorMode;
```

### 参数和局部变量 (Parameters and Local Variables)

#### 规则
- **命名风格**：snake_case（小写 + 下划线分词）
- **构造函数参数**：若与成员同名，使用 `in_` 前缀（如 `in_system`）
- **赋值格式**：单空格，不做列对齐（`clang-format` 中 `AlignConsecutiveAssignments: false`）

#### 示例
```cpp
void Tick(float delta_time);
void Initialize(WindowCreateInfo create_info);
void SetPosition(const Vector3& new_translation);

void ProcessEvents() {
    int frame_count = 0;
    bool should_render = true;
    Vector2 cursor_uv = {0.0f, 0.0f};
    auto visible_objects = CullObjects();
}
```

---

## 枚举

### 枚举类型 (Enum Class)

#### 规则
- **类型命名**：CamelCase
- **推荐使用**：`enum class` 而不是 `enum`（提供类型安全）

### 枚举值 (Enum Values)

#### 规则
- **命名风格**：CamelCase（无前缀，简洁清晰）

#### 示例
```cpp
enum class LogLevel : uint8_t {
    Debug,
    Info,
    Warn,
    Error,
    Fatal
};

enum class RenderPass {
    Depth,
    GBuffer,
    Lighting,
    PostProcess
};

enum class ObjectFlags : uint32_t {
    None        = 0,
    PendingKill = 1 << 0,
    NeedLoad    = 1 << 1,
    NeedPostLoad = 1 << 2
};

// 使用示例
LogLevel level = LogLevel::Debug;
RenderPass pass = RenderPass::GBuffer;
```

---

## 常量

### 规则
- **命名风格**：UPPER_CASE（全大写，下划线分隔）
- **适用于**：`const`、`constexpr`、静态常量

### 示例
```cpp
// 类内静态常量
class Engine {
    static constexpr int MAX_BUFFER_SIZE = 1024;
    static constexpr float DEFAULT_FPS = 60.0f;
    static const float FPS_ALPHA;
};

// 命名空间作用域的常量
constexpr int MAX_ENTITY_COUNT = 10000;
constexpr int MAX_LIGHTS = 128;
const char* const DEFAULT_TITLE = "ZEngine";
```

---

## 命名空间

### 规则
- **命名风格**：**PascalCase**（与文件名、文件夹名保持一致）
- **命名原则**：
  - 主命名空间通常反映项目或模块名称
  - 子命名空间使用描述性名称
  - 与类名、文件名风格统一

### 示例
```cpp
namespace Engine
{
    class WindowSystem {};
}

namespace Engine::Render
{
    class Pipeline {};
    class Material {};
}

namespace Engine::Physics
{
    class RigidBody {};
    class Collider {};
}

namespace Engine::Core
{
    class Math {};
    class Memory {};
}
```

### 命名空间层次示例
```cpp
// 主命名空间
namespace Engine { }

// 功能模块命名空间
namespace Engine::Render { }
namespace Engine::Physics { }
namespace Engine::Animation { }

// 核心系统命名空间
namespace Engine::Core { }
namespace Engine::Core::Math { }
namespace Engine::Core::Memory { }
```

---

## 类型别名

### 规则
- **命名风格**：CamelCase

### 示例
```cpp
using OnResetFunc = std::function<void()>;
using OnKeyFunc = std::function<void(int, int, int, int)>;
using FieldFunctionTuple = std::tuple<SetFunction, GetFunction, GetNameFunction>;

template<typename T>
using SharedPtrVector = std::vector<std::shared_ptr<T>>;

using EntityId = uint64_t;
using ComponentMask = uint32_t;
```

---

## 宏定义

### 规则
- **命名风格**：UPPER_CASE（全大写，下划线分隔）
- **前缀**：项目特定的宏使用 `Z_` 前缀

### 示例
```cpp
// 平台检测宏
#define Z_PLATFORM_WINDOWS
#define Z_PLATFORM_IS_WINDOWS 1

// 导出宏
#define EXPORT_RUNTIME __declspec(dllexport)

// 反射宏
#define REFLECTION_BODY(class_name)
#define META(...)
#define CLASS(class_name, ...)

// 断言和日志
#define Z_ASSERT(condition)
#define Z_LOG(level, message)
```

---

## 文件名

### 规则
- **命名风格**：**PascalCase**（与类名保持一致，游戏引擎惯例）
- **扩展名**：
  - 头文件：`.h`
  - 源文件：`.cpp`
  - 内联文件：`.inl`
- **命名原则**：
  - 文件名通常与类名相同（如 `WindowSystem.h` 对应 `class WindowSystem`）
  - 接口文件使用 `I` 前缀（如 `IRenderer.h` 对应 `class IRenderer`）
  - 结构体文件通常与结构体名相同（如 `RenderConfig.h` 对应 `struct RenderConfig`）

### 示例
```
WindowSystem.h
WindowSystem.cpp
IRenderer.h
LogSystem.h
MemoryManager.h
TransformComponent.h
RenderPipeline.h
RenderConfig.h
Engine.h
```

### 特殊文件
- **预编译头**：`pch.h`, `pch.cpp`（保持小写，约定俗成）
- **主函数**：`main.cpp`（保持小写）
- **工具/脚本文件**：可保持小写（如 `build.sh`, `setup.py`）

---

## 文件夹命名

### 规则
- **命名风格**：**PascalCase**（与文件命名保持一致）
- **命名原则**：
  - 文件夹名通常反映其包含的模块或功能
  - 使用描述性名称，避免缩写（除非广泛认可）

### 示例
```
Runtime/
  Core/
    Math/
    Containers/
    Memory/
  Function/
    Render/
    Physics/
    Animation/
  Engine/
Editor/
Launcher/
```

### 文件夹结构示例
```
engine/
  source/
    runtime/
      Core/
        Math/
          Vector3.h
          Matrix4.h
        Containers/
          Vector.h
          String.h
      Function/
        Render/
          RenderSystem.h
          RenderPipeline.h
        Physics/
          PhysicsSystem.h
      Engine/
        Engine.h
    editor/
      EditorWindow/
        InspectorWindow.h
        MaterialEditor.h
```

---

## 模板参数

### 规则
- **命名风格**：CamelCase
- **简单模板**：使用单个大写字母（如 `T`, `U`, `V`）
- **复杂模板**：使用 CamelCase 描述性名称

### 示例
```cpp
// 简单模板
template<typename T>
class Container {};

// 复杂模板
template<typename KeyType, typename ValueType>
class Map {};

template<typename ElementType, size_t Alignment = 16>
class AlignedArray {};

template<typename... Args>
void Log(Args&&... args) {}
```

---

## STL 兼容接口

### 规则
为了与 C++ 标准库兼容（如 range-based for 循环、STL 算法），以下函数名使用 **小写**：

### 允许的小写方法名
| 类别 | 函数名 |
|------|--------|
| **迭代器** | `begin`, `end`, `cbegin`, `cend`, `rbegin`, `rend` |
| **容器信息** | `size`, `empty`, `data`, `capacity` |
| **元素访问** | `front`, `back`, `at`, `find`, `count`, `contains` |
| **修改操作** | `insert`, `erase`, `push_back`, `pop_back`, `emplace`, `clear`, `resize`, `reserve`, `swap` |
| **智能指针** | `get`, `reset`, `release` |

### 示例
```cpp
class MyContainer {
public:
    // STL 兼容接口 - 小写
    iterator begin();
    iterator end();
    size_t size() const;
    bool empty() const;
    
    // 自定义方法 - CamelCase
    void DoSomething();
    void ProcessData();
    bool IsValid() const;
    int GetElementCount() const;
};
```

---

## 代码示例

### 完整类示例

```cpp
// WindowSystem.h
#pragma once

#include <memory>
#include <string>

#include "Runtime/ExportRuntime.h"

namespace Engine
{

struct WindowCreateInfo
{
    int         width{1280};
    int         height{720};
    const char* title{"ZEngine"};
    bool        isFullscreen{true};
};

class IWindowCallbacks
{
public:
    virtual ~IWindowCallbacks() = default;
    virtual void OnResize(int width, int height) = 0;
    virtual void OnClose() = 0;
};

class WindowSystem
{
public:
    // 常量
    static constexpr int DEFAULT_WIDTH = 1280;
    static constexpr int DEFAULT_HEIGHT = 720;

    WindowSystem() = default;
    ~WindowSystem();

    // 生命周期
    void Initialize(WindowCreateInfo createInfo);
    void Shutdown();

    // 每帧更新
    void PollEvents() const;
    bool ShouldClose() const;

    // Getter/Setter
    void SetTitle(const char* title);
    bool GetFocusMode() const { return m_isFocusMode; }
    void SetFocusMode(bool mode);
    int  GetWidth() const { return m_width; }
    int  GetHeight() const { return m_height; }

    // 查询
    bool IsMouseButtonDown(int button) const;
    bool IsKeyPressed(int keyCode) const;

private:
    GLFWwindow*        m_window{nullptr};
    int                m_width{0};
    int                m_height{0};
    bool               m_isFocusMode{false};
    IWindowCallbacks*  m_callbacks{nullptr};
};

}  // namespace Engine
```

### 枚举示例

```cpp
namespace Engine
{

enum class LogLevel : uint8_t
{
    Debug,
    Info,
    Warn,
    Error,
    Fatal
};

class LogSystem
{
public:
    static constexpr int MAX_LOG_LENGTH = 1024;

    void Log(LogLevel level, const std::string& message);
    void SetMinLevel(LogLevel level) { m_minLevel = level; }
    LogLevel GetMinLevel() const { return m_minLevel; }

private:
    LogLevel m_minLevel{LogLevel::Debug};
    bool     m_isEnabled{true};
};

}  // namespace Engine
```

### Engine 类示例

```cpp
namespace Engine
{

extern bool g_isEditorMode;

class Engine : public IEngineSystem
{
public:
    // IEngineSystem 接口
    bool Initialize() override;
    void Shutdown() override;

    // Engine 特有方法
    void Run();
    bool TickOneFrame(float deltaTime);

    bool IsQuit() const { return m_isQuit; }
    int  GetFPS() const { return m_fps; }
    bool MayUpdate() const { return m_disallowUpdating == 0; }

protected:
    void  LogicalTick(float deltaTime);
    bool  RendererTick(float deltaTime);
    void  CalculateFPS(float deltaTime);
    float CalculateDeltaTime();

private:
    static constexpr float FPS_ALPHA = 0.9f;

    bool                                    m_isQuit{false};
    float                                   m_averageDuration{0.f};
    int                                     m_frameCount{0};
    int                                     m_fps{0};
    int                                     m_disallowUpdating{0};

    std::chrono::steady_clock::time_point m_lastTickTimePoint{
        std::chrono::steady_clock::now()};
};

}  // namespace Engine
```

---

## 风格选择理由

### 为什么用 `m_` 前缀而不是尾部下划线？

```cpp
// m_ 前缀 - 更容易识别
void Update() {
    m_position.x = 0;    // 明确是成员变量
    position.x = 0;      // 这是什么？局部变量？参数？
}

// 在 Profiler/Debugger 中
m_frameCount = 42       // 立即知道是成员
frame_count_ = 42       // 需要看后面
```

### 为什么枚举值不用 `k` 前缀？

```cpp
// 无前缀 - 更简洁
RenderPass::GBuffer
LogLevel::Error
ObjectFlags::PendingKill

// k 前缀 - 冗余
RenderPass::kGBuffer
LogLevel::kError
ObjectFlags::kPendingKill
```

### 为什么常量用 UPPER_CASE？

```cpp
// UPPER_CASE - 传统游戏引擎风格，一眼识别
static constexpr int MAX_LIGHTS = 128;
static constexpr float DEFAULT_FOV = 75.0f;

// kCamelCase - 与普通变量容易混淆
static constexpr int kMaxLights = 128;
```

---

## 注意事项

1. **STL 兼容性**：容器类的 `begin()`, `end()`, `size()` 等方法必须保持小写

2. **一致性**：团队内保持一致比追求"完美"风格更重要

3. **自动化**：使用 `clang-format` + `clang-tidy` 自动检查和格式化

4. **渐进迁移**：不建议一次性重构，在修改功能时顺便更新命名

---

## 参考资料

- [Unreal Engine Coding Standard](https://docs.unrealengine.com/5.0/en-US/epic-cplusplus-coding-standard-for-unreal-engine/)
- [Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html)
- 本项目 `.clang-tidy` 配置文件
- 本项目 `.clang-format` 配置文件

---

**文档版本**: 3.0 (Game Engine Hybrid Style)  
**最后更新**: 基于 .clang-tidy 配置  
**维护者**: ZEngine 开发团队
