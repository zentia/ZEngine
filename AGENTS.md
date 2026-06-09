# ZEngine — AI Agent Working Notes

This file is read at the start of every AI assistant session. It records
**long-lived project conventions** that are easy to forget across turns and
sessions. Keep it short and decision-oriented; design rationale lives in `doc/`.

If you (the agent) discover a new project-wide convention while working with
the user, append it here under the appropriate section so the next session
inherits the knowledge.

---

## 1. Reference codebases (read-only)

- `../unity2023.1/` — Unity engine source. Reference for high-level
  Editor/Runtime API surface and tooling UX.
- `../UnrealEngine/`  — Unreal Engine source. Reference for low-level
  systems (asset registry, reflection, hot-reload, packaging).
- ZEngine's own implementations should look at both, but **ZEngine is not a
  fork** — copy ideas, not files.

## 2. Hard project conventions

These override "common practice" elsewhere. Do not re-litigate them in a new
session; if the user wants to change one, update this file in the same turn.

### 2.1 No `.meta` sidecar files

ZEngine does **not** use Unity-style `.meta` sidecar files for any asset type.
Reasons:

- Doubles the file count in version control and the project window.
- Easy for users to delete by accident, breaking GUID stability.
- Forces every external tool (TS compiler, image editor, etc.) to learn the
  convention.

**Replacement strategy (UE-inspired)**:

| Asset kind                                  | Identity carrier                     |
|---------------------------------------------|--------------------------------------|
| Binary assets (`.zasset`)                   | GUID embedded inside the .zasset header. The header is a 176-byte `AssetFileHeader` prefix (magic = "ZASS") stamped at file offset 0 by `SerializedFile::WriteHeaderAndMetadata`; AssetRegistry reads it back via direct `ifstream::read` in `scanSingleAsset`. The GUID is a 32-char lowercase hex string derived deterministically from the absolute output path (lower-cased on Windows) using FNV-1a 64×2, so re-writes on the same path stay GUID-stable across editor sessions. Files written before this convention landed have no prefix and are still loadable -- AssetRegistry assigns them a `"legacy:" + path` synthetic GUID instead. See `engine/Source/Runtime/Core/Serialize/SerializedFile.cpp` (P2 #6 markers) and `engine/Source/Runtime/asset/asset_file.h`. |
| Text source assets (`.ts`, `.js`, `.glsl`)  | GUID stored in a single per-project registry file: `<Project>/AssetRegistry/script_registry.json` (**checked into VCS** — analogous to Unity's `ProjectSettings/`, NOT `Intermediate/`). The registry is the source of truth; missing entries are auto-created on first scan from a deterministic hash of the relative path (`Hash128(rel_path_lower)`), so a clone whose registry was lost rebuilds identical GUIDs. **JSON writes are debounced** (500 ms quiet window via `ScheduleSave` / `TickDeferredSave` in `EditorAssetManager::TickWatcher`; `FlushPendingSave` on `Initialize`/`Shutdown`) so rapid watcher bursts do not rewrite the whole file every event. |
| Project-config assets (`.zproject`, scenes) | path-based, no GUID needed.          |

When a user renames or moves a `.ts` file, the `ScriptRegistry` updates the
`path → guid` mapping in place; existing references (which store the GUID,
not the path) survive automatically. This is structurally equivalent to UE's
`ObjectRedirector`, just centralised in one JSON instead of one redirector
asset per move.

### 2.2 Directory layout for scripting

```
<Project>/
├── Assets/                       # binary .zasset files only
├── Scripts/                      # user-authored .ts (UE-style, peer of Assets/)
├── Shaders/                      # user-authored .shader / .hlsl / .compute
│                                 # / .raytrace (UE-style, peer of Assets/)
├── AssetRegistry/                # checked into VCS
│   └── script_registry.json      # path<->GUID for all .ts (UE Redirector equiv.)
├── Intermediate/                 # gitignored; engine-managed
│   ├── Scripts/                  # tsc output (.js + .js.map)
│   ├── Shaders/                  # compiled SPIR-V/DXIL cache (mtime-keyed)
│   └── Typings/                  # engine-emitted .d.ts for binding
├── tsconfig.json                 # engine-generated if missing
├── package.json                  # engine-generated if missing
└── *.zproject
```

Decided in P1+P2:
- `Scripts/` peer of `Assets/` (UE-style) - chosen because demo project
  `I:\ZEngineDemo\` already had `Scripts/` at project root.
- `AssetRegistry/` peer of `Assets/` - **checked into VCS**, not under
  `Intermediate/`. Rationale: registry IS the GUID source of truth; losing
  it on a fresh clone would silently break cross-scene references. We accept
  occasional 3-way merges on this JSON (entries are line-stable).
- Content Browser shows `Scripts/` as a **top-level root peer of Assets/**,
  not nested under it.

Decided for Shaders (mirrors Scripts 1:1):
- `Shaders/` peer of `Assets/` and `Scripts/`. Created on project open by
  `ProjectInfo::ensureScriptsScaffold()` (yes, same function -- the name
  is now historical; it scaffolds both Scripts and Shaders).
- `Intermediate/Shaders/` holds the on-disk shader cache.
  - For `.shader` files compiled through `ShaderLabCompiler` (currently
    only the standalone unit test `shader_lab_test.cpp` -- ShaderLab is
    NOT on any editor / runtime hot path yet), the cache filename layout
    is `<src-path-hash>_<sub>_<pass>_<variant-hash>_<stage>.spv`. Pointed
    at by `ShaderLabCompiler::SetCacheDirectory()` (per-instance API; no
    project-wide default).
  - For `.hlsl` files compiled through `DX12ShaderCompiler` (the actual
    editor/runtime hot path -- runtime pass `createShaderModuleFromFile`,
    preview renderer's `compileFromSource`, inspector's shader-validate
    button), the cache filename layout is
    `<src-path-hash>_<stage>_<variant-hash>.dxil`. Caching is wired
    engine-wide via `DX12ShaderCompiler::setDefaultCacheDirectory()`,
    which is set ONCE in `DX12RHI::Initialize` from
    `ProjectInfo::getIntermediateShadersRoot()`. All future
    default-constructed `DX12ShaderCompiler` instances inherit this
    default automatically -- preview renderer's
    `static DX12PreviewRenderer renderer;`, the inspector's local
    `DX12ShaderCompiler compiler;`, and the lazy
    `DX12RHI::m_shader_compiler` all benefit zero-config. Per-instance
    override is available via `setCacheDirectory()`.
  - For `.glsl` / `.vert` / `.frag` / etc. compiled through the Vulkan
    backend's `::ShaderCompiler` (note the bare class name -- no
    `Vulkan` prefix; it lives at
    `runtime/function/render/interface/vulkan/shader_compiler.{h,cpp}`
    and is the runtime hot path on Android / OHOS / Linux, plus the
    SPIR-V emit stage of `ShaderLab::ShaderLabCompiler`), the cache
    filename layout is `<src-path-hash>_<stage>_<variant-hash>.spv`.
    Caching is wired engine-wide via
    `ShaderCompiler::setDefaultCacheDirectory()`, set ONCE in
    `VulkanRHI::Initialize` from
    `ProjectInfo::getIntermediateShadersRoot()`. Symmetrical to the
    DX12 wiring; new default-constructed instances pick up the default
    automatically. Variant key for the Vulkan path is
    `stage + sorted(defines)` (entry point is hard-coded to "main" by
    the glslang code path, so it doesn't participate). Both glslang
    in-process and the external `glslangValidator` fallback path go
    through the same cache layer (it sits above the `#if
    GLSLANG_AVAILABLE` split).
  - All three caches use FNV-1a 64 of `lower(absolute_path(src))` for
    the source-key portion (so `Foo.hlsl` vs `foo.hlsl` collapse to
    the same slot on case-insensitive filesystems). Variant-key
    portion hashes (entry_point + stage + sorted defines) for DX12,
    (stage + sorted defines) for Vulkan, or
    `stringifyVariant(ShaderVariantKey)` for ShaderLab. mtime
    invalidation: `last_write_time(cache) < last_write_time(src)` ->
    miss.
  - **Include-header tracking (DX12 + Vulkan)**: on cache hit, both
    backends do a second-stage freshness check that recursively walks
    `#include "..."` and `#include <...>` from the top-level source,
    accumulating max(mtime). If any transitively-included file is
    newer than the cache blob, the cache is treated as stale and the
    next compile overwrites the slot in place. The cache key itself
    does NOT depend on include contents -- it stays stable across
    edits, only the freshness comparator widens. The scanner is a
    blind text scan (no `#if` / `#ifdef` gating respected), which
    over-counts conservatively: false-positive recompiles cost a few
    hundred ms; false-negative stale binaries would cost hours of
    debugging. Recursion is guarded by a visited set + 64-deep cap.
    Implementation: `scanIncludesRecursive` in the anonymous namespace
    of each compiler's `.cpp`, called from `compileInternal` only on
    cache hit (zero overhead on cache miss, where DXC / glslang
    compilation dwarfs the scan cost anyway). NOTE: `WebGL2`
    deliberately has no equivalent because the WebGL2 backend has no
    host-side shader compiler -- emcc forwards GL ES calls straight
    to the browser, which compiles GLSL device-side at link time.
  - `compileFromSource` paths whose `shader_name` is NOT a real on-disk
    file path skip the disk cache entirely (no stable mtime ground
    truth). Preview renderer DOES go through this path (it strips
    `#pragma` lines and re-feeds the shader as a synthetic source) BUT
    sets `shader_name = preview_source.path.string()`, which IS a real
    file -- so the cache still hits there. Verify this remains true if
    the preview pipeline is refactored.
- **`ShaderRegistry` (landed)**: `<Project>/AssetRegistry/shader_registry.json`
  maps each `.shader` under `Shaders/` to a deterministic GUID, ShaderLab
  name, and generated `.zasset` path (`Assets/_Generated/Shaders/...`).
  `ShaderRegistry` is an `IEngineSystem` (see
  `Runtime/Function/Render/ShaderRegistry.{h,cpp}`). `MaterialRes::
  SetShaderByName` / load-by-`m_ShaderGuid` resolve through the registry
  first, then fall back to `AssetManager::getAssetsByType("ShaderRes")`.
  Rename detection uses content-hash like `ScriptRegistry`. JSON persistence
  uses the same 500 ms debounced save as `ScriptRegistry` (see table above).
  Editor shader watcher (PR-AI2) calls `OnShaderFileEvent` for `.shader`
  changes.
  **Still by-name downstream**: `MaterialSourceDesc` / render paths keep
  `eastl::string` shader names via `GetShaderName()` (PR-SE3c decision).
  Note this is bigger than just adding the registry file: today
  `MaterialRes::m_shader` is still `eastl::string` (the shader's name)
  and the same `eastl::string` is propagated through `MaterialSourceDesc`
  / `RenderMaterialData` / `RenderObject` / `VulkanPBRMaterial::
  shader_name`. A real GUID-ification pass needs to migrate
  `MaterialRes` schema (read-old-write-new, since existing `.zasset`
  materials embed the name string), and either GUID-ify the entire
  downstream chain OR introduce a single guid->name resolution
  point at the `MaterialSourceDesc` boundary. Hard-coded built-in
  shader names like `"StandardLit"` (special-cased in `FindShaderByName`
  to return early) stay outside the registry. Until that work is
  scheduled, rename of a project `.shader` breaks every material
  referencing it -- this is the concrete user-visible symptom that
  motivates eventually doing it.

  **PR-SE3 staging plan** (the migration is split across multiple PRs to
  keep the surface area auditable). PR-SE3a itself further splits into
  three sub-PRs (shadow / refine / migrate) because typed PPtr
  resurrection is a binary-format change that must be testable in
  isolation before it touches any res_type:
  * **PR-SE3a-shadow (DONE)**: schema-evolution scaffolding only.
    `MaterialRes` grows a sibling field `m_shader_guid` (`eastl::string`,
    default `""`) next to the existing `m_shader`. Both are written;
    reader prefers `m_shader_guid` once it's non-empty, otherwise falls
    back to `m_shader`. Crucially this is the first PR that puts the
    SafeBinaryRead schema-evolution path on a real production res_type
    -- old `.zasset` files (whose embedded TypeTree has no
    `"shader_guid"` node) load fine because SafeBinaryRead returns
    `kNotFound` for the missing field and leaves `m_shader_guid` at its
    default `""`. End-to-end coverage lives in scenarios M1+M2 of
    `engine/Source/Runtime/Core/Serialize/test/schema_evolution_smoke_test.cpp`.
    No ShaderRegistry yet, no downstream consumer changes -- writer
    leaves `m_shader_guid` empty, so downstream still resolves by name
    and behaviour is byte-identical for users.
  * **PR-SE3a-refine (DONE)**:
    resurrects `PPtr<T>`.
    Today `PPtr<T>::Transfer()` at `engine/Source/Runtime/BaseClasses/
    PPtr.h:97-99` is an empty stub, so the 39 `PPtr<X>` fields scattered
    across res_type / Prefab / Component / animation / Render have never
    serialized -- every cross-asset reference goes through ad-hoc
    string fields (shader name, texture path, prefab GUID string)
    because the reference layer that *should* solve this is dead code.
    This sub-PR (a) fills `PPtr<T>::Transfer()` with Unity's
    `(m_FileID:int32, m_PathID:int64)` pair contract; (b) bumps the
    inner `SerializedFileHeader.version` from 0 -> 1 (the outer ZASS
    prefix is untouched -- `scanSingleAsset` keeps working unmodified);
    (c) appends a real `m_Externals` table (`FileIdentifier{guid,
    asset_type, has_path, path}` array) to the metadata buffer, after
    the existing object table -- mirroring Unity's
    `SerializedFile.cpp:1814-1825` exactly; (d) introduces
    `IPPtrResolver` (`Runtime/BaseClasses/IPPtrResolver.h`) and
    `SerializedFilePPtrResolver` to do the
    `(InstanceID <-> (fileIndex, LFID))` mapping symmetrically with
    Unity's `ILSOIResolver`; the active resolver is **per-thread** via a
    `thread_local` stack + `ScopedPPtrResolver` RAII (NOT a single shared
    pointer like Unity does), so multi-thread AssetRegistry scan and
    AssetManager bulk loads don't contend on a mutex. **Hard constraint**:
    this PR may not modify any file under `Resource/res_type/**` or
    `Function/**`. Provable end-to-end through a new `PptrSmokeTest.cpp`
    (P1-P7: local round-trip, external round-trip, externals dedup, null
    PPtr, v1-file backward compat, dangling-target forward compat,
    concurrent-read TLS contract). Full design including binary layout,
    runtime contract, migration timeline, and risk register lives at
    **`doc/asset_management/PPTR_DESIGN.md`** (read that BEFORE writing
    any code for this sub-PR).
  * **PR-SE3a-migrate (DONE, merged with PR-SE3b)**:
    `MaterialRes` now has `PPtr<ShaderRes> m_shader_pptr` alongside the
    legacy `m_shader` string (shadow phase -- both written, reader prefers
    PPtr when valid, falls back to `m_shader`). `GetShaderName()` /
    `SetShaderByName()` accessors encapsulate the resolution priority.
    `Material.cpp::Transfer()` writes `m_shader_pptr` after `m_shader_guid`.
    Downstream consumers (`base_renderer`, `inspector_window`,
    `shader_preview_renderer`, `project_window`) switched to
    `GetShaderName()` / `SetShaderByName()`. M3a + M3b scenarios in
    `schema_evolution_smoke_test.cpp` cover old-layout→new read and
    PPtr round-trip. All 7 smoke-test scenarios pass.
  * **PR-SE3b (DONE, merged with PR-SE3a-migrate)**:
    chose strategy A2+B2 (no separate ShaderRegistry JSON; first-time
    seeding via `ShaderImporter` + reuse `AssetManager::getAssetsByType
    ("ShaderRes")` for name resolution). `ShaderImporter` scans
    `<Project>/Shaders/*.shader`, generates `ShaderRes .zasset` files
    under `<Project>/Assets/_Generated/Shaders/` only if the `.zasset`
    doesn't already exist (A2: first-time seeding, no continuous watch).
    `ProjectInfo::ensureScriptsScaffold()` creates `_Generated/Shaders/`.
    `EditorAssetManager::Initialize()` calls `ImportProjectShaders()`.
    `MaterialRes::SetShaderByName()` iterates
    `AssetManager::getAssetsByType("ShaderRes")` to find a match and
    sets the PPtr; built-in shaders (`"StandardLit"` etc.) leave PPtr
    null and use the `m_shader` string directly.
  * **PR-SE3c (DONE, decision: no downstream GUID-ification)**:
    Decision: **do NOT GUID-ify downstream** (`MaterialSourceDesc` /
    `RenderMaterialData` / `VulkanPBRMaterial` etc.). Shader reference
    resolution stays at the `MaterialSourceDesc` boundary via
    `MaterialRes::GetShaderName()` (which prefers `m_ShaderPptr` when
    valid, falls back to `m_Shader` string). Downstream structures
    continue to store shader as `eastl::string` and call
    `GetShaderName()` at the boundary. Rationale: no use case for
    cross-project material bindings today; widening the GUID surface
    would add complexity with no user-visible benefit. If a future
    use case emerges (e.g. sharing `.zasset` materials across projects
    with stable shader GUIDs), revisit this decision.
- **Compatibility strategy B (current)**: `.shader` files under
  `<Project>/Assets/` are still resolved (legacy demo projects rely on
  this), but every match logs a `LOG_WARNING(ZShader, ...)` deprecation
  notice. New shaders created via the Content Browser default to
  `<Project>/Shaders/`. A future hard-cutover can remove the legacy walk
  once all in-house demos have migrated.
- Content Browser shows `Shaders/` as a **top-level root peer of Assets/
  and Scripts/**, registered in `EditorFileService::buildEngineFileTree`
  right after the Scripts root.

Decided for Data (mirrors Scripts/Shaders, with one twist):
- `Data/` peer of `Assets/`, `Scripts/`, `Shaders/`. Source location for
  CSV (V1) / XLSX (V2) tables; **checked into VCS**. Created on project
  open by `ProjectInfo::ensureScriptsScaffold()` (the historical name now
  scaffolds Scripts + Shaders + Data). Content Browser shows `Data/` as a
  top-level root, registered in `EditorFileService::buildEngineFileTree`
  right after the Shaders root.
- **Compile output goes UNDER `Assets/`, not `Intermediate/`**: the path
  is `<Project>/Assets/_Generated/Data/<rel>.zasset`, with the source ->
  product path mapping being a 1:1 relative-path mirror, only the file
  extension changes. Rationale: the editor's `AssetRegistry` already
  scans the entire content directory, so emitting products under
  `Assets/_Generated/` makes them discoverable with **zero changes** to
  the registry / Content Browser / Inspector. The whole `_Generated/`
  subtree is gitignored via the scaffolding marker block in `.gitignore`
  (an entry `/Assets/_Generated/` was added next to `/Intermediate/`).
  This is the one place where Data deviates from the Scripts/Shaders
  template, which use `Intermediate/`. The reason Scripts/Shaders can
  use `Intermediate/` is that they are NOT consumed via AssetRegistry --
  scripts are loaded by ScriptingManager directly from
  `Intermediate/Scripts/` and shaders by the RHI's per-stage cache.
- Row class layering: `Object` -> `DataTableBase` (abstract,
  reflection-registered) -> `DataTable<TRow>` (template, type-erased
  helpers) -> user `WeaponDataTable : DataTable<WeaponRow>` final
  class. The user's wrapper class is the one that goes through
  `REGISTER_CLASS` -- `IMPLEMENT_REGISTER_CLASS` cannot swallow `<` `>`
  in a typename, so the template itself is never registered. We provide
  `DECLARE_DATA_TABLE(WeaponDataTable, WeaponRow)` /
  `IMPLEMENT_DATA_TABLE(WeaponDataTable, WeaponRow)` macros so the user
  side stays one line of declaration + one line of implementation.
- Row contract: a row is a plain `struct` (NOT an `Object` subclass)
  with `DECLARE_SERIALIZE(MyRow)` + `template<TF> Transfer()`. The first
  field touched by `Transfer()` must be `eastl::string id;` -- it is the
  primary key, hashed into `DataTable::m_key_index` on `onPostLoad()`,
  and CSV's first column maps to it. String fields use `eastl::string`
  per AGENTS.md 2.3.
- Storage shape: `std::vector<TRow> m_rows;` (NOT `eastl::vector` --
  `SerializeTraits` is only specialised for `std::vector`, see
  `Runtime/Core/Serialize/SerializeTraits.h`).
- No `DataRegistry` yet -- generated `.zasset` files inherit GUIDs from
  `SerializedFile` (same as every other binary asset), and the
  AssetRegistry indexes them automatically because they live under
  `Assets/`. If a future need for cross-project data-table references
  via stable GUID arises, it would be addressed at the AssetRegistry
  layer, not via a per-table-type sidecar registry.
- **CSV importer (V1) + XLSX importer (V2)** -- both landed. CSV lives
  at `engine/Source/Editor/asset_pipeline/data_table_importer/
  data_table_importer.{h,cpp}` (extension `.csv`); XLSX lives at the
  sibling `xlsx_importer.{h,cpp}` (extension `.xlsx`). Both implement
  `AssetImporter`. On editor startup, `EditorAssetManager::Initialize`
  calls `DataTableImporter::compileProject()` AND
  `XlsxImporter::compileProject()` which each walk `<Project>/Data/`
  for their own extension and emit products under
  `<Project>/Assets/_Generated/Data/<rel>.zasset`. Mirrors
  `TypeScriptCompiler`'s startup `tsc` pass on `.ts` sources.
  XLSX parsing is a hand-written SAX-style scanner targeting the
  Office Open XML subset that real spreadsheets emit (sharedStrings.xml
  + sheet1.xml + 5 core entities + numeric `&#NNN;` + `<![CDATA[]]>`);
  ZIP unpacking goes through `minizip-ng` (vendored at
  `engine/3rdparty/minizip-ng/`, linked PRIVATE into ZEditor). Multi-
  sheet workbooks: V1 reads only sheet1 by design (Q&A choice A);
  not a TODO. Stem collisions (`Weapon.csv` + `Weapon.xlsx`): XLSX
  wins, with a `LOG_WARNING(ZDataTable, ...)` so the user notices the
  shadowed CSV. GUID stability: `XlsxImporter::deriveStableGuid`
  hashes `lower(generic_string("Data/<rel>.xlsx"))` with FNV-1a 64,
  symmetric with `DataTableImporter::deriveStableGuid` and unrelated
  to the CSV slot (so a project that contains BOTH a `Weapon.csv`
  and a `Weapon.xlsx` produces two different `.zasset` products with
  two different GUIDs even though only one survives the stem collision
  -- this is intentional so post-hoc `git revert` of the XLSX
  unshadow's the CSV cleanly).
- **Schema registration**: each user `WrapperClass` registers itself with
  the importer via `REGISTER_DATA_TABLE(WrapperClass, RowType, alias,
  applier_lambda)` in an editor-side translation unit. The applier is a
  `void(TRow& row, const eastl::vector<eastl::string>& cells, const
  eastl::vector<eastl::string>& headers)` callback that converts CSV
  cells to row fields by header name. The user must fill `row.id`
  inside the lambda (we keep this convention so the applier stays
  symmetric across all columns). Default CSV->wrapper resolution policy:
  filename stem matches the registered alias (e.g.
  `<Project>/Data/Combat/Weapon.csv` -> alias `"Weapon"`). CSVs with no
  registered schema are skipped with a single batched warning per
  `compileProject` pass.
- **Editor / Runtime split**: the wrapper class
  (`DECLARE_DATA_TABLE`+`IMPLEMENT_DATA_TABLE`) lives in `ZRuntime` so
  prefabs/scenes that PPtr the table can deserialise in player builds.
  The `REGISTER_DATA_TABLE` call (which pulls in the editor-only
  `CsvSchemaRegistry`) lives in a sibling editor TU under
  `engine/Source/Editor/asset_pipeline/data_table_importer/Demo/
  <WrapperClass>Editor.cpp`. The reference demo
  (`WeaponDataTable`+`WeaponRow`) follows this layout exactly; copy it
  when adding a new table type.
- **CSV format (V1)**: RFC 4180 minimal -- comma separator, double-quote
  escape (`""` inside quoted fields), UTF-8 with optional BOM (BOM is
  stripped). First row is the header; first column header MUST be `id`
  (case-sensitive primary key contract). Rows shorter than the header
  are padded with empty cells; longer rows are truncated. Rows whose
  `id` cell is empty are skipped with a warning. Comments / alternate
  delimiters / multi-char quotes are V2.
- **XLSX format (V2)**: drop-in replacement for CSV at the source
  layer. First row is the header; first column header MUST be `id`.
  Cells are read as their displayed string (numeric `<v>` cells go
  through `to_string`-style conversion at parse time, NOT through
  Excel's locale-dependent format strings -- so "0.1" stays "0.1"
  on every machine). Only sheet1 is read (multi-sheet workbooks are
  trimmed silently; this is design choice A from V2 Q&A, not a TODO).
  XLSX support is **read-only from the editor's perspective** -- the
  Inspector edit-cells path is gated behind a `.csv` source check and
  XLSX rows render through the read-only V2 introspection path
  (below). To edit an XLSX-sourced table the user opens it in Excel /
  LibreOffice, saves, and the file watcher fires `compileOne` on the
  next focus pass. Not a TODO: writing `.xlsx` from the editor would
  require a full Office Open XML emitter, which is strictly larger
  than the parser, and the round-trip-through-Excel UX is what real
  data designers prefer anyway.
- **GUID stability for generated tables**: `DataTableImporter::
  deriveStableGuid` hashes `lower(generic_string(rel_csv_path))` with
  FNV-1a 64 -> formats as a canonical 8-4-4-4-12 GUID. Deleting
  `Assets/_Generated/Data/` and re-running `compileProject` produces
  bitwise-identical GUIDs, so any cross-asset references survive a
  clean. Same trick `ScriptRegistry` uses for source-file GUIDs.
- **Logging**: all importer messages go through the `ZDataTable` log
  category (added to `bq_log_category_config.ini` after `ZScripting`).
  Don't introduce a separate per-table category; the importer already
  prefixes the source path in every line.
- **Incremental rebuild (V1+V2)**: `EditorAssetManager` owns a SECOND
  `FileSystemWatcher` (`m_data_watcher`) rooted at `<Project>/Data/`
  with extension filter `{".csv", ".xlsx"}`. Events route by
  extension: `.csv` -> `DataTableImporter::compileOne / deleteGeneratedFor`;
  `.xlsx` -> `XlsxImporter::compileOne / deleteGeneratedFor`. The
  watcher is drained from `EditorAssetManager::TickWatcher` AFTER the
  primary `m_file_watcher` so the same frame's `_Generated/Data/<x>.zasset`
  write gets picked up by the registry one tick later -- this avoids
  fighting any in-flight `ReadObject` the inspector might be holding.
  Don't widen the primary watcher's root to `<Project>/` instead of
  Content/; that would inflate event volume by ~100x for projects with
  large `Scripts/` and force every existing onFileChanged path to
  discriminate by extension. Two narrow watchers stay simpler and have
  near-zero per-frame cost when idle.
- **Inspector view**: `inspector_window.cpp` ships
  `DrawDataTableInspector(asset_path, type*)` which renders any
  `DataTableBase`-derived `.zasset` regardless of which user wrapper
  class. Detection: `ResolveDataTableType` reads the asset's class name
  from the .zasset header, resolves to `Type*` via
  `TypeManager::ClassNameToType`, then walks `Type::base` up to
  `TypeOf<DataTableBase>()`. The branch lives in `InspectorWindow::onGUI`
  AFTER shader/material (those are more specialised) but BEFORE
  `IsGenericInspectorZAssetType` (otherwise DataTables would fall
  through to the unsupported-asset stub). The view shows class / source
  CSV / row count, action buttons (`Reimport` + Win32-only `Open Source
  CSV`), and a filterable per-row table. The "Reimport" button calls
  `DataTableImporter::compileOne` (or `XlsxImporter::compileOne` when
  the source is `.xlsx`) on the recorded source path, useful when the
  watcher missed a kernel notification (sandboxed editors that bypass
  FS events) or when a designer wants a clean rebuild after fixing an
  applier bug. The per-row table runs in EDIT mode by default (PR #7
  write-back) when the source is a reachable `.csv`, falling back to a
  read-only memory view rendered through the V2 introspection path
  (below) when the source is `.xlsx` (PR #6 deliberate fallback -- we
  do not write `.xlsx` from the editor, see V2 caveat below) or when
  the CSV is missing/unreadable.
- **Row introspection (V2 columns)**: `DataTableBase` exposes three
  virtual hooks implemented generically by `DataTable<TRow>`:
  `fillRowTypeTree(out)`, `rowDataAt(i)`, `rowSize()`. The Inspector
  materialises the row's `TypeTree` once per frame from a stack-local
  `TRow{}` probe (so empty tables still produce a header row), walks
  `Root().Children()` in declaration order to build a column list,
  and reads each cell as `*reinterpret_cast<const T*>(rowBase +
  byteOffset)`. Supported cell types (must match
  `SerializeTraits<...>::GetTypeString` exactly): `string`, `int`,
  `uint32_t`, `int64_t`, `uint64_t`, `float`, `bool`, `char`. Anything
  else (vectors, nested structs, PPtrs) renders as `<typename>` so the
  table layout stays valid and the gap is obvious. Bounds check:
  `byte_offset + byte_size > rowSize()` -> render `<oob>` instead of
  reading garbage. Per-frame cost on a 5-column row is ~10us
  (negligible vs ImGui's draw budget); if profiling ever shows it on
  top, the natural cache key is `const Type*` (row schema is invariant
  across instances of the same wrapper). The introspection path adds
  zero per-game cost -- new wrapper classes (`EnemyDataTable`,
  `SkillDataTable`, ...) inherit it for free via `DataTable<TRow>`.
  Strings are a special case: they ARE marked `kFlagIsArray` in the
  TypeTree (because `SerializeTraits<eastl::string>::Transfer` calls
  `TransferStringData` which routes through `TransferArray`), but the
  column collector overrides `is_array=false` for type-string `string`
  and reads them as `eastl::string*` directly -- this is safe because
  every `Transfer()`'d string field IS an `eastl::string` per
  AGENTS.md 2.3.
- **DataTable write-back (PR #7)**: when the source CSV is reachable,
  the Inspector enters EDIT mode and renders cells as
  `ImGui::InputText` / `Checkbox` widgets backed by an in-memory
  `(headers, rows[][])` string matrix. The matrix is the canonical
  authoring state during an editing session; the loaded
  `DataTableBase` is consulted only for the row-struct's per-column
  type info (so the Inspector knows which columns are `bool`, which
  are unsupported-and-therefore-read-only, etc.). Edits flip a `dirty`
  flag; `Save to CSV` calls `DataTableImporter::writeCsv()` which
  writes UTF-8-with-BOM + LF + RFC4180 quoting to a sibling `.tmp`
  and atomically renames onto the target. The PR #5 file watcher
  then sees the CSV change and fires `compileOne()`, refreshing the
  `.zasset`; the Inspector picks up the new mtime on its next frame
  and silently re-parses the matrix (only when `dirty == false` --
  pending edits are never clobbered by the watcher). `Discard`
  re-parses from disk, dropping pending edits. Rationale for
  round-tripping through the CSV (instead of mutating the loaded
  Object and re-serialising the .zasset): the CSV is the single
  source of truth (AGENTS.md "Data pipeline"); writing the .zasset
  directly would let the two drift, breaks `git diff` on data, and
  loses any CSV columns the row struct's applier doesn't read.
  Editable types match `IsSupportedCellType`'s whitelist (string /
  int32 / uint32 / int64 / uint64 / float / bool / char). Unsupported
  columns render as disabled `<typename>` cells but their CSV string
  values pass through `writeCsv` verbatim -- so e.g. a future
  `damage_curve: AnimationCurve` column added to `WeaponRow` won't
  destroy designer-authored data on first save through the
  Inspector. `parseCsv` is now public on `DataTableImporter` (it was
  private before PR #7) precisely so the Inspector edit session can
  load the matrix on selection change. V1 scope deliberately
  excludes: row add/remove (use external CSV editor for now), column
  reorder/rename (CSV header authoritative), undo/redo (`Discard`
  is the escape hatch).







### 2.2b Text serialization for scenes / prefabs (YAML)

Authoring data uses **text YAML**; DDC-imported data stays **binary `.zasset`**:

| Data | Format | File |
|------|--------|------|
| Scenes | YAML object graph | `.scene` |
| Prefabs | YAML object graph | `.prefab` |
| Materials | YAML object graph | `.mat` |
| Textures / meshes / animations / etc. | binary `SerializedFile` | `.zasset` |

Full design: **`doc/serialization/TEXT_SERIALIZED_FILE.md`**. Read it before
touching any of `Core/YamlSerialize/{YamlText,YAMLWrite,YAMLRead,YamlObjectGraph}`,
`Level::{save,load}`, or the `ImmediatePtr`/`PPtr` resolver path.

Key invariants:
- `YamlObjectGraph` is the text analogue of `SerializedFile`: many objects in
  one document (`objects:` array, each with `fileID` + class-name `type` tag +
  `data`), external `.zasset` refs in an `externals:` table by GUID.
- Cross-refs reuse the SAME per-thread `IPPtrResolver` as binary; local target
  = `(m_FileID 0, m_PathID = fileID)`, external = `(m_FileID = idx+1, ...)`.
  `ImmediatePtr<T>::Transfer` (was a stub) now resolves through it, like
  `PPtr<T>`.
- `AssetManager::{WriteObjectsToYaml,ReadObjectsFromYaml}` are the asset-system
  entry points; reuse the same external-ref hooks as the binary writer.
- **`TypeManager::ClassNameToType` is now content-keyed** (added
  `m_ClassNameStringToType`). The old `m_KlassNameToType` keyed on the raw
  `const char*` address, so by-name deserialization (and the asset-type-label /
  DataTable-inspector callers) only worked by accident before.
- Scene/graph code discriminates concrete types with `GetType() == TypeOf<T>()`,
  NOT `Object::Is()` -- `Is()`/`IsBaseOf` need a fully-built DFS type tree and
  return false for leaf types when `descendantCount == 0` in minimal processes.

### 2.3 String types in serialised fields

`SerializeTraits<T>` is specialised only for `eastl::string`, not
`std::string`. **Any field that participates in `Transfer()` must be
`eastl::string`.** Non-serialised path strings can stay `std::string`.

### 2.4 Source-file character set

Sources are compiled under MSVC code page 936 (GBK) on Windows. Avoid
non-ASCII punctuation in code/comments (em-dash `—`, fancy quotes, etc.) —
they trigger C4819 warnings. Use ASCII `-` and `"`. Chinese text in comments
is fine; only fancy ASCII-adjacent characters are problematic.

### 2.5 Log categories

Adding a new `LOG_INFO(ZFoo, ...)` call requires **two** steps:

1. Add `ZFoo` to `engine/3rdparty/bq_log_category_config.ini`.
2. Run (paths must be **absolute** - the cmake script doesn't resolve
   workspace-relative paths):
   ```
   cmake -DCONFIG_FILE=e:/Engine/ZEngine/engine/3rdparty/bq_log_category_config.ini ^
         -DOUTPUT_DIR=e:/Engine/ZEngine/engine/Source/Runtime/Core/log/generated ^
         -DCLASS_NAME=engine_log ^
         -P engine/Source/Precompile/generate_engine_log.cmake
   ```
   to regenerate `engine_log.h`.

Forgetting step 2 produces unresolved-symbol errors that look unrelated.

### 2.6 TypeScript toolchain (P3+)

- ZEngine **does not** bundle node/tsc. It either uses the project-local
  install at `<project>/node_modules/.bin/tsc(.cmd)` (preferred) or a
  globally-installed `tsc`. If neither is available the engine still boots
  but `TypeScriptCompiler` enters degraded mode (no compilation; logs a
  warning).
- For the `I:\ZEngineDemo` smoke project, run `npm install` once after
  fresh clone so tsc is available at the project-local path. The Node binary
  on this dev machine is `E:\emsdk\node\22.16.0_64bit\bin\node.exe`; ensure
  it's on `PATH` before launching ZEditor (Start-Process inherits the parent
  shell's PATH).
- `FileSystemWatcher` ships with a default extension filter of `{".zasset"}`
  for backward compatibility. Any new caller (script reload, shader hot-
  reload, config watching, ...) must call `setExtensionFilter({...})`
  BEFORE `watchDirectory()` or events will be silently dropped.

### 2.7 puerts backend selection (`PAPI_TYPE`)

ZEngine is a **JavaScript-only** scripting host (TypeScript -> tsc -> .js
-> JS VM) with a deliberate **two-VM strategy** that mirrors what
Cocos Creator / UE PuerTS / Unity JS hosts converged on:

- `quickjs` -> Web (emscripten/wasm) and mobile (iOS App Store bans JIT).
  ~400KB pure-interpreter VM. Same VM on every JIT-restricted platform
  so script semantics don't silently diverge.
- `v8`      -> Desktop editor + PC/console runtime. JIT brings script
  logic close to C++ perf, plus V8 Inspector gives Chrome DevTools /
  VSCode debugging out of the box.

Lua, Python and Node.js backends were all removed:
- **Lua / Python**: ZEngine commits to TypeScript-first, single source of
  truth for scripting.
- **Node.js**: game scripts don't need libuv / `fs` / `net` / `cluster`
  (the engine owns assets, networking and scheduling), and using Node-
  only APIs would silently break iOS / Web builds. Pure V8 gives the
  same JS engine without the Node embedding cost (libuv, npm modules,
  multi-MB binary). No mainstream game engine embeds Node for scripting.

`PAPI_TYPE` is selected at configure time via `-DPAPI_TYPE=...`:

| Value | `#define` | Status | Used for |
|-------|-----------|--------|----------|
| `quickjs` (**default** in `gen_windows.bat`, `gen_macos.sh`, `setup_ninja.bat`, `build_web.bat`, iOS scripts) | `PAPI_QUICKJS=1` | Fully wired in `ScriptingManager::Initialize` via `BackendQuickJS`. | Production path on Web / iOS / Android. |
| `v8` | `PAPI_V8=1` | **Wired and fully linking on Win64 (verified 2026-05-16, `bin\RelWithDebInfo\ZEditor.exe` 44.9 MB).** `BackendV8` mirrors `BackendQuickJS` symmetrically (both go through pesapi so `ScriptEnv` / `TypeScriptComponent` / etc. are backend-agnostic). `PapiV8.lib`, `ZRuntime.lib`, `Jolt.lib`, `glfw.lib`, `libcurl.lib` and the `ShaderLab` side-target all build cleanly under `/MT[d]` (Plan C). The earlier `editor_ui_pass.cpp` ifdef compile-phase blocker has been fixed -- the `#else` branch is now `#elif defined(Z_HAS_VULKAN)` with a no-op `#else` fallback, since on Windows the DX12 branch always early-returns and Vulkan, while compiled in as a switchable backend, is not on the editor ImGui hot path. **The `__std_*` STL toolset gap is resolved**: VS Installer was upgraded to 17.14.37301.10 which ships **MSVC 14.44.35207** alongside the legacy 14.38/14.39 SxS components; selecting 14.44 at build time (`/p:VCToolsVersion=14.44.35207` on the msbuild command line, or `$env:VCToolsVersion = "14.44.35207"`) supplies the missing `__std_min_*` / `__std_max_*` / `__std_remove_*` / `__std_find_*` / `__std_replace_*` / `__std_mismatch_*` / `__std_find_end_*` / `__std_find_first_of_trivial_pos_*` vectorized helpers that `wee8.lib` was emitted against. The CMake configure step is left at "default toolset"; only the build command needs the override. **V8 Inspector wired and runtime-verified (2026-05-16)**: `BackendV8::OpenRemoteDebugger / OnTick / DebuggerTick / CloseRemoteDebugger` forward into puerts' `CreateInspector / InspectorTick / DestroyInspector` exports (PapiExport.cpp). The v8::Isolate* is recovered on demand via the `GetV8Isolate(pesapi_env_ref)` export; BackendV8 captures the live ScriptEnv in `OnEnter`. ScriptEnv was reordered so `OnEnter` runs before `OpenRemoteDebugger` and the destructor calls `CloseRemoteDebugger` symmetrically. **Critically, `BackendV8::OnTick` was changed from a no-op stub to forwarding into `DebuggerTick` (= `InspectorTick(isolate)` = `Server.poll()` on the websocketpp server)** -- without this, /json HTTP discovery hangs because the kernel-accepted TCP connections never get drained by user-space. Editor main loop already calls `ScriptingManager::Tick -> ScriptEnv::Tick -> Backend::OnTick` once per frame, so this is enough. Smoke-tested end-to-end on 2026-05-16: launch with `$env:ZENGINE_V8_DEBUG_PORT="9230"`; `GET http://127.0.0.1:9230/json/version` returns `{"Browser":"Puerts/v1.0.0","Protocol-Version":"1.1"}` and `GET /json` returns the inspector target with `webSocketDebuggerUrl=ws://127.0.0.1:9230`. Chrome DevTools / VSCode can attach via that URL. Inspector port selection lives in `ScriptingManager::Initialize` and reads `getenv("ZENGINE_V8_DEBUG_PORT")` (positive int 1..65535); default `-1` keeps the inspector closed, matching the QuickJS path. | Desktop editor + PC/console runtime. |

The `papi-lua/`, `papi-python/` and `papi-nodejs/` subtrees stay vendored
under `engine/3rdparty/puerts/unity/native/` (we don't fork upstream)
but are never `add_subdirectory`'d, and `Runtime/Scripting/backends/{lua,python}/`
have been deleted. Trying `-DPAPI_TYPE=lua|python|nodejs` now FATAL_ERRORs
at configure time.

When debugging scripting issues, **always check
`build/CMakeCache.txt:PAPI_TYPE` first** to confirm the active backend.

Reconfigure with: `cmake -S . -B build -DPAPI_TYPE=quickjs` (or `v8`).

Or via the gen scripts (both already accept `--papi`):
- `gen_windows.bat --papi v8`
- `gen_macos.sh --papi v8`
- `setup_ninja.bat --papi v8`

The default stays at `quickjs` so a fresh clone without the V8 backend
binaries still configures cleanly. To switch the project-wide default to
v8 once the V8 binaries are vendored / auto-downloaded, change the four
gen scripts plus the `set(PAPI_TYPE "quickjs" CACHE STRING ...)` line in
`engine/3rdparty/CMakeLists.txt`.

**Build-time prerequisite for `v8`**: puerts' `papi-v8` CMakeLists pulls
V8 headers + static libs from `engine/3rdparty/puerts/unity/native/papi-v8/.backends/papi-v8/`.
puerts upstream expects the directory under `.backends/` to be named after
`JS_ENGINE` (default `v8`), but in this clone the directory is actually
named `papi-v8`, so `engine/3rdparty/CMakeLists.txt` overrides
`set(JS_ENGINE papi-v8)` before `add_subdirectory(papi-v8)`. It also has
to inline the platform-specific `BACKEND_INC_NAMES`/`BACKEND_LIB_NAMES`/
`BACKEND_DEFINITIONS` (puerts' upstream `make_win64.bat` /
`make_linux64.sh` / `make_osx64.sh` set these via the CLI).

The vendored Windows `wee8.lib` (V8 13.6.233) requires C++20 -- the
`BACKEND_DEFINITIONS` therefore include `V8_129_OR_NEWER`, which makes
`papi-v8/CMakeLists.txt` pick `CMAKE_CXX_STANDARD 20`.

Other platforms need the matching `.backends/papi-v8/` slot populated
(Android: `Lib/Android/<abi>/`, iOS: `Lib/iOS/`, macOS: `Lib/macOS_arm64/`
or `Lib/macOS/`, Linux: `Lib/Linux/`). Missing libs surface as link-time
`unresolved external symbol` for V8 internals, NOT for our `BackendV8`
-- if the `BackendV8` symbols themselves are unresolved, check the
`Papiv8`-vs-`PapiV8` gotcha below.

**Win64 v8 build prerequisites (RESOLVED 2026-05-16)**:
The vendored `Lib/Win64/wee8.lib` is built with `MT_StaticRelease` (`/MT`
static release CRT, `_ITERATOR_DEBUG_LEVEL=0`) AND was compiled with
clang-cl on a more recent MSVC STL than 14.39. Three cascading issues
were identified in the original analysis. As of 2026-05-16 **all three
are closed**: #1 by Plan C in this repo, #2 by upgrading to MSVC
14.44.35207 on the dev box (selected per-build via `VCToolsVersion`,
see "Action items" block below), #3 by fixing the `editor_ui_pass.cpp`
Vulkan ifdef leak. The block below is kept as a tombstone so future
maintainers don't repeat the diagnosis from scratch if a similar
toolchain mismatch reappears.

1. **CRT flavor mismatch (resolved by Plan C in this repo)**: ZEngine's
   default `/MD[d]` produces `LNK2038: 'RuntimeLibrary'` when pulling
   `wee8.lib` + `PapiV8.lib` into a Debug binary. Resolved by
   `Plan C` -- the top-level `CMakeLists.txt` now sets
   `CMAKE_MSVC_RUNTIME_LIBRARY="MultiThreaded"` (plain `/MT`, no Debug
   variant) when `PAPI_TYPE=v8` on Windows, AND
   `engine/3rdparty/CMakeLists.txt` flips
   `USE_MSVC_RUNTIME_LIBRARY_DLL=OFF` (GLFW), `CURL_STATIC_CRT=ON`
   (cURL), and uses /MT Jolt knobs in the same condition. Both branches
   of the gate use FORCE-cache so flipping `PAPI_TYPE` between v8 and
   quickjs in an existing `build/` tree resets cleanly. Stock `Debug`
   config is rejected (`wee8.lib` ships only `MT_StaticRelease`, no
   `MTd_StaticDebug`) -- on multi-config generators (Visual Studio) we
   print a warning at configure time, on single-config (Ninja) we
   `FATAL_ERROR`. Use `--config Release`, `--config RelWithDebInfo`, or
   `--config DebugV8`.

   **`DebugV8` config (added 2026-05-17)**: a custom build configuration
   that gives "Debug-friendly UX on top of Release ABI", so V8 path
   users still get full single-step debugging. It is registered into
   `CMAKE_CONFIGURATION_TYPES` only when `PAPI_TYPE=v8` on Windows
   (multi-config generators). Flag matrix:

   | Knob                            | DebugV8       | Stock Debug | RelWithDebInfo |
   |---------------------------------|---------------|-------------|----------------|
   | `RuntimeLibrary`                | `/MT`         | `/MTd` *    | `/MT`          |
   | `_ITERATOR_DEBUG_LEVEL`         | `0`           | `2` *       | `0`            |
   | `_DEBUG`                        | not defined   | defined *   | not defined    |
   | Optimisation                    | `/Od /Ob0`    | `/Od /Ob0`  | `/O2 /Ob1`     |
   | `/Zi` (PDB)                     | yes           | yes         | yes            |
   | `/RTC1` (runtime checks)        | yes           | yes         | no             |
   | Single-step lands every line    | yes           | yes         | partial        |
   | Variables visible (no opt-out)  | yes           | yes         | partial        |
   | STL bound/iterator checks       | **NO**        | yes         | no             |
   | Links against vendored wee8.lib | **YES**       | LNK2038 *   | yes            |

   `*` = stock Debug under `PAPI_TYPE=v8` actually fails to link, so
   the cells marked `*` only describe what the *flags* would be.

   `CMAKE_MAP_IMPORTED_CONFIG_DEBUGV8` is set to
   `Release;RelWithDebInfo;` so any imported library lookup under
   DebugV8 picks the Release flavor (which is what `wee8.lib` IS).

   The Jolt knobs in `engine/3rdparty/CMakeLists.txt` use
   `$<CONFIG:Debug>` (literal `Debug` only, NOT DebugV8) so DebugV8
   automatically falls through to the `_jolt_compile_release_crt`
   branch, getting `/MT` -- matching the global Plan-C choice.
   `_ITERATOR_DEBUG_LEVEL=0` is supplied for DebugV8 by the top-level
   `CMAKE_CXX_FLAGS_DEBUGV8` instead of by Jolt's per-target options,
   so consistency is centralised.

   **Usage**:
   ```powershell
   # configure step (Visual Studio multi-config generator)
   cmake -S . -B build -DPAPI_TYPE=v8 -G "Visual Studio 17 2022"
   # build step
   cmake --build build --config DebugV8
   # or via the preset:
   cmake --build --preset windows_pc_v8_debug
   # or zbuild:
   python zbuild.py build --config debugv8
   ```

   **What you LOSE compared to stock Debug**: `_IDL=2` STL bound/
   iterator checks (you still have `/RTC1` for stack/uninitialised-var
   checks). `assert()` from `<cassert>` is still active because
   `NDEBUG` is not defined in DebugV8 either.

   **What you GAIN compared to RelWithDebInfo**: `/Od /Ob0` instead of
   `/O2 /Ob1`, so single-step always lands on the next source line and
   no local variables are optimised away.

   **Limitation**: only useful on multi-config generators
   (`Visual Studio 17 2022`, `Ninja Multi-Config`). On Ninja
   single-config you'd reconfigure with `-DCMAKE_BUILD_TYPE=DebugV8`,
   which works but defeats the point of having Debug + Release builds
   side by side.

2. **MSVC STL toolset gap (VERIFIED 2026-05-16, BLOCKING)**: Once
   issue #3 was fixed, the build progressed to the link step and
   produced exactly the predicted symptom: 50 `LNK2019` lines for
   `__std_*` symbols coming from `wee8.lib` (`heap.obj`, `search-util.obj`,
   `regexp-parser.obj`, `regexp-compiler-tonode.obj`,
   `maglev-graph-printer.obj`, `v8-debugger-agent-impl.obj`,
   `json.obj`, ...) culminating in
   `LNK1120: 17 unresolved externals` (full log:
   `e:\Engine\ZEngine\build_v8_link2.log`). The vectorized helpers
   `__std_find_end_2`, `__std_min_4u`, `__std_mismatch_2`,
   `__std_replace_8`, etc. are emitted by clang-cl when `wee8.lib` is
   compiled, but only exist in MSVC STL toolset >= 14.40 (VS 2022
   17.10+). On this dev box (VS 2022 17.9.6 + MSVC 14.38 / 14.39) those
   symbols are absent from `libcpmt.lib`, so the link fails. There is
   no Plan-C-style workaround at the CMake level for this -- the only
   fixes are toolset upgrade or rebuilding `wee8.lib`.

3. **`editor_ui_pass.cpp` Vulkan ifdef leak (FIXED 2026-05-16)**: The
   Windows `#else` branch of
   `engine/Source/Editor/render/pass/editor_ui_pass.cpp` used to
   reference `VulkanRHI` / `VulkanRenderPass` / `imgui_impl_vulkan.h`
   unconditionally, but those types/headers are only pulled in under
   `#if defined(Z_HAS_VULKAN)`. The `#else` is now `#elif defined(Z_HAS_VULKAN)`
   with a no-op `#else` fallback (Windows always early-returns through
   the DX12 branch above; macOS early-returns through the Metal branch;
   the no-op `#else` only fires on platforms that have neither, which
   today is none of our shipping targets). **Project-wide policy**
   (revised 2026-05-16):
   - Windows editor's **default** backend is **DX12** (the
     editor_ui_pass.cpp fast path early-returns through the DX12
     branch on Windows for ImGui rendering).
   - Vulkan is a **first-class supported, switchable backend on
     Windows** — both `Z_HAS_VULKAN` and the `VulkanRHI` runtime path
     remain compiled in for Win64. Bindless via
     `VK_EXT_descriptor_indexing` is auto-enabled on PC desktop GPUs
     (NVIDIA / AMD / Intel) the same way as on Android / HarmonyOS;
     PC just uses a higher default cap (16384 sampled images vs 4096
     mobile), clamped at runtime against
     `maxPerStageDescriptorUpdateAfterBindSampledImages`.
   - Mobile (Android / HarmonyOS) ships with Vulkan as the **only**
     backend.
   - When adding a new render-feature code path, do not write
     `#if defined(_WIN32)` to disable Vulkan; instead key off the
     active RHI selection at runtime (or the existing
     `Z_HAS_VULKAN` / `Z_HAS_DX12` configure-time macros).

**Action items to fully unblock v8 on Win64** (RESOLVED 2026-05-16):
- **MSVC toolset**: VS Installer was upgraded to **17.14.37301.10**,
  which ships **MSVC 14.44.35207** alongside the legacy 14.38/14.39
  components. The 14.44 STL exposes the vectorized `__std_*` helpers
  that `wee8.lib` was emitted against, eliminating the LNK2019 wall.
  Selection is done at **build time** (not configure time) by setting
  `VCToolsVersion`, e.g.:
  ```
  $env:VCToolsVersion = "14.44.35207"
  & MSBuild.exe build_v8\engine\Source\Editor\ZEditor.vcxproj `
      /p:Configuration=RelWithDebInfo /p:Platform=x64 /m `
      /p:VCToolsVersion=14.44.35207
  ```
  CMake configure (`cmake -S . -B build_v8`) is left at default toolset.
  Do NOT pass `-T version=14.44` to CMake -- CMake will FATAL_ERROR
  if the cached toolset doesn't match (`Either remove the CMakeCache.txt
  file ... or choose a different binary directory.`); the per-build
  override is sufficient and avoids a 30+ minute rebuild.
- **Windows SDK**: `10.0.22621.0` is reinstalled and the build no
  longer trips on `MSB8036`.
- **Optional long-term**: Rebuild `wee8.lib` from V8 source with a
  pinned MSVC version (or any version >= 14.39 paired with
  `_DISABLE_VECTOR_ANNOTATION` / disabling the vectorized
  `<algorithm>` helpers) + `/MD` to produce a CRT-flexible static
  library. This is what puerts upstream eventually does in their CI;
  we just haven't run that pipeline ourselves. Not needed now that
  14.44 is on the box.

**`Directory.Build.props` toolset gate (2026-05-29)**: the root
`Directory.Build.props` auto-pins `VCToolsVersion=14.44.35207` so both
`cmake --build` and the VS IDE pick up the v8 toolset without a
per-session env var. The gate now requires the 14.44 toolset to be
**actually installed** (it probes the well-known VS 2022
`Professional/Enterprise/Community/BuildTools\VC\Tools\MSVC\14.44.35207`
paths) AND the vendored `wee8.lib` to be present, before forcing it.
The earlier version keyed only off `wee8.lib` existing, which broke
the CMake `VCTargetsPath` configure probe with `MSB8052` ('14.44.35207'
not compatible with v143) on any box that lacked 14.44 (e.g. a fresh
14.39-only machine, or pre-upgrade MSBuild < 17.10 which maps 14.44 to
v144). `$(VCInstallDir)` is not yet defined when Directory.Build.props
is imported, hence the hardcoded path probe. Net effect: a clone on a
14.39-only box configures (QuickJS) cleanly; a 14.44 box still forces
14.44 for v8. Callers can always override by setting `VCToolsVersion`
in the environment.

**Second dev box brought to v8 parity (2026-05-29)**: the
`ZENTIALI-PC19` box was VS Professional **17.9.6** with only MSVC
**14.39.33519** (MSBuild 17.9.8, which rejects 14.44 for v143). Updating
it to v8 required a full `setup.exe update --channelUri
https://aka.ms/vs/17/release/channel` to **17.14.37314.3** (NOT just a
`modify --add` of the 14.44 component -- the 14.9 catalog has no 14.44,
and MSBuild 17.9 can't use it anyway). The update replaced 14.39 with
**14.44.35207** as the sole "Latest" build-tools toolset. Gotcha:
`Start-Process -ArgumentList @(...)` mangles the `--installPath "C:\Program
Files\..."` quoting (truncates at the first space -> installer errors
`Could not find an installed product matching ... installPath: C:\Program`);
pass the whole arg list as a **single string** with embedded quotes
instead. After the update, wipe `build/CMakeCache.txt` + `build/CMakeFiles`
and reconfigure (the cached 14.39 compiler path is gone); v8
`RelWithDebInfo` then links clean.

**Verified end-to-end Win64 v8 link (2026-05-16, `bin\RelWithDebInfo\
ZEditor.exe`, 44.9 MB)**: Plan C (CRT switch to `/MT[d]` when
`PAPI_TYPE=v8`) + 14.44 toolset + ShaderLab side-target (see below) +
`editor_ui_pass.cpp` Vulkan ifdef fix. Quickjs path is unaffected.

**Build environment regression (post VS 17.14 upgrade, hit + recovered
2026-05-16)**: The IDE upgrade transiently broke the build in two
unrelated ways before VS Installer reseated everything:
1. `cmake -S . -B build_v8` (re-)configure failed with
   `could not find any instance of Visual Studio` even though
   `vswhere -prerelease` found 17.14. Cause: `CMakeCache.txt` pinned
   `CMAKE_GENERATOR_INSTANCE` to a path/instance ID that the new VS
   Installer didn't know about. Fix was to delete that cache line
   (`findstr /V CMAKE_GENERATOR_INSTANCE CMakeCache.txt > .new && move`)
   AND let VS Installer re-register the instance (which it does as
   part of installing the SDK component below).
2. `MSBuild` failed at `_CheckWindowsSDKInstalled` with
   `MSB8036: The Windows SDK version 10.0.22621.0 was not found.`
   The `C:\Program Files (x86)\Windows Kits\10\Include\` directory
   was missing. Fix: install the matching Windows SDK component via
   Visual Studio Installer; that also fixed (1) because VS Installer
   reseats the instance ID at the same time.
Both are noted here for the next time someone bumps VS major versions.

**ShaderLab side-target (FIXED 2026-05-16)**: Independent of v8, an
existing latent bug in the Editor link surface was exposed by the
v8 link logs: `inspector_window.obj`, `project_window.obj` and
`shader_preview_renderer.obj` all reference
`ZEngine::ShaderLab::ShaderLabParser` ctor / `Parse()`, but on
`Win+!Vulkan / Apple / Web` configurations
`engine/source/runtime/CMakeLists.txt` deliberately EXCLUDE's
`Function/ShaderLab/*.cpp` from ZRuntime (because
`shader_lab_compiler.{h,cpp}` includes
`Function/Render/interface/vulkan/shader_compiler.h`). That left those
three Editor TUs with no ShaderLabParser implementation to link
against -> 6 LNK2019/LNK2001 lines. On Linux/Android/OHOS/iOS/Win+Vulkan
this never reproduced because there ShaderLab cpp ships inside
ZRuntime. Fix:
- `engine/source/runtime/Function/ShaderLab/CMakeLists.txt` rewritten
  to compile a Vulkan-free **parser-only** STATIC (`shader_lab_lexer.cpp`
  + `shader_lab_parser.cpp`) when `ZENGINE_USE_VULKAN` is OFF, and
  add `shader_lab_compiler.cpp` only when it is ON. PRIVATE-links
  `ZRuntime` to inherit the bqlog include directory transitively (the
  cpp files include `runtime/core/base/macro.h` which pulls in
  `<bq_log/bq_log.h>` from ZRuntime's INTERFACE). PRIVATE keeps it
  off ShaderLab's INTERFACE so ZEditor doesn't double-propagate.
  No circular dependency: this CMakeLists is only add_subdirectory'd
  on configs where ZRuntime no longer contains the parser cpp itself.
- `engine/source/runtime/CMakeLists.txt` appended an
  `add_subdirectory(Function/ShaderLab)` call gated by the same
  three configurations that EXCLUDE ShaderLab cpp from ZRuntime
  (Web / Apple / `WIN32 AND NOT ZENGINE_USE_VULKAN`), guarded by
  `if(NOT TARGET ShaderLab)` so it's idempotent.
- `engine/Source/Editor/CMakeLists.txt` adds
  `target_link_libraries(${TARGET_NAME} PUBLIC ShaderLab)` guarded
  by `if(TARGET ShaderLab)`. On Vulkan-capable hosts the target
  doesn't exist (cpp is in ZRuntime already), so the link doesn't
  duplicate.

ShaderLab CMakeLists no longer has `target_link_libraries(...PUBLIC Base Render)`
(those targets don't exist as separate libs in this codebase -- the
original CMakeLists was unreachable code).

**Gotcha**: puerts' `papi-v8/CMakeLists.txt` declares the target as
`PapiV8` (capital V), while `papi-quickjs` uses `PapiQuickjs`. CMake target
names are case-sensitive, so `engine/3rdparty/CMakeLists.txt` and
`engine/Source/Runtime/CMakeLists.txt` must reference `PapiV8` exactly --
typo'ing it as `Papiv8` produces silent "TARGET PapiV8 not found" no-ops
and a degraded-mode binary at runtime (`m_ScriptEnv == nullptr`).

### 2.8 QuickJS libbf MSVC patch

`engine/3rdparty/puerts/unity/native/papi-quickjs/quickjs/libbf.c` uses GCC-
style operator overloading on `__m256d` (e.g. `r + m`, `a * b`) inside its
AVX2 FFT/NTT bignum path. MSVC's intrinsic headers don't support those
operators, so the AVX2 code doesn't compile under cl.exe. The parent
`engine/CMakeLists.txt` adds `/arch:AVX2` globally, which makes MSVC
predefine `__AVX2__` and pull the offending code in.

Workaround (in `papi-quickjs/CMakeLists.txt`): give `quickjs/libbf.c`
`COMPILE_OPTIONS "/arch:SSE2"` on MSVC only. MSVC's documented behavior is
that the LAST `/arch:` on the command line wins, so this overrides the
inherited `/arch:AVX2` and stops `__AVX2__` from being predefined for that
TU. Functionally harmless: the AVX2 path is only entered for `>=100`-limb
bignum mul, which scripts won't trigger. clang-cl is fine without the
patch (it understands the GCC operators), so the patch is gated on
`MSVC AND NOT CXX_COMPILER_ID STREQUAL "Clang"`.

### 2.9 Bindless texture path

Full design / per-PR landing notes / open items moved to
**`doc/BINDLESS_TEXTURE_PATH.md`** (the section was >400 lines and
overshadowing the rest of this file). Read that doc first if you are
about to touch any of:

- `RHI::supportsBindlessTextures()` / `getBindlessTextureManager()` /
  `RHIBindlessTextureManager` interface in
  `runtime/function/render/interface/rhi.h`.
- Vulkan backend: `vulkan_bindless_texture_manager.{h,cpp}`,
  `VulkanRHI::Initialize / clear` bindless paths.
- DX12 backend: `dx12_bindless_texture_manager.{h,cpp}`,
  `DX12RHI::createPipelineLayout` bindless detection,
  `DX12RHI::getBindlessRootSignatureFlags()`,
  `DX12RHI::kBindlessStaticSamplerCount` (PR7 SSOT for the
  static-sampler bank size),
  `RHI::cmdSetBindlessIndexPFN`.
- DX12 bindless utility pipeline (PR7):
  `runtime/function/render/interface/dx12/utility/bindless_texture_blit_pipeline.{h,cpp}`
  + `dx12/utility/shaders/bindless_blit_{vs,ps}.hlsl`. First
  RHI-abstracted bindless graphics pipeline. Production consumers
  landed in PR8a/b/c (see `doc/BINDLESS_TEXTURE_PATH.md`):
  PR8a = scoped `SetDescriptorHeaps` swap at the call site;
  PR8b = `bindless_blit_smoke.{h,cpp}` dev-only ImGui canary widget;
  PR8c = `bindless_texture_preview.{h,cpp}` 256x256 inspector preview.
  **REMOVED (2026-05, ImGui cleanup)**: both editor widgets
  (`BindlessBlitSmoke` and `BindlessTexturePreview`) were dead ImGui
  code after the inspector went fully native ZSlate -- `DrawWidget` /
  `DrawPreview` had no remaining call site (the legacy ImGui
  `InspectorWindow` that dispatched them is gone). The
  `bindless_texture_blit_pipeline` itself is UNCHANGED and still the
  reference bindless graphics pipeline; the only live remnant of the
  preview module is the `texture2d` type predicate, now folded into
  `InspectorAssetCommon::IsTexture2DInspectorAssetType`. Texture images
  are viewed through the native ZSlate Preview window, not an inline
  inspector preview.
- HLSL `ResourceDescriptorHeap[NonUniformResourceIndex(...)]` use
  sites or GLSL `nonuniformEXT(...)` use sites.
- The `BindlessIndex::pack / unpackTexture / unpackSampler` helper
  (rhi.h, immediately below `RHIBindlessTextureManager`) -- engine-wide
  source of truth for the 32-bit `tex | (samp << 16)` packing.
- The DX12 bindless smoke-test under
  `runtime/function/render/interface/dx12/test/`
  (`bindless_smoke.hlsl`, `bindless_smoke_vs.hlsl`,
  `dx12_bindless_smoke_test.cpp`, gated by
  `ZENGINE_BUILD_BINDLESS_SMOKE_TEST=ON`).

Quick-reference invariants that must NOT drift without updating both
this file's pointer block AND `doc/BINDLESS_TEXTURE_PATH.md`:

1. `RHIBindlessTextureManager::kInvalidBindlessIndex` is `0xFFFFFFFF`
   and slot 0 is the engine-managed default-white placeholder (cannot
   be freed).
2. The 32-bit root constant pushed via `cmdSetBindlessIndexPFN` is
   always `BindlessIndex::pack(tex, samp)` -- low 16 bits texture,
   high 16 bits sampler. The smoke-test pins this with
   `static_assert`s against the HLSL literals.
3. Sampler-bindless (`SAMPLER_HEAP_DIRECTLY_INDEXED`) is intentionally
   NOT enabled -- static-sampler array is the current model on DX12,
   and the high half of the packed index addresses that array.
4. Bindless and bindful bindings cannot be mixed inside the **same**
   descriptor set on either backend.
5. (PR7) Every bindless-aware DX12 root signature carries
   **exactly 4 static samplers** at `s0..s3 / RegisterSpace 0`
   in the order `LinearWrap, LinearClamp, PointWrap, PointClamp`.
   The count is the `DX12RHI::kBindlessStaticSamplerCount` constexpr;
   the order is mirrored by the `BindlessBlitSampler` enum in
   `dx12/utility/bindless_texture_blit_pipeline.h` and by the
   `register(s0..s3)` slots in
   `dx12/utility/shaders/bindless_blit_ps.hlsl`. All three are
   pinned by `static_assert`s in the smoke-test; widening the
   bank requires updating all four sites (RHI cpp, constexpr,
   enum, HLSL) in lockstep.

### 2.10 Content Browser display rules (UE Content Browser model)

The Content Browser applies a **per-root extension whitelist** -- a file is
shown in a given top-level tree only if its extension belongs to that
tree's set, even when it physically lives there:

| Root        | Allowed extensions                                |
|-------------|---------------------------------------------------|
| `Assets/`   | `.zasset`, `.json`, `.scene`, `.prefab`, `.mat` |
| `Scripts/`  | `.ts`, `.tsx`, `.js`                              |
| `Shaders/`  | `.hlsl`, `.shader`, `.compute`, `.raytrace`       |
| `Data/`     | `.csv`, `.xlsx`                                   |
| `Textures/` | `.png`, `.jpg`, `.jpeg`, `.tga`, `.bmp`          |
| `Models/`   | `.fbx`, `.obj`, `.gltf`, `.glb`                  |

Image and mesh source files surface under `Textures/` and `Models/` (checked
into VCS). Other source kinds (`.wav`, `.mp3`, ...) remain hidden until they
get their own top-level root. Source files are **never** surfaced under
`Assets/` -- only binary products (`.zasset`, scenes, prefabs) appear there.

**Single source of truth for the rules**:
`engine/Source/Editor/editor_file_service/editor_file_service.cpp`
function `shouldDisplayInContentBrowser(path, root_label)`. The
`root_label` parameter is the lower-case identifier that
`EditorFileService::buildEngineFileTree` passes to `buildRoot()`
(`"asset"`, `"scripts"`, `"shaders"`, `"data"`, `"textures"`, `"models"`).
Adding a new top-level root means adding a new branch here.

**Import product placement (UE rule)**:
`AssetsMenu::convertAsset(source_path, target_dir)` routes the `.zasset`
output into one of two places, in priority order:

1. `target_dir / <source-stem>.zasset` -- when `target_dir` is non-empty
   and lies inside `<Project>/Assets/`. This is what the Content Browser
   passes when the user has a folder selected inside the Assets tree at
   the moment they click Import.
2. `<Project>/Assets/<source-stem>.zasset` -- the project-content
   fallback. Used when the user's selection is in `Scripts/`, `Shaders/`,
   `Data/`, or anywhere else outside the AssetRegistry's scan root.

The product is NEVER written next to the source file (the pre-PR-PW2
behaviour) because that placement leaks `.zasset` files outside the
registry's scan root and silently breaks discovery. `target_dir` membership
in Assets/ is checked lexically against `ProjectInfo::getProjectContent()`
so it works even when the directory is freshly created in the Project
window and not yet on disk.

**Cross-process drag-drop import (PR-AI1, landed)**: dragging a file from
the OS file manager (Windows Explorer / Finder / Linux file manager) onto
the editor window auto-imports it via the same code path as the right-click
"Import…" entry. Wiring is single-direction and minimal:
`WindowSystem` already calls `glfwSetDropCallback(...)` and forwards into
the existing `registerOnDropFunc` listener vector; `ZSlateContentBrowserWindow`'s ctor
subscribes one listener that copies the GLFW path strings (which are only
valid for the duration of the callback per GLFW docs) into a
mutex-guarded queue, and `executePendingOsDropImports()` drains the queue
at the tail of `onGUI()` -- right after `executePendingDelete` /
`executePendingPrefabCreate`, so we inherit the same UAF-avoidance
deferral that those use against `buildEngineFileTree()` mid-walk. Drop
target is `resolveDropTargetFolder(m_selected_node)`; PR-PW2's two-stage
fallback in `AssetsMenu::convertAsset` then guarantees products always
land inside `<Project>/Assets/` even when the user had a Scripts/ /
Shaders/ root selected. Files with no registered importer (extension
not handled by any `AssetImporter`) are skipped with a single warning
per file -- equivalent to UE's `IsImportExtensionAllowed` gate.
Directory drops are explicitly out of scope (UE's
`ContentBrowserAssetDataSource` rejects them too). After a successful
batch we reset `m_last_file_tree_update` so the periodic Project-tree
rebuild (1 Hz) fires immediately on the next frame.

**Live HLSL recompile (PR-AI2, landed)**: editing
`<Project>/Shaders/Foo.hlsl` (or `.hlsli` / `.shader` / `.compute` /
`.raytrace`) outside the editor invalidates the matching DX12 DXIL cache
slot(s); the next PSO build the engine performs against that source
recompiles automatically thanks to the cache miss + DXC compile path.
Wiring is single-direction and minimal:
`EditorAssetManager` owns a third watcher `m_shaders_watcher` rooted at
`ProjectInfo::getShadersRoot()` with the source-extension filter listed
above. Events from create / change / delete all funnel through
`queueShaderInvalidation(path)` which stamps a 200 ms debounce deadline
in `m_pending_shader_invalidations` (mutex-guarded). `TickWatcher()`
calls `flushPendingShaderInvalidations()` once per frame after the data
watcher drain; entries past their deadline get popped and routed
through `DX12ShaderCompiler::invalidateCacheForSource(src)`. That static
method recomputes the same FNV-1a / `toLower` / `generic_string()` /
`absolute()` chain that `buildCacheFilePath()` uses, then prefix-matches
every `<src_hash>_*.dxil` under the default cache directory and deletes
them in one pass -- so all (stage, entry, defines, target_profile,
hlsl_version) variants sharing that source disappear together. 200 ms
matches Unity's coalesce window and is half UE's; it covers VS Code's
write-temp-then-rename save sequence on every platform we've measured.
`.hlsli` events currently no-op at the cache layer (the cache key is
the top-level source, not the header) but the recursive include-mtime
scan inside `dx12_shader_compiler.cpp` already rejects stale caches at
the next compile, so editing a header still recompiles dependent shaders
without an explicit reverse index. PSO rebuild itself happens lazily on
the next render-thread reference -- no broadcast event bus, deliberately
simpler than UE's `FShaderCompilingManager`.

**PR-AI3 (AutoReimport for externally-modified source files)** — landed.
The cross-platform `FileSystemWatcher` infrastructure
(`engine/Source/Editor/file_system_watcher/`) is already wired to
`Assets/` (`m_file_watcher`), `Data/` (`m_data_watcher`),
`Shaders/` (`m_shaders_watcher`), and `Intermediate/Scripts/`
(`m_js_watcher`); PR-AI3 added no new watcher modules. The trigger
model is **focus-driven, Unity-style** (not UE-style realtime
`FContentDirectoryMonitor`): on GLFW window-focus-gained, the editor
walks a `(.zasset → source_abs_path, source_mtime_ns)` registry, stats
each source, and enqueues changed entries into a throttled drain
(`kReimportsPerFrame = 10`) running inside `EditorAssetManager::TickWatcher`.
Rationale: ZEngine's user base is small; a focus-driven model is half
the code of UE's state machine and behaves more predictably (no
`PromptUser` race against typing). After a focus pass enqueues work,
`m_reimport_paused_until_focus = true` suppresses redundant rescans on
subsequent focus pulses (e.g., clicking between ImGui windows on
platforms where focus oscillates) until the queue drains.

The `(.zasset, source)` mapping is persisted to
`<Project>/AssetRegistry/source_registry.json` via the new
`SourceAssetRegistry` (composition member of `EditorAssetManager`,
**not** an `IEngineSystem`; it follows the same VCS-checked-in pattern
as `ScriptRegistry`). JSON shape: `{ "version": 1, "entries":
[ { "zasset", "source", "mtime_ns" }, ... ] }`. The registry is
written atomically (tmp file + rename, with `copy_file` + `remove`
fallback on Windows). Keys are normalised through
`SourceAssetRegistry::normaliseKey` (lowercase on `_WIN32`) so a JSON
shipped from a case-different machine still hits the same slot. Entries
are populated by `EditorAssetManager::recordImportSource(zasset, source)`,
called from `AssetsMenu::convertAsset` immediately after a successful
`importAsset` (the call site uses
`std::dynamic_pointer_cast<EditorAssetManager>(GET_SYSTEM(AssetManager))`
since `IEngineSystem` is polymorphic). Removed `.zasset` files are
unregistered in `EditorAssetManager::onFileDeleted`.

**Important format-stability decision**: the design doc originally
proposed adding `source_path` (64B) + `mtime` (8B) fields to
`AssetFileHeader::reserved[4]` (only 32B available -- would have
required a header-version bump and touched
`SerializedFile::ReadHeader`'s `m_ReadOffset = 176` constant). We
switched to the sidecar JSON approach to keep every existing `.zasset`
**bit-for-bit identical**. This is the same trade-off as
`ScriptRegistry` vs. embedded GUID for `.ts` files (per AGENTS.md §2.1):
the registry is the source of truth, the binary asset stays untouched.

**Importer interface change**: `AssetImporter` gained a
3-arg `reimport(zasset, source, settings)` virtual whose default impl
calls `import(source, zasset, settings, AssetMetadata{})`. This means
existing importers (`TextureImporter`, `ShaderImporter`,
`DataTableImporter`, etc.) work without per-importer overrides; only
importers that need source-discovery shortcuts (none currently) would
override it. The pre-existing single-arg `reimport(asset_path)` stub
on `TextureImporter` is unaffected.

**TickWatcher ordering** (deliberate, do not reshuffle):
`m_file_watcher` → `m_data_watcher` → `m_shaders_watcher` →
`tickReimportQueue()`. Reimports write `.zasset` files **after**
`m_file_watcher` has already polled this frame, so the resulting
register/refresh fires on the *next* frame -- a one-frame delay that
matches the data-watcher rationale and avoids re-entrant registry
mutations during `forEach`. Lock discipline inside `onEditorFocusGained`
and `tickReimportQueue` follows collect-under-lock → act-outside-lock
to avoid deadlocking against `recordImportSource` (which is callable
from importer code paths that may run on the same thread).
- **Manual reimport API**: `EditorAssetManager::reimportAsset(zasset_path)`
  resolves the source via `SourceAssetRegistry` and calls the importer's
  3-arg `Reimport(zasset, source, settings)` (same path as the focus queue).
  `importSourceAsset` wraps `AssetImportManager::ImportAsset` plus
  `RecordImportSource` + registry refresh. `TextureImporter::Reimport`
  (2-arg) delegates to `reimportAsset` when an editor registry entry exists.

### 2.12 Console window (editor)

- **Search**: filter bar with regex (default on), case-sensitive toggle,
  Ctrl+F focus, `visible/total` count. Matching uses `std::regex` in
  `ConsoleWindow.cpp` only (not the header).
- **Category**: `ImGui::BeginCombo` populated from
  `LogSystem::GetInstance().m_Logger->get_categories_name_array()` (same
  names as `engine_log` / `bq_log_category_config.ini`). Default **All**;
  combines with text/regex filter.
- **Deferred IBL logs**: `DX12RHI::LogDeferredIblCubemapDiagnostics()` re-emits
  cubemap GPU-mipgen lines after `EditorUI` init so early `RenderSystem`
  messages appear in the Console (on-disk log always has the originals).
- **Console commands (UE-style)**: `ConsoleManager` (`Runtime/Function/Console/`)
  owns CVars + commands; the Console window input bar calls
  `ExecuteString` (Tab autocomplete, Up/Down history). Case-insensitive
  lookup. CVar syntax: `set Name Value`, `Name Value`, `Name=Value`, or bare
  `Name` to print. Runtime builtins register in
  `RegisterRuntimeConsoleCommands()` (`stat fps`, `quit`/`exit`, `echo`,
  `t.MaxFPS`, `r.ShowFPS`). Editor builtins register in
  `RegisterEditorConsoleCommands()` from `Editor::Initialize` (`obj list`,
  `level reload|save`, `asset.find`, `asset.reimport`, `asset.count`,
  `play`, `pause`). Prefer registering new commands from module
  `Initialize()` rather than static `REGISTER_CONSOLE_*` macros (those run
  before `ConsoleManager` exists). See `doc/CONSOLE_MANAGER_USAGE.md`.

### 2.11 DX12 global texture upload (IBL / LUT)

- HDR cubemap and 2D globals use **`ExecuteDedicatedUploadCommands`** in
  `DX12RHI.cpp`: a one-shot allocator + command list + fence, never the
  frame `m_CommandAllocators[m_CurrentFrameIndex]`. Mirrors
  `DX12BindlessTextureManager::CreateAndUploadPlaceholder`.
- **`RHI_FORMAT_R32G32B32A32_SFLOAT` sources** are stored as
  `RHI_FORMAT_R16G16B16A16_SFLOAT` on DX12; CPU converts float32 to
  float16 in `CreateTextureUploadStaging` (`owned_pixels` keeps storage
  alive through submit). Log line: `DX12: storing HDR as R16G16B16A16`.
- **Init order (DX12 `RenderSystem`)**: `RenderPipeline::Initialize`
  first (root signatures on a clean device), then
  `UploadGlobalRenderResource`, then
  `DX12MainCameraPass::OnGlobalRenderResourceUploaded()` for skybox
  bindless + RP1/RP2 descriptor refresh.
- **Cubemap mips**: `CreateCubeMap` uploads mip 0 via staging, then
  `DX12CubemapMipGenerator` (linear PS downsample, same mip count as
  `RenderResource::CreateIBLTextures` / Vulkan `GenerateTextureMipMaps`).
  Implementation: `Utility/DX12CubemapMipGen.{h,cpp}` on the dedicated
  upload command list.
- Design / path-B landing notes: **`doc/DX12_SUBPASS_RHI.md`**.
- **DX12 editor sky / IBL**: active path is RP1 deferred UNLIT -> specular
  cubemap -> `backup_odd` HDR (not swapchain overlay). Full flow, UE
  comparison, and RenderDoc checklist: **`doc/rendering/DX12_SKYBOX_RENDERING.md`**.

### 2.12 Local variable and assignment style

- **Locals and parameters**: `snake_case` (see `doc/CODING_STYLE.md`). Constructor
  params that shadow a member use `in_` prefix (e.g. `in_system`).
- **Members / static / globals**: unchanged (`m_`, `s_`, `g_` + camelCase).
- **Assignment layout**: single space around `=`, no column alignment. Enforced by
  root `.clang-format` (`AlignConsecutiveAssignments: false`,
  `AlignConsecutiveDeclarations: false`). Re-format with VS LLVM:
  `"C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Tools\Llvm\x64\bin\clang-format.exe" -i <file>`.
- **Bulk camelCase->snake_case renames** need a TU-aware tool (regex breaks on lambdas
  and class inline bodies). Rename locals incrementally when touching a file.

### 2.13 MegaLights (Vulkan deferred, V1)

UE-inspired stochastic direct lighting for many local point lights without per-light
shadow maps. Full design: **`doc/rendering/MEGALIGHTS_DESIGN.md`**.

- **Toggle**: `r.MegaLights.Enable` (default `0`). When on, Vulkan skips
  `PointLightShadowPass`, zeros `point_light_num` in the mesh UBO, and
  `MainCameraPass` uses `megalights_deferred` instead of `deferred_lighting`.
- **CPU**: `MegaLightsSystem` in `RenderResource` (8x8 tile cull, SSBO upload).
  `RenderScene::SyncPointLightsFromLevel` rebuilds `m_PointLightList` from
  enabled `LightComponent` each frame. Debug ring: `r.MegaLights.SpawnTestLights [N]`.
  SS shadow ray march uses `subpassLoad(depth, offset)` with viewport size in the
  SSBO header (`ml_viewport_width/height`).
- **GPU**: `engine/shader/glsl/megalights_deferred.{vert,frag}` + `megalights_shading.inl`
  (bindings 8-10 on mesh global set). Rebuild `ZShaderCompile` after shader edits.
- **Vulkan**: `MainCameraPass` + GLSL `megalights_deferred.*`. **DX12**: `MainCameraRp1Pass` +
  HLSL `engine/shader/hlsl/rp1/megalights_deferred.frag.hlsl` (SSBO t8-t10 on mesh global set).
- **Not in V1**: HWRT, temporal denoise, rect/spot lights.

### 2.14 Editor dock layout (recurring clipped / black UI)

Symptom: dock panels squashed, large black regions, Game/Scene hints truncated
(`Press Left Alt...` cut off). Usually stale `<Project>/saved/config/imgui.ini`
or GLFW window size != ImGui `DisplaySize` after resize / DPI change.

**User recovery**: `Window` menu -> `Layouts` -> **Reset All Layouts** (or pick
`Default`). Restart editor if needed.

**Code SSOT**:
- `engine/Source/Editor/EditorLayout/EditorLayoutConstants.h` — toolbar height
  reserved above dock host (`MenuController` + `DefaultLayout` must match).
- `DefaultLayout::OnGUI` — `DockSpace(..., ImGui::GetContentRegionAvail())`,
  no `PassthruCentralNode`; auto-rebuild when root dock node size drifts
  >= 48px from host; ini auto-repair threshold 120px / 15% (was 400 / 30%).

Do not reintroduce `DockSpace(..., ImVec2(0,0), PassthruCentralNode)` for the
embedded main dock inside `MenuController`.

### 2.15 ZEngine Insights (CPU timing profiler)

Unreal-Insights-inspired CPU timing profiler. Replaces the abandoned Tracy
integration (we don't vendor a 3rd-party analyzer; the timeline renders natively
in ZSlate, both in an in-editor panel AND a standalone `ZInsights.exe` viewer).
Cooperating layers:

- **Capture core** -- `Runtime/Profiler/InsightsTrace.{h,cpp}`
  (`ZEngine::Insights::InsightsTrace` singleton). Per-thread event streams: only
  the owning thread appends (guarded by that thread's mutex; contended only by
  the occasional UI snapshot / frame prune on the main thread). Scope names are
  interned into a global string table (events store a 4-byte id). `BeginScope` /
  `EndScope` keep a strict LIFO open-stack per thread (a `-1` sentinel for scopes
  that began while paused, so begin/end stay balanced across capture toggles).
  Frame boundaries recorded by `EndFrame()`; a sliding window of the last
  `SetRetainedFrames(N)` frames is kept (default 150) and older fully-closed
  events are pruned (open-stack indices are shifted by the erased prefix).
  **Lock order is fixed**: name table -> (release) -> per-thread; the registry /
  frame locks are never held while a per-thread lock is held, and vice versa, so
  there's no deadlock between hot-path appends and `EndFrame` / `BuildSnapshot`.
  `BuildSnapshot()` copies a consistent `InsightsSnapshot` for the UI (the UI
  never touches live buffers).

- **Capture is heartbeat-gated, default OFF.** The Insights window pulses
  `RequestCapture()` every visible, non-paused frame; `EndFrame()` ages the
  heartbeat and flips `m_Capturing` off ~2 frames after the pulses stop (panel
  closed / hidden / paused). So a session that never opens the window pays only
  one relaxed-bool read per scope. When capture stops, pruning stops too, so the
  last view is frozen for inspection.

- **Instrumentation source**: the existing `ProfileScope` RAII helper (driver of
  the `Z_PROFILE_SCOPE` / `Z_PROFILE_FUNCTION` macros, ~30 call sites) now feeds
  BOTH the legacy per-frame `Profiler` aggregator AND `InsightsTrace`. Don't add
  a parallel macro; new instrumentation just uses `Z_PROFILE_SCOPE("Name")` and
  shows up on the timeline automatically. Frame boundary is
  `Application::TickOneFrame` -> `InsightsTrace::EndFrame()`. Thread track labels
  come from `ThreadManager` (`GameThread` in the ctor; worker threads via
  `SetThreadName`).

- **UI widget** -- `Runtime/Profiler/SInsightsTimeline.h` (header-only
  `SLeafWidget`, **lives in Runtime so both the editor panel and the standalone
  viewer share it verbatim**). Paints a frame ruler + one stacked flame-chart
  track per thread; left-drag pans, mouse wheel zooms about the cursor, hover
  reports the scope name + duration. It only depends on the abstract
  `UIRenderer` + `InsightsSnapshot`, so any `UIRenderer` backend can host it.

- **In-editor panel** -- `Editor/EditorWindow/ZSlateInsightsWindow/`.
  `ZSlateInsightsWindow` (toolbar Pause/Resume, Clear, Fit, **Save & View** +
  status line) builds a fresh snapshot each frame and keeps it as a member so
  the widget's snapshot pointer stays valid across paint+input. Renders through
  the editor's RHI-backed `BatchedUIRenderer` (`ZSlateEditorOverlay`).
  Registered as window id `kInsights` (`EditorLayoutWindowIds.h`) in
  `EditorWindowRegistry` (`default_open=false`), so it auto-appears in the
  Window menu via `BuildEditorWindowZSlateMenu`.

- **`.ztrace` file format + standalone viewer** -- `InsightsTrace.{h,cpp}`
  exposes `SaveTrace(path, snapshot)` / `LoadTrace(path, out)`: a flat
  little-endian dump of `InsightsSnapshot` (magic `'ZTRC'` + u32 version, names,
  frame marks, then per-track events). Free of engine-system deps so the
  standalone viewer reads it without `START_SYSTEM`. The editor panel's
  **Save & View** button (and the `insights.dump [view]` console command) writes
  a timestamped `<cwd>/Insights/trace_*.ztrace` and launches `ZInsights.exe` on
  it (`ShellExecute`, resolved next to `ZEditor.exe`).
  **`ZInsights.exe`** (`engine/Source/Insights/`, Win32-only target gated on
  `WIN32`) is a tiny Win32 + Direct2D/DirectWrite host: `D2DUIRenderer`
  implements the abstract `UIRenderer` over a `ID2D1HwndRenderTarget` (96-DPI
  forced so mouse pixels == widget DIPs), `ZInsightsMain.cpp` drives the shared
  `SInsightsTimeline` (open via argv / drag-drop / `O` key, `F` to fit). It
  links the **static `ZRuntime`** (same pattern as `ZProfilerCsvExporter`) but
  references only the timing-trace + Slate-base objects, so the linker pulls no
  RHI/GLFW/scripting code. No live socket transport yet (the data path is
  offline file by design choice).

- **GPU track (DX12)** -- `Interface/DX12/Utility/DX12GpuProfiler.{h,cpp}`.
  Records matched begin/end `D3D12_QUERY_TYPE_TIMESTAMP` queries on the frame
  command list into a per-frame-slot region of one READBACK buffer
  (`k_max_frames_in_flight` slots x `kQueriesPerFrame`). `DX12RHI::PrepareBeforePass`
  arms the slot (draining the previous use once its frame fence has signalled --
  so readback latency is `k_max_frames_in_flight` frames) and opens a whole-frame
  `"GPU Frame"` span; `SubmitRendering` closes it, `ResolveQueryData`s the range,
  and (after the queue Signal) records the gating fence value via
  `MarkSubmitted`. Per-pass sub-bars come for free: `RHIDrawList::ExecuteAll`
  wraps each entry in `BeginGpuTimingScope/EndGpuTimingScope(entry.debug_name)`
  (the same name as its CPU `Z_PROFILE_SCOPE`), gated on `IsCapturing()`.
  Resolved ticks are converted to the Insights trace clock with
  `ID3D12CommandQueue::GetClockCalibration` (GPU ticks <-> QPC) plus a
  per-frame QPC->steady_clock offset, then pushed via
  `InsightsTrace::PushExternalEvent("GPU", ...)` into a named track that renders
  exactly like a thread track. New RHI seam: `RHI::BeginGpuTimingScope` /
  `EndGpuTimingScope` (default no-op; only DX12 implements them today --
  Vulkan/Metal/WebGL2 get no GPU track yet). All GPU work is gated on capture
  being active (sampled once per frame in `DX12GpuProfiler::BeginFrame`), so a
  closed Insights window adds nothing to the command list.

- **V1 scope** (deliberately): CPU timing + frames + per-thread tracks + DX12 GPU
  track + hover + `.ztrace` save/load + standalone Win32/Direct2D viewer. No
  Vulkan/Metal GPU track, no counters/memory, no vertical scroll on tracks, no
  aggregated stats table, and no LIVE (socket) connection to a running editor --
  the standalone viewer is offline/file-based. All are natural follow-ups on the
  same snapshot model.

### 2.16 Cross-platform render pipeline modules

Full design / roadmap: **`doc/rendering/CROSS_PLATFORM_PIPELINE.md`**. Read that
before touching anything under
`engine/Source/Runtime/Function/Render/Pipeline/`.

A render path (Desktop = deferred+forward, Mobile = forward) is a first-class,
runtime-swappable **`RenderPipelineModule`** that lives ABOVE the RHI.
`RenderPipeline` is now a thin dispatcher holding
`std::unique_ptr<RenderPipelineModule> m_ActiveModule`; it forwards
`Initialize / BuildDrawLists / SubmitDrawLists / UpdateAfterRecreate` + the
query methods to the module. The named pass `shared_ptr` slots
(`m_MainCameraPass`, `m_DirectionalLightPass`, ...) still live on
`RenderPipelineBase` (external code reads them); modules are `friend`s and
populate/reset those slots in `Setup`/`Shutdown`.

**Convention (the important part for new work)**: a new render feature attaches
to a **module's** pass list (`DesktopRenderPipelineModule` and/or
`MobileRenderPipelineModule`), NOT to `RenderPipeline` directly, and must
declare its Desktop/Mobile applicability. Each concrete pass keeps its own
internal `getGraphicsAPI()==DirectX12` / `#if Z_HAS_VULKAN` branch -- the module
layer only abstracts *which passes run, in what order, under which path*.

Selection: `RenderPipelineSettings` (`RenderPath { Auto, Desktop, Mobile }`).
`Auto` -> Desktop on PC, Mobile on Android/iOS/OHOS. CVar `r.RenderPath`
(0/1/2) + editor `Window -> Render Path` submenu + `r.renderpath` editor
command, all via `SetConfiguredPath()` which flips a `std::atomic` dirty flag.
The actual switch is deferred to a safe point:
`RenderSystem::ApplyPendingRenderPathChange` (top of `Tick`) does
`ConsumePathDirty -> FlushRenderingCommands -> ExecuteOnRHIThread(SetActiveModule
+ RewireRenderResourceLayoutsAfterPathSwitch) -> GPU idle`. Never call
`SetActiveModule` inline from the game thread. Startup env override
`ZENGINE_RENDER_PATH` (`auto|desktop|mobile` or `0|1|2`) for headless/CI/smoke
(mirrors `ZENGINE_V8_DEBUG_PORT`).

**Mobile is a SKELETON in Milestone 1**: `MobileRenderPipelineModule` composes
the shared main-camera assembly (an internal `DesktopRenderPipelineModule`) so
the scene renders and the Desktop<->Mobile switch is verifiable end-to-end on
DX12; only `BuildDrawLists` diverges (forward-lite strip: no point-light shadow,
no MegaLights). The bespoke G-buffer-free forward chain (via `mesh_forward`
shaders), lightmap ambient, and simplified shadows are documented follow-up
sub-milestones in the design doc -- do NOT assume Mobile is a real forward
renderer yet. **Vulkan path** of either module currently hits a *pre-existing*
crash in `MainCameraPass::SetupModelGlobalDescriptorSet` (proven via A/B revert,
unrelated to the module refactor); DX12 is the verified backend.

### 2.17 Per-platform texture cook pipeline

Full design / phase landing notes / verification at
**`doc/asset_management/TEXTURE_COOK_PIPELINE.md`**. Read it before touching any
of: `Texture2D` mip/format schema, `TextureCompressor`, the DDC, `TextureImporter`
cook paths, `MaterialRes` texture PPtrs, or `RenderResourceBase::LoadTexture`.

Conventions (do not re-litigate):

- **One source image -> per-platform cooked variants.** No `.meta`, no ETC2.
  BC1/BC3/BC7 on desktop + WebGL, ASTC LDR on mobile (Android / iOS / OHOS).
- **`Texture2D`** stores a concatenated mip chain (`m_Pixels` + `m_MipOffsets`)
  and a real `RHIFormat` ordinal in `m_Format` (incl. BC*/ASTC). New `Transfer`
  nodes are append-only; old single-mip RGBA8 `.zasset` still load (SafeBinaryRead
  `kNotFound` -> default). See `Texture2D.{h,cpp}` + T1/T2 in
  `SchemaEvolutionSmokeTest.cpp`.
- **Three output locations**:
  1. `Assets/<stem>.zasset` -- editor-platform variant (Standalone/BC7 on the DX12
     editor). Consumed by editor preview + scene materials. Path-derived GUID.
  2. `Intermediate/DDC/` -- LMDB cache of raw cook artifacts, keyed
     `(cache_type="Texture", asset_guid, hash(settings+platform+encoderVer))` via
     `Runtime::MakeDDCCacheKey`. Opened by `Runtime::GetDerivedDataCache()`.
  3. `Intermediate/Cooked/<Platform>/<rel>.zasset` -- per-platform cook output.
     Reuses the **source** GUID (so player builds resolve references), written via
     `AssetManager::WriteObjectToDiskWithGuid(path, obj, source_guid)` (the only
     explicit-GUID write API; everything else uses `DeterministicGuidFromPath`).
  Both `Intermediate/*` roots are gitignored; settings JSON
  (`AssetRegistry/texture_import_settings.json`) is the only VCS-checked artifact
  (the `.meta` replacement).
- **`MaterialRes`** carries `PPtr<Texture2D>` shadow fields beside each
  `m_*TextureFile` string (write both, read prefer PPtr via `Get*TextureFile()`,
  fall back to the path string -- same staged pattern as `m_shader_pptr`). PPtr
  nodes are appended last in `Transfer()` so old material `.zasset` read back null.
- **Consumption**: `RenderResourceBase::LoadTexture` tries the cooked
  `<source>.zasset` first (`TryLoadCookedTexture`), else falls back to the legacy
  `stb_image` source decode. **Block-compressed cooked variants are gated to DX12**
  -- the Vulkan `CreateGlobalImage` path here does not yet decode BC/ASTC, so on
  Vulkan the fallback path runs (uncompressed, GPU mipgen via `miplevels=0`). ASTC
  is never sampleable on the DX12 editor -> verify ASTC cooks by file inspection,
  not rendering.
- **Entry points**: `Build -> Cook Textures for <Platform>` (MenuController) and
  console `asset.cook <standalone|android|ios|ohos|webgl>` (EditorConsoleCommands).
  Startup `TextureImporter::ImportProjectTextures()` (in `EditorAssetManager::
  Initialize`) seeds the editor-platform `Assets/<stem>.zasset` for any source
  image lacking one (A2 first-time seeding, idempotent).
- **Encoders** (`bc7enc_rdo`, ARM `astc-encoder`) link PRIVATE into ZEditor only
  but compile on the host so the Windows editor can cross-encode mobile ASTC.

## 3. Build / test invocations

- Configure: pre-existing `build/` is fine; CMake presets in `CMakePresets.json`.
- Incremental Debug build:
  `cmake --build build --config Debug --target ZRuntime ZEditor`
- Editor-only script: `build_windows_editor.bat` (wraps `zbuild.py`, default
  target `ZEditor`). In Cursor/VSCode, `zbuild.py` clears `VSCODE_NLS_CONFIG`
  during configure/build so the `windows_visual_studio` preset is not disabled.
- Editor exe lives at `bin/Debug/ZEditor.exe` (NOT under `build/`).
- Demo project for smoke tests: `I:\ZEngineDemo\ZEngineDemo.zproject`.
  Launch with `-p <path-to-zproject>`.

## 4. Active multi-phase work

### TypeScript scripting integration (`doc/TYPESCRIPT_SCRIPTING_DESIGN.md`)

| Phase | Status | Summary |
|-------|--------|---------|
| P1 | done   | `ProjectInfo` scaffolding (Scripts/, Intermediate/, tsconfig.json, package.json, .gitignore). |
| P2 | done   | `ScriptAsset` + `ScriptRegistry` (path-deterministic GUID, content-hash rename detection, atomic JSON save) + Content Browser Scripts root. Smoke test on `I:\ZEngineDemo`: rename preserves GUID, delete removes entry, restart is idempotent. |
| P3 | done   | `TypeScriptCompiler` (Editor-only IEngineSystem). Spawns `tsc --watch` (project-local `node_modules/.bin/tsc.cmd` preferred, falls back to PATH; degraded mode if neither). Pipes tsc stdout/stderr line-by-line into ZTSC log category via a worker thread + per-frame `Tick()` drain. Generalised `FileSystemWatcher` to take a `setExtensionFilter()` (default still `{".zasset"}` for legacy callers); compiler watches `.js` under `Intermediate/Scripts/` and exposes `JsChangeHandler` hook for P4. |
| P4 | done   | Module loader API on `ScriptingManager` (folded into the existing system instead of a separate `ScriptingEngine` class - it already owned the env). `LoadModule/ReloadModule/UnloadModule/IsModuleLoaded/InvokeExportedFunction/PathToModuleId`, plus IIFE `(function(module,exports){...})` wrapper for QuickJS (no native `require`). `ScriptEnv::InstallConsoleAndDebugGlobals()` plumbs `console.{log,info,debug,warn,error}` and `Debug.{Log,LogWarning,LogError}` through native pesapi callbacks into the `ZScripting` log category. `EditorApplication` wires `TypeScriptCompiler::SetOnJsModuleChanged` -> `ReloadModule`/`LoadModule` with 200ms per-module debounce, and at startup scans `js_root/*.js` to load already-compiled modules so cold restarts aren't dependent on tsc re-emitting. Smoke test: `I:\ZEngineDemo\Scripts\P4Smoke.ts` (`console.log` + `Debug.Log` calls) appears in BqLog under `[ZScripting]` on both startup-load and hot-reload paths. |
| P5 | done   | `Behaviour` abstract Component + `TypeScriptComponent` subclass (serialises `m_script_guid` + `m_class_name` as `eastl::string`; `m_js_instance` stored as `void*` to keep pesapi out of the public header). Lifecycle mapped onto ZEngine's `Component` API: `postLoadResource` -> JS `OnAwake` + `OnStart`, `tick(dt)` -> `OnUpdate(dt)`, dtor -> `OnDestroy` then `pesapi_release_value_ref`. `ScriptEnv::CreateInstance/DestroyInstance/InvokeInstanceMethod*` work around pesapi's missing `new` operator via a JS-side `__zNewInstance(ctor)` shim, and `globalThis.Behaviour` placeholder class is injected by the same shim so user code can `class Foo extends Behaviour` without a real binding. Engine emits `Intermediate/Typings/zengine.d.ts` (declares `Behaviour`, `Debug`, `console`, `__zNewInstance`) on every project open in `ProjectInfo::ensureScriptsScaffold` -- tsc reports 0 errors. Inspector got a minimal "Add TypeScript Behaviour..." popup that lists `ScriptRegistry::getAll()` entries with non-empty `m_default_class_name`, attaches a `TypeScriptComponent` and explicitly fires `postLoadResource` (since `addComponent` doesn't). `EditorApplication::registerEdtorTickComponent("TypeScriptComponent")` makes `OnUpdate` fire in edit mode (Unity `[ExecuteAlways]` equiv). Smoke test: `I:\ZEngineDemo\Scripts\Hello.ts` extends `Behaviour`, throttled `OnUpdate` logs every 60 ticks; `module loaded: Hello` + `[Hello] OnAwake/OnStart` appear in BqLog. `m_serialized_fields` (per-instance script field overrides) deferred to P7. |
| P6 | done   | Hot reload of live `TypeScriptComponent` instances + Project-window double-click -> VSCode. `ScriptingManager` got a generic observer hook (`AddModuleReloadObserver`/`RemoveModuleReloadObserver`/`NotifyModuleReloaded`); `ReloadModule` fires it on success after `ScriptEnv::ReloadModule` finishes. Each `TypeScriptComponent` subscribes once at the end of `BindAndAwake` (so unbound components don't pay) and unsubscribes in `TearDown`/dtor; the callback filters by `m_module_id` and on a match just calls `BindAndAwake()` again -- which itself starts with `if (m_js_instance) TearDown();`, so OnDestroy(old) -> DestroyInstance -> CreateInstance(new) -> OnAwake -> OnStart all happen in one place. State is intentionally NOT preserved across reload (matches Unity MonoBehaviour); per-instance `m_serialized_fields` overrides deferred to P7. `NotifyModuleReloaded` snapshots the observer list under a mutex before iterating, so re-entrant subscribe/unsubscribe from inside a callback is safe. Cold-start batch `LoadModule` does NOT fire the observer (no components have bound yet). Added `EditorUtility::openInExternalEditor(path)` with Win/macOS impls: tries `ZENGINE_EXTERNAL_EDITOR` env override first, then `code` (Windows: `ShellExecuteW(L"open", L"code", ...)` resolves via PATH+PATHEXT to `code.cmd`; macOS: `open -a "Visual Studio Code"`), falling back to OS file association. `project_window.cpp` double-click on `.ts/.tsx/.js/.json` now routes here instead of `revealInFinder`. Smoke test on `I:\ZEngineDemo`: editing `Hello.ts` while editor runs produces `[ZTSC] Compiled module updated` -> `[ZScripting] hot-reloading module: Hello` -> `module unloaded: Hello` -> `module loaded: Hello` in BqLog within ~600ms, editor stays at 60 FPS. |
| P7 | done   | Per-instance script field overrides (`[SerializeField]` analogue). Storage: `std::vector<eastl::string> m_serialized_fields` flat alternating `[k0,v0,...]` (the only `SerializeTraits`-supported shape - no `pair<eastl::string,eastl::string>` path). `TypeScriptComponent` got `Get/Set/Remove/CountSerializedField`, `ApplySerializedFields` (called between `CreateInstance` and `OnAwake` in `BindAndAwake`, so values are reapplied on every cold-load AND every hot-reload), `LiveSetField` (Inspector edits write both to `m_serialized_fields` and live JS), and `EnumerateLiveFields` -> Inspector. Type coercion lives JS-side: `__zApplyField(instance, key, valueStr)` reads `typeof instance[key]` set by the field-initialiser and routes to `parseFloat`/`==='true'`/`String()` accordingly. `__zEnumerateFields` returns own-enumerable, non-function, non-`_`/`$`-prefixed properties packed as `name\u0001type\u0001value` records joined by `\u0002` (control chars never collide with JS identifiers, simpler than array marshalling). Inspector renders `InputDouble`/`Checkbox`/`InputText` per field with `EnterReturnsTrue` (no every-frame writeback); unbound components show stored overrides read-only. Hot-reload preservation (key user-visible difference vs P6 stateless rebuild): authoring data persists, runtime state still drops -- matches Unity Editor. Smoke: `I:\ZEngineDemo\Scripts\P7Smoke.ts` (`speed:number`, `enabled:boolean`, `label:string`, `_ticks:number`-private). Inspector lists 3 rows (filtered `_ticks`); edits round-trip through scene save/load AND through .ts hot-reload. Object/array fields out of scope (return false, log WARN). |

(P8 is intentionally absent from this table -- the
`TYPESCRIPT_SCRIPTING_DESIGN.md` plan ends at P7. The historical
"P5..P8" wording in `inspector_window.cpp:5404` is pre-merge and
predates the AssetRegistry-persistence work being split out into
its own track below.)

### Asset registry persistence (`doc/asset_management/AUTO_IMPORT_AND_FILE_WATCHER.md`)

This is an independent track from the TypeScript scripting integration
above; it was historically logged as "P9" alongside the TS phases but
shares no code with them and gets its own row here so the numbering
gap above is intentional, not lost work.

| Phase | Status | Summary |
|-------|--------|---------|
| P9 | done   | AssetRegistry persistence + live FileSystemWatcher hook-up. `saveCache`/`loadCache` write a versioned binary cache to `<Project>/Intermediate/asset_registry.cache` via tmp-file-rename (`'ZARC'` magic + u32 version + scan_root + N x [path,guid,asset_type,mtime,size]; **v2** appends `dep_owner_count` + per-owner `[path, dep_count, dep_paths...]` before `'ZARE'` tail; `m_ReferencerMap` rebuilt on load). v1 caches still load (no deps; lazy `IndexDependenciesLocked` on first query). Writes always emit v2. `EditorAssetManager::Initialize` now does `loadCache -> scanAssetsAsync` where the async scan SKIPS the per-file header read when an in-memory entry's `(mtime,size)` already matches the on-disk file (UE-style "incremental" semantics) -- warm restarts go from O(N header reads) to O(N filesystem stats). `Shutdown` calls `stopWatching -> waitForScanComplete -> saveCache` so the cache always reflects a fully-populated map. `FileSystemWatcher` callbacks finally wired (they had been allocated with `watchDirectory` but no `setOnFileXxx`!): `onFileCreated/Changed -> AssetRegistry::refreshAsset`, `onFileDeleted -> removeAsset`. The watcher event queue is drained on the main thread by `EditorAssetManager::TickWatcher()` slotted into `EditorApplication`'s per-frame tick (between `EditorInputManager` and `TypeScriptCompiler`). Path-key invariance: `refreshAsset/removeAsset` now normalise inputs to scan-root-relative form via a private `normaliseToRegistryKey` helper, so callers can pass either absolute (watcher path) or relative (saveAsset). `EditorFileService::computeAssetTypeLabel` got a fast path `computeAssetTypeLabelFromRegistry` that hits `AssetRegistry::getAssetIndex` before falling back to the `getAssetTypeName` header read; this is the third hot-path Inspector-rebuild walk that's now O(map hit) instead of O(N header reads). Cache version is v2; versions outside `[1,2]` are rejected (full rescan). The legacy `"legacy:"`-prefixed synthetic GUID for magic-mismatch fallback files (already added in phase B) round-trips through cache cleanly because the loader uses the same `upsertEntryLocked` invariants. |

### DX12 MainCamera path B (`doc/DX12_SUBPASS_RHI.md`)

Independent from the TypeScript / AssetRegistry tracks above. DX-B0
(subpass RHI emulation) through DX-B8 (RenderPipeline parity) are **done**,
including RP1 skybox callback, bindless tonemap between RP1/RP2, RP2
post/UI chain, DX12 global IBL/LUT init + GPU cubemap mipgen (see
section 2.11). No numbered code phases remain on this track; optional
RenderDoc checklist is in `doc/DX12_SUBPASS_RHI.md` (manual only).
Cross-backend bindless backlog (Metal / WebGL2 tonemap parity, etc.) is
in `doc/BINDLESS_TEXTURE_PATH.md`, not here.

## 5. Deferred backlog (not in section 4)

Section 4 tracks numbered phases only; the items below are intentional
follow-ups documented elsewhere. Do not treat their absence from section 4
as lost work.

| Area | Doc pointer | Notes |
|------|-------------|-------|
| ShaderRegistry downstream GUID chain | AGENTS.md 2.2 | Registry + `m_ShaderGuid` landed; full downstream GUID-ification still deferred (PR-SE3c). |
| ShaderLab hot path | AGENTS.md 2.2 | **DX12 variant (2026-05)**: `ShaderLabVariant` Cartesian `multi_compile`/`shader_feature`; `ShaderLabDx12Compiler` precompiles variants to `Intermediate/Shaders/` DXIL cache with Unity-style **shader_feature strip** (`GenerateVariantCombinationsBuildStrip` + per-project `MaterialRes::m_EnabledShaderKeywords` scan in `ShaderImporter`). `multi_compile` stays full Cartesian; unused `shader_feature` combos are skipped at precompile. `DX12ShaderCompiler` + `MaterialRes` keyword macros at draw time. Vulkan `ShaderLabCompiler` still uses full Cartesian (SPIR-V). |
| Bindless Metal / WebGL2 | `doc/BINDLESS_TEXTURE_PATH.md` | WebGL2 permanently excluded; Metal deferred. |
| Route A (texture) | `doc/BINDLESS_TEXTURE_PATH.md` PR9-11 backlog | Project-window source hiding and `AssetFile` stub cleanup are **done**; `Texture` base reflection, embedded source in header, and Inspector Reimport button for textures remain open. |
| Data Inspector | AGENTS.md 2.2 Data | Row add/remove, column reorder, `.xlsx` write-back out of V1 scope. |
| RenderDoc | `doc/DX12_SUBPASS_RHI.md` | Manual checklist only. |
| World Partition V3+ | `doc/world_partition/WORLD_PARTITION_DESIGN.md` | V1+V2 landed: grid streaming, async preload, level ref-count, render delete on unload, editor cell overlay. Data layers / HLOD / per-cell `.zasset` LevelRes still open. |
| LWC (Large World Coordinates) | `doc/world_partition/LARGE_WORLD_COORDINATES.md` | L1+L2 landed: `r.LWC.Enable`, 2^21 render tiles, `Vector3d` transform/hierarchy, schema read float / write double. L3 shader types + L4 per-cell origins open. |



