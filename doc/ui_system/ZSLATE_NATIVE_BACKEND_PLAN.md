# ZSlate native RHI backend (replacing SlateImGuiRenderer)

Goal: stop rendering ZSlate widgets through Dear ImGui draw lists and instead
submit them through the engine's RHI directly, so ImGui can eventually be
removed. This is "P1" of the ImGui-removal roadmap.

> **STATUS (P9, 2026-05): COMPLETE -- the `SlateImGuiRenderer` painter and the
> `r.ZSlate.NativeBackend 0` / `r.ZSlate.NativeMenuBar 0` ImGui fallbacks are
> deleted.** The native `BatchedUIRenderer` is now the sole editor renderer. The
> milestone notes below (M1..) are kept as build history; wherever they say a
> CVar "gates" the native path or an "ImGui fallback" exists, that fallback no
> longer exists -- the native path is unconditional. Only `SlateImGuiTextMeasurer`
> survives (layout-time text metrics over the ImGui font atlas, which the native
> renderer also draws from), and ImGui stays linked for fonts + panel hosting
> (`ImGui::Begin`) + input. Full P9 details: `ZSLATE_NATIVE_DOCK_PLAN.md` 3i.

## What already exists (reused, not rebuilt)

ZSlate widgets are already ImGui-independent: every widget paints through the
abstract `UIRenderer` (`FPaintContext::Renderer` is a `UIRenderer*`). The
runtime already ships the full native stack:

- `BatchedUIRenderer : UIRenderer` -- records quads / textured quads / glyph
  runs / clip rects / transforms into a CPU `UiRenderBatch`.
- `UiGpuResources` -- font atlas + white texture + per-texture descriptor sets
  (RHI). NOTE: glyphs are still baked via ImGui's `ImFont`/`ImFontAtlas`, so
  ImGui stays *linked* for fonts even after this work. A pure-removal needs a
  separate font-atlas replacement (out of P1 scope).
- `UIPass` -- uploads the batch to RHI vertex/index buffers and draws it with
  the `ui_batched.{vert,frag}` shaders.
- `SlateUIRendererTextMeasurer` -- forwards layout-time measurement to a
  `UIRenderer`.

`SlateImGuiRenderer` is the throwaway translation layer; swapping it for
`BatchedUIRenderer` touches **zero widget code**.

## Backend integration points (the only real differences)

The editor composites its UI differently per backend:

| Backend | Editor UI target | Has RHIRenderPass? | Pipeline target |
|---------|------------------|--------------------|-----------------|
| DX12    | swapchain overlay (`DX12RHI::BeginSwapchainOverlayDraw`, raw RTV) | NO | `CreateGraphicsPipelines` with `renderPass == nullptr` defaults `RTVFormats[0]` to the swapchain format (`ApplyPsoRenderTargetFormats`). |
| Vulkan  | main-camera render pass, `_main_camera_subpass_ui` | YES (`EditorUIPass::m_Framebuffer.render_pass`) | reuse that pass + subpass, exactly like the runtime `UIPass`. |
| Metal   | `EditorMetalUIRender` | n/a | deferred. |

So no heavy RHI extension is needed: build the UI pipeline with `renderPass=null`
on DX12 and the editor pass + UI subpass on Vulkan. The draw itself stays
backend-agnostic via `RHICommandBuffer*` + `Cmd*PFN` (same as `UIPass::Draw`).

## Known risks (need on-device iteration)

1. **DX12 descriptor heap (addressed M2 2026-05)**: during the swapchain overlay draw the single active
   shader-visible CBV/SRV/UAV heap is the *bindless* heap (ImGui SRVs live there
   too, per `AllocateImGuiSrvDescriptor`). `UiGpuResources` textures now relocate
   their SRVs into the bindless heap via `RHI::EnsureShaderVisibleImageView`, and
   `ZSlateEditorOverlay::DrawBatch` sets `DX12RHI::SetOverlayDescriptorBindActive`
   so `CmdBindDescriptorSetsPFN` keeps the bindless (+ sampler) heaps bound instead
   of switching back to the legacy `m_CbvSrvUavHeap`.
2. **UiGpuResources init ordering**: in pure edit mode no runtime `UIPass` runs,
   so `UiGpuResources` may be uninitialized. The overlay initializes it lazily
   from the editor RHI.
3. **Clip / scissor**: `UiDrawCommand` carries no per-command clip; clipping is
   geometry-level in `UiRenderBatch`. Per-panel scissor for real windows is a
   follow-up; the self-test does not need it.

## Staging

- **M1 (this PR)**: editor overlay (`ZSlateEditorOverlay`) owns a shared
  `BatchedUIRenderer` + UI pipeline (ported from `UIPass`), drawn in
  `EditorUIPass::Draw` after ImGui. Gated behind CVar `r.ZSlate.NativeBackend`
  (default 1). A dev self-test records a fixed quad + text so the GPU path
  (init, pipeline, upload, font-atlas descriptor, overlay compositing) can be
  validated in isolation before any real window is moved. DX12 + Vulkan.
- **M2 (2026-05)**: `r.ZSlate.NativeBackend` defaults on; DX12 bindless heap
  fix for `UiGpuResources` SRVs + overlay descriptor bind. ZSlate windows paint
  into the shared `BatchedUIRenderer` when native backend is enabled.
- **M3 (in progress)**: per-panel scissor / clip correctness; overlay menus
  (`GetForegroundDrawList`) replacement with a ZSlate overlay layer.
  - **Native menu bar (2026-05, landed)**: the editor's main menu bar
    (File / Window / Edit / ...) is now a ZSlate widget when
    `r.ZSlate.NativeMenuBar` is 1 (default; also requires
    `r.ZSlate.NativeBackend` 1). `ZSlate::ZSlateEditorMenuBar`
    (`engine/Source/Editor/Menu/ZSlateMenuBar.{h,cpp}`) paints the title
    strip + active dropdown chain into the shared `BatchedUIRenderer` under a
    `BeginWindowGroup(ImGui::GetForegroundDrawList())` key (so it composites
    above every other ZSlate window group) and routes input through one
    `SlateInputRouter` per open level. `SMenu` gained `AddCheckItem` /
    `AddSubMenu` / `GetHoveredSubMenu` / `GetSubMenuAnchor`; nested submenus are
    managed as a host-side stack (diagonal-navigation friendly: a child stays
    open while the cursor is over it OR its parent row is hovered). Menu content
    is declared via `Menu::BuildZSlateMenu` overrides (`FileMenu`, `WindowMenu`)
    + `EditorUI::BuildEditorWindowZSlateMenu`; builders run once on open
    (Unity-style rebuild-on-open, so toggle/check state samples at open time).
    Because the dropdown is a foreground overlay popup (not an ImGui menu) it
    can grow as tall as it needs and is clamped on-screen, so the long Window
    menu no longer clips without a scrollbar. `MenuController` raises a modal
    full-viewport invisible ImGui window while a dropdown is open so clicks under
    the popup don't fall through to the panels behind it. ImGui `BeginMenuBar`
    path is the fallback (`r.ZSlate.NativeMenuBar 0`).
  - **Reusable context-menu popup (2026-05, landed)**: `ZSlate::ZSlatePopupMenu`
    (`engine/Source/Editor/Menu/ZSlatePopupMenu.{h,cpp}`) factors the menu bar's
    dropdown-chain logic (anchored `SMenu` stack, diagonal-navigation-friendly
    submenu open/close, outside-click dismiss, item-fire teardown, on-screen
    clamping) into a renderer-agnostic component (`Render(UIRenderer&, ...)`,
    so it works for both the native `BatchedUIRenderer` overlay and the legacy
    `SlateImGuiRenderer` foreground-draw-list fallback). `ZSlateHierarchyWindow`
    and `ZSlateProjectWindow` dropped their duplicated single-level inline menu
    plumbing (`m_Menu` + `m_MenuOpen` + `m_MenuInput` + `Open/CloseMenu`) for one
    `ZSlatePopupMenu m_Popup` each; the Project window's flat "Create X" items now
    roll up into a nested **Create** submenu for free. `Open(anchor, scale,
    builder)` runs the builder once on open (rebuild-on-open). The host calls
    `m_Popup.Render(...)` each frame while `IsOpen()`; right-click still records
    a pending target and opens on the button-up edge. The menu bar
    (`ZSlateEditorMenuBar`) now ALSO delegates its dropdown chain to a
    `ZSlatePopupMenu` member -- it only retains the title-strip layout/paint and
    the open/close/hover-switch interaction. The popup gained an `auto_close`
    flag (default true) that the bar sets false, because the bar owns dismissal:
    a left-edge that opens a new dropdown via a title click would otherwise be
    seen by the popup as an outside click and self-close on the same frame. So
    the dropdown/submenu stack, painting, clamping, input routing and
    diagonal-navigation now live in exactly one place (`ZSlatePopupMenu::Render`).
  - **Overlay NDC DisplayPos fix (2026-05, landed)**: `ZSlateEditorOverlay::
    UploadBatch` now subtracts the main-viewport `DisplayPos` in the vertex NDC
    transform AND the scissor clip-rect mapping, mirroring ImGui's own render
    backend. Previously ZSlate-drawn content was shifted down-right by
    `DisplayPos` (e.g. `(11,45)` for a non-maximized GLFW window) relative to
    ImGui content and `io.MousePos`, which broke hit-testing for the native menu
    bar (clicks landed ~45px below the bar) and any ZSlate panel widget. With the
    fix, ZSlate geometry (in ImGui screen coords) and the mouse align directly,
    so no per-widget coordinate compensation is needed.
  - **Per-panel scissor / clip (2026-05, verified done)**: all six ZSlate
    windows (`ZSlate{Hierarchy,Project,Inspector,Console,Demo}Window` +
    `UMGDesignerWindow`) push their panel rect via `pushClipRect(region, true)`
    around `Paint` in BOTH the native overlay and legacy fallback paths.
    `UiRenderBatch` records the active clip per `UiDrawCommand` and breaks a
    command run whenever the clip changes (`beginCommand`), and the overlay's
    `DrawBatch` maps each command's clip rect to a GPU scissor (with the
    DisplayPos subtraction from the fix above). Nested clips intersect
    (`intersect_with_current`), so a `SScrollBox` inside a panel is bounded by
    the panel. Foreground popups/menus paint AFTER the panel's `popClipRect`, so
    they correctly overflow their host panel. M3 is complete.
- **M4**: Metal; font-atlas replacement to drop ImGui from the text path.
  - **Native font atlas (2026-05, landed)**: ZSlate text is now rasterized by a
    native stb_truetype glyph atlas instead of ImGui's `ImFontAtlas`/`ImFont`,
    gated by `r.ZSlate.NativeFont` (default 1; set 0 to fall back to the ImGui
    draw path). `ZFontAtlas` (`engine/Source/Runtime/UI/Render/ZFontAtlas.{h,cpp}`)
    bakes glyphs **on demand** -- the first time a `(codepoint, pixel-size)` pair
    is drawn -- into a fixed 2048x2048 RGBA bitmap (white RGB, glyph coverage in
    alpha so the UI shader's color modulation tints text). This is CJK-capable
    without pre-ranging the full CJK block: only glyphs actually drawn are baked.
    Packing is a naive left-to-right shelf packer with 1px gaps; when the atlas
    fills, new glyphs render blank (logged once) rather than corrupt -- 2048x2048
    holds well over the editor's Latin + working-set CJK at typical sizes (no
    eviction in V1). stb_truetype is compiled as a **file-local (`STBTT_STATIC`)**
    copy of the already-vendored `engine/3rdparty/imgui/imstb_truetype.h`, so its
    `stbtt_*` symbols have internal linkage and do not clash with the copy ImGui
    instantiates in `imgui_draw.cpp`.
    - `UiGpuResources` owns the native atlases: `ResolveNativeFont(Font*)` /
      `ResolveNativeFontPath` (one `ZFontAtlas` per source path, default font
      from `ResolveDefaultFontPath`), `GetNativeFontTextureId` (one GPU texture
      per atlas, created on first draw), and `RefreshNativeFontAtlasesIfDirty`
      (re-uploads any atlas whose bitmap grew this frame, keeping the texture's
      `handle_id` stable via the same in-place transplant trick as the ImGui
      atlas -- shared `ReuploadTextureInPlace` helper). `GetDescriptorSet` resolves
      native-font texture ids alongside white/font-atlas/Texture2D.
    - `BatchedUIRenderer::drawText` / `measureText` try the native atlas first
      (glyph quads relative to the pen at text top-left, matching the old ImGui
      glyph convention; character-level wrap for `TextWrapMode::Wrap`), falling
      back to the ImGui `ImFont` path only when the native font is unavailable.
    - The per-frame refresh is called after all UI records and before the batch
      is drawn: `ZSlateEditorOverlay::DrawBatch` (editor) and `UIPass::Draw`
      (runtime), right next to the existing `RefreshFontAtlasIfDirty`.
    - **Note**: ImGui is still linked and keeps its own atlas for any content it
      renders itself (docking host, remaining ImGui windows); this milestone makes
      *ZSlate* text ImGui-independent, which is the prerequisite for eventually
      removing ImGui from the editor entirely.
  - **Remaining**: Metal backend parity; full ImGui removal (docking/host) once
    all editor windows are ZSlate-native.
