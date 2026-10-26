# ZEngine ConsoleManager 使用指南

## 概述

ZEngine的ConsoleManager参考了Unreal Engine的ConsoleManager设计，提供了运行时控制台命令和变量（CVar）管理功能。它允许开发者在运行时动态调整游戏参数、执行调试命令等。

## 核心功能

1. **控制台变量（CVar）**：支持Int、Float、Bool、String四种类型
2. **控制台命令（CCommand）**：执行自定义函数
3. **命令历史记录**：自动保存最近执行的命令
4. **自动补全**：根据前缀匹配命令和变量名
5. **帮助系统**：查看所有命令和变量的帮助信息

## 内置命令（2026-05）

在编辑器 Console 窗口底部输入框执行；输出写入 BqLog（`ZConsole` 类别），与日志列表一起显示。

| 命令 | 说明 |
|------|------|
| `help` / `help <name>` | 列出或查询命令/CVar |
| `set <Name> <Value>` | 设置 CVar（UE 风格） |
| `get <Name>` | 打印 CVar |
| `<Name>` / `<Name> <Value>` / `<Name>=<Value>` | 读/写 CVar 简写 |
| `stat fps` | 当前 FPS |
| `quit` / `exit` | 请求关闭 |
| `echo <text>` | 打印一行 |
| `obj list` | 列出当前关卡 GameObject（仅编辑器） |
| `level reload` / `level save` | 重载/保存关卡（仅编辑器） |
| `asset.find <filter>` | 搜索 AssetRegistry（仅编辑器） |
| `asset.reimport <path>` | 按源文件重导入 .zasset（仅编辑器） |
| `asset.count` | 打印注册表资产数量（仅编辑器） |
| `play` / `pause` | 播放/暂停（仅编辑器） |

注册入口：`RegisterRuntimeConsoleCommands()`（`ConsoleManager::Initialize`）、`RegisterEditorConsoleCommands()`（`Editor::Initialize`）。

## 基本使用

### 1. 注册控制台变量

#### 使用宏自动注册（推荐）

```cpp
#include "runtime/function/console/console_macros.h"

// 注册Int变量（绑定到存储）
static int g_max_fps = 60;
REGISTER_CONSOLE_VARIABLE_INT(r.MaxFPS, "Maximum frames per second", 60, &g_max_fps);

// 注册Float变量（不绑定存储，使用内部存储）
REGISTER_CONSOLE_VARIABLE_FLOAT(r.ShadowDistance, "Shadow rendering distance", 1000.0f, nullptr);

// 注册Bool变量
static bool g_enable_debug = false;
REGISTER_CONSOLE_VARIABLE_BOOL(r.DebugMode, "Enable debug rendering", false, &g_enable_debug);

// 注册String变量
static std::string g_log_level = "info";
REGISTER_CONSOLE_VARIABLE_STRING(r.LogLevel, "Log level (verbose/debug/info/warning/error/fatal)", "info", &g_log_level);
```

#### 手动注册

```cpp
#include "runtime/function/console/console_manager.h"
#include "runtime/core/base/system_registry.h"

auto* console = GET_SYSTEM(ConsoleManager);
if (console)
{
    // 注册变量
    auto var = console->registerIntVariable("r.MaxFPS", "Maximum frames per second", 60, &g_max_fps);
    
    // 设置值改变回调
    var->setOnChangedCallback([]() {
        LOG_INFO(ZEngine, "MaxFPS changed to: {}", g_max_fps);
    });
}
```

### 2. 注册控制台命令

#### 使用宏自动注册（推荐）

```cpp
#include "runtime/function/console/console_macros.h"

// 注册Lambda命令
REGISTER_CONSOLE_COMMAND(quit, "Exit the application", 
    [](const std::vector<std::string>& args) -> bool {
        // 退出应用
        return true;
    });

// 注册静态函数命令
static bool ReloadShaders(const std::vector<std::string>& args)
{
    LOG_INFO(ZEngine, "Reloading shaders...");
    // 重新加载着色器
    return true;
}
REGISTER_CONSOLE_COMMAND_STATIC(reloadShaders, "Reload all shaders", ReloadShaders);

// 注册成员函数命令
class MySystem
{
public:
    bool handleCommand(const std::vector<std::string>& args)
    {
        LOG_INFO(ZEngine, "Command executed with {} arguments", args.size());
        return true;
    }
};

static MySystem g_my_system;
REGISTER_CONSOLE_COMMAND_MEMBER(myCommand, "My custom command", &g_my_system, &MySystem::handleCommand);
```

#### 手动注册

```cpp
auto* console = GET_SYSTEM(ConsoleManager);
if (console)
{
    console->registerCommand("quit", "Exit the application",
        [](const std::vector<std::string>& args) -> bool {
            // 退出应用
            return true;
        });
}
```

### 3. 执行命令

```cpp
#include "runtime/function/console/console_macros.h"

// 使用宏执行命令
EXEC_CONSOLE_COMMAND("r.MaxFPS=120");
EXEC_CONSOLE_COMMAND("help");
EXEC_CONSOLE_COMMAND("reloadShaders");

// 直接调用
auto* console = GET_SYSTEM(ConsoleManager);
if (console)
{
    console->executeString("r.MaxFPS=120");
    console->executeString("help");
    console->executeString("reloadShaders arg1 arg2");
}
```

### 4. 获取变量值

```cpp
#include "runtime/function/console/console_macros.h"

// 使用宏获取变量（需要传入字符串字面量）
auto max_fps_var = GET_CONSOLE_VARIABLE_INT(r.MaxFPS);
if (max_fps_var)
{
    int current_fps = max_fps_var->getValue();
    LOG_INFO(ZEngine, "Current MaxFPS: {}", current_fps);
}

// 或者直接使用字符串（推荐）
auto* console = GET_SYSTEM(ConsoleManager);
if (console)
{
    auto var = console->findVariable("r.MaxFPS");
    if (var)
    {
        LOG_INFO(ZEngine, "r.MaxFPS = {}", var->getStringValue());
    }
}

```

### 5. 设置变量值

```cpp
// 通过命令字符串设置
EXEC_CONSOLE_COMMAND("r.MaxFPS=120");

// 直接设置
auto max_fps_var = GET_CONSOLE_VARIABLE_INT("r.MaxFPS");
if (max_fps_var)
{
    max_fps_var->setValue(120);
}
```

## 命令语法

### 变量操作

```cpp
// 查询变量值
r.MaxFPS          // 输出: r.MaxFPS = 60 (Maximum frames per second)

// 设置变量值
r.MaxFPS=120      // 设置MaxFPS为120
r.ShadowDistance=2000.0
r.DebugMode=1     // 设置Bool为true
r.DebugMode=0     // 设置Bool为false
r.LogLevel="debug" // 设置String（带引号）
```

### 命令执行

```cpp
// 执行命令（无参数）
help
history
clear

// 执行命令（带参数）
reloadShaders arg1 arg2
```

## 内置命令

ConsoleManager提供了以下内置命令：

- `help [name]` - 显示所有命令和变量的帮助信息，或特定命令/变量的帮助
- `history` - 显示命令历史记录
- `clear` - 清空命令历史记录

## 高级功能

### 自动补全

```cpp
auto* console = GET_SYSTEM(ConsoleManager);
if (console)
{
    // 获取匹配的命令/变量列表
    auto matches = console->getAutoCompleteList("r.");
    for (const auto& match : matches)
    {
        LOG_INFO(ZEngine, "Match: {}", match);
    }
}
```

### 命令历史

```cpp
auto* console = GET_SYSTEM(ConsoleManager);
if (console)
{
    // 获取历史记录
    const auto& history = console->getHistory();
    for (const auto& cmd : history)
    {
        LOG_INFO(ZEngine, "History: {}", cmd);
    }
    
    // 清空历史
    console->clearHistory();
}
```

### 值改变回调

```cpp
auto* console = GET_SYSTEM(ConsoleManager);
if (console)
{
    auto var = console->registerIntVariable("r.MaxFPS", "Maximum FPS", 60, &g_max_fps);
    
    // 设置值改变时的回调
    var->setOnChangedCallback([]() {
        LOG_INFO(ZEngine, "MaxFPS changed! New value: {}", g_max_fps);
        // 可以在这里更新渲染设置等
    });
}
```

## 实际应用示例

### 渲染系统集成

```cpp
// render_system.h
class RenderSystem : public IEngineSystem
{
private:
    int m_max_fps = 60;
    float m_shadow_distance = 1000.0f;
    bool m_enable_vsync = true;
    
public:
    bool initialize() override
    {
        // 注册控制台变量
        auto* console = GET_SYSTEM(ConsoleManager);
        if (console)
        {
            console->registerIntVariable("r.MaxFPS", "Maximum frames per second", 60, &m_max_fps);
            console->registerFloatVariable("r.ShadowDistance", "Shadow rendering distance", 1000.0f, &m_shadow_distance);
            console->registerBoolVariable("r.VSync", "Enable vertical sync", true, &m_enable_vsync);
            
            // 注册命令
            console->registerCommand("r.ReloadShaders", "Reload all shaders",
                [this](const std::vector<std::string>&) -> bool {
                    reloadShaders();
                    return true;
                });
        }
        
        return true;
    }
    
private:
    void reloadShaders() { /* ... */ }
};
```

### 物理系统集成

```cpp
// physics_manager.h
class PhysicsManager : public IEngineSystem
{
private:
    float m_gravity = -9.8f;
    bool m_enable_debug_draw = false;
    
public:
    bool initialize() override
    {
        auto* console = GET_SYSTEM(ConsoleManager);
        if (console)
        {
            console->registerFloatVariable("p.Gravity", "Gravity strength", -9.8f, &m_gravity);
            console->registerBoolVariable("p.DebugDraw", "Enable physics debug drawing", false, &m_enable_debug_draw);
        }
        return true;
    }
};
```

## 注意事项

1. **系统初始化顺序**：ConsoleManager在`PreInit`阶段初始化，确保在需要时已经可用
2. **宏注册时机**：使用宏自动注册时，如果系统还未初始化，注册会被跳过。建议在系统初始化后手动注册，或使用延迟注册机制
3. **线程安全**：当前实现不是线程安全的，如需在多线程环境中使用，需要添加同步机制
4. **命令参数解析**：命令参数使用空格分隔，支持带引号的字符串参数

## 与Unreal Engine的对比

| 功能 | Unreal Engine | ZEngine |
|------|---------------|---------|
| CVar支持 | ✅ | ✅ |
| 命令支持 | ✅ | ✅ |
| 自动注册宏 | `FAutoConsoleVariable` | `REGISTER_CONSOLE_VARIABLE_*` |
| 命令宏 | `FAutoConsoleCommand` | `REGISTER_CONSOLE_COMMAND` |
| 命令历史 | ✅ | ✅ |
| 自动补全 | ✅ | ✅ |
| 帮助系统 | ✅ | ✅ |
| 配置文件支持 | ✅ | ✅（`cvar.save` / `cvar.load`，2026-10-25） |
| 网络同步 | ✅ | ❌ |

## CVar 配置文件（2026-10-25）

UE `ConsoleVariables.ini` 风格的纯文本：每行 `name=value`。

```
cvar.save Saved/ConsoleVariables.ini     # 写出所有变量（按名字排序，首行是 ; 注释）
cvar.load Saved/ConsoleVariables.ini     # 逐行应用
```

- 读取时忽略空行、`#` / `;` 注释行和 `[Section]` 行（可直接读 UE 带 `[Startup]` 的文件）；名字不区分大小写；
  `=` 两侧与值两端的空白去掉；成对的双引号去掉。CRLF 文件可读。
- 未知变量、非法值、缺 `=` 的行各记一条警告并跳过，**后面的行照常应用**；`LoadVariablesFromFile` 返回成功条数
  （打不开文件返回 -1），可选输出失败行原文。
- `SaveVariablesToFile(path, &snapshot)` 只写与快照（`SnapshotVariableValues()`）不同的变量，用来存“用户改过的值”。
- 写出时会给两端带空白、或本身被引号包住的字符串值加引号，保证读回逐字一致；float 写成能精确读回的最短形式
  （原 `std::to_string` 固定 6 位小数，`1e-7` 会存成 `0.000000`）。
- 数值解析改为整串校验：`12abc`、超出 int 范围、空串都判为非法（原 `std::stoi` 会把 `12abc` 当成 12；web 构建关闭了
  异常捕获，`stoi` 遇到非数字直接让进程 abort）。
### 启动时自动加载（2026-10-26）

编辑器（`Editor/Main/Main.cpp`）和 `ZGame` 在 `START_SYSTEM()` 之后、第一帧之前调用
`ConsoleManager::LoadStartupVariables()`：

| 来源（优先级从高到低） | 效果 |
|---|---|
| 命令行 `--cvar-file <path>` | 读这个文件；`none` / `off` / `0` 关闭 |
| 环境变量 `ZENGINE_CVAR_FILE` | 同上 |
| 默认 | `<项目根>/Config/ConsoleVariables.ini`（项目级，进版本库），再叠加 `<项目根>/Saved/Config/ConsoleVariables.ini`（用户级，`Saved/` 不进版本库，个人调试开关放这里）；后者覆盖前者。没有项目时项目根取当前目录 |

- 文件不存在不算错误（只记一条 INFO），关闭时同样返回 0。
- 之后再应用命令行的 `--cvar name=value`，所以**命令行覆盖文件**（UE 同序）。
- 只在所有系统初始化完之后读，此时各模块的 CVar 都已注册；之后才注册的变量（例如某些面板首次打开时才注册）
  不会被启动文件命中，会记 `unknown variable` 警告。

- 测试：`Function/Console/Test/ConsoleVariablesFileSmokeTest.cpp`（62 项，C1-C8；C8 为启动文件 + 用户层），CMake 开关
  `ZENGINE_BUILD_CONSOLE_VARIABLES_FILE_SMOKE_TEST`；无头运行 `python scripts/web_smoke_run.py <该文件>`。

## 未来改进

- [x] 配置文件支持（保存/加载CVar值）——见下方“CVar 配置文件”
- [x] 启动时自动加载项目级 `Config/ConsoleVariables.ini`（2026-10-26，见上）
- [x] 用户级覆盖文件 `Saved/Config/ConsoleVariables.ini`（2026-10-26；显式 `--cvar-file` 时只用那一个文件，不叠加）
- [ ] 网络同步（多人游戏中同步CVar）
- [ ] 命令权限系统（区分开发/发布命令）
- [ ] 更强大的参数解析（支持命名参数等）
- [ ] 命令别名支持
- [ ] 线程安全改进

