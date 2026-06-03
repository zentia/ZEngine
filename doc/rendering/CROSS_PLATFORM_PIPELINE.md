# Cross-Platform Render Pipeline Modules (ZEngine)

Goal: combine UE's "one set of assets, cross-platform auto-branching" with
Unity's "modular SRP, pluggable, flexible editor switching". A single
resource / material / scene drives both a high-fidelity Desktop (deferred)
path and a lightweight Mobile (forward) path, selectable at runtime within one
build, with editor preview switching and (future) automated per-platform
packaging.

This doc covers **Milestone 1: Architecture Foundation** (landed) and the
phased roadmap for the rest.

## 1. Why a module layer above the RHI

Before Milestone 1, `RenderPipeline` hardcoded the pass set with
`if(api==DirectX12)` / `#if Z_HAS_VULKAN` branches. "Desktop vs Mobile" only
existed implicitly as "which backend got linked". The design vision (a high-end
phone running a Desktop-lite path still on Vulkan; a one-click editor preview
toggle) requires Desktop/Mobile to be a first-class layer **above** the RHI, not
a side effect of backend selection.

```
RenderSystem
  -> RenderPipeline (thin dispatcher)
       -> m_ActiveModule : RenderPipelineModule
            -> DesktopRenderPipelineModule  (deferred + forward)
            -> MobileRenderPipelineModule   (forward, feature-stripped)
                 -> ordered draw list -> RHIDrawList -> active RHI (DX12 / Vulkan)
```

Each concrete pass keeps its own internal `getGraphicsAPI()==DirectX12` /
`#if Z_HAS_VULKAN` branch. The module layer only abstracts **which passes run,
in what order, under which path**.

## 2. Core types (`engine/Source/Runtime/Function/Render/Pipeline/`)

| Type | Role |
|------|------|
| `RenderPipelineModule` | Abstract base for a render path. Owns the path's pass assembly and the per-frame draw-list build. Operates on its host `RenderPipelineBase`'s pass slots via friendship, so existing external accessors (`RenderSystem` reading `m_MainCameraPass`, the editor calling `GetUIRenderPass()`, etc.) keep working unchanged while the *path* becomes the swappable thing. |
| `RenderPassContext` | Small per-frame state struct (post-UI callbacks, skybox visibility, axis gizmo state) so passes stop taking long argument lists. Carried into `RenderPassBase::AppendToDrawList`. |
| `RenderPassBase::AppendToDrawList(RHIDrawList&, const RenderPassContext&)` | Uniform hook (default no-op) for a leaf pass to push its named draw lambda(s). The Mobile module is the first consumer; the Desktop module keeps its composite lambdas inline (verbatim relocation, zero output diff). |
| `DesktopRenderPipelineModule` | The existing deferred+forward assembly, relocated verbatim out of `RenderPipeline`. DX12: RP1/RP2 split inside `DX12MainCameraPass` + `BindlessTonemap` + shadow passes. Vulkan: `MainCameraPass` + ColorGrading/FXAA/UI/Combine/Pick/Particle/Lumen. Output is pixel-identical to pre-milestone. |
| `MobileRenderPipelineModule` | Mobile forward path. **Milestone 1 ships a skeleton** (see section 4). |
| `RenderPipelineSettings` | Selection layer: `RenderPath` enum + platform Auto resolution + `r.RenderPath` CVar + deferred-apply dirty flag. |

`RenderPipeline` itself is now a thin dispatcher: it holds
`std::unique_ptr<RenderPipelineModule> m_ActiveModule` and forwards
`Initialize / BuildDrawLists / SubmitDrawLists / UpdateAfterRecreate` and the
query methods to it. It also stores the `RenderPipelineInitInfo` so
`SetActiveModule` can re-`Setup` a freshly constructed module at runtime.

## 3. RenderPath selection

`enum class RenderPath { Auto, Desktop, Mobile }`.

- **Auto** resolves to `Desktop` on Windows / macOS / Linux and `Mobile` on
  Android / iOS / OHOS (`ResolveAuto()` keys off the `Z_PLATFORM_IS_*` macros).
- **CVar** `r.RenderPath` (0=Auto, 1=Desktop, 2=Mobile) registered in
  `RuntimeConsoleCommands.cpp`. Its on-changed callback flips a `std::atomic`
  dirty flag (it does **not** switch inline -- a path switch from the game
  thread mid-frame would be unsafe).
- **Startup env override** `ZENGINE_RENDER_PATH` (`auto|desktop|mobile` or
  `0|1|2`, case-insensitive), read once on first `GetConfiguredPath()`. Mirrors
  the `ZENGINE_V8_DEBUG_PORT` pattern; lets headless / CI runs and quick smoke
  tests force a path without the editor UI.
- **Editor UI**: `Window` menu -> `Render Path` submenu (Auto / Desktop /
  Mobile check items) and the `r.renderpath` editor console command, both call
  `RenderPipelineSettings::SetConfiguredPath()`.

### Safe runtime switch

A path switch tears down GPU resources (passes, framebuffers, render passes)
and rebuilds them, so it must run with the GPU idle and no frame in flight.
The flow (`RenderSystem::ApplyPendingRenderPathChange`, called at the top of
`RenderSystem::Tick`):

1. `ConsumePathDirty()` -- atomically clears + reads the dirty flag.
2. `FlushRenderingCommands()` -- drain the render/RHI command stream.
3. `RenderingThread::ExecuteOnRHIThread(...)` runs the actual switch:
   - `RenderPipeline::SetActiveModule(path)` -> old `module->Shutdown()`
     (resets all pass `shared_ptr`s, freeing GPU resources), construct + `Setup`
     the new module.
   - `RenderSystem::RewireRenderResourceLayoutsAfterPathSwitch()` re-points the
     `RenderResource` descriptor-set layouts at the new passes and rebinds
     global resources (DX12 + Vulkan).
4. GPU idle wait closes the switch.

`SetActiveModule` early-outs if the requested module is already active (matched
by `GetName()`), so toggling to the current path is free.

## 4. Mobile module status -- skeleton (Milestone 1)

The end-state target is a bespoke G-buffer-free **forward** pipeline:

```
optional shadow -> forward opaque -> forward transparent -> skybox -> tonemap -> UI
```

reusing the `mesh_forward` shaders the desktop path already maintains for
transparent draws (HLSL `engine/shader/hlsl/rp1/mesh_forward.frag.hlsl`, GLSL
under `engine/shader/glsl/`), extended to draw opaque lit directly so no
deferred G-buffer is needed.

**What Milestone 1 actually ships**: a skeleton that proves the architecture (a
second, structurally distinct, runtime-swappable module) without yet forking
the main-camera pass into a forward-only variant. `MobileRenderPipelineModule`
**composes** the shared main-camera assembly (via an internal
`DesktopRenderPipelineModule` so the scene renders correctly and the
Desktop<->Mobile switch is verifiable end-to-end on the working DX12 backend),
and applies the first forward-lite **feature strip** at draw-list assembly time:

- no point-light shadow pass (point lights are unshadowed under Mobile),
- no MegaLights stochastic direct lighting.

`Setup / SubmitDrawLists / UpdateAfterRecreate / GetGuidOfPickedMesh /
GetUIRenderPass / GetUiLayerColorView / FinishDx12ShadowPassDescriptorSetup /
ConsumeParticleSwapData` all delegate to the composed assembly; only
`BuildDrawLists` diverges (it omits the `PointShadow` entry).

**Documented TODOs to graduate skeleton -> real forward path** (each a follow-up
sub-milestone):

1. Replace the composed deferred main-camera pass with a single-pass forward
   opaque+transparent pass driven by `mesh_forward` (no G-buffer allocation).
2. Drop SSAO / dynamic GI / Lumen; swap to a lightmap + simple ambient path.
3. Simplified single-cascade directional shadow.

## 5. Verification (Milestone 1)

- **Build**: DebugV8, Win64, `--target ZRuntime ZEditor` -- clean.
- **Desktop (DX12)**: editor boots and runs with zero behavior change vs
  pre-milestone (the Desktop module is a verbatim relocation).
- **Mobile (DX12)**: launched with `ZENGINE_RENDER_PATH=mobile`; log confirms
  `MobileRenderPipelineModule: forward-lite skeleton` selected, the shared
  assembly initialized (`DesktopRenderPipelineModule(DX12): shadow passes +
  main camera initialized`), level loaded, scripts ticked, editor ran steadily
  with no crash.
- **Vulkan**: blocked by a **pre-existing** crash in
  `MainCameraPass::SetupModelGlobalDescriptorSet` (proven via A/B revert during
  Phase A -- same fault site on unmodified code). Unrelated to this milestone;
  tracked separately.
- **Manual-only** (cannot be checked headlessly): runtime UI toggle
  Desktop<->Mobile and a pixel-level diff of the two paths.

## 6. Roadmap (deferred to later milestones)

Maps to the rest of the design vision; ordered roughly by dependency.

| Item | Depends on | Notes |
|------|-----------|-------|
| Real Mobile forward path | Milestone 1 | The section 4 TODOs: G-buffer-free forward opaque+transparent via `mesh_forward`, lightmap ambient, single-cascade shadow. |
| Single-material dual-variant compilation | Milestone 1 modules | Material asset gains a target-platform variant set; resolve the parallel HLSL/GLSL tree duplication. |
| DeviceProfile auto-tiering + QualitySettings tiers | dual-variant | Per-device Auto beyond OS; per-path quality knobs (shadow res, SSAO, RT toggles) as CVars/asset. |
| Single-scene dual lighting | DeviceProfile | Baked lightmap (Mobile) vs dynamic GI (Desktop); needs a lightmap baker + `LightComponent` type enum (spot/dir), which don't exist yet. |
| ShaderGraph dual-target | dual-variant | Wire the `ZSlateMaterialEditorWindow` prototype to real codegen. |
| Editor dual-preview side-by-side + per-pipeline frame debugger | Milestone 1 toggle | Build on the RenderPath toggle + the stubbed RenderDoc module. |
| Packaging auto-bind per platform | DeviceProfile | Asset cooking + platform pipeline binding. |
| Console module | proven abstraction | Add as a third `RenderPipelineModule` once Desktop/Mobile are stable. |

## 7. Convention for new render features

See `AGENTS.md` section 2.15. In short: a new render feature attaches to a
**module's** pass list (Desktop and/or Mobile), not to `RenderPipeline`
directly, and must declare its Desktop/Mobile applicability.
