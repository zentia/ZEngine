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
| Text source assets (`.ts`, `.js`, `.glsl`)  | GUID stored in a single per-project registry file: `<Project>/AssetRegistry/script_registry.json` (**checked into VCS** — analogous to Unity's `ProjectSettings/`, NOT `Intermediate/`). The registry is the source of truth; missing entries are auto-created on first scan from a deterministic hash of the relative path (`Hash128(rel_path_lower)`), so a clone whose registry was lost rebuilds identical GUIDs. |
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
├── AssetRegistry/                # checked into VCS
│   └── script_registry.json      # path<->GUID for all .ts (UE Redirector equiv.)
├── Intermediate/                 # gitignored; engine-managed
│   ├── Scripts/                  # tsc output (.js + .js.map)
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
- Project window shows `Scripts/` as a **top-level root peer of Assets/**,
  not nested under it.

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

## 3. Build / test invocations

- Configure: pre-existing `build/` is fine; CMake presets in `CMakePresets.json`.
- Incremental Debug build:
  `cmake --build build --config Debug --target ZRuntime ZEditor`
- Editor exe lives at `bin/Debug/ZEditor.exe` (NOT under `build/`).
- Demo project for smoke tests: `I:\ZEngineDemo\ZEngineDemo.zproject`.
  Launch with `-p <path-to-zproject>`.

## 4. Active multi-phase work

### TypeScript scripting integration (`doc/TYPESCRIPT_SCRIPTING_DESIGN.md`)

| Phase | Status | Summary |
|-------|--------|---------|
| P1 | done   | `ProjectInfo` scaffolding (Scripts/, Intermediate/, tsconfig.json, package.json, .gitignore). |
| P2 | done   | `ScriptAsset` + `ScriptRegistry` (path-deterministic GUID, content-hash rename detection, atomic JSON save) + Project window Scripts root. Smoke test on `I:\ZEngineDemo`: rename preserves GUID, delete removes entry, restart is idempotent. |
| P3 | done   | `TypeScriptCompiler` (Editor-only IEngineSystem). Spawns `tsc --watch` (project-local `node_modules/.bin/tsc.cmd` preferred, falls back to PATH; degraded mode if neither). Pipes tsc stdout/stderr line-by-line into ZTSC log category via a worker thread + per-frame `Tick()` drain. Generalised `FileSystemWatcher` to take a `setExtensionFilter()` (default still `{".zasset"}` for legacy callers); compiler watches `.js` under `Intermediate/Scripts/` and exposes `JsChangeHandler` hook for P4. |
| P4 | next   | `ScriptingEngine` (puerts wrapper; Editor + Runtime). |
| P5 | todo   | `TypeScriptComponent` (Behaviour base class, inspector binding). |
| P6 | todo   | Hot reload + Project-window double-click → VSCode. |
