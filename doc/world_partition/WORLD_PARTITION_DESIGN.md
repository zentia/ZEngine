# World Partition (UE-style)

## Overview

ZEngine World Partition splits a world into **spatial cells** on the **X-Y plane** (Z is up, matching gravity on `-Z`). Each loaded cell owns one runtime `Level` instance (same as UE one `UWorld::StreamingLevel` / cell payload).

Runtime code lives under `engine/Source/Runtime/Function/Framework/World/`:

| Type | Role |
|------|------|
| `WorldPartitionTypes.h` | `WorldPartitionCellCoord`, `WorldPartitionCellEntry`, `WorldPartitionSettings` |
| `WorldPartition` | Cell math, desired-cell set, load/unload |
| `WorldManager` | Boots world, ticks all streamed levels, streaming source |
| `WorldRes` | Authored partition metadata (JSON / `.zasset`) |

## Authored data (`WorldRes`)

| Field | JSON key | Description |
|-------|----------|-------------|
| `m_EnableWorldPartition` | `enable_world_partition` | Turn on streaming |
| `m_CellSize` | `cell_size` | World units per cell edge |
| `m_LoadingRange` | `loading_range` | Chebyshev radius in cells (1 = 3x3) |
| `m_GridOrigin` | `grid_origin` | World-space origin of cell (0,0) |
| `m_CellLevelUrlPattern` | `cell_level_url_pattern` | Pattern with `{coord_x}` / `{coord_y}` |
| `m_PartitionCells` | `partition_cells` | Explicit `[{coord_x, coord_y, level_url}, ...]` |

If `partition_cells` is non-empty, **only** listed cells can stream. If empty, any cell index is allowed when a pattern is set.

Legacy worlds (`enable_world_partition: false`) still load `default_level_url` as a single level.

Example: `engine/asset/world/partition_demo.world.json`. Set `DefaultWorld=asset/world/partition_demo.world.json` in `ZEditor.ini` to smoke-test.

## Runtime flow

1. `WorldManager::LoadWorld` reads `WorldRes`, calls `WorldPartition::InitializeFromWorldRes`.
2. When partition is enabled, no monolithic default level load; first `Tick` calls `UpdateStreaming`.
3. Streaming source (in order): active `Character` position in any loaded cell, main camera world position, `grid_origin`.
4. For each cell in range: `LoadLevelAdditive` (new `Level` per URL). Cells leaving range: `UnloadLevelByUrl`.
5. `Tick` runs **every** loaded `Level` when partition is on.
6. `m_CurrentActiveLevel` points at the level with the character, else main camera, else any loaded cell (editor / hierarchy).

## Editor console

| Command | Description |
|---------|-------------|
| `wp.status` | Enabled flag, cell size, range, loaded cell count |
| `wp.cells` | Log loaded level URLs |

## UE parity / gaps (V1)

| UE feature | ZEngine V1 |
|------------|------------|
| Grid / cell coords | Yes |
| Streaming source | Character / camera |
| One Level per cell | Yes |
| Data layers | No |
| HLOD / actor packing | No |
| Async cell IO | No (`PreloadManager` hook planned) |
| Editor partition window | No |
| Cross-cell actor IDs | Process-local `GObjectID` only |

## V2 (landed)

| Feature | Implementation |
|---------|----------------|
| Async cell IO | `WorldRes::m_AsyncCellLoading` (default true). `PreloadManager::PreloadAsset` then `AcquireLevelUrl` when ready. Pending cells tracked in `WorldPartition::m_PendingCells`. |
| Level URL ref-count | `WorldManager::AcquireLevelUrl` / `ReleaseLevelUrl` so multiple cells can share one `level_url` without premature unload. |
| Render teardown | `Level::FlushRenderDeletes()` queues `RenderSwapContext::AddDeleteGameObject` for every GObject before `clear()`. |
| Editor grid viz | `DrawWorldPartitionEditorOverlay()` (green=loaded, yellow=pending, blue=desired-only) from `EditorSceneManager::Tick`. |
| Cell bounds helper | `ComputeWorldPartitionCellBounds()` in `WorldPartitionTypes.cpp`. |

Console: `wp.status` reports pending cell count; `wp.cells` lists active level URLs.

## Next steps (V3+)

- Data layers / HLOD actor packing.
- Editor toggle for partition overlay + loading radius gizmo.
- Per-cell `.zasset` `LevelRes` with `m_Objects` binary Transfer (JSON levels still use the custom writer in `Level::save()`).
- Cross-cell stable actor GUIDs.
