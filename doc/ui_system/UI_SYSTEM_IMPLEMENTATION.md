# ZEngine UI系统实现总结

> **状态 (2026-05, UGUI 退役)**: 本文档描述的 UGUI 控件模型
> (`Canvas` / `Widget` / `RectTransform` / `Selectable` / `UIEventSystem` /
> `Controls/*` / `Layout/*` / `Demo/*`) **已删除**, 由 UE 风格的 UMG
> (`Runtime/UMG/`, 构建在 `Runtime/Slate/` 之上) 取代。详见
> `doc/` 下的 UMG/ZSlate 设计与本仓库的 `Runtime/UMG`、`Runtime/Slate`。
> **仍然保留**的是共享 GPU 渲染层 (`UIRenderer` / `BatchedUIRenderer` /
> `UiRenderBatch` / `UiGpuResources` / `UiAffine2D`) + `Font` + `UITypes`,
> 这些已从 `Runtime/UGUI/` **迁移到中性目录 `Runtime/UI/`**
> (`UI/Render/` + `UI/Core/`)。运行时 `PreRender`/`WindowUI` 驱动角色由
> `UGUI/Core/CanvasManager` 改为 `Runtime/UI/UISystem`。下面关于控件类
> (Canvas/Button/Image/Text/...) 的章节仅作历史记录。

## 概述

本文档总结了ZEngine UI系统的实现情况，包括已完成的组件和待实现的功能。

## 已实现的组件

### 1. 核心类型定义 (`ui_types.h`)

- `UIColor`: UI颜色类型（基于Vector4，RGBA）
- `UIRect`: UI矩形结构
- `RectOffset`: 矩形偏移（用于padding等）
- `TextAnchor`: 文本对齐方式枚举
- `TextWrapMode`: 文本换行模式枚举
- `AnchorPreset`: Anchor预设枚举
- `CanvasRenderMode`: Canvas渲染模式枚举
- `InputEventType`: 输入事件类型枚举
- `InputEvent`: 输入事件结构
- `ButtonState`: 按钮状态枚举

### 2. Widget基类 (`widget.h/cpp`)

所有UI元素的基础类，提供：
- 可见性和启用状态管理
- RectTransform访问
- 事件处理接口（onMouseEnter, onMouseExit, onMouseDown等）
- 布局接口（getPreferredSize, onLayoutChanged）
- 脏标记系统
- 射线检测（hitTest）

### 3. RectTransform (`rect_transform.h/cpp`)

2D UI变换组件，提供：
- Anchor系统（anchorMin, anchorMax）
- Pivot系统
- 位置和大小（anchoredPosition, sizeDelta）
- Anchor预设（setAnchorPreset）
- 变换矩阵计算（getLocalToWorldMatrix, getLocalToParentMatrix）
- 父子关系管理
- 矩形计算（getRect）

### 4. Canvas (`canvas.h/cpp`)

UI画布，管理UI渲染和事件：
- 多种渲染模式（ScreenSpaceOverlay, ScreenSpaceCamera, WorldSpace）
- 相机支持（用于ScreenSpaceCamera和WorldSpace模式）
- 渲染管理（render）
- 事件处理（handleInput）
- 布局更新（updateLayout）
- 根Widget管理（addRootWidget, removeRootWidget）
- 射线检测（raycast）

### 5. UI事件系统 (`ui_event_system.h/cpp`)

全局UI事件管理器：
- 单例模式
- Canvas注册/注销
- 输入事件处理（processInput）
- 射线检测（raycast）
- 焦点管理（setFocusedWidget, getFocusedWidget）
- 悬停Widget管理

### 6. 基础UI控件

#### Image (`image.h/cpp`)
- Sprite/Texture支持
- 颜色设置
- 填充量（用于进度条等）
- 渲染接口

#### Text (`text.h/cpp`)
- 文本内容设置
- 字体和字体大小
- 颜色设置
- 对齐方式
- 换行模式
- 渲染接口

#### Button (`button.h/cpp`)
- 点击回调
- 状态颜色（Normal, Highlighted, Pressed, Disabled）
- 目标图形（Image组件）
- 事件处理（鼠标进入/退出/按下/释放/点击）
- 状态自动更新

### 6. UI runtime orchestration (2026-05, post-UGUI-retirement)

- `UISystem` (`Runtime/UI/UISystem.*`): `IEngineSystem` + `WindowUI`. Owns the
  shared `UIRenderer`, paints the retained-mode ZSlate root (UMG's
  `UMGViewport` overlay) and routes GLFW input into `SlateInputRouter`, all from
  `PreRender()` which `UIPass` calls once per frame. Replaces the retired
  `CanvasManager` as the `WindowUI`/`PreRender` driver.
- Shared render layer now under `Runtime/UI/` (moved out of `UGUI/`):
  - `UIRenderer` (`UI/Render/UIRenderer.h/cpp`): factory selects batched GPU backend by default.
  - `UiRenderBatch` + `BatchedUIRenderer`: CPU-side quads/outlines with clip stack; per-texture draw commands with UVs; consumed by `UIPass`.
  - `UiGpuResources`: GPU white texture, font atlas, Texture2D upload cache.
  - `Font` + `UITypes` live at `UI/Core/`.
- `UIPass`: RP2 UI subpass draw (`backup_even`), alpha-blended indexed triangles with texture sampling (Vulkan + DX12 shaders `ui_batched.*`).

Editor modularization phase 1: `EditorPropertyDrawer` extracted from Inspector (see `UI_MODULARIZATION_PLAN.md`).

Editor modularization phase 2: `EditorSerializedFieldDrawer` extracted from Inspector; `MakeTypeHeaderLabel` shared via `EditorPropertyDrawer`.

Editor modularization phase 3: `ProjectWindow` split into `ProjectTreeView`, `ProjectAssetActions`, `ProjectContextMenu`, `ProjectDragDrop`, and `ProjectWindowHelpers`; `ProjectWindow.cpp` is now a thin orchestrator (~200 lines).

## 待实现的功能

### 1. 布局系统

需要实现以下布局组件：
- `LayoutComponent`: 布局组件基类
- `HorizontalLayout`: 水平布局
- `VerticalLayout`: 垂直布局
- `GridLayout`: 网格布局
- `ContentSizeFitter`: 内容大小适配器

### 2. UI渲染系统

**Phase 1 (done, 2026-05):**

- `UiRenderBatch` / `BatchedUIRenderer` record solid quads + outlines with clip rects.
- `UIPass::Draw()` uploads batch geometry and draws in RP2 UI subpass (Vulkan + DX12).
- `CombineUIPass` composites scene + UI layer unchanged.

**Phase 2 (done, 2026-05):**

- `UiGpuResources`: 1x1 white texture + default font atlas (ImFontAtlas rasterized to GPU).
- `UiRenderBatch` records per-texture draw commands with UVs; `UIPass` binds descriptor sets per command.
- `BatchedUIRenderer::drawText` emits glyph quads sampled from font atlas; `drawTexturedQuad` wired end-to-end.
- `Image::OnRender` uploads `Texture2D` pixel blobs via `UiGpuResources::EnsureTexture2D`.
- Shaders `ui_batched.*` sample `sampler2D` and multiply by vertex color.

**Phase 3 (done, 2026-05):**

- `UiAffine2D` + `pushTransform` / `popTransform` on `UIRenderer` and `UiRenderBatch` (cumulative 2D affine applied when recording vertices).
- `RectTransform::GetWidgetSpaceToScreenAffine()` (pivot + scale + Z rotation); `Canvas::RenderSubtree` pushes it around each widget `OnRender`.
- Widgets draw in **local space** `(0..width, 0..height)`; batch transforms to screen pixels.
- `Image`: `Sprite::rect` normalized to texture UVs; 9-slice uses border fractions in sprite pixel space.

**Phase 4 (done, 2026-05):**

- `Font` asset (`REGISTER_CLASS`): serialised `m_SourceRelPath` + `m_DefaultSize`; `GetSourcePath()` resolves to absolute via `ProjectInfo`.
- `FontImporter`: `.ttf` / `.otf` / `.ttc` -> `Font` `.zasset` under `Assets/` (Import dialog + AutoReimport via `SourceAssetRegistry`).
- `FontAssetInspector`: source path, default size, Reimport.
- `UiGpuResources::ResolveFont` / `ResolveFontPath`: one `ImFont` per source path in a shared `ImFontAtlas`; per-widget sizes via `ImFont::GetFontBaked(font_size)`.
- Default atlas font: `ZENGINE_UI_FONT` env, else `ConfigManager::GetEditorFontPath()`, else Windows system fonts, else `AddFontDefault`.
- `UIRenderer::drawText` / `measureText` accept optional `Font*`; `Text` passes `PPtr<Font>`.
- Atlas rebuild re-uploads GPU texture when a new TTF is added.

**Still TODO (UGUI V5+):**

- `RectTransform::UpdateMatrix` full sync with layout + rotation for non-UI consumers

### 3. 资源系统

需要实现：
- `Sprite`: Sprite资源类（runtime `Texture2D` path done; asset import TBD）
- `Font`: runtime TTF path + GPU atlas + `.zasset` import (done)
- 资源加载和管理

### 4. 其他UI控件

- `InputField`: 文本输入框
- `ScrollView`: 滚动视图
- `Slider`: 滑块
- `Toggle`: 开关
- `Dropdown`: 下拉菜单
- `Scrollbar`: 滚动条

### 5. 与现有系统集成

- ~~在`UIPass`中集成UI系统渲染~~ (done: `UISystem::PreRender` + `UIPass::Draw`)
- GLFW input hooks now feed `UISystem` (`SlateInputRouter`); legacy `UIEventSystem` removed
- `RenderSystem::InitializeUIRenderBackend` wires `UISystem` into pipeline (done)
- Editor ImGui remains on swapchain overlay via `EditorUIPass` / post-UI callbacks (unchanged)

## 使用示例

### 创建Canvas

```cpp
// 创建Canvas GameObject
auto canvas_go = world->createGameObject("Canvas");
auto canvas = canvas_go->addComponent<Canvas>();
canvas->initialize(CanvasRenderMode::ScreenSpaceOverlay);

// 注册到事件系统
UIEventSystem::getInstance()->registerCanvas(canvas);
```

### 创建按钮

```cpp
// 创建按钮GameObject
auto button_go = world->createGameObject("Button", canvas_go);
auto rect_transform = button_go->addComponent<RectTransform>();
rect_transform->setAnchorPreset(AnchorPreset::MiddleCenter);
rect_transform->setSizeDelta(Vector2(200, 50));

// 添加Image组件作为背景
auto image = button_go->addComponent<Image>();
image->setColor(UIColor(0.2f, 0.6f, 1.0f, 1.0f));

// 添加文本
auto text_go = world->createGameObject("Text", button_go);
auto text = text_go->addComponent<Text>();
text->setText("Click Me");
text->setAlignment(TextAnchor::MiddleCenter);

// 添加Button组件
auto button = button_go->addComponent<Button>();
button->setOnClick([]() {
    LOG_INFO("Button clicked!");
});
```

## 架构特点

1. **组件化设计**: 所有UI元素都是Component，可以附加到GameObject上
2. **反射支持**: 使用ZEngine的反射系统，支持序列化
3. **事件驱动**: 基于事件系统处理用户输入
4. **脏标记优化**: 使用脏标记系统避免不必要的计算
5. **可扩展性**: 易于添加新的UI控件和布局组件

## 下一步工作

1. ~~Inspector: extract `EditorSerializedFieldDrawer`~~ (done)
2. ~~Split `ProjectWindow` into tree/actions/drag-drop modules~~ (done)
3. ~~UGUI V2: sprite/font GPU atlases + textured draw path in `UIPass`~~ (done)
4. ~~UGUI V3: transform stack + sprite sub-rect UVs + widget-local draw space~~ (done)
5. ~~UGUI V4: TTF Font asset + multi-size baking~~ (done)
6. 实现布局系统（HorizontalLayout, VerticalLayout, GridLayout）
7. 实现其他UI控件（InputField, ScrollView等）

## 注意事项

1. 当前实现中的一些TODO标记需要后续完成：
   - Sprite和Font的资源加载
   - 实际的渲染实现
   - 子Widget的遍历（需要GameObject的父子关系支持）

2. 需要确保与ZEngine的现有系统兼容：
   - 反射系统
   - 组件系统
   - 渲染管线
   - 输入系统

3. 性能优化：
   - 批处理渲染
   - 裁剪优化
   - 对象池
   - 异步资源加载

