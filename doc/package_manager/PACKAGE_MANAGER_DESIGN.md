# ZEngine Package Manager (ZPM)

Unity UPM-inspired package management for ZEngine. Editor-only in V1; player builds consume
already-linked engine targets (CMake), not a live resolver.

## Goals

| Unity UPM concept | ZPM V1 | ZPM later |
|-------------------|--------|-----------|
| `Packages/manifest.json` | Project direct deps | Scoped registries |
| `Packages/packages-lock.json` | Resolved graph (VCS) | Git hash locks |
| `package.json` per package | Parse name/version/deps/modules | Samples, tests |
| Embedded `Packages/<id>/` | Auto-detect + override builtin | Embed UI |
| Built-in / registry packages | Engine catalog `engine/Packages/manifest.json` | npm registry |
| `file:` dependency | Relative path from project root | Tarballs |
| Asset import from package | N/A (C++ CMake targets) | Optional content roots |
| Package Manager window | ImGui `Package Manager` panel (Window menu) | Add/Remove UI |

## Layout

```
<ZENGINE_ROOT>/
  engine/
    Packages/
      manifest.json          # built-in catalog (com.zengine.*)
    Source/Runtime/UGUI/
      package.json             # per-package manifest + module map

<Project>/
  Packages/
    manifest.json              # user dependencies (checked in)
    packages-lock.json         # resolver output (checked in)
    com.my.game.ui/            # optional embedded package folder
      package.json
  package.json                 # npm/tsc only (unchanged, NOT ZPM)
```

**Naming:** `<project>/package.json` remains the TypeScript/npm manifest. ZPM uses
`<project>/Packages/manifest.json` (capital P), matching Unity.

## Resolution (V1)

1. Load project `Packages/manifest.json` `dependencies`.
2. For each package name, pick source in order:
   - **Embedded:** `<Project>/Packages/<name>/package.json` exists.
   - **File:** manifest value starts with `file:` (folder relative to project root).
   - **Builtin:** entry in `engine/Packages/manifest.json` with matching name + version.
3. Recurse into each resolved package's `dependencies` (from its `package.json`).
4. Version constraints: exact `x.y.z` or `>=x.y.z` (numeric tuple compare).
5. Detect cycles; fail resolve with logged error.
6. Write `packages-lock.json` and `Intermediate/Packages/zpackages.cmake` (include paths).

Registry / git URLs are rejected in V1 with a clear log message.

## CMake bridge

Built-in packages like `com.zengine.ugui` already build via `ZUGUI` in the main engine CMake graph.
ZPM does **not** dynamically `add_subdirectory` in V1 -- it records roots for tooling and future
optional packages. The generated `zpackages.cmake` exposes:

```cmake
set(ZENGINE_PKG_com_zengine_ugui_ROOT "...")
```

Downstream editor tools or future `ZENGINE_USE_PACKAGES` CMake mode can include this file.

## Editor integration

### Package Manager window

- Menu: **Window -> Package Manager** (same toggle list as other editor panels).
- Implementation: `engine/Source/Editor/EditorWindow/PackageManagerWindow/`.
- Shows resolved packages (name / version / source / depth / path), filter box, **Re-resolve**,
  and buttons to open `Packages/manifest.json` / `packages-lock.json` in the external editor.
- Double-click a path row to reveal that folder in Explorer/Finder.

### Backend

- `PackageManager` (`IEngineSystem`, editor-only) runs after `ProjectInfo::EnsureScriptsScaffold`.
- Depends on `ProjectInfo`; calls `Resolve()` on project open.
- Re-resolve when project manifest or lock mtime changes (same frame as cold start in V1).
- **Degraded mode:** resolve failures log `LOG_ERROR` but `Initialize()` still returns `true`
  so the editor boots (built-in targets like `ZUGUI` remain available via CMake). Engine root
  is discovered from `ZENGINE_ROOT` (validated), then the editor executable path, then cwd.

## API surface

```cpp
class PackageManager : public IEngineSystem {
  bool Resolve(bool force = false);
  const ResolvedPackage* FindPackage(const std::string& name) const;
  std::vector<const ResolvedPackage*> GetResolvedPackages() const;
  std::filesystem::path GetPackageRoot(const std::string& name) const;
};
```

## Reference

Unity sources (read-only): `unity2023.1/Modules/PackageManager/`, project
`Packages/manifest.json`, `packages-lock.json`, per-package `package.json`.
