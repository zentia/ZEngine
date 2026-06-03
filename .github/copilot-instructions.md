# Copilot / AI Agent Instructions for ZEngine

This file gives concise, actionable guidance for AI coding agents working in this repository.

1) High-level architecture (quick):
- Engine core: [engine/source/runtime](engine/source/runtime) – runtime systems and core libs.
- Editor & tooling: [engine/source/editor](engine/source/editor) – ZEditor UI and tools.
- Launcher / profiler / modules: [engine/source/launcher](engine/source/launcher), [engine/source/profiler](engine/source/profiler), [engine/source/modules](engine/source/modules).
- Shaders & codegen: [engine/shader](engine/shader) and CMake helpers in [cmake/ShaderCompile.cmake](cmake/ShaderCompile.cmake) and [cmake/GenerateShaderCPPFile.cmake](cmake/GenerateShaderCPPFile.cmake).
- 3rd-party and platform glue: [engine/3rdparty](engine/3rdparty) and top-level `CMakeLists.txt`.

2) Critical developer workflows (exact commands):
- Quick Windows build (recommended): run `build_windows.bat` (uses `zbuild.py`). See [build_windows.bat](build_windows.bat).
- Or manual CMake generate + build:
  - Configure: `cmake -S . -B build`
  - Build: `cmake --build build --config Debug` (or `Release`).
- Generate compilation DB for clangd: run [scripts/generate_compile_db.bat](scripts/generate_compile_db.bat) or the equivalent `cmake -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -B compile_db_temp -G "Unix Makefiles"` then copy `compile_commands.json`.
- Run tests: build then `ctest --test-dir build` or run test binaries in `build` (tests wired in [tests/CMakeLists.txt](tests/CMakeLists.txt)).

3) Important environment/config requirements discovered in CMake:
- `VULKAN_SDK` environment variable is mandatory; the engine fails configuration if missing (see [engine/CMakeLists.txt](engine/CMakeLists.txt)).
- On Windows MSVC is preferred: the project enables `/MP` and sets `ZEditor` as the VS startup project.

4) Project-specific patterns & conventions to follow (concrete):
- Naming: See [README.md](README.md) — classes PascalCase, functions camelCase, member vars `m_`, static `s_`, globals `g_`, files snake_case.h/.cpp.
- Shader build: shaders are compiled into SPV then embedded as C++ headers via the `compile_shader` helper (`cmake/ShaderCompile.cmake`). When adding shaders, ensure they are listed where `compile_shader` is invoked in `engine/shader` CMake.
- Resource embedding: small binary resources are converted to C++ arrays via `GenerateShaderCPPFile.cmake` style helper — follow the same pattern for new embed targets.
- Codegen: there is a `ZPreCompile` codegen target included in `engine/CMakeLists.txt`. New generated targets should be wired into the engine dependency graph using `add_dependencies` as shown.

5) Build tooling and scripts:
- Primary Windows entrypoint: `build_windows.bat` → `zbuild.py` (Python build driver). Inspect `zbuild.py` for custom build steps.
- Presets: repo uses [CMakePresets.json](CMakePresets.json) (includes `presets/entry.json`). Use VS Code CMake integration or `cmake --preset` if needed.
- Shader compilation relies on `glslangValidator` from the Vulkan SDK; CMake looks up the binary via `VULKAN_SDK`.

6) Where to make changes (examples):
- Add a new runtime module: create `engine/source/modules/<module>/CMakeLists.txt` and `add_subdirectory` it from `engine/CMakeLists.txt` or `source/modules` parent CMakeLists.
- Add editor UI: modify `engine/source/editor` and update its CMake target. Follow existing target properties (FOLDER, OUTPUT_NAME).

7) Quick debugging tips:
- On Windows: run the generated solution in `build` (open `ZEngine.sln`) in Visual Studio; the project sets `ZEditor` as startup.
- To enable physics debug renderer: configure with `-DENABLE_PHYSICS_DEBUG_RENDERER=ON` and clean `build` first (see [README.md](README.md)).

8) Tests and CI hints:
- Tests are registered via CTest in [tests/CMakeLists.txt](tests/CMakeLists.txt). CI workflows build with platform-specific presets (see `.github/workflows` in upstream repository if present).

9) Safety & scope for AI edits:
- Prefer small, local changes: update a CMakeLists, add a shader, or refactor a single class. Large cross-cutting changes (rearchitecting systems) must be accompanied by a detailed plan and a CI-aware test run.
- When adding external deps, prefer the `engine/3rdparty` pattern and update CMake accordingly.

If anything here is unclear or you'd like more examples (e.g., where shader targets are declared, or how `zbuild.py` orchestrates builds), tell me which area to expand and I'll iterate.
