# ZEngine UI系统快速开始指南

## 概述

ZEngine UI系统是一个基于组件和Widget树的UI框架，结合了Unity UGUI和Unreal Engine UMG的设计理念。

## 核心概念

### 1. Widget（UI元素）

所有UI元素都继承自`Widget`类，可以附加到`GameObject`上作为组件。

### 2. Canvas（画布）

`Canvas`是UI的根容器，负责管理UI渲染和事件分发。每个场景可以有多个Canvas。

### 3. RectTransform（2D变换）

`RectTransform`提供2D UI的定位、缩放和旋转，类似于Unity的RectTransform。

### 4. 事件系统

`UIEventSystem`是全局单例，负责处理所有UI输入事件。

## 快速开始

### 步骤1: 创建Canvas

```cpp
#include "runtime/function/ui/canvas.h"
#include "runtime/function/ui/ui_event_system.h"

// 创建Canvas GameObject
auto canvas_go = world->createGameObject("Canvas");
auto canvas = canvas_go->addComponent<Canvas>();
canvas->initialize(CanvasRenderMode::ScreenSpaceOverlay);

// 注册到事件系统
UIEventSystem::getInstance()->registerCanvas(canvas);
```

### 步骤2: 创建按钮

```cpp
#include "runtime/function/ui/button.h"
#include "runtime/function/ui/image.h"
#include "runtime/function/ui/text.h"
#include "runtime/function/ui/rect_transform.h"

// 创建按钮GameObject（作为Canvas的子对象）
auto button_go = world->createGameObject("Button", canvas_go);

// 添加RectTransform
auto rect_transform = button_go->addComponent<RectTransform>();
rect_transform->setAnchorPreset(AnchorPreset::MiddleCenter);
rect_transform->setSizeDelta(Vector2(200, 50));

// 添加Image作为背景
auto image = button_go->addComponent<Image>();
image->setColor(UIColor(0.2f, 0.6f, 1.0f, 1.0f));

// 创建文本子对象
auto text_go = world->createGameObject("Text", button_go);
auto text_rect = text_go->addComponent<RectTransform>();
text_rect->setAnchorPreset(AnchorPreset::StretchAll);

auto text = text_go->addComponent<Text>();
text->setText("Click Me");
text->setAlignment(TextAnchor::MiddleCenter);
text->setColor(UIColor(1.0f, 1.0f, 1.0f, 1.0f));

// 添加Button组件
auto button = button_go->addComponent<Button>();
button->setOnClick([]() {
    LOG_INFO("Button clicked!");
});
```

### 步骤3: 处理输入事件

在输入系统中，将事件传递给UI系统：

```cpp
#include "runtime/function/ui/ui_event_system.h"
#include "runtime/function/ui/ui_types.h"

void InputSystem::tick(float delta_time)
{
    // 创建输入事件
    InputEvent event;
    event.type = InputEventType::MouseClick;
    event.mouse_position = Vector2(mouse_x, mouse_y);
    event.mouse_button = 0;
    
    // 先传递给UI系统
    if (UIEventSystem::getInstance()->processInput(event))
    {
        return;  // UI系统已处理，不传递给游戏逻辑
    }
    
    // 否则传递给游戏逻辑
    // ...
}
```

## 常用操作

### 设置Anchor

```cpp
// 使用预设
rect_transform->setAnchorPreset(AnchorPreset::TopLeft);

// 手动设置
rect_transform->setAnchors(Vector2(0.0f, 1.0f), Vector2(0.0f, 1.0f));  // TopLeft
rect_transform->setAnchors(Vector2(0.5f, 0.5f), Vector2(0.5f, 0.5f));  // Center
rect_transform->setAnchors(Vector2(0.0f, 0.0f), Vector2(1.0f, 1.0f)); // StretchAll
```

### 设置位置和大小

```cpp
// 设置位置（相对于anchor）
rect_transform->setAnchoredPosition(Vector2(100, 50));

// 设置大小
rect_transform->setSizeDelta(Vector2(200, 100));
```

### 设置文本

```cpp
text->setText("Hello World");
text->setFontSize(24);
text->setColor(UIColor(1.0f, 0.0f, 0.0f, 1.0f));  // 红色
text->setAlignment(TextAnchor::MiddleCenter);
```

### 设置图片

```cpp
// 设置颜色
image->setColor(UIColor(1.0f, 1.0f, 1.0f, 1.0f));

// 设置填充量（用于进度条等）
image->setFillAmount(0.75f);  // 75%

// TODO: 设置Sprite/Texture（待实现）
// image->setSprite(sprite);
```

### 按钮状态

```cpp
button->setNormalColor(UIColor(0.2f, 0.6f, 1.0f, 1.0f));
button->setHighlightedColor(UIColor(0.3f, 0.7f, 1.0f, 1.0f));
button->setPressedColor(UIColor(0.1f, 0.5f, 0.9f, 1.0f));
button->setDisabledColor(UIColor(0.5f, 0.5f, 0.5f, 1.0f));
```

## Anchor预设说明

- `TopLeft`, `TopCenter`, `TopRight`: 顶部对齐
- `MiddleLeft`, `MiddleCenter`, `MiddleRight`: 中间对齐
- `BottomLeft`, `BottomCenter`, `BottomRight`: 底部对齐
- `StretchTop`, `StretchMiddle`, `StretchBottom`: 垂直拉伸
- `StretchLeft`, `StretchCenter`, `StretchRight`: 水平拉伸
- `StretchAll`: 完全拉伸（填充父容器）

## 注意事项

1. **Canvas注册**: 创建Canvas后必须注册到`UIEventSystem`
2. **RectTransform**: 所有UI元素都需要`RectTransform`组件
3. **父子关系**: 通过GameObject的父子关系建立UI层次结构
4. **事件处理**: 输入事件先传递给UI系统，如果被处理则不传递给游戏逻辑
5. **渲染**: UI渲染需要在渲染管线中调用（待实现）

## 待实现功能

- 布局系统（HorizontalLayout, VerticalLayout, GridLayout）
- UI渲染系统（UIRenderer）
- 资源系统（Sprite, Font）
- 其他UI控件（InputField, ScrollView, Slider等）

## 参考文档

- [UI系统设计文档](UI_SYSTEM_DESIGN.md)
- [UI系统实现总结](UI_SYSTEM_IMPLEMENTATION.md)

