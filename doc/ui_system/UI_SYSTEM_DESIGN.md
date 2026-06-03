# ZEngine UI系统设计文档

## 概述

ZEngine UI系统是一个基于组件和Widget树的UI框架，结合了Unity UGUI和Unreal Engine UMG的设计理念，为ZEngine提供完整的用户界面解决方案。

### 设计目标

1. **易用性**：提供直观的API和编辑器支持
2. **性能**：高效的渲染和布局计算
3. **灵活性**：支持复杂的布局和自定义控件
4. **可扩展性**：易于扩展新的UI组件
5. **多线程安全**：与ZEngine的多线程渲染架构集成

## 系统架构

### 整体架构图

```
┌─────────────────────────────────────────────────────────────┐
│                    UI System Layer                          │
├─────────────────────────────────────────────────────────────┤
│  Widget Tree  │  Layout System  │  Event System            │
│  (Widget基类)  │  (布局计算)      │  (输入处理)               │
└──────────┬───────────┬───────────┬──────────────────────────┘
           │           │           │
           └───────────┴───────────┘
                      │
           ┌──────────▼───────────┐
           │   UI Render System    │  ← UI渲染系统
           │   (UI Renderer)        │
           └──────────┬───────────┘
                      │
           ┌──────────▼───────────┐
           │   Render Pipeline    │  ← 现有渲染管线
           │   (UIPass)            │
           └──────────────────────┘
```

### 核心组件

1. **Widget系统**：UI元素的基础类
2. **Canvas系统**：UI渲染画布
3. **RectTransform系统**：2D变换和布局
4. **Layout系统**：自动布局组件
5. **Event系统**：输入和事件处理
6. **Renderer系统**：UI渲染器

## 核心组件设计

### 1. Widget（UI元素基类）

Widget是所有UI元素的基础类，类似于Unity的UIBehaviour和Unreal的UWidget。

```cpp
class Widget : public Component
{
public:
    // 基础属性
    bool isVisible() const;
    void setVisible(bool visible);
    
    bool isEnabled() const;
    void setEnabled(bool enabled);
    
    // 变换
    RectTransform* getRectTransform();
    
    // 渲染
    virtual void onRender(UIRenderer* renderer);
    
    // 事件
    virtual void onMouseEnter();
    virtual void onMouseExit();
    virtual void onMouseDown();
    virtual void onMouseUp();
    virtual void onClick();
    
    // 布局
    virtual Vector2 getPreferredSize() const;
    virtual void onLayoutChanged();
    
protected:
    bool m_visible {true};
    bool m_enabled {true};
    RectTransform* m_rect_transform {nullptr};
};
```

### 2. Canvas（画布）

Canvas是UI的根容器，负责管理UI渲染和事件分发。

```cpp
enum class CanvasRenderMode
{
    ScreenSpaceOverlay,      // 屏幕空间覆盖（类似UGUI）
    ScreenSpaceCamera,       // 屏幕空间相机
    WorldSpace              // 世界空间
};

class Canvas : public Widget
{
public:
    void initialize(CanvasRenderMode mode);
    
    // 渲染
    void render(UIRenderer* renderer);
    
    // 事件处理
    bool handleInput(const InputEvent& event);
    
    // 布局更新
    void updateLayout();
    
private:
    CanvasRenderMode m_render_mode;
    std::shared_ptr<Camera> m_camera;  // 用于ScreenSpaceCamera模式
    std::vector<Widget*> m_root_widgets;  // 根Widget列表
};
```

### 3. RectTransform（2D变换）

RectTransform提供2D UI的定位、缩放和旋转，类似于Unity的RectTransform。

```cpp
enum class AnchorPreset
{
    TopLeft, TopCenter, TopRight,
    MiddleLeft, MiddleCenter, MiddleRight,
    BottomLeft, BottomCenter, BottomRight,
    StretchTop, StretchMiddle, StretchBottom,
    StretchLeft, StretchCenter, StretchRight,
    StretchAll
};

class RectTransform : public Component
{
public:
    // Anchor和Pivot
    void setAnchorMin(const Vector2& min);
    void setAnchorMax(const Vector2& max);
    void setPivot(const Vector2& pivot);
    
    // 位置和大小
    void setAnchoredPosition(const Vector2& position);
    void setSizeDelta(const Vector2& size);
    
    // 预设
    void setAnchorPreset(AnchorPreset preset);
    
    // 变换矩阵
    Matrix4x4 getLocalToWorldMatrix() const;
    Rect getRect() const;
    
    // 父子关系
    void setParent(RectTransform* parent);
    std::vector<RectTransform*> getChildren() const;
    
private:
    Vector2 m_anchor_min {0.0f, 0.0f};
    Vector2 m_anchor_max {1.0f, 1.0f};
    Vector2 m_pivot {0.5f, 0.5f};
    Vector2 m_anchored_position {0.0f, 0.0f};
    Vector2 m_size_delta {100.0f, 100.0f};
    
    RectTransform* m_parent {nullptr};
    std::vector<RectTransform*> m_children;
    
    bool m_dirty {true};
    Matrix4x4 m_local_to_world_matrix;
};
```

### 4. Layout系统

#### 4.1 Layout组件基类

```cpp
class LayoutComponent : public Component
{
public:
    virtual void calculateLayout() = 0;
    virtual Vector2 getPreferredSize() const = 0;
    
protected:
    bool m_dirty {true};
};
```

#### 4.2 HorizontalLayout（水平布局）

```cpp
class HorizontalLayout : public LayoutComponent
{
public:
    void setSpacing(float spacing);
    void setPadding(const RectOffset& padding);
    void setChildAlignment(TextAnchor alignment);
    
    void calculateLayout() override;
    Vector2 getPreferredSize() const override;
    
private:
    float m_spacing {0.0f};
    RectOffset m_padding;
    TextAnchor m_child_alignment {TextAnchor::UpperLeft};
};
```

#### 4.3 VerticalLayout（垂直布局）

类似HorizontalLayout，但垂直排列。

#### 4.4 GridLayout（网格布局）

```cpp
class GridLayout : public LayoutComponent
{
public:
    void setCellSize(const Vector2& size);
    void setSpacing(const Vector2& spacing);
    void setStartCorner(GridLayoutGroup::Corner corner);
    void setStartAxis(GridLayoutGroup::Axis axis);
    void setChildAlignment(TextAnchor alignment);
    void setConstraint(GridLayoutGroup::Constraint constraint);
    void setConstraintCount(int count);
    
    void calculateLayout() override;
    Vector2 getPreferredSize() const override;
    
private:
    Vector2 m_cell_size {100.0f, 100.0f};
    Vector2 m_spacing {0.0f, 0.0f};
    // ... 其他属性
};
```

### 5. 基础UI控件

#### 5.1 Image（图片）

```cpp
class Image : public Widget
{
public:
    void setSprite(std::shared_ptr<Sprite> sprite);
    void setColor(const Color& color);
    void setFillAmount(float amount);  // 用于进度条等
    
    void onRender(UIRenderer* renderer) override;
    
private:
    std::shared_ptr<Sprite> m_sprite;
    Color m_color {1.0f, 1.0f, 1.0f, 1.0f};
    float m_fill_amount {1.0f};
};
```

#### 5.2 Text（文本）

```cpp
class Text : public Widget
{
public:
    void setText(const std::string& text);
    void setFont(std::shared_ptr<Font> font);
    void setFontSize(int size);
    void setColor(const Color& color);
    void setAlignment(TextAnchor alignment);
    void setWrapMode(TextWrapMode mode);
    
    void onRender(UIRenderer* renderer) override;
    Vector2 getPreferredSize() const override;
    
private:
    std::string m_text;
    std::shared_ptr<Font> m_font;
    int m_font_size {14};
    Color m_color {0.0f, 0.0f, 0.0f, 1.0f};
    TextAnchor m_alignment {TextAnchor::UpperLeft};
    TextWrapMode m_wrap_mode {TextWrapMode::Wrap};
};
```

#### 5.3 Button（按钮）

```cpp
class Button : public Widget
{
public:
    // 状态
    enum class ButtonState
    {
        Normal,
        Highlighted,
        Pressed,
        Disabled
    };
    
    void setOnClick(std::function<void()> callback);
    void setNormalColor(const Color& color);
    void setHighlightedColor(const Color& color);
    void setPressedColor(const Color& color);
    
    void onMouseEnter() override;
    void onMouseExit() override;
    void onMouseDown() override;
    void onMouseUp() override;
    void onClick() override;
    void onRender(UIRenderer* renderer) override;
    
private:
    ButtonState m_state {ButtonState::Normal};
    std::function<void()> m_on_click;
    Color m_normal_color;
    Color m_highlighted_color;
    Color m_pressed_color;
    
    Image* m_target_graphic {nullptr};  // 目标图形组件
};
```

#### 5.4 其他控件

- **InputField**：文本输入框
- **ScrollView**：滚动视图
- **Slider**：滑块
- **Toggle**：开关
- **Dropdown**：下拉菜单
- **Scrollbar**：滚动条

### 6. Event系统

#### 6.1 输入事件

```cpp
enum class InputEventType
{
    MouseMove,
    MouseDown,
    MouseUp,
    MouseClick,
    MouseEnter,
    MouseExit,
    KeyDown,
    KeyUp,
    Scroll
};

struct InputEvent
{
    InputEventType type;
    Vector2 mouse_position;
    int mouse_button;
    int key_code;
    float scroll_delta;
};
```

#### 6.2 事件分发器

```cpp
class UIEventSystem
{
public:
    static UIEventSystem* getInstance();
    
    // 事件处理
    bool processInput(const InputEvent& event);
    
    // 射线检测
    Widget* raycast(const Vector2& screen_position);
    
    // 焦点管理
    void setFocusedWidget(Widget* widget);
    Widget* getFocusedWidget() const;
    
private:
    std::vector<Canvas*> m_canvases;
    Widget* m_focused_widget {nullptr};
    Widget* m_hovered_widget {nullptr};
};
```

### 7. UI渲染系统

#### 7.1 UI渲染器

```cpp
class UIRenderer
{
public:
    void initialize(std::shared_ptr<RHI> rhi);
    void shutdown();
    
    // 渲染命令
    void drawQuad(const Rect& rect, const Color& color);
    void drawTexture(const Rect& rect, std::shared_ptr<Texture> texture, const Color& color);
    void drawText(const Rect& rect, const std::string& text, std::shared_ptr<Font> font, const Color& color);
    
    // 裁剪
    void pushClipRect(const Rect& rect);
    void popClipRect();
    
    // 变换
    void pushTransform(const Matrix4x4& transform);
    void popTransform();
    
    // 提交
    void submit();
    
private:
    std::shared_ptr<RHI> m_rhi;
    std::vector<UIRenderCommand> m_render_commands;
    std::stack<Rect> m_clip_stack;
    std::stack<Matrix4x4> m_transform_stack;
};
```

#### 7.2 UI渲染命令

```cpp
enum class UIRenderCommandType
{
    DrawQuad,
    DrawTexture,
    DrawText
};

struct UIRenderCommand
{
    UIRenderCommandType type;
    Rect rect;
    Color color;
    std::shared_ptr<Texture> texture;
    std::shared_ptr<Font> font;
    std::string text;
    Matrix4x4 transform;
    Rect clip_rect;
};
```

## 使用示例

### 创建简单的UI

```cpp
// 创建Canvas
auto canvas_go = world->createGameObject("Canvas");
auto canvas = canvas_go->addComponent<Canvas>();
canvas->initialize(CanvasRenderMode::ScreenSpaceOverlay);

// 创建按钮
auto button_go = world->createGameObject("Button", canvas_go);
auto rect_transform = button_go->addComponent<RectTransform>();
rect_transform->setAnchorPreset(AnchorPreset::MiddleCenter);
rect_transform->setSizeDelta(Vector2(200, 50));

auto image = button_go->addComponent<Image>();
image->setColor(Color(0.2f, 0.6f, 1.0f, 1.0f));

auto text_go = world->createGameObject("Text", button_go);
auto text = text_go->addComponent<Text>();
text->setText("Click Me");
text->setAlignment(TextAnchor::MiddleCenter);

auto button = button_go->addComponent<Button>();
button->setOnClick([]() {
    LOG_INFO("Button clicked!");
});
```

### 使用布局系统

```cpp
// 创建水平布局
auto layout_go = world->createGameObject("HorizontalLayout", canvas_go);
auto layout = layout_go->addComponent<HorizontalLayout>();
layout->setSpacing(10.0f);
layout->setPadding(RectOffset(10, 10, 10, 10));

// 添加子元素
for (int i = 0; i < 5; ++i)
{
    auto item_go = world->createGameObject("Item", layout_go);
    auto item_image = item_go->addComponent<Image>();
    // ...
}
```

## 性能优化

1. **脏标记系统**：只有标记为dirty的Widget才重新计算布局
2. **批处理渲染**：合并相同材质的渲染命令
3. **裁剪优化**：使用层次包围盒快速剔除不可见元素
4. **对象池**：重用UI元素对象
5. **异步加载**：纹理和字体异步加载

## 与现有系统集成

### 与渲染管线集成

UI系统通过UIPass集成到现有渲染管线：

```cpp
// 在UIPass中
void UIPass::draw()
{
    // 原有的ImGui渲染
    if (m_window_ui)
    {
        m_window_ui->preRender();
        ImGui::Render();
        // ...
    }
    
    // 新的Widget系统渲染
    if (m_ui_system)
    {
        m_ui_system->render(m_rhi);
    }
}
```

### 与输入系统集成

```cpp
// 在InputSystem中
void InputSystem::tick(float delta_time)
{
    // 处理输入事件
    InputEvent event;
    // ... 填充event
    
    // 先传递给UI系统
    if (UIEventSystem::getInstance()->processInput(event))
    {
        return;  // UI系统已处理，不传递给游戏逻辑
    }
    
    // 否则传递给游戏逻辑
    // ...
}
```

## 未来扩展

1. **UI动画系统**：支持补间动画和状态机
2. **UI编辑器**：可视化UI编辑器
3. **数据绑定**：MVVM模式支持
4. **样式系统**：类似CSS的样式系统
5. **国际化**：多语言支持
6. **无障碍功能**：屏幕阅读器等支持

## 总结

ZEngine UI系统结合了UGUI和UMG的优点，提供了：
- 基于组件的灵活架构
- 强大的布局系统
- 高效的事件处理
- 与现有渲染管线无缝集成
- 易于扩展和定制

