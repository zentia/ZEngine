# Large World Coordinates (LWC, UE-aligned)

## Problem

`float` world space loses centimeter-level precision beyond roughly **512 km** from the origin
(UE rule of thumb: ~1 cm error at 2^18 units). Open worlds + World Partition need stable transforms
far from `(0,0,0)`.

## UE model (reference)

| Layer | Type | Role |
|-------|------|------|
| Game / logic | `FVector` = `FVector3d` (`double`) | Absolute world positions, bounds, streaming |
| Render tile | `FVector3d` tile + `FVector3f` offset | Rebases GPU math near the camera |
| View | `FViewMatrices::ViewOrigin` + `PreViewTranslation` | `TranslatedWorld = World + PreViewTranslation` |
| GPU | `FRelativeViewMatrices`, shader `FLWCVector3` | Matrices and attributes stay `float` |

Key identity (from `FViewMatrices`):

```
World = TranslatedWorld - PreViewTranslation
TranslatedWorld = World + PreViewTranslation
```

Shaders: `LargeWorldCoordinates.ush` splits values into **Tile** (large, shared per frame) + **Offset** (small).

## ZEngine strategy (phased)

### L1 (landed) -- render origin + CVar

- `Vector3d` for absolute world samples.
- `LargeWorldCoordinates` namespace: **2^21** world-unit render tiles (same order as UE).
- `r.LWC.Enable` CVar + `ZENGINE_LWC_ENABLE` env override.
- Each frame: `WorldManager` sets the render tile from the streaming source (character / camera / grid origin).
- `RenderCamera` keeps `m_WorldPositionD`; `position()` returns **render-space** `Vector3` when LWC is on.
- `BaseRenderer` rebases object `LocalToWorld` matrices into render space before draw submission.

This fixes GPU jitter for distant cells **without** migrating every `Transform` field to `double` yet.

### L2 (landed) -- authoritative double transforms

- `TransformTRS::m_Position` and `Transform::m_LocalPosition` are `Vector3d` (`LocalTransform` removed).
- `TransformHierarchy` composes world translation in double (`CalculateGlobalPositionD`).
- Serialization: read legacy `m_LocalPosition` / `m_position` floats, write `m_LocalPositionD` / `m_positionD`.
- `Transform::GetWorldPositionD()` / `SetPosition(Vector3d)` for absolute world edits.
- `GetPosition()` still returns render-space `Vector3` when `r.LWC.Enable` is on.

Remaining L2 gaps: editor inspector fields as double, gizmo picking in absolute double (uses render-space today).

### L3 (landed) -- shader LWC frame fields

- `MainCameraPerFrame` tail: `pre_view_translation`, `render_tile` (CPU upload from `LargeWorldCoordinates` when `r.LWC.Enable` is on).
- HLSL: `shader/hlsl/common/LargeWorldCoordinates.hlsl` + `ZLwcFromPerFrame()` in `scene_lighting_structs.hlsli`.
- GLSL mirror: `shader/include/structures.h` (same tail layout).
- Model matrices stay render-space on CPU (`BaseRenderer`); shaders can reconstruct absolute via `ZLwcRenderToAbsolute` when needed.

Follow-up: port UE `FLWCVector3` only where world-space shader math needs split tile+offset (MegaLights, VT).

### L4 (planned) -- World Partition integration

- Per-cell **origin offset** in `WorldPartitionSettings` (cell world min corner as tile).
- Streaming loads levels with actors already in absolute double; render tile follows active cell cluster.

## API surface (L1)

```cpp
// Runtime/Core/Math/LargeWorldCoordinates.h
bool IsEnabled();
Vector3d GetRenderTile();
void SetRenderTileFromWorldPosition(Vector3d world_position);
Vector3 WorldToRender(Vector3d world);
Vector3d RenderToWorld(Vector3 render);
Matrix4x4 ApplyRenderOriginToModelMatrix(Matrix4x4 world_matrix);
```

```cpp
// RenderCamera
Vector3d worldPosition() const;
void SetWorldPosition(Vector3d world_position);
Vector3 GetPreViewTranslation() const;  // -offset within tile, as float
```

## Console / env

| Name | Description |
|------|-------------|
| `r.LWC.Enable` | `0` = legacy float-only camera path (default). `1` = tile rebasing. |
| `ZENGINE_LWC_ENABLE` | `1` / `true` seeds the CVar at startup (like `ZENGINE_RENDER_PATH`). |

## Verification

1. Enable partition demo world, set `r.LWC.Enable 1`.
2. Teleport / move streaming source to `(2e6, 2e6, 0)` via script or editor.
3. Without LWC: mesh shimmer, grid wobble. With LWC: stable silhouettes; `wp.status` still loads cells.

## Files

| Path | Role |
|------|------|
| `Runtime/Core/Math/Vector3d.h` | Double-precision vector |
| `Runtime/Core/Math/LargeWorldCoordinates.{h,cpp}` | Tile math + matrix rebase |
| `Runtime/Function/Render/LargeWorldCoordinatesSettings.{h,cpp}` | CVar registration |
| `Runtime/Function/Framework/World/WorldManager.cpp` | Per-frame tile update |
| `Runtime/Function/Render/RenderCamera.{h,cpp}` | Double world position |
| `Runtime/Function/Framework/Component/Mesh/BaseRenderer.cpp` | Render-space draw matrices |
| `Runtime/Core/Math/TransformTRS.h` | Serializable TRS (`LocalTransform` removed) |
| `shader/hlsl/common/LargeWorldCoordinates.hlsl` | Shader LWC helpers |
| `Runtime/Function/Render/RenderResource*.cpp` | Per-frame `pre_view_translation` / `render_tile` upload |

## Related

- `doc/world_partition/WORLD_PARTITION_DESIGN.md` -- cell streaming (orthogonal; LWC fixes precision inside loaded cells).
