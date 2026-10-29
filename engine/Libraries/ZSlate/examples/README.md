# ZSlate Examples

This directory contains example code demonstrating how to use the ZSlate UI framework.

## Quick Start

### Minimal Example (Console Output)

The `simple_window.cpp` file demonstrates a minimal "Hello World" application using ZSlate with a mock renderer that outputs draw commands to the console.

### Real Application (with ZEngine)

To use ZSlate in a ZEngine-based project, see the Editor modules which already integrate ZSlate:
- `engine/Source/Editor/ZSlate/` - Editor UI implementation
- `engine/Source/Runtime/UI/` - Runtime UI system

## Architecture Overview

ZSlate is a **retained-mode** UI framework (similar to Unreal Engine's Slate). It has three main abstraction layers:

### 1. Core Types (`ZSlate/Core/`)

| Header | Description |
|--------|-------------|
| `ZSlateTypes.h` | `Vector2`, `Vector4`, `UIRect`, `UIColor` math types |
| `SlateGeometry.h` | Geometry utilities, margins, alignment |
| `SlatePaint.h` | `ISlateRenderer`, `ISlatePlatform` interfaces |
| `SlateBrush.h` | Brush definitions for styling |
| `SlateClipboard.h` | Clipboard abstractions |
| `SlateReply.h` | Input event replies |

### 2. Application Framework (`ZSlate/Application/`)

| Header | Description |
|--------|-------------|
| `SlateApplication.h` | Application singleton, layout + paint cycle |
| `SlateInput.h` | Input routing, event dispatch |
| `SlateDragDrop.h` | Drag-and-drop support |

### 3. Widget Library (`ZSlate/Widgets/`)

| Header | Description |
|--------|-------------|
| `SWidget.h` | Base widget class |
| `SLeafWidget.h` | Leaf widget (no children) |
| `SCompoundWidget.h` | Container widget (has children) |
| `SBoxPanel.h` | Vertical/Horizonal box layout |
| `SBorder.h` | Decorative border container |
| `SButton.h` | Clickable button |
| `STextBlock.h` | Text display |
| `SSpacer.h` | Empty space |
| `SCheckBox.h` | Check box |
| `SSlider.h` | Slider control |
| `SScrollBox.h` | Scrollable container |
| `SOverlay.h` | Z-ordered overlay (Z-Stack) |
| `SSplitter.h` | Resizable splitter |
| `SEditableTextBox.h` | Text input field |
| `SMenu.h` | Menu system |

## Platform Integration

To use ZSlate, you must provide implementations of the following interfaces:

### ISlateRenderer

```cpp
struct ISlateRenderer
{
    virtual void drawQuad(const UIRect& rect, const UIColor& color) = 0;
    virtual void drawRect(const UIRect& rect, const UIColor& color, float thickness = 1.0f) = 0;
    virtual void drawConvexPoly(const Vector2* points, int count, const UIColor& color) = 0;
    virtual void drawRoundedRect(const UIRect& rect, float radius, const UIColor& color) = 0;
    virtual void drawTexturedQuad(const UIRect& rect, void* texture_handle, const UIColor& tint = Colors::White) = 0;
    virtual void drawBox(const UIRect& rect, const FMargin& margin, void* texture_handle, const UIColor& tint) = 0;
    virtual void drawBorder(const UIRect& rect, const FMargin& margin, void* texture_handle, const UIColor& tint) = 0;
    virtual void drawText(const UIRect& rect, const std::string& text, float font_size, const UIColor& color, TextAnchor alignment = TextAnchor::MiddleLeft, TextWrapMode wrap = TextWrapMode::NoWrap, void* font_handle = nullptr) = 0;
    virtual void drawText(const std::string& text, const Vector2& pos, float font_size, const UIColor& color) = 0;
    virtual Vector2 measureText(const std::string& text, float font_size) const = 0;
    virtual void pushClipRect(const UIRect& rect) = 0;
    virtual void popClipRect() = 0;
    virtual void beginFrame() = 0;
    virtual void endFrame() = 0;
    virtual void flush() = 0;
};
```

### ISlatePlatform

```cpp
struct ISlatePlatform
{
    virtual ISlateRenderer* getRenderer() = 0;
    virtual Vector2 getMousePosition() const = 0;
    virtual bool isMouseButtonDown(int button) const = 0;
    virtual bool isKeyDown(int key) const = 0;
    virtual float getTimeSeconds() const = 0;
    virtual Vector2 getWindowSize() const = 0;
};
```

## Basic Usage Pattern

```cpp
#include "ZSlate/Application/SlateApplication.h"
#include "ZSlate/Core/SlatePaint.h"
#include "ZSlate/Widgets/SButton.h"
#include "ZSlate/Widgets/STextBlock.h"

// 1. Implement ISlateRenderer and ISlatePlatform
class MyRenderer : public ZSlate::ISlateRenderer { /* ... */ };
class MyPlatform : public ZSlate::ISlatePlatform
{
    MyRenderer Renderer;
    // ...
};

int main()
{
    // 2. Create and set platform
    MyPlatform platform;
    ZSlate::SetPlatform(&platform);

    // 3. Create text measurer
    auto measurer = std::make_shared<ZSlate::SlateUIRendererTextMeasurer>(&platform.Renderer);
    ZSlate::SlateApplication::Get().SetTextMeasurer(measurer.get());

    // 4. Build widget tree
    auto root = std::make_shared<ZSlate::SVerticalBox>();
    
    auto title = std::make_shared<ZSlate::STextBlock>();
    title->Text = "Hello ZSlate!";
    title->FontSize = 24.0f;
    root->AddSlot(title);

    auto button = std::make_shared<ZSlate::SButton>();
    button->SetText("Click Me");
    button->OnClicked.AddLambda([app]() {
        app->Exit();
    });
    root->AddSlot(button);

    // 5. Set root and paint
    ZSlate::SlateApplication::Get().SetRootContent(root);

    platform.Renderer.beginFrame();
    ZSlate::UIRect region(0, 0, 800, 600);
    ZSlate::SlateApplication::Get().PaintInto(&platform.Renderer, region);
    platform.Renderer.endFrame();
    platform.Renderer.flush();

    return 0;
}
```

## CMake Integration

Add ZSlate to your CMake project:

```cmake
# Find ZSlate (when installed)
# find_package(ZSlate REQUIRED)

# Or build from source
add_subdirectory(${ZSLATE_PATH}/Libraries/ZSlate ${CMAKE_CURRENT_BINARY_DIR}/ZSlate)

add_executable(MyApp main.cpp)
target_link_libraries(MyApp PRIVATE ZSlate)

# Include directories are set automatically
target_include_directories(MyApp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
```

## ZEngine Integration

ZEngine provides a built-in platform implementation at:
- `engine/Libraries/ZSlate/platform/ZEngine/ZEngineSlateRenderer.h`
- `engine/Libraries/ZSlate/platform/ZEngine/ZEngineSlateRenderer.cpp`

This bridges ZSlate to ZEngine's `UiGpuResources` for GPU-accelerated UI rendering.

## Type Reference

### Vector2

```cpp
struct Vector2
{
    float x, y;
    
    Vector2 operator+(const Vector2& Other) const;
    Vector2 operator-(const Vector2& Other) const;
    Vector2 operator*(float S) const;
    float Length() const;
    Vector2 GetNormalized() const;
};
```

### UIRect

```cpp
struct UIRect
{
    float x, y, w, h;
    
    float Left() const;
    float Top() const;
    float Right() const;
    float Bottom() const;
    Vector2 Position() const;
    Vector2 Size() const;
    float Width() const;
    float Height() const;
    bool Contains(float Px, float Py) const;
    bool Intersects(const UIRect& Other) const;
};
```

### UIColor

```cpp
using UIColor = Vector4;  // RGBA, 0.0-1.0

namespace Colors
{
    constexpr UIColor White = UIColor(1, 1, 1, 1);
    constexpr UIColor Black = UIColor(0, 0, 0, 1);
    constexpr UIColor Red = UIColor(1, 0, 0, 1);
    constexpr UIColor Green = UIColor(0, 1, 0, 1);
    constexpr UIColor Blue = UIColor(0, 0, 1, 1);
    constexpr UIColor Yellow = UIColor(1, 1, 0, 1);
    constexpr UIColor Cyan = UIColor(0, 1, 1, 1);
    constexpr UIColor Magenta = UIColor(1, 0, 1, 1);
    constexpr UIColor Transparent = UIColor(0, 0, 0, 0);
}
```

## License

ZSlate is part of the ZEngine project. See the project LICENSE for details.
