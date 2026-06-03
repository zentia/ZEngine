# Editor UI Modularization Plan

> **Status update (2026-05): largely superseded by the ZSlate native backend.**
> The original premise below -- "keep ImGui as the ZEditor panel stack" -- was
> reversed once the editor windows were migrated to native ZSlate widgets
> (`ZSlateInspectorWindow`, `ZSlateProjectWindow`, ...). The ImGui drawer
> modules this plan extracted have since been **deleted as dead code**:
> - `EditorSerializedFieldDrawer` (`DrawSerializedObjectFields` + internals) --
>   the native inspector walks reflection itself; this had no caller left.
> - `EditorPropertyDrawer`'s ImGui rows (`DrawVecControl`, `DrawTextRow`,
>   `DrawFloatRow`, `DrawIntRow`, `DrawUInt8Row`, `DrawBoolRow`,
>   `DrawStringRow`) -- only `MakeDisplayLabel` / `MakeTypeHeaderLabel`
>   (pure string helpers, no ImGui) survive and are used by
>   `ZSlateInspectorWindow`.
> - `ProjectTreeView` + `ProjectContextMenu` -- the ImGui project-window
>   tree/menu rendering, replaced by `ZSlateProjectWindow`'s widget tree.
>
> The Project-window service modules that the ZSlate window still consumes
> (`ProjectWindowContext`, `ProjectWindowHelpers`, `ProjectAssetActions`,
> `ProjectDragDrop`) remain. See `doc/editor/EDITOR_UI_ARCHITECTURE.md` for the
> current ZSlate architecture. The phase notes below are kept for history.

## Goal

Keep **ImGui** as the ZEditor panel stack (Inspector, Project, docking). Split monolithic window TUs into reusable modules without introducing a second retained editor UI framework.

## Principles

| Track | Framework | Rule |
|-------|-----------|------|
| ZEditor panels | ImGui immediate mode | Extract drawers/services; do not fork Unity UGUI for editor chrome |
| Runtime game UI | Canvas / Widget tree | GPU path in RP2 UI subpass (`backup_even`), separate from editor overlay |

## Phase 1 (done)

### `EditorPropertyDrawer`

Location: `engine/Source/Editor/EditorUI/PropertyDrawer/`

Extracted from `InspectorWindow.cpp`:

- `MakeDisplayLabel`
- `DrawTextRow`, `DrawFloatRow`, `DrawIntRow`, `DrawUInt8Row`, `DrawBoolRow`, `DrawStringRow`

Inspector keeps a `using EditorPropertyDrawer::...` import in its anonymous namespace.

## Phase 2 (done)

### `EditorSerializedFieldDrawer`

Location: `engine/Source/Editor/EditorUI/SerializedFieldDrawer/`

Extracted from `InspectorWindow.cpp`:

- `DrawSerializedObjectFields` (TypeTree walk entry)
- Internal `DrawSerializedLeaf` / `DrawSerializedNode` / `DrawSerializedChildren`
- PPtr resolution + `CameraMode` combo row

Inspector-specific widgets (`DrawVecControl`, `DrawTransformValue`) stay in Inspector and are injected via `EditorSerializedFieldDrawer::Hooks`.

`MakeTypeHeaderLabel` moved to `EditorPropertyDrawer` (shared with component headers).

## Phase 3 (done)

### `ProjectWindow` split

Location: `engine/Source/Editor/EditorUI/ProjectWindow/`

`ProjectWindow.cpp` (~2400 lines) slimmed to a thin orchestrator (~200 lines). Extracted modules:

| Module | Responsibility |
|--------|----------------|
| `ProjectWindowContext` | shared mutable state struct passed to modules |
| `ProjectWindowHelpers` | path/node/rename/drop helpers |
| `ProjectTreeView` | tree build, selection, drag-drop targets |
| `ProjectAssetActions` | import, delete, rename, prefab/material/create |
| `ProjectContextMenu` | right-click menus |
| `ProjectDragDrop` | OS drop queue + deferred import |

`ProjectWindow` keeps ctor/dtor wiring, `OnGUI` orchestration, and `ExecutePendingImportDialog` delegation.

## Phase 4 (planned)

### Shared editor chrome (optional)

Only if duplication appears across windows:

- `EditorSplitLayout` - two/three pane split helpers
- `EditorSearchFilter` - filter bar used by Project + future pickers

## Runtime UGUI package (done, 2026-05)

Physical package layout: `engine/Source/Runtime/UGUI/` (`Core` / `Render` / `Controls` / `Layout` / `Demo`).

- CMake target: `ZUGUI` (linked `PUBLIC` into `ZRuntime`)
- Manifest: `UGUI/package.json` (`com.zengine.ugui`)
- Design notes: `doc/ui_system/UGUI_PACKAGE.md`

See `UI_SYSTEM_IMPLEMENTATION.md` for GPU batch status.

Render flow:

```
CanvasManager::PreRender()
  -> BatchedUIRenderer records UiRenderBatch
UIPass::Draw() in RP2 UI subpass
  -> upload verts/indices -> alpha blend into backup_even
CombineUIPass
  -> composite backup_odd + backup_even -> swapchain
Editor ImGui (EditorUIPass / post-UI callbacks)
  -> swapchain overlay AFTER RP2 (unchanged)
```

## Out of scope

- Replacing ImGui editor panels with runtime Canvas widgets
- `.meta` sidecar files for UI assets (project uses registry / embedded GUID rules per AGENTS.md)
- Full font atlas / textured sprite path (tracked as UGUI V2 after solid-color batch path lands)
