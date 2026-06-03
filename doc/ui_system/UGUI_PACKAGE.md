# UGUI Package (`com.zengine.ugui`) - RETIRED (2026-05)

> The Unity-style UGUI control package described by earlier revisions of this
> file has been **removed**. ZEngine's runtime UI is now UE-style **UMG**
> (`Runtime/UMG/`) built on the retained-mode **ZSlate** framework
> (`Runtime/Slate/`). This file is kept as a migration pointer.

## What was removed

The UGUI **control model** under `engine/Source/Runtime/UGUI/` is gone:

- `Core/Canvas`, `Core/Widget`, `Core/RectTransform`, `Core/Selectable`,
  `Core/UIEventSystem`, `Core/CanvasManager`
- `Controls/*` (Image, Text, Button, Toggle, Slider, InputField, ScrollRect,
  Scrollbar, RectMask2d, Sprite)
- `Layout/*` (LayoutGroup, ContentSizeFitter, LayoutElement)
- `Demo/*` (UIDemoCanvas)
- The `ZUGUI` static library and its `add_subdirectory(UGUI)` wiring in
  `Runtime/CMakeLists.txt`
- Editor entry points: `Editor/EditorUgui/HierarchyUguiCreation.*`, the UGUI
  branch of `EditorHierarchyReparent`, and the Hierarchy "Create UI" menu hook.

## What was kept (and where it moved)

The shared GPU render layer + font/types are **not** UGUI-specific; ZSlate/UMG
depend on them. They moved out of `UGUI/` into the neutral `Runtime/UI/`:

```
engine/Source/Runtime/UI/
  Render/   # UIRenderer, BatchedUIRenderer, UiRenderBatch, UiGpuResources, UiAffine2D
  Core/     # Font (.zasset asset), UITypes, WindowUI (PreRender interface)
  UISystem.{h,cpp}   # IEngineSystem + WindowUI; the PreRender/WindowUI driver
```

These compile directly into `ZRuntime` via the normal `GLOB_RECURSE` (no
separate static lib anymore). `UIPass` still lives under
`Function/Render/Passes/` (RHI hook, RP2 UI subpass; shaders `ui_batched.*`).

`UISystem` replaces `CanvasManager` as the runtime `WindowUI`/`PreRender`
driver: it owns the `UIRenderer`, paints the ZSlate root that UMG's
`UMGViewport` publishes via `SlateApplication::SetRootContent`, and routes GLFW
input through `SlateInputRouter`.

## Includes

```cpp
#include "Runtime/UI/Render/UIRenderer.h"
#include "Runtime/UI/Core/Font.h"
#include "Runtime/UI/UISystem.h"
#include "Runtime/Slate/Widgets/STextBlock.h"   // ZSlate widgets
#include "Runtime/UMG/Widgets/UTextBlock.h"      // UMG wrappers
```

## Authoring UI now

- UMG widget trees are authored as `.zasset` blueprints in the **UMG Designer**
  editor window (`Editor/EditorWindow/UMGDesignerWindow/`), not as scene
  GameObjects. Save/load round-trips through `UMGAssetIO` /
  `UMGWidgetSerializer` / `UWidgetAsset`.
- At runtime a `UUserWidget` is built (C++ `Build()` or loaded from a
  `.zasset`) and shown via `AddToViewport()`.
- Fonts still import through `FontImporter` (.ttf/.otf/.ttc -> `Font.zasset`)
  and are consumed by the shared render layer.

No `.meta` sidecars (see AGENTS.md 2.1).
