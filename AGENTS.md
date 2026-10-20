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

`gen_windows.bat` additionally accepts `--no-vulkan`, which forwards
`-DZENGINE_USE_VULKAN=OFF`. That produces a **DX12-only Windows build**: no
`VULKAN_SDK` env var / Vulkan SDK install is required (otherwise
`engine/CMakeLists.txt` FATAL_ERRORs), `Z_HAS_VULKAN` is not defined, and
`Runtime/CMakeLists.txt` strips the Vulkan / Metal / non-DX12-pass TUs while
`ShaderLab` drops `ShaderLabCompiler.cpp`. Use it for slim CI or on machines
without a Vulkan SDK. `--vulkan` is the (default) inverse. Run
`gen_windows.bat --help` for the full flag list.

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
  `WIN32`) is a plain `ZSlate::FSlateStandaloneApp` tool: shell owns window,
  DPI, pump, input router **and** the D3D11 render backend, so the exe is only
  the widget tree — a vertical box with an auto-height status bar over the
  shared `SInsightsTimeline` filling the rest (open via argv / drag-drop / `O`
  key, `F` to fit) — plus trace loading. It used to carry a bespoke Direct2D
  host (`D2DUIRenderer`); that bought DirectWrite subpixel text and per-label
  clipping, but the shell's glyph atlas already bakes at the *scaled* integer
  pixel size (`FSlateDpiScaleRenderer` scales `font_size` before the atlas), so
  the sharpness argument did not survive — and the in-editor panel paints the
  same widget through the engine's ZSlate renderer anyway. It links the
  **static `ZRuntime`** (same pattern as `LuaMemPreview`, which also absorbed the former
  `ZProfilerCsvExporter` as `LuaMemPreview -batchmode <in.zasset> [out_base]`) but
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
  track + hover + `.ztrace` save/load + standalone ZSlate/D3D11 viewer. No
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

### 2.18 ZSlate has exactly one copy

- The only ZSlate source tree is the submodule `engine/Libraries/ZSlate`
  (`github.com/zentia/ZSlate`). Standalone tools (e.g.
  `engine/tools/ClientProjBranches`) consume it with
  `add_subdirectory(<repo>/engine/Libraries/ZSlate ...)` + link `ZSlate`;
  never fork a private copy under `engine/tools/`. Missing widgets go into the
  submodule (commit there, then bump the pointer).
- Layout contract: a widget never asks to fill; the parent slot decides
  (`SBoxPanel::AddSlot(w).Fill()`, UE `SBoxPanel` semantics).
- Modal / popup layers: `SlateApplication::ShowModal` (used by `SDialog`)
  and `SlateInputRouter::OpenPopup` (used by `SComboBox` dropdowns). Hosts
  must route mouse to `SlateApplication::GetInputRoot()` and paint through
  `SlateApplication::PaintInto`, which draws modal + popup on top.
- **Diagnostics go out through a sink, not through `fprintf`**: `ZSlate::Log()`
  (`ZSlate/Core/SlateLog.h`) formats and dispatches to a process-wide
  `FSlateLogSink`; the default sink is the old stderr/stdout pair, so a host
  that installs nothing is unaffected. Hosts that own a real logging stack
  install one (`ZSlate::SetLogSink`) -- ZEnvironmentCollector's GUI routes every
  ZSlate line into 扁鹊, because a WINDOWS-subsystem process has no stderr
  anyone can read. Do not add direct `fprintf`/`printf` to ZSlate sources.

### 2.19 Localization: mechanism in ZSlate, strings in the tool

New UI text goes into the source **in English**, tagged with an id:
`ZSlate::Loc("collection.start", "Start collection and build ZIP")` (the GUI
wraps it in a short `S(id, english)` helper). Translations are tables the host
registers at start-up -- ZSlate ships none of its own. Adding a language is a
new `ELanguage` value plus a table; adding a string is one `Loc()` call plus one
row in the table.

- A missing entry falls back to the English literal and logs the id once through
  ZSlate's sink, so a typo shows up in the log instead of as a blank label.
- Strings resolve when a widget is **built**, so switching language rebuilds the
  tree (`ShellApp::RebuildUi`, which carries the form's contents across).
- The collector's table and the remembered choice live in `UiLocalization.cpp`
  (registry `HKCU\Software\ZEngine\EnvironmentCollector\Language`); that file is
  deliberately ZSlate-free because the CLI-only build links it too.
- **Report file content (TXT/JSON) stays Chinese regardless of the UI language.**
  Those files go to support, not to the player.

### 2.20 CRT mode: /MT tools inside a /MD tree

`MSVC_RUNTIME_LIBRARY` is baked into each object file, so a /MT executable
linking a /MD static library fails with LNK2038 ("RuntimeLibrary" mismatch)
**and** LNK2019 on the library's `__imp_*` symbols (`__imp_strncat`,
`__imp__wstat64`, ...). A tool directory that forces
`CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded` must therefore either build every
library it links itself, or inherit the tree's CRT when it is configured as part
of the tree (where `minizip-ng` / `BqLog` / `zlib-ng` already exist as /MD).
`engine/tools/ZEnvironmentCollector/CMakeLists.txt` does exactly that via
`ENVIRONMENT_CHECK_STANDALONE`: standalone (its own `cmake -S`, everything
local) -> /MT, which is what ships; inside the tree -> inherit. Do not "fix" a
link error here by forcing /MT unconditionally.

`-DZENGINE_STATIC_CRT=ON` (root `CMakeLists.txt`) flips the **whole tree** to
/MT, `ZRuntimeShared.dll` included. That is the switch that lets a tool which
links the engine DLL still ship to a machine with no VC redist. It has to be
whole-tree: one DLL cannot mix CRTs against the static libraries it links.
Crossing into the DLL is safe regardless, because the tool-facing API is
`extern "C"` only and never moves an allocator-owned object across the boundary.

### 2.21 Audio: three layers, one owner, headless by default

Audio landed as a vertical slice (`Runtime/Function/Audio/`, PR-A1). Keep to
this layering when extending it:

| Layer | Type | Rule |
|---|---|---|
| DSP | `AudioCommon.h`, `AudioDecoder`, `AudioMixer` | **No engine headers**: no LogSystem, no eastl, no engine math (`AudioVec3` exists for this reason). This is what lets `AudioMixerSmokeTest` compile and run standalone in CI. |
| Device | `IAudioDevice` + `AudioDeviceWasapi` / `AudioDeviceNull` | Platform-only. Owns its render thread and pulls frames through a callback; never touches clips or voices. |
| Engine | `AudioSystem` (an `IEngineSystem`, Gameplay phase) | Owns the clip cache, the mixer and the device. Components talk to it, never to the mixer directly. |

Invariants that are easy to break:

1. **Clips are cached for the process lifetime.** A voice stores a raw
   `const AudioClipData*`; evicting a clip while a voice plays is a
   use-after-free. `UnloadAllClips()` must only run after `StopAll()`.
2. **`ZENGINE_AUDIO_DISABLE=1` must keep every API working** with a null
   device, and `AudioSystem::Initialize()` must never return false because of a
   device failure. Headless CI, `-batchmode` and the standalone tools all
   depend on this; a failed `Initialize()` aborts the whole startup sequence.
3. **Mixer API is thread-safe; `Render()` runs on the device thread.** Every
   public mixer entry point takes `m_Mutex`. Keep the critical section free of
   allocation / I/O / logging -- that assumption is why a plain mutex is safe
   instead of a lock-free command queue.
4. **V1 is WAV only.** `DecodeAudioBuffer` reports `.ogg` / `.mp3` as
   unsupported instead of silently playing nothing. Compressed formats need a
   vendored decoder (stb_vorbis / dr_mp3) added to `engine/3rdparty/` first.
5. **Log category is `ZAudio`** (already added to
   `bq_log_category_config.ini` + regenerated `engine_log.h`).
6. Windows links `Mmdevapi` for WASAPI (see the `WIN32` block in
   `Runtime/CMakeLists.txt`). Other platforms get the null device; adding
   CoreAudio / AAudio means a new `AudioDevice*.cpp` plus a factory branch.

Known gaps (intentionally out of the first slice): streaming (clips are fully
decoded in memory), compressed formats, DSP effects / reverb sends, doppler,
OGG/MP3 import, an audio mixer editor window, and an audio importer in
`Editor/AssetPipeline/`.

### 2.22 Animation & skinning: conventions and current state

PR-SK1 landed the skeletal-skinning foundation
(`engine/Source/Runtime/Function/Animation/Skinning.{h,cpp}` + the DX12 GPU
path). Conventions that are easy to get wrong, in the order they bite:

1. **The GPU joint palette has an extra identity slot in front.** Slot 0 is the
   root; bone `i` lives at slot `i+1`; the shaders test `index > 0` and skip a
   zero index. `Skeleton::OutputAnimationResult()` writes `GetID() + 1`, and
   `AnimationSkinning::BuildGpuPaletteWithRootSlot()` is the CPU-side helper.
   Both `mesh.vert` (Vulkan) and `mesh.vert.hlsl` (DX12) rely on this.
2. **Clip channels are DELTAS on the bind pose, not absolute local transforms.**
   `Skeleton::ApplyAnimation()` resets to the bind pose every frame, then
   composes each sampled pose with `AnimationSkinning::ComposeDelta()`
   (position adds, rotation pre-multiplies, scale multiplies). Sampling returns
   absolute poses; never skip the compose step.
3. **Never trust `RawBone::tpose_matrix`.** Nothing in the codebase writes it
   (grep it), so the inverse bind matrices are always derived from
   `binding_pose` through the hierarchy
   (`AnimationSkinning::ComputeInverseBindMatrices()`). Trusting
   `tpose_matrix` silently renders the mesh unskinned.
4. **Skeletons are topologically sorted at build time.**
   `Skeleton::BuildSkeleton()` copies the asset's `bones_map` and runs
   `EnsureTopologicalOrder()`; it never mutates the shared cached `SkeletonData`
   and no longer bails out silently on unordered input (that failure mode used
   to show up as "the character does not animate at all" with no log line).
5. **DX12 storage-buffer SRVs need an explicit stride.**
   `RHIDescriptorBufferInfo::stride` was added for this: DX12 used to guess
   "80 bytes when the range divides by 80", which mis-sizes the 32-byte
   joint-binding buffer whenever the vertex count is a multiple of 5. Set it
   for every new structured buffer (joint palette = 64, RenderMeshInstance =
   80, joint binding = 32).
6. **Log category is `ZAnimation`.**

### 2.23 FBX import: what dropping a file produces (PR-SK2)

`AssetImportManager::FindImporter()` dispatches on **extension only**, so
`.fbx` is handled by `MeshImporter` alone -- a separate animation importer can
never be reached. One dropped `.fbx` therefore produces everything, Unity's
ModelImporter style:

```
Assets/Models/hero.fbx
  -> Assets/Models/hero.zasset                    MeshData (geometry + bind[])
  -> Assets/Models/hero.skeleton.zasset           SkeletonData   (bones, if any)
  -> Assets/Models/hero.anim.zasset               AnimationAsset (one per stack)
  -> Assets/Models/hero.animskelmap.zasset        AnimSkelMap    (channel -> bone)
```

Rules that keep the two importers from drifting apart:

1. **Bone numbering lives in `Editor/AssetPipeline/FbxSkeleton.h` and
   `FbxAnimation.h`** (header-only, guarded by `ZENGINE_HAS_UFBX`). The mesh
   importer uses it for `MeshData::bind`; the animation importer uses it for
   `AnimSkelMap::convert`. Never rebuild the bone list locally -- if the two
   disagree the character animates the wrong bones and nothing warns you.
2. **`MeshData::bind` stores GPU palette slots (bone + 1)**, matching
   `AnimationLoader::addBoneBind()` and the `index > 0` test in both mesh
   shaders. Build them with
   `AnimationSkinning::PackVertexInfluence()` (top-4 + renormalise).
3. **Skinned meshes keep their authored coordinates.** `RecenterMeshAtOrigin()`
   and `ApplyAxisFlip()` are skipped when the file has bones -- moving vertices
   without moving the skeleton slides the mesh out of its own bind pose.
4. **Clip channels are baked DELTAS** (`FbxAnimation::BakeStack()`), sampled at
   the file's own FPS unless `AnimationImporterSettings::sample_rate` overrides
   it; `max_frames` + a 10k hard cap bound asset size.
5. `AnimationAsset::skeleton_file_path` is intentionally left empty at import
   time; it is filled when a clip is wired to a mesh's skeleton in the editor.

### 2.25 Animator: state machine (PR-SK3)

`AnimationComponent` used to mix a fixed clip list at a static weight, always
looping, with no speed and no events -- a character could not react to gameplay.
The state machine adds that, Unity's AnimatorController shape adapted to what
the engine already has:

```
AnimatorController  (asset: states + parameters + transitions + events)
        |
AnimatorInstance    (runtime: parameter values, per-state playhead, active fade)
        |  GetBlendState()  ->  at most 2 clips with weights (1-w, w)
        v
AnimationSystem::GetBlendStateWithClipData()  ->  Skeleton::ApplyAnimation()
```

Rules that are easy to break:

1. **The evaluator does not sample poses.** `AnimatorInstance` emits a
   `BlendState` and hands it to the existing mixer, so there is exactly one
   sampler in the engine. Never add a second sampling path.
2. **A state owns ONE clip, addressed by PATH** (`.zasset`), same as the
   existing `BlendState`. Not a `PPtr` -- PPtr round-tripping is still being
   rolled out (PR-SE3a).
3. **`AnimatorState::clip_length` is in SECONDS and is required.**
   `AnimationClip` only stores `total_frame` and the bake FPS is not persisted,
   so seconds cannot be derived. `<= 0` means "hold at frame 0".
4. **Conditions are evaluated AFTER candidates are collected and sorted by
   priority**, because a satisfied Trigger condition CONSUMES the trigger.
   Evaluating every transition first would let a losing one eat it.
5. **Transitions are not re-evaluated during a fade** (no interruption source,
   matching Unity's default). `Play()` / `CrossFade()` are the escape hatch.
6. **Enums are persisted as `int`** -- the serializer has no enum trait. Cast at
   the boundary; do not "fix" this by adding enum support to the serializer.
7. **Events fire once per pass**; the per-state fired flags reset when the
   playhead wraps or is seeked backwards.
8. `AnimationComponentRes::controller_file_path` is OPTIONAL and empty in every
   pre-existing asset: empty == legacy blend-state playback. This is a
   SafeBinaryRead schema evolution (PR-SE3a pattern) -- do not make it
   mandatory.

**Authoring loop**: `ZSlateAnimatorWindow` (dock title "Animator", Window menu,
category Animation) is the authoring surface -- parameters / states /
transitions as three editable tables, plus New / Open / Save against a
`<Project>/Assets/<name>.controller.zasset` path. A controller asset in the
Content Browser also offers "Open in Animator" on right-click
(`ContentBrowserHelpers::IsAnimatorControllerNode` matches the
`*.controller.zasset` suffix). Attaching it to a character is a plain string
field: `AnimationComponentRes::controller_file_path` shows up in the Inspector
through the generic TypeTree drawer (it is an `eastl::string`, which maps to
`FieldKind::Str`) -- type the path there.

Editing rules for the panel (easy to break):

1. **Transitions address states BY INDEX**; conditions address parameters BY
   NAME. Hence `RemoveState` must drop every transition touching the removed
   state AND renumber the rest, and `RemoveParameter` must drop conditions that
   named it (they would otherwise never fire).
2. **Text commits must not rebuild the chrome tree** -- a rebuild replaces the
   `SEditableTextBox` and drops keyboard focus. Only structural edits / enum
   cycles set `m_UiDirty`.
3. Numeric fields are `SEditableTextBox` + tolerant parse: a malformed value
   keeps the previous one instead of writing NaN into the asset.

**Graph view** (the default tab): one hand-painted node per state, arrows for
transitions.
- Node positions live on the asset (`AnimatorState::pos_x` / `pos_y`) in
  LOGICAL units, multiplied by the UI scale at paint time, so a layout survives
  a DPI change. They were added after the fact, so a controller written before
  the graph existed has every state at (0,0) -- the window treats "all zero" as
  "not laid out yet" and auto-arranges on load.
- Gestures: drag a node body to move it, drag its **bottom port** onto another
  node to create a transition (duplicates are rejected; the new arrow is
  selected immediately). Click a node or an arrow to select it (mutually
  exclusive -- picking one clears the other); clicking empty canvas clears both.
  **Right-click an arrow deletes it** (the host has no `WasRightPressedThisFrame`,
  so the edge is latched from `IsRightDown()` into `m_RightWasDown`).
- The selected arrow opens an editing **strip under the canvas** with that
  transition's own rows: from/to, fade, exit time, priority, conditions. It
  reuses `BuildTransitionBody`, the same function the Transitions tab calls per
  row, so the two editors cannot drift apart.
- The selected **node** opens the same kind of strip (name, clip path, anim skel
  map, wrap cycler, speed, length, events, plus `Set Default` / `Delete` /
  `Deselect`), reusing `BuildStateBody` for the same reason. `AddState` selects
  the new node immediately -- an unselected node has nowhere to type its clip.
- `Delete` in the state strip is a button, NOT a right-click gesture: removing a
  state renumbers every transition that referenced it.
- **`Browse`** next to the clip and anim-skel-map fields opens a picker over the
  right pane. It browses the **Content Browser's own `EditorFileService` tree**
  (via `ZSlateContentBrowserWindow::GetInstance()->GetFileService()`), filtered
  to `m_AssetType == "animationasset"` (clip) / `"animskelmap"` (map). Two rules
  matter:
  1. It must **never call `BuildEngineFileTree()` itself** -- that clears the
     node array the browser holds raw pointers into (`m_SelectedNode`,
     `m_BrowsedFolderNode`) and leaves them dangling. Rebuilding is the
     browser's job (it rebinds its selection afterwards). While the tree is
     empty we scan the content root with `std::filesystem` instead.
  2. Picked paths are stored **content-root-relative** (`ToContentRelative`),
     the form `AssetManager::loadAsset` resolves, so a controller survives the
     project moving.
- The picker's filter box does not rebuild the tree on commit (that would drop
  keyboard focus); rows are filtered every frame by toggling
  `SWidget::Visibility` between `Visible` and `Collapsed`, which `SBoxPanel` and
  `SScrollBox` both honour in layout.
- **Canvas is also a drop target**: drag a `.zasset` out of the Content Browser
  onto a node -> `animationasset` becomes its clip, `animskelmap` its map; onto
  blank canvas an `animatorcontroller` opens. The hovered node is outlined cyan
  and a cursor-following label says what will land where (`-> clip` /
  `(unsupported here)`). While a drag is in flight the canvas' own gestures are
  off, so a drop can never also move the node.
- The type is read with `AssetManager::GetAssetTypeName` and normalized by
  `NormalizeAssetType` (lowercase, trailing "Res" stripped), because the ZAsset
  header sometimes names the Resource wrapper class.

### 2.26 Cross-window drag-drop (Content Browser -> canvas / Inspector)

- The **source side already existed**: Content Browser asset rows/tiles emit
  `FAssetDragDropOp` (`PayloadType = "AssetPath"`, `AssetPath` = ABSOLUTE path)
  through `ZSlate::SetActiveDragOperation`. Do not write a second payload type.
- `SlateInputRouter::ProcessMouse` already dispatches cross-window: it uses
  `m_DragOp ? m_DragOp : GetActiveDragOperation()`, so a window that did not
  start the gesture still delivers `OnDragOver` / `OnDrop` to **its own** widget
  tree. Widget drop targets use `SDropTarget` (`CanAcceptDrop` / `OnDropHandler`).
- **Two consumers exist for a reason — a hand-painted region cannot use
  `SDropTarget`.** The Animator canvas is painted with `ISlateRenderer`, so it
  has no widget to receive `OnDrop`; it caches the op and acts on its own
  release edge (the `ZSlateSceneWindow` pattern). The Inspector's Mesh /
  Material rows are widgets, so they use `SDropTarget`.
- **Race (this is why the router was patched)**: the publishing router clears
  the process-wide channel on its release frame, and window draw order decides
  whether the target ticks before or after that. `SlateInputRouter` therefore
  keeps `m_LastObservedDragOp` and delivers the drop from it; consumers of the
  global channel cache their own copy for the same reason. Detection must key on
  `IsLeftDown() || WasLeftReleasedThisFrame()` -- on the release frame
  `IsLeftDown()` is already false.
- **Path form**: a drag carries an ABSOLUTE path, but serialized fields store
  content-root-relative (what `InspectorAssetCommon::ResolveProjectAssetPath`
  resolves). All drop targets convert through
  `EditorDragDrop::MakeDroppedAssetStoredPath` so they cannot disagree.
- Inspector drops write the path into the text box and then call that box's own
  `OnTextCommitted`: dropping goes through exactly the same code path as typing,
  so validation and the read-only info labels cannot drift.

### 2.27 Animator as a drag SOURCE (clip -> Inspector component field)

- The **clip line inside a node** is the drag-out handle (`ClipChipRect`, shared
  by painting and hit-testing). Press there and move >5px -> the canvas publishes
  an `FAssetDragDropOp`. A press without travel still just selects the node.
- **The source must unpublish the op itself.** Normally the `SlateInputRouter`
  that started the gesture clears the channel; the canvas is hand-painted and has
  no widget (no `OnDragDetected`, no router), so `EndAssetDragOut()` owns it.
  Forget this and the channel stays hot -- every window then thinks a drag is in
  flight forever.
- Payloads carry ABSOLUTE paths; the controller stores content-root-relative, so
  the path is resolved on publish (same root as
  `MakeDroppedAssetStoredPath` uses in reverse).
- States with an empty `clip_file_path` have no handle: there is nothing to drag.
- **AnimationComponent now has a dedicated Inspector section** (Controller /
  Skeleton / Clip / Anim Skel Map / Clip Length). Without it the generic TypeTree
  walk renders `blend_state.blend_clip_file_path` as `<array> (not editable)` --
  exactly the slot a dragged clip has to land in.
- The slot accessors live on `AnimationComponent`, not in the Inspector: a
  `BlendState` is a set of PARALLEL vectors, a clip only counts as usable when
  its bone map is present, and `Tick()` bails out when length / ratio are missing
  (a "clip assigned" character would stand perfectly still). The component owns
  that invariant; callers must not hand-roll the vectors.
- Clip length is authored, not derived: a clip stores frames and the bake FPS is
  not persisted (see 2.25).

### 2.28 Any asset path row is a two-way slot (generalized drag-out)

- **Two kinds of drag SOURCE, and they are not interchangeable:**
  - *Widget rows* answer `SWidget::OnDragDetected`; the router that saw the press
    publishes the op AND clears it on release. Use `SDragSource` (the mirror of
    `SDropTarget`) via `EditorDragDrop::MakeAssetPathDragSource`.
  - *Hand-painted regions* (the Animator canvas) have no widget, so they publish
    with `SetActiveDragOperation` and **must unpublish themselves** -- see 2.27.
- `EditorDragDrop::MakeAssetDragOperation` is the only way to build the op:
  stored (content-root-relative) path -> ABSOLUTE payload path, and an empty path
  returns null so no gesture starts (otherwise the user drags "nothing" and every
  target lights up for a payload it cannot use). Its inverse is
  `MakeDroppedAssetStoredPath`; `MakeDroppedAssetAbsolutePath` is the other half.
- `MakeAssetPathDragSource` polls the value at drag start, so a path typed a
  second ago is the one that travels.
- Animator nodes now show **two** asset lines (clip, anim->skeleton map) and both
  are handles; `kNodeH` grew 56 -> 74 to fit them. `AssetSlotPath` /
  `AssetChipRect` are shared by painting, hit-testing and drag-out, so a handle
  can never carry something other than what it shows.
- The States tab and the in-canvas strip share `BuildStateBody`, so both got
  drag-out for free. Prefer that: one row builder, two call sites, no drift.
- Inspector path rows (`MakeAssetSlot`) are now **two-way**: drop target in, drag
  source out, on the same row.
- Caveat: `OnDragDetected` is polled over the whole press path, so a drag inside a
  focused text box beats selecting its text. Fine for path rows, wrong for prose.

### 2.29 Content Browser as a drop TARGET (move / copy)

- Three drop sites, one policy: **folder rows, asset tiles, and the list
  background** all route through `CanAcceptAssetDrop` / `HandleAssetDrop`. Rows
  and tiles are deeper than the background catch-all, and `OnDragOver` bubbles
  deepest -> root, so the specific row wins and the background only sees blank
  space. Never duplicate the policy per site.
- Ctrl decides move-vs-copy and is read at **drop** time (not at drag start): the
  user makes up their mind mid-drag, and `CanAcceptAssetDrop` is polled every
  frame so the highlight follows the modifier.
- The actual work lives in `ContentBrowserDragDrop::CanDropAssetPath` /
  `DropAssetPath`, and it is registry-aware:
  - a registry-indexed `.zasset` goes through `EditorAssetManager::MoveAsset`,
    which re-keys the registry + dependency graph and rolls the file back if the
    registry update fails. A bare `std::filesystem::rename` would leave the
    registry pointing at the old path.
  - everything else is a plain rename, falling back to copy-then-delete because
    `rename()` cannot cross volumes.
- **Copying a registry `.zasset` re-writes it, it is NOT a byte copy** (see 2.30):
  the GUID is derived from the file's absolute path and stamped into the binary
  header, so a byte copy would carry the SOURCE's GUID to the destination -- two
  files, one GUID, every dependency edge ambiguous. The copy goes through
  ReadObjects* -> WriteObjectsToDiskThreadSafe so it gets its own GUID.
  Non-registry files (shaders, json, ...) copy byte-for-byte, and Ctrl+drag in
  place is the duplicate gesture (`"name 1"`).
- A folder cannot be moved into itself or its own subtree; a move back into the
  folder it already lives in is a no-op and does not light the row up.
- After a successful drop: `RefreshContentBrowserSelection` (rebuild + select the
  new path) and `asset_tree_dirty = true` so the window rebuilds its widget tree.

### 2.30 Duplicating an asset = read the graph, write it out again

- **The identity that must change is the GUID, not the local file id.** A binary
  `.zasset` stamps `DeterministicGuidFromPath(path)` into its header and YAML
  authoring assets derive their GUID the same way, which is exactly why a byte
  copy is broken. Local ids are therefore kept as read: in-file `PPtr`s address
  objects by local id, so renumbering would need a full remap pass and buys
  nothing.
- The copy is safe because the write session calls `RegisterLocalObject` for every
  object it writes *before* any `PPtr` transfers: the graph is embedded in the
  copy, not turned into external references pointing back at the source.
  (The writer hook only fires for instances that are NOT in the write set.)
- Reader/writer pair is chosen by `AssetManager::IsYamlAuthoringAssetPath`
  (`.mat` / `.prefab` / `.scene` are YAML, imported `.zasset` is binary);
  `WriteObjectsToDiskThreadSafe` routes both.
- **Folder copies recurse file-by-file** through the same rule -- a recursive
  `std::filesystem::copy` would byte-copy every `.zasset` inside and produce a
  folder full of duplicate GUIDs.
- Call `EditorAssetManager::RefreshAsset` on the copy: the directory watcher would
  only pick it up on a later tick, and until then it is in no picker.
- `MoveAsset` declines when the destination is outside the registry (e.g. into
  Scripts/) and rolls back before returning false, so falling through to a plain
  rename is safe -- do not report that as a failure.
- Arrows are hit-tested as **fat segments** (point-to-segment distance), not
  rects, since they run at any angle; nodes are tested before arrows so an arrow
  passing under a node cannot steal its clicks. Painting and hit-testing both go
  through `GetTransitionEndpoints`, so they cannot disagree.
- A self-transition (A -> A) has no room for a real arrow: it is drawn as a short
  stub just under the node so it stays visible and clickable.
- Selecting anything rebuilds the chrome (the strip appears/disappears), so only
  a *changed* selection may set `m_UiDirty` -- see rule 2 below.
- The canvas is hand-painted into the rect a **placeholder `SBox`** was
  arranged into (`m_GraphSlot->GetCachedGeometry()`), so the painted region can
  never drift from the widget layout. Do not replace it with analytic maths.
- There is no `DrawLine`: an arrow is a 4-point `DrawConvexPoly` shaft plus a
  3-point head. Note `UIRenderer::DrawConvexPoly` takes the ENGINE `Vector2`,
  which is a different type from `ZSlate::Vector2` -- the graph paints through
  `ZSlate::ISlateRenderer` (which also has `DrawTextLabel`, `UIRenderer` does
  not).
- Node drags deliberately do NOT set `m_UiDirty`: repainting happens every
  frame, and a rebuild would drop a text box's keyboard focus.
- No pan / zoom yet; positions are clamped to the visible canvas.

Known gaps: no layers / blend trees / sub-state machines, no graph pan/zoom, the
Content Browser does not accept drops yet (a path cannot be dragged *into* it),
and no live preview of the driven character's parameters while editing.

### 2.29 Undo/Redo: two layers, and you record BEFORE you edit

`engine/Source/Editor/Undo/` (`doc/UNDO_SYSTEM.md`).

- **Two layers, one owner.** `UndoSystem.{h,cpp}` is the transaction stack and
  depends on **nothing but the standard library** -- that is deliberate, it is
  what makes `UndoSystemSmokeTest` run headless. Everything that knows about
  GameObjects / selection / the level lives in `EditorUndo.{h,cpp}`
  (`EditorUndoSystem`, an `IEngineSystem` in the PostInit phase, the single
  owner of one `UndoSystem`) and in `EditorUndoRecords.{h,cpp}`. Do not leak
  engine types into `UndoSystem.h`.
- **Records are swap-based.** `IUndoRecord::Apply()` exchanges the state it
  stores with the live state and must be its own inverse, so one implementation
  serves both undo and redo and never needs to know the direction. UE's
  `FTransaction` model. `Finalize()` runs once when the transaction closes and
  returns false for "nothing changed" -- the record is then dropped, so
  committing the same value never pollutes the history.
- **Record BEFORE the mutation, never after.** Call sites use the
  `EditorUndo::` facade: `RecordComponent` / `RecordTransform` /
  `RegisterCreatedGameObject` / `DestroyGameObjects`, optionally wrapped in
  `EditorUndo::ScopedTransaction("Title")` (RAII, has `Cancel()`). Without a
  scope, everything one event handler records lands in the *implicit*
  transaction that `EditorUndoSystem::Tick()` flushes once per frame -- Unity's
  per-event undo group.
- **Resolve targets by InstanceID, never by raw pointer.** Records go through
  `ObjectManager::IDToPointer`; between undo and redo the object is very likely
  at a different address. `GameObjectLifetimeRecord` therefore *owns* the
  removed subtree, so undo restores the same objects with the same IDs (and
  every `PPtr` to them stays valid) instead of clones.
- **Component snapshots go through the normal `Transfer()` path** into a YAML
  **DOM** (not emitted text) with `ScopedPPtrResolver(nullptr)` pushed, which
  makes `PPtr` / `ImmediatePtr` serialise raw instance IDs. That is why undo is
  bit-exact for floats and why anything that survives a scene save/load
  survives undo. A `Transform` is the exception: it is routed to
  `RecordTransform`, because a raw snapshot would overwrite its hierarchy links.
- **Deleting objects must go through `EditorUndo::DestroyGameObjects`,** not
  `Level::DeleteGObjectByID` directly -- otherwise there is no undo step (and no
  `CancelDeleteGameObject` on the render swap).
- **A long drag is one step.** The gizmo opens the transaction lazily on first
  move and `EditorSceneManager::EndGizmoUndo()` closes it when the mouse is
  released; `RecordTransform` coalesces by instance ID inside the open
  transaction, so a 300-frame drag is still one Ctrl+Z.
- **Not recorded:** play mode (recording is suspended, matching Unity/UE),
  asset edits (Inspectors write straight to disk), component add/remove (no
  editor UI for it yet).
- **New editor entry points must add a `Record*` call** before they write.
  Current ones: gizmo `MoveEntity`, `OnDeleteSelectedGObject`,
  `ZSlateInspectorWindow` (every field commit), `CreateEmpty`,
  `EditorHierarchyReparent`, `EditorScenePlacement`.
- Entry points for the user: Ctrl/Cmd+Z, Ctrl+Y, Ctrl+Shift+Z
  (`EditorInputManager::OnKey`), the Edit menu (`MenuController`), and the
  console (`undo [steps]` / `redo [steps]` / `undo.history [rows]` /
  `undo.clear`). New `ZUndo` log category -- see 2.5.

### 2.31 RenderGraph: three phases, and the compiler must stay RHI-free

`engine/Source/Runtime/Function/Render/Graph/` (`doc/RENDER_GRAPH.md`).

- **Setup / Compile / Execute.** Setup (`RenderGraphBuilder`) only *declares* reads and
  writes -- no RHI call, no allocation; it is cheap enough to run every frame. Compile
  is pure CPU (topological sort, culling, lifetimes, memory aliasing, barriers,
  validation) and deterministic. Execute (`RenderGraphExecutor`) resolves handles to
  real resources and records commands.
- **The compiler depends on nothing but the standard library.** `RenderGraphTypes.h`
  forward-declares `RHIImage` / `RHIImageView` / `RHIBuffer` / `RHICommandBuffer` as
  opaque pointers and mirrors the enums as `RGFormat` / `RGTextureUsage` /
  `RGResourceState`. Do **not** include `Interface/RHI.h` in `RenderGraphTypes.h`,
  `RenderGraphBuilder.*` or `RenderGraph.*` -- that is what lets
  `RenderGraphSmokeTest` run headless. `RenderGraphResources.*` is the one place that
  knows the RHI.
- **Declare before you touch.** A pass's setup callback declares the "before" state;
  the mutation happens in the execute callback. Same rule as the editor undo system
  (2.29).
- **Roots are what survive culling:** a pass is kept if it writes an imported
  (external) resource, writes an extracted resource, or carries
  `RGPassFlags::NeverCull`. Everything else that nobody consumes disappears. A pass
  that only reads and writes nothing *is* culled -- that is correct, not a bug.
- **A resource read and written by the same pass is rejected** (no RW textures), as is
  declaring one with two different states. Both surface as `RGCompiledGraph::errors`,
  never as UB.
- **Never rely on pipeline stage masks for correctness.** DX12's
  `CmdPipelineBarrier` drops `srcStageMask` / `dstStageMask` entirely and degrades
  every buffer/UAV barrier to `D3D12_RESOURCE_BARRIER_TYPE_UAV`
  (`DX12RHI.cpp:5346`). The stage in `RGMapState` is a hint for Vulkan only.
- **RHI destroy/free take `T*&`** (they null the caller's pointer), so values copied
  out of a `const` reference must go through a mutable local first.
- **Not adopted by the render pipeline yet.** `DesktopRenderPipelineModule` and
  `MainCameraPass::Draw(...)` are untouched. The prepared seams, lowest risk first:
  `RenderPassBase::AppendToDrawList` (a no-op hook that already exists), splitting the
  post passes out of `MainCameraFramebufferResources`, and deleting the six hand-written
  `UpdateAfterFramebufferRecreate` calls in `DesktopRenderPipelineModule.cpp:480`.
  Converting the pipeline needs per-frame visual verification -- do not attempt it
  headless.
- Test: `-DZENGINE_BUILD_RENDERGRAPH_SMOKE_TEST=ON` -> `RenderGraphSmokeTest`
  (78 checks). Its `main()` is carved out of the ZRuntime glob in
  `Runtime/CMakeLists.txt`; the compiler files themselves are part of ZRuntime.

### 2.24 Packaging: `.zbundle` + manifest (PR-PAK1)

There was no pak, no manifest and no dependency pruning, so "shipping" meant
copying `Assets/` whole. `AssetBundle.h` was a 16-line stub whose `GetImpl()`
body was `return nullptr`. Now:

```
<project>/Build/<Platform>/
├── <project>.manifest.zasset   AssetBundleManifest (bundles + deps)
└── Bundles/<name>.zbundle      archive of cooked .zasset blobs
```

Rules that are easy to break:

1. **A bundle stores cooked `.zasset` FILE BYTES, not re-serialised objects.**
   There is exactly one binary writer/reader in the engine (`SerializedFile`);
   inventing a second one for bundles would guarantee drift. Assets are
   addressed by REGISTRY-RELATIVE path with forward slashes
   (`"Models/hero.zasset"`) -- the same key `AssetRegistry` uses.
2. **Bundle serving hangs off ONE hook: `SerializedFile::InitializeRead()`.**
   It asks `AssetBundleManager` first and, on a hit, reads the blob into
   `SerializedFile::m_OwnedMemory` and builds a `MemoryCacheReader` over it.
   That is what makes `loadAsset<T>()`, PPtr resolution and `Level::load` work
   unchanged in a player build with no `Assets/` tree. Do not add a second
   lookup path.
3. **Bundle wins over disk.** A mounted bundle shadows an on-disk file of the
   same path. That is deliberate (mounting is an explicit act) but it means
   never mount bundles in a normal editing session.
4. **Cached blobs must outlive readers.** `MemoryCacheReader` holds its buffer
   BY REFERENCE. `AssetBundle::GetAssetBytes()` caches per asset and never
   moves or frees it until `Unload()`, so unmount only after dropping the
   assets (`AssetManager::UnloadStream` per asset).
5. **The manifest is a plain `.zasset`** (`AssetBundleManifest`), not JSON:
   ZRuntime has no general-purpose JSON writer. It carries `platform`,
   `content_root`, and per bundle: `asset_paths` + `dependencies`.
6. **Bundle assignment**: one bundle per root (auto-detected `LevelRes` /
   `WorldRes` / `PrefabAsset` / `SceneRes`, or an explicit list); anything
   reachable from >=2 roots goes to a single `shared` bundle, so loading one
   root needs at most one extra bundle. Bundle dependencies are derived from
   the asset graph, not guessed.
7. **Log category is `ZPak`.**

Entry points: Build menu (`Build Asset Bundles (...)`) and the console
command `bundle.build <standalone|android|ios|ohos|webgl>`.

Known gaps, in the order they should be closed (PR-PAK2+):
- **No player bootstrap mounts the bundles yet.** `AssetBundleManager` is
  registered and `MountDirectory()` works, but nothing calls it in a standalone
  player -- a shipped build still has to be handed `Assets/` or call it
  itself. Auto-mounting is deliberately NOT implicit (rule 3: it would shadow
  real files in the editor). Wire it behind an explicit player flag.
- **No compression**: v1 writes STORED blobs (the `compression` field and
  `uncompressed_size` are already in the format). ZRuntime links no compressor.
- No incremental build, no content-hash change detection, no per-asset
  reference counting (unloading a bundle drops everything it cached).
- Only textures have a real cook today; every other asset type is packed as-is.

Known gaps, in the order they should be closed (PR-SK3+):
- **No importer test coverage**: the baking path is compile-verified only.
  There is no FBX fixture in the repo, so exercise it manually once with a real
  character asset before trusting it.
- glTF/GLB produce no skinning data at all (`.gltf` is not even parsed -- the
  mesh importer returns "not implemented yet").
- No IK, no root motion, no retargeting, no animation compression.
- The state machine has **no layers** (no upper-body / additive overrides), no
  blend trees inside a state (a state owns exactly one clip), and no sub-state
  machines.
- No layers / blend trees / sub-state machines in the state machine (see 2.25).
- `AnimationImporter` exists but is **not registered**: registration would
  shadow `MeshImporter` on `.fbx` and break mesh import. It runs the same
  `FbxAnimation::BakeAllStacks()` and is there for explicit / future use.
- DX12 **shadow** pass does not skin: skinned meshes cast their bind-pose
  shadow (the shadow VS has no joint palette bound yet). Vulkan's
  `mesh_directional_light_shadow.vert` does.
- Bone-count / instance limits are engine constants: 1024 joints per instance
  and 64 instances per drawcall (`s_MeshVertexBlendingMaxJointCount`,
  `s_MeshPerDrawcallMaxInstanceCount`).

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

## 6. Tools: ZEnvironmentCollector (env collector)

`engine/tools/ZEnvironmentCollector/` ships **one** executable,
`ZEnvironmentCollector.exe`: GUI by default (three tabs — collection /
compatibility check / GPU select verification), CLI under `-batchmode`
(`--collect`, `--diagnose`, `--gpu-selection`, and the plain check). The former
separate `ZWindowsEnvironmentCheck.exe` CLI and `ZGpuSelectionInspector.exe` GUI
are gone; `-batchmode` is the Unity/UE spelling on purpose. The exe is
**WINDOWS-subsystem** (`wWinMain`), so a double-click never flashes a console —
as a CONSOLE binary the loader's console existed before any of our code ran, so
`FreeConsole()` could only close it after it had already flashed. `-batchmode`
attaches to the caller's console (`AttachConsole(ATTACH_PARENT_PROCESS)`) unless
the caller already redirected stdout to a pipe or a file, so cmd.exe/PowerShell
still get stdout and the exit code, and cmd still waits in a `.bat` (measured,
not assumed). The collector re-launches *itself* as a
Job-managed stage worker and as an elevated WER helper, so both call sites read
`collector::ToolExecutablePath()` — renaming the output must go through that
constant. It packages **game logs** together with the environment report:

- The log directory is declared per project profile, never guessed: `logRoot`
  (`localLow` default, else `localAppData` / `roaming` / `programData`) +
  `logSubPath`. OSGame = `LocalLow\Tencent\osgameCNOfficial\Cache\Logs`.
  CLI override: `--log-dir`. Empty `logSubPath` disables the stage.
- **Real OSGame logs are `.logcompr`** (compressed per-session 扁鹊 log), not
  `.log`; plain `.log` only shows up under `bqLog\ccs_log\`. A filter that
  accepts only `.log`/`.txt` silently collects nothing on a real machine.
- Caps: 8 files, 16 MiB each, 48 MiB total; incident-window files first, then
  newest; oversized files are skipped, never truncated. Logs are copied into
  the report dir (`gamelog-NN-<name>`), never moved / modified / renamed.
  Index in `game-logs.json`; missing or empty log dir is status `NO_LOGS`
  and does **not** mark the report partial.
- **Its own diagnostics go through 扁鹊** (`EnvCheckLog.h/.cpp`, category
  `ZEnvironmentCheck`): `%USERPROFILE%\AppData\LocalLow\ZEngine\Logs\ZEnvironmentCollector_*.log`,
  same directory the engine uses. `ZSlate::SetLogSink` forwards ZSlate's own
  lines (font atlas misses, "no CJK face") there too. The manifest carries the
  path as `toolLog`; the log itself is **not** copied into the ZIP.
- **It links `ZRuntimeShared`** — see §6.1. The log instance is then the engine's
  `LogSystem` (reached over the C ABI), not a second bqlog linked into the tool.
  A standalone configure has no `ZRuntimeShared` target and falls back to its own
  `bq::engine_log`.
- **A package without an incident anchor is not an error, but must say so.**
  Missing `incidentTimeUtc` / `incidentKind` / `gameExecutable` means the package
  can only describe the machine, never attribute anything — and three field
  packages in October 2026 came back exactly like that, with the discovery left
  to whoever opened the ZIP. `DescribeAttributionAnchor()` drives a summary.txt
  header, manifest `attributionUsable` / `missingAttributionInputs`, a red line
  on the GUI form plus a confirm dialog, and a CLI stdout warning. Details and
  the `isTargetGame` field of `events-redacted.json` (schemaVersion 2) live in
  `engine/tools/ZEnvironmentCollector/README.md` §1.8.1.

### 6.1 Why the collector links ZRuntimeShared, and what it must not start

The collector needs two things the engine already owns, and both are worse when
duplicated inside a tool that ships to players:

1. **Logging.** It is a WINDOWS-subsystem binary, so stderr goes nowhere when it
   is double-clicked, and the failures it exists to report are exactly the ones a
   player cannot describe.
2. **Localised UI strings.** Hard-coding 100+ Simplified Chinese literals into
   `UiLocalization.cpp` made translation a code change. They are now data: a real
   engine `DataTable` (`Runtime/Resource/Asset/Data/LocalizationTable.h`) cooked
   from `resource/ui_strings_zh-CN.csv`. A table is engine capability —
   `AssetManager` + `SerializedFile` + reflection — so it comes from the same
   place as the log.

The seam is `Runtime/ZEngineApi/ZEngineToolApi.h` (`EXPORT_RUNTIME`, i.e.
`extern "C"`). Nothing that needs an allocator crosses it: `const char*` in,
`const char*` owned by the DLL out. That is what makes an /MT executable able to
consume the DLL regardless of the DLL's own CRT choice.

**`ZRuntime_ToolInit()` starts the minimum slice and no more**:
`RegisterCore()` + `RegisterPlatform()` — reflection, memory, `FileSystem`,
`ObjectManager`, `AssetManager`, plus `LogSystem`. It deliberately does **not**
register `Application`, `WindowSystem`, any RHI, `ScriptingManager` or
`ProjectInfo`. A tool sent to diagnose a machine must not open a window or create
a GPU device on it. (Same shape as `ZEngineApi::InitMemoryProfilerData()`, minus
`ProfilerRuntime`.)

Loading takes absolute paths: `AssetManager::GetFullPath()` passes them straight
through, so a tool with no project can still read the `.zasset` it ships beside
its own exe.

Cook step: `DataTableCook` (`engine/tools/DataTableCook`) bakes
`resource/ui_strings_zh-CN.csv` -> `.zasset` at build time, driven from the
collector's POST_BUILD. It lives in Runtime (not the Editor's
`CsvSchemaRegistry`) because ZRuntime must not depend on ZEditor and the cook has
to run with no project open. The shipped tool never parses CSV.

Missing DLL = process does not start (import-lib link, by decision). Missing
`.zasset` = UI degrades to English and the tool still works.

## 7. Standalone ZSlate tools: use FSlateStandaloneApp, do not hand-roll a host

Every standalone ZSlate GUI tool (LuaMemPreview, TexPreview,
ClientProjBranches, ZEnvironmentCollector — the former ZGpuSelectionInspector is
now a tab inside the collector) used to
carry its own copy of: an `ISlatePlatform` subclass, WNDCLASS + CreateWindow +
PeekMessage host, DPI declaration / `WM_DPICHANGED` handling, and a hardcoded
`C:\Windows\Fonts\...` atlas load. The copies drifted — two had **no DPI
support at all**, four of five hardcoded the Fonts drive letter, and the DPI
decorator was built with different pixel-snapping flags.

That is now one implementation, in ZSlate:

| Concern | Where it lives |
|---|---|
| Window + message pump + `ISlatePlatform` + D3D11 backend lifetime | `ZSlate/App/SlateStandaloneApp.h` (`FSlateStandaloneApp`) |
| DPI awareness, per-window scale, DPI-sized window framing | `ZSlate/Platform/Win32/SlateWin32Dpi.h` (`Win32::DeclareDpiAwareness`, `QueryDpiScaleForWindow`, `QuerySystemDpiScale`, `ResizeWindowForDpi`) |
| System font discovery + CJK fallback, key mapping, clipboard, UTF-8 argv | `ZSlate/Platform/Win32/SlateWin32Host.h` (`Win32::LoadSystemFonts`, `KeyFromVirtualKey`, `InstallClipboardBridge`, `CommandLineArgsUtf8`) |

**A new standalone ZSlate tool must start from `FSlateStandaloneApp`.** Doing
anything else re-introduces per-tool DPI/font bugs.

Coordinate contract (stated once, in `SlateStandaloneApp.h`): Win32 messages and
the swapchain are **physical**; the widget tree is **logical** (96-DPI units).
The shell is the only place the two meet — its WndProc divides on the way in,
`FSlateDpiScaleRenderer` multiplies on the way out.

Known exception: `CollectorGui.cpp` keeps its own window/WndProc because it
needs a Unicode window, a WM_CLOSE veto while a background collection runs,
popups that pre-empt widget input, and a `busy` input gate. It still uses
`Win32::LoadSystemFonts` / `Win32::ResizeWindowForDpi`, so no DPI/font
knowledge is duplicated. Do not "unify" it by adding shell hooks only it uses.

### 7.1 SListView is NOT broken — do not repeat the old claim

`LuaMemPreview/LuaMemWidgets.h` used to justify its hand-painted 180K-row table
with "SListView's recycling is broken in two ways" (stale data in recycled rows,
and `ReleaseInvisibleWidgets()` never called → a widget retained per row).
**Both were already fixed**: the index→widget map is keyed by index, released
when rows scroll off screen, and cleared whenever the data source revision
changes. The comment was stale and kept being cited as the reason to duplicate
list widgets — it has been rewritten, and the behaviour is now pinned by
`engine/Libraries/ZSlate/tests/slate_list_view_tests.cpp`
(`-DZSLATE_BUILD_TESTS=ON`), which scrolls 180K rows and asserts bounded text
draws / bounded live widgets / fresh data after recycling / balanced clip stack.
Measured: 27 live widgets at 180K rows.

`SLuaMemTable` is still hand-painted, but now for a real reason, not a
workaround: 4 fixed columns with independent sort, a filter and type-colour
chips would cost ~135 live widgets rebuilt every scroll step if routed through
`SListView`. Do not "unify" the two tables without measuring.

Other pieces sunk into ZSlate in the same pass: `SScrollPanel`
(`ZSlate/Widgets/Layout/`, was `CollectorScrollPanel.h`) and
`Win32::EnsureConsole` / `Win32::StdOutIsRedirected`.

### 7.2 App icons for GUI tools: one generator, one CMake function

Four tools (LuaMemPreview, TexPreview, ClientProjBranches,
ZEnvironmentCollector) each need an application icon, and each used to
hand-roll all four pieces of it: a `.ico`, a `.rc`, an `if(MSVC)` block in its
CMakeLists, and an `#ifndef IDI_ICON1` in its `main.cpp`. That is now two
shared pieces:

| Piece | Where | What a tool does |
|---|---|---|
| `z_embed_app_icon(<target>)` | `engine/cmake/AppIcon.cmake` (+ `AppIcon.rc.in`) | one call in the tool's CMakeLists, after `set_target_properties(... OUTPUT_NAME ...)`. Embeds `resource/<OUTPUT_NAME>.ico`, defines `IDI_ICON1=101`, no-op off Windows / when RC is unavailable / before the .ico exists. `engine/CMakeLists.txt` includes it; tools that configure standalone `include()` it by path. |
| `gen_icon.ps1 -Name <Tool> -Motif <...> -OutDir <tool>/resource` | `engine/resource/gen_icon.ps1` | draws the `.ico` (16/32/48/256, PNG-compressed entries) on the shared dark rounded tile. Motifs: `bars` (LuaMemPreview), `branch` (ClientProjBranches), `checker` (TexPreview), `check` (ZEnvironmentCollector). |
| `z_embed_app_manifest(<target> [MANIFEST <file>])` | `engine/cmake/AppManifest.cmake` (+ `AppManifest.manifest`) | one call, same place. Declares per-monitor DPI v2 by embedding the shared manifest; `MANIFEST <file>` replaces it wholesale for a tool needing something else (execution level, comctl32 v6). MSVC-only — CMake has no mt.exe rule for MinGW — and a no-op elsewhere, so the call can be committed anywhere. Same include arrangement as `AppIcon`. |

`z_embed_app_manifest()` is **required** for a GUI that does not go through
`ZSlate::FSlateStandaloneApp` — today only `ZEnvironmentCollector`, which
registers its own `WNDCLASSEX` so `DeclareDpiAwareness()` never runs on its
path. (ZInsights used to be in that bucket, and a prior note claimed it was
covered "via `ZRuntime`'s `SetProcessDpiAwarenessContext()`" — it was not:
that call lives in `WindowsApplication::Initialize` and ZInsights never
constructs a `WindowsApplication`. **Linking ZRuntime declares nothing; only
running that code does.** Use this test, not the link line, when deciding
whether a target needs the manifest.) Tools on the shell declare the same mode
in code (`DeclareDpiAwareness()` inside `FSlateStandaloneApp::Initialize`), so
for them the manifest is redundancy that costs nothing and is slightly safer:
it is in force before `main()` runs, so it cannot be beaten by a window created
too early.

Declaring awareness is only half of a crisp GUI; the other half is **letting
the scale reach the glyph rasteriser**. `FSlateDpiScaleRenderer` multiplies
`font_size` *before* it hits the font atlas, and the atlas bakes per integer
pixel size — so at 150% a 14px label is genuinely rasterised at 21px instead of
a 14px bitmap stretched 1.5x. That is why no ZSlate tool needs a Direct2D
backend for crisp text, and why pinning a raw D2D target to 96 DPI with a
"1 DIP == 1 pixel so mouse coords line up" comment is the bug: it renders 1:1
and lets DWM do the scaling instead. If you ever do add a non-ZSlate surface,
create it in physical pixels at `GetDpiScale() * 96` and recreate it from
`OnDpiChanged`.

**The .rc alone is not enough.** It only puts the icon in the *executable*
(Explorer, Properties). The title bar, Alt-Tab entry and taskbar button come
from the **window class**, which is a separate step:

- ZSlate tools: `desc.LargeIconResId = desc.SmallIconResId = IDI_ICON1;`
- raw Win32: `WNDCLASSEXW::hIcon` / `hIconSm` — **WNDCLASSEX, not WNDCLASS**;
  the plain class has no `hIconSm`, so the taskbar keeps the default icon.

Both are guarded by `#ifdef IDI_ICON1`, so a config where the .rc was not
embedded still compiles. Do not re-add per-tool `#define IDI_ICON1 101` — the
CMake function is the only source of that id.

**The editor has two icon mechanisms, and they are easy to confuse.** They
cover different surfaces and neither alone is enough:

- **Embedded in the executable** (Explorer, Properties, taskbar):
  `engine/Source/Editor/resource/ZEditor.ico`, attached by `z_embed_app_icon()`
  in `engine/Source/Editor/CMakeLists.txt`. `AppIcon.cmake` generates the .rc
  into the *binary* dir, so the `file(GLOB ... resource/ZEditor.rc)` in that
  CMakeLists still matches nothing -- harmless, but it means dropping a
  hand-written `resource/ZEditor.rc` in there would silently take over.
- **Loaded at runtime** (window title bar, Alt-Tab): `ZEngineEditorBigIcon.png`
  and `ZEngineEditorSmallIcon.png` under `engine/resource/`, named by
  `BigIconFile` / `SmallIconFile` in `configs/{development,deployment}/ZEditor.ini`
  and read by `EditorUI.cpp: LoadWindowIcons`.

The runtime pair is PNG, so it is decoded with stb_image. Do not go back to
`LoadImage(..., IMAGE_ICON, ...)` for them: that reads .ico/.cur only and
returns NULL, which is how this path went unnoticed for so long.

`ZEngineEditorBigIcon.png` is also the particle system's logo texture
(`engine/asset/global/particle.global.json`), so it is not dead weight.

### 7.3 ZAppShell — the startup bits every executable repeats

`engine/Libraries/ZAppShell/include/ZAppShell/AppMain.h`. **Header-only, and it
must stay that way**: `ZEnvironmentCollector` ships `/MT` while the tree is
`/MD`, so anything compiled into a library serves only one of them (`LNK2038`)
— which is exactly why the collector had its own copy of the console-attach
logic instead of calling the `ZSlate` one. Inline code is compiled by whoever
includes it, with that target's own runtime library. It therefore calls nothing
but Win32 and the CRT: no ZSlate, no ZRuntime.

| What | Why it is shared |
|---|---|
| `CommandLine::Acquire()` | `CommandLineToArgvW(GetCommandLineW())` was open-coded in five places (ZEditor, ZInsights, CommandSystem, LogSystem, WEC). Owns the array and `LocalFree`s it, so call sites stop leaking it. `.Utf8()` for `std::filesystem`/printing. |
| `EnsureConsole()` | A `/SUBSYSTEM:WINDOWS` exe has no console, so printf goes nowhere and a headless mode looks like it hung. Two implementations existed (ZSlate's and WEC's) and had drifted; the ZSlate one is now gone. No-op when the caller already redirected stdout to a pipe or file, so CI captures keep working. |

**Never `LocalFree` the array yourself** — `CommandLine` owns it. ZEditor's
`resolveStartupModeFromCommandLine` had a manual `LocalFree` that became a
compile error the moment `CommandLine` took ownership.

`EnsureConsole()` must be called **only on the path that prints**. A GUI run
that called it would flash a console window on double-click — the whole reason
these executables are `/SUBSYSTEM:WINDOWS`.

**Do not read `__argv`.** It is in the ANSI codepage, so a non-ASCII path
arrives already mangled — and `LuaMemPreview -batchmode` takes a path.

**Not shared on purpose:** deciding whether a run is GUI or batch. The three
executables with a batch mode disagree — WEC takes `-batchmode` as a switch
anywhere on the line, `LuaMemPreview` takes it as a positional subcommand next
to `--stats`/`--dump-ui`, and ZEditor reads `-p`/`--rhi` to pick a startup mode.

### 7.4 Output & build directories for standalone GUI tools

Two directories per tool, both fixed by convention so an exe never has to be
hunted for:

| Kind | Path | Set by |
|---|---|---|
| **Output** (the `.exe`) | `<repo>/bin/Tools/<Config>[/<subdir>]/` | `z_set_tool_output_dir(<target> [SUBDIR <subdir>])` — `engine/cmake/ToolOutputDir.cmake` |
| **Build tree** (`-B`) | `<repo>/build/tools/<ToolName>/` | convention only, documented here and in each tool's README |

A tool that ships more than its `.exe` passes `SUBDIR` and gets its own level,
so its extra files cannot be mistaken for another tool's.
`ZEnvironmentCollector` is the only one today:

```
bin/Tools/Release/
├── TexPreview.exe
├── LuaMemPreview.exe
├── ClientProjBranches.exe
└── ZEnvironmentCollector/
    ├── ZEnvironmentCollector.exe
    ├── ZDllLoadProbe.exe          # re-launched from the exe's own directory
    ├── ZRuntimeShared.dll         # engine slice (log + localization table)
    ├── resource/ui_strings_zh-CN.zasset   # cooked by DataTableCook at build time
    ├── profiles/default.ini       # read from the exe's own directory
    └── LICENSE-*.txt
```

`z_set_tool_output_dir()` resolves the root as: `Z_ROOT_DIR` if set (full-tree
configure) → else walk up from `CMAKE_CURRENT_SOURCE_DIR` to the directory
containing `engine/CMakeLists.txt` (standalone configure, no hardcoded path) →
else `CMAKE_BINARY_DIR/bin/Tools`. If `BINARY_ROOT_DIR` is already defined it
wins, so the tools follow `ZENGINE_OUTPUT_SUBDIR` too: a cross-compile layout
`bin/win64/x64/` becomes `bin/win64/x64/Tools/<Config>/`.

**Build any of them with one command** (all four land under
`bin/Tools/<Config>/`, and the command prints the exe path when it is done):

```powershell
python zbuild.py tools                          # list them
python zbuild.py tool TexPreview --config release
python zbuild.py tool ZEnvironmentCollector     # --config defaults to release
python zbuild.py tool --all                     # all four, stops at the first failure
```

`--all` includes the two full-tree tools, so the first run also configures the
whole engine — that configure, not the tools, is the slow part. `--reconfigure`
forces a fresh configure of either kind.

`zbuild.py` knows that the four do *not* share a build tree, only a source
directory and an output directory:

- `TexPreview`, `LuaMemPreview` — **full tree**. They link ZRuntime, so they
  cannot configure standalone; `zbuild.py` configures the tree if it has not
  been configured yet, then builds `--target <Name>` in `build/`.
- `ClientProjBranches` — **own configure** into `build/tools/<Name>/`.
- `ZEnvironmentCollector` — **both**, and they are not equivalent:

```powershell
# engine-backed: what ships. Links ZRuntimeShared (log + DataTable localization).
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DZENGINE_STATIC_CRT=ON
cmake --build build --config Release --target ZEnvironmentCollector

# standalone: self-contained, no engine DLL. Kept so the directory still
# configures on its own; the localization table is compiled into the binary and
# logging uses a statically linked bqlog.
cmake -S engine\tools\ZEnvironmentCollector -B build\tools\ZEnvironmentCollector -G "Visual Studio 17 2022" -A x64 -DENVIRONMENT_CHECK_BUILD_GUI=ON
```

  `ClientProjBranches` stays out of the full tree on purpose (OFF there).
  `ZEnvironmentCollector` used to as well, because its GUI was `/MT` while the
  tree's ZSlate is `/MD` (`LNK2038`); `-DZENGINE_STATIC_CRT=ON` removes that
  reason — with it, tree and tool agree on /MT and the GUI can be built in the
  tree (`-DENVIRONMENT_CHECK_BUILD_GUI=ON`) alongside the engine back-end.
  `--reconfigure` forces a fresh configure of either kind.

Why a `Tools/` level instead of `bin/<Config>/` next to ZEditor: the tools share
nothing with the engine's runtime layout (no configs, no assets, no ZRuntime
DLL), and `ZEnvironmentCollector` is built `/MT` to ship to machines with no VC
redist — its output directory *is* its distribution unit, because `profiles/`
and the LICENSE files are copied next to the exe. "Zip
`bin/Tools/<Config>/ZEnvironmentCollector`" is therefore a correct packaging
step, and `install(COMPONENT EnvironmentCollector)` produces the same folder.

`scripts\build_lib\win64\win_build_lua_mem_preview.bat` builds through the full
tree with `-DZENGINE_OUTPUT_SUBDIR=win64/<arch>`, so its output is
`bin\win64\<arch>\Tools\<Config>\LuaMemPreview.exe`; its echoed `Output:` lines
and `OUT_DIR` must stay in sync with this rule.

## 8. Where a tool lives: one home

All four standalone GUI tools live in **one** directory, `engine/tools/`:

| Tool | Needs | Added by | In a full tree |
|---|---|---|---|
| `engine/tools/TexPreview` | ZRuntime (ASTC/BC7/ETC2 decompressors) | `engine/CMakeLists.txt`, desktop branch | always |
| `engine/tools/LuaMemPreview` | ZRuntime (`SerializedFile`, asset registry) | `engine/CMakeLists.txt`, desktop branch | always |
| `engine/tools/ZEnvironmentCollector` | ZSlate only, `/MT`, shipped to players | `engine/CMakeLists.txt` | ON, but GUI OFF (below) |
| `engine/tools/ClientProjBranches` | ZSlate only | `engine/CMakeLists.txt` | OFF by default |

They used to be split across `engine/Source/Tools/` and root `tools/`, and the
old rule ("two homes, split by dependency") is **gone** — one directory is
easier to manage than two, which is what the split was costing.

**Why `engine/tools/`, not root `tools/`:** `ENGINE_ROOT_DIR` is set at
`engine/CMakeLists.txt:1`, i.e. in the engine's own directory scope. A sibling
directory added from the root does **not** inherit it, so TexPreview /
LuaMemPreview would have had to redefine it — exactly the kind of duplicated
path this repo keeps removing. Inside `engine/` every tool inherits
`ENGINE_ROOT_DIR`, `Z_ROOT_DIR`, `BINARY_ROOT_DIR` and the `cmake/*.cmake`
helpers for free.

**The ordering constraint is still load-bearing.** The block that adds
`engine/tools/ZEnvironmentCollector` and `engine/tools/ClientProjBranches`
sits *after* `add_subdirectory(Source/Runtime)`, where `ZSlate` is defined. Both
link ZSlate, and each guards its own `add_subdirectory(.../Libraries/ZSlate)`
with `if(NOT TARGET ZSlate)` for the standalone case — move that block above
`Source/Runtime` and they would create a second `ZSlate`, colliding with the
engine's (`add_library cannot create target "ZSlate" because another target with
the same name already exists`).

`tools/` at the root is **not** a CMake directory any more and has no
`CMakeLists.txt`: it holds the renderdoc submodule, `tools/renderdoc_build`,
`tools/renderdoc-mcp` and the `analyze_*.py` capture-analysis scripts.

### Two options that must stay OFF in a full-tree configure

- `ENVIRONMENT_CHECK_BUILD_GUI` (the ZSlate front-end of
  `ZEnvironmentCollector.exe`): the tool sets `CMAKE_MSVC_RUNTIME_LIBRARY` to
  `/MT` because it ships to machines with no VC redist. Standalone, ZSlate is
  added inside that scope and inherits `/MT`. In a full tree ZSlate already
  exists as `/MD`, so linking it into a `/MT` collector fails with
  `LNK2038 (RuntimeLibrary mismatch)`. One ZSlate instance cannot be both. The
  executable is still built when this is OFF — it just swaps in
  `EnvironmentCheckGuiFallback.cpp` and serves `-batchmode` only. Build the GUI
  from its own configure:
  `cmake -S engine/tools/ZEnvironmentCollector -B <dir>`.
- `ZENGINE_BUILD_CLIENT_PROJ_BRANCHES`: a self-contained CMake project with its
  own C++ standard and output layout; normally configured standalone
  (`cmake -S engine/tools/ClientProjBranches -B build`). Available in-tree, but
  OFF so a full configure does not pick up unrelated work.




## Reflection code generation (ZHeaderTool)

Decided 2026-10-20 (phase 1 landed). UHT-style generator, **Python 3.9+,
stdlib only** (host-side, so Android / OHOS / iOS / Emscripten cross builds
need nothing extra). Do not re-litigate Python vs C++.

- Tool: `engine/tools/ZHeaderTool/` (`ZHeaderTool.py`, `zht/` lexer -> parser
  IR -> codegen). Unit tests: `python3 engine/tools/ZHeaderTool/tests/test_zht.py`.
- CMake: `cmake/ZHeaderTool.cmake`, `zengine_header_tool(MODULE ... TARGETS ...
  INCLUDE_ROOT ... HEADERS ...)`; wired for ZRuntime + ZRuntimeShared in
  `engine/Source/Runtime/CMakeLists.txt`. Outputs go to
  `<build>/Generated/ZHT/<Module>/{Inc,Src}` (Inc is a PUBLIC include dir).
- Markers (`Runtime/BaseClasses/ObjectMacros.h`): `ZCLASS(Abstract, CustomTransfer, NoSuperTransfer)`,
  `ZGENERATED_BODY(Name)` (replaces `REGISTER_CLASS` + `DECLARE_OBJECT_SERIALIZE`),
  `ZPROPERTY(Name="..", HideInEditor, ReadOnly, StrongPPtr, Deprecated,
  TreatIntegerAsBool, Hex64, Transient, meta=(Tooltip, DisplayName, Category,
  EditCondition, ClampMin, ClampMax, UIMin, UIMax))`. `meta` is parsed and
  validated now; consumed from phase 2 (Inspector).
- Rules: `#include "<Name>.generated.h"` (base name of the header, one per
  header even with several ZCLASSes) must be the **last** include; reflected
  classes live at global scope or in **exactly one named namespace**
  (`IMPLEMENT_REGISTER_CLASS_3` token-pastes it) and derive from an
  Object-based class; base names of reflected headers must be unique per module.
- `NoSuperTransfer`: the generated Transfer() skips `Super::Transfer()`. Only
  for classes whose old Transfer() did so (Behaviour, RenderCamera, Texture2D,
  Box/Sphere/Capsule, PhysicsControllerConfig). Note: Behaviour therefore does
  NOT serialize Component's `enabled` -- pre-existing behaviour, kept on purpose.
- `CustomTransfer`: only registration is generated; IMPLEMENT_OBJECT_SERIALIZE,
  Transfer() and INSTANTIATE stay hand-written (GameObject, Transform,
  Material, WorldRes -- WorldRes serializes the base member `Object::name`).
- Helper structs (DECLARE_SERIALIZE) whose Transfer() template is defined in
  a .cpp are no longer instantiated implicitly once the owning ZCLASS's
  Transfer() moves to its .gen.cpp: add
  `INSTANTIATE_TEMPLATE_TRANSFER_WITH_DECL(Struct, , Transfer, void)` in that
  .cpp (see AnimatorController.cpp, GlobalRendering.cpp).
- Targets that compile reflected headers without linking the module (the
  shared PCH) go in `INCLUDE_CONSUMERS`.
- **Serialized order == ZPROPERTY declaration order.** When migrating a
  hand-written `Transfer()`, reorder members to match the old Transfer order and
  pin old field names with `Name="..."` (default name = snake_case of the member
  without `m_`). In-memory byte offsets may change (not persisted:
  `TypeTree::WriteTypeTree` does not write them); the serialized TypeTree and
  payload must not.
- Migration gate: `-DZENGINE_BUILD_REFLECTION_CODEGEN_SMOKE_TEST=ON`, build
  `ReflectionCodegenSmokeTest`, run `--no-offsets <Classes...>` before and
  after migrating and compare (`--golden <file>` does the diff). Goldens are
  ABI-specific, keep them local.
- Adding / removing `ZCLASS` in an existing header: the first build fails once
  with "the set of ZCLASS headers changed"; the next build re-configures
  automatically. That is expected.
- Migrated: every registered ZRuntime class (51) except the ones below.
  Not migrated: `Component`, `Geometry`, `ControllerConfig`, `RenderTexture`,
  `TimelineClip`, `TimelineTrack` declare REGISTER_CLASS but are **never
  registered** (no IMPLEMENT_REGISTER_CLASS -> `TypeOf<T>()` is the
  "[UNREGISTERED]" placeholder, not under Object in the type tree): fixing that
  changes the type hierarchy and is a separate change. `WrapperClass_` lives in
  the IMPLEMENT_DATA_TABLE macro. Editor classes (`AssetImporter`,
  `PrefabInstance`) wait for the ZEditor module to be wired.
