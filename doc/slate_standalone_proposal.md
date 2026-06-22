# Slate 独立工程提案

## 目标

将 ZSlate UI 系统抽离为独立的静态库 `ZSlate`，类似 ImGui 的使用模式：

```cpp
// 用户代码示例
#include "ZSlate/Core/SlateApplication.h"
#include "ZSlate/Widgets/SButton.h"

// 链接 ZSlate 库
#pragma comment(lib, "ZSlate.lib")
```

## 当前状态 ✅ 全部完成

### Git 仓库

ZSlate 现在是一个独立的 Git 仓库，通过 submodule 集成到 ZEngine：

- **GitHub 仓库**: https://github.com/zentia/ZSlate
- **Submodule 路径**: `engine/Libraries/ZSlate`
- **ZEngine .gitmodules**: 已配置 submodule 引用

### ZSlate 库结构

```
engine/Libraries/ZSlate/
├── CMakeLists.txt                    # 独立构建配置
├── include/ZSlate/
│   ├── Core/                         # 核心类型和接口
│   │   ├── ZSlateTypes.h             # Vector2, Vector4, UIRect, UIColor
│   │   ├── SlateEnums.h              # 枚举定义
│   │   ├── SlateKeys.h               # 键定义
│   │   ├── SlateGeometry.h           # 几何类型
│   │   ├── SlatePaint.h              # ISlateRenderer, ISlatePlatform 接口
│   │   ├── SlateBrush.h              # Brush 定义
│   │   ├── SlateClipboard.h          # 剪贴板支持
│   │   └── SlateReply.h              # 输入回复
│   ├── Application/                  # 应用程序框架
│   │   ├── SlateApplication.h        # 应用程序入口
│   │   ├── SlateInput.h              # 输入路由器
│   │   └── SlateDragDrop.h           # 拖放支持
│   ├── Widgets/                      # Widget 类库 (20 个)
│   │   ├── SWidget.h                 # 基类
│   │   ├── SLeafWidget.h             # 叶子 Widget
│   │   ├── SCompoundWidget.h         # 复合 Widget
│   │   ├── SBoxPanel.h               # Box 面板
│   │   ├── SBorder.h                 # 边框 Widget
│   │   ├── SImage.h                  # 图像 Widget
│   │   ├── STextBlock.h              # 文本 Widget
│   │   ├── SSpacer.h                 # 空白填充
│   │   ├── SButton.h                 # 按钮 Widget
│   │   ├── SCheckBox.h               # 复选框
│   │   ├── SDropTarget.h             # 拖放目标
│   │   ├── SDragFloat.h              # 拖拽数值
│   │   ├── SEditableTextBox.h        # 文本输入框
│   │   ├── SSlider.h                 # 滑块
│   │   ├── SScrollBox.h              # 滚动容器
│   │   ├── SMenu.h                   # 菜单
│   │   ├── SOverlay.h                # Z-Stack 容器
│   │   ├── SSplitter.h               # 分割器
│   │   └── SWindowTitleBar.h         # 窗口标题栏/调整大小
│   └── Backend/
│       └── SlateUIRendererBackend.h  # 后端支持
├── src/                              # 实现文件
│   ├── Application/
│   │   ├── SlateApplication.cpp
│   │   └── SlateInput.cpp
│   └── Widgets/
│       ├── SBoxPanel.cpp
│       └── SSplitter.cpp
├── platform/ZEngine/                 # ZEngine 平台实现
│   ├── ZEngineSlateRenderer.h        # ISlateRenderer 实现
│   └── ZEngineSlateRenderer.cpp      # 桥接到 UiGpuResources
└── examples/
    └── simple_window.cpp             # 使用示例
```

### Runtime 集成状态

- ✅ `Runtime/CMakeLists.txt` 已配置链接 ZSlate 库
- ✅ ZEngine 平台实现 (`ZEngineSlateRenderer`) 已添加到 ZRuntime
- ⚠️ 原有 `Runtime/Slate/` 文件保留（向后兼容）

### 外部依赖 Slate 的文件

仍有 99+ 个文件使用旧的 include 路径（`Runtime/Slate/...`），这些文件在迁移期间保持不变。

## 依赖分析

### Slate 当前依赖

| 依赖 | 类型 | 说明 |
|------|------|------|
| `eastl` | 核心 | 容器、字符串 |
| `RHI` | 渲染 | 纹理上传、绘制命令 |
| `WindowSystem` | 平台 | 窗口、输入事件 |
| `Texture2D` | 资源 | 图片加载 |
| `UiGpuResources` | 渲染 | GPU 资源管理 |

### 依赖 Slate 的模块

| 模块 | 说明 | 处理方式 |
|------|------|----------|
| `Editor/ZSlate*` | 编辑器窗口 | 保留在 Editor，链接 ZSlate |
| `Runtime/UMG/` | UMG 框架 | 保留在 Runtime，链接 ZSlate |
| `Runtime/UI/` | UI 系统 | 保留在 Runtime，链接 ZSlate |
| `Tools/TexPreview/` | 独立工具 | 可直接链接 ZSlate |

## 实施计划

### 阶段 1：创建 ZSlate 独立库 ✅ 完成

- ✅ 创建 `Libraries/ZSlate/` 目录结构
- ✅ 创建独立的 `CMakeLists.txt`
- ✅ 定义抽象接口 (`ISlateRenderer`, `ISlatePlatform`)
- ✅ 复制所有 Widget 头文件 (20 个)
- ✅ 创建 ZEngine 平台实现 (`ZEngineSlateRenderer`)
- ✅ 更新 Runtime CMakeLists.txt 链接 ZSlate

### 阶段 2：更新依赖文件 (P2) ✅ 完成

已将 68+ 个外部依赖文件的 include 路径从：
```cpp
#include "Runtime/Slate/Widgets/SButton.h"
```
改为：
```cpp
#include "ZSlate/Widgets/SButton.h"
```

**保留的原始文件**（31 个）：
`Runtime/Slate/` 目录下的原始实现继续使用 `Runtime/Slate/` 路径。

**更新的外部文件**（68+ 个）：
- Editor/ 目录下的所有 ZSlate 窗口和菜单
- Runtime/UMG/ 目录下的 UMG Widget
- Runtime/UI/ 目录下的 UISystem
- Tools/TexPreview/ 目录下的工具
改为：
```cpp
#include "ZSlate/Widgets/SButton.h"
```

**策略**：渐进式迁移，可分批进行。

### 阶段 3：清理原始文件 (P3) ✅ 完成

已删除 `Runtime/Slate/` 目录（31 个文件）。

**删除的文件**：
- `Runtime/Slate/Application/` - SlateApplication, SlateInput
- `Runtime/Slate/Core/` - SlateGeometry, SlatePaint
- `Runtime/Slate/Backend/` - SlateUIRendererBackend
- `Runtime/Slate/Widgets/` - 20 个 Widget 头文件和 2 个实现文件
│       └── ZSlate/
│           ├── CMakeLists.txt      # 独立工程入口
│           ├── include/            # 公开头文件
│           │   └── ZSlate/
│           │       ├── Core/
│           │       ├── Application/
│           │       └── Widgets/
│           └── src/                 # 实现文件
│               ├── Core/
│               ├── Application/
│               ├── Widgets/
│               └── Backend/
```

### 阶段 3：构建系统改造 (P3) ✅ 完成

ZSlate 库的 CMakeLists.txt 已在 P1 阶段创建，包含：
- 独立的静态库目标
- 平台抽象支持
- 安装规则

Runtime CMakeLists.txt 已更新：
- 链接 ZSlate 库
- 包含 ZEngine 平台实现（ZEngineSlateRenderer）

**ZSlate/CMakeLists.txt**:

```cmake
cmake_minimum_required(VERSION 3.20)
project(ZSlate LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)

# 选项
option(ZSLATE_BUILD_STANDALONE "Build as standalone library" ON)
option(ZSLATE_BUILD_TESTS "Build tests" OFF)

# 平台抽象
if(NOT TARGET ZSlatePlatform)
    add_subdirectory(Platform/Default)  # 默认空实现
endif()

# Slate 核心
file(GLOB_RECURSE SLATE_SOURCES
    src/*.cpp
)

file(GLOB_RECURSE SLATE_HEADERS
    include/*.h
)

# 排除测试文件
list(FILTER SLATE_SOURCES EXCLUDE REGEX ".*_test\.cpp$")

add_library(ZSlate STATIC
    ${SLATE_SOURCES}
    ${SLATE_HEADERS}
)

target_include_directories(ZSlate PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(ZSlate PUBLIC
    ZSlatePlatform
)

# 导出头文件
install(DIRECTORY include/ZSlate
    DESTINATION ${CMAKE_INSTALL_PREFIX}/include
)

install(TARGETS ZSlate
    ARCHIVE DESTINATION ${CMAKE_INSTALL_PREFIX}/lib
    LIBRARY DESTINATION ${CMAKE_INSTALL_PREFIX}/bin
)
```

**修改 engine/Source/Runtime/CMakeLists.txt**:

```cmake
# 添加 Slate 依赖
set(ZSLATE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../../Libraries/ZSlate")
add_subdirectory(${ZSLATE_PATH} ${CMAKE_CURRENT_BINARY_DIR}/ZSlate)

# ZRuntime 链接 ZSlate
target_link_libraries(ZRuntime PRIVATE ZSlate)
```

### 阶段 4：提供平台实现 (P4)

**ZSlate/Platform/Default/CMakeLists.txt** (空实现，用于独立使用):

```cpp
// ZSlate/Platform/Default/DefaultSlatePlatform.h
namespace ZSlate
{
    // 默认空实现，独立使用时需要用户提供实现
    struct DefaultSlatePlatform : ISlatePlatform
    {
        void* CreateTexture(uint32_t, uint32_t, void*) override { return nullptr; }
        void DestroyTexture(void*) override {}
        void SubmitDrawCommands(DrawCommandBatch&) override {}
        // ... 其他空实现
    };
}
```

**ZSlate/Platform/ZEngine/CMakeLists.txt** (ZEngine 实现):

```cpp
// ZSlate/Platform/ZEngine/ZEngineSlatePlatform.h
#include "Runtime/RHI/RHI.h"
#include "Runtime/Function/Render/Platform/WindowSystem.h"
#include "Runtime/UI/Render/UiGpuResources.h"

namespace ZSlate
{
    // ZEngine 具体实现
    struct ZEngineSlatePlatform : ISlatePlatform
    {
        void* CreateTexture(uint32_t width, uint32_t height, void* pixels) override
        {
            return UiGpuResources::Instance()->CreateFromPixels(
                static_cast<uint8_t*>(pixels), width, height, RHI_FORMAT_R8G8B8A8_UNORM);
        }
        // ... 其他 ZEngine 特定实现
    };
}
```

### 阶段 5：使用示例 (P5)

**独立项目使用 ZSlate**:

```cpp
// main.cpp
#include "ZSlate/Core/SlateApplication.h"
#include "ZSlate/Widgets/SButton.h"
#include "ZSlate/Widgets/STextBlock.h"

// 平台实现（用户需要提供）
#include "MySlatePlatform.h"

int main()
{
    // 设置平台实现
    ZSlate::SetPlatform(new MySlatePlatform());
    
    // 创建应用
    auto app = ZSlate::SlateApplication::Create();
    
    // 创建窗口内容
    auto root = ZSlate::SVerticalBox::Create();
    
    auto text = ZSlate::STextBlock::Create();
    text->SetText("Hello ZSlate!");
    root->AddChild(text);
    
    auto button = ZSlate::SButton::Create();
    button->SetText("Click Me");
    button->OnClicked([app]() {
        app->Exit();
    });
    root->AddChild(button);
    
    // 运行
    app->SetRootWidget(root);
    app->Run();
    
    return 0;
}
```

**CMakeLists.txt (独立项目)**:

```cmake
find_package(ZSlate REQUIRED)

add_executable(MyApp main.cpp)
target_link_libraries(MyApp PRIVATE ZSlate)
```

## 实施优先级

| 阶段 | 内容 | 状态 | 工作量 |
|------|------|------|--------|
| P1 | 抽象平台依赖 | ✅ 完成 | 中 |
| P2 | 重构目录结构 | ✅ 完成 | 低 |
| P3 | 构建系统改造 | ✅ 完成 | 中 |
| P4 | 提供平台实现 | ✅ 完成 | 中 |
| P5 | 文档和示例 | ✅ 完成 | 低 |

## 风险和注意事项

1. **破坏性变更**: 需要修改所有依赖 Slate 的代码
2. **循环依赖**: 确保 ZSlate 不依赖 Runtime 的高层模块
3. **API 稳定性**: 抽象接口需要设计得足够通用
4. **性能**: 虚函数调用可能带来轻微开销

## 时间估算

- P1+P2+P3: 2-3 周（核心改造）
- P4: 1 周（ZEngine 平台实现）
- P5: 0.5 周（文档）

## 备选方案

如果完整抽离成本太高，可以考虑：

**方案 B：保持现状，仅提供头文件**
- 不改变构建结构
- 将 Slate 头文件复制/链接到独立位置
- 用户仍需链接 ZRuntime
- 优点：改动小
- 缺点：用户仍需依赖整个 ZRuntime

**方案 C：渐进式抽离**
- 先抽离 Core 和 Application（无平台依赖的部分）
- Widgets 保留在 Runtime 中
- 逐步将 Widget 迁移到独立库

### 阶段 5：文档和示例 (P5) ✅ 完成

已创建以下文档和示例：

**文档**：
- `engine/Libraries/ZSlate/examples/README.md` - 完整的使用指南
  - 架构概览（Core、Application、Widgets 三层）
  - 所有可用 Widget 的列表
  - `ISlateRenderer` 和 `ISlatePlatform` 接口文档
  - 类型参考（Vector2、UIRect、UIColor）
  - CMake 集成指南

**示例代码**：
- `engine/Libraries/ZSlate/examples/simple_window.cpp` - 最小化 Hello World 示例
  - MockRenderer 实现（输出到控制台）
  - 基础 Widget 使用（STextBlock、SButton）
  - 自定义 Widget（SDemoButton）

- `engine/Libraries/ZSlate/examples/standalone_app_example.cpp` - 完整应用示例
  - 完整的 MockRenderer 实现
  - 多种 Widget 的使用（SButton、SCheckBox、SSlider、SScrollBox）
  - 自定义 Widget（SColorPicker、SLabelledField）
  - 100 项滚动列表示例

- `engine/Libraries/ZSlate/examples/CMakeLists.txt` - 示例构建配置

**构建选项**：
- ZSlate CMakeLists.txt 已包含 `ZSLATE_BUILD_EXAMPLES` 选项
- 启用后自动构建示例程序
