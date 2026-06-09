# RenderDoc build wrapper (ZEngine)

ZEngine-specific CMake glue for building the vendored [RenderDoc](https://github.com/baldurk/renderdoc) submodule at `tools/renderdoc/`.

Upstream RenderDoc disables its own CMake on Windows; this wrapper drives `renderdoc.sln` via CMake-selected VS 2022 / MSBuild (no IDE solution upgrade prompts).

## Layout

```
tools/
  build_renderdoc.bat       entry script
  renderdoc_build/          ZEngine-owned (this folder)
    cmake/                  CMake wrapper + presets
    build/                  cmake configure output (gitignored)
  renderdoc/                git submodule -> baldurk/renderdoc
    x64/                    MSBuild output (gitignored by submodule)
```

## Build

From repo root:

```bat
tools\build_renderdoc.bat
tools\build_renderdoc.bat --release
tools\build_renderdoc.bat --reconfigure
```

Outputs: `tools/renderdoc/x64/Development/` (or `Release/`).

## Submodule only

Do not commit ZEngine files under `tools/renderdoc/`; keep that tree as a clean upstream submodule. Bump the pinned commit via `.gitmodules` / `git submodule update` when upgrading RenderDoc.
