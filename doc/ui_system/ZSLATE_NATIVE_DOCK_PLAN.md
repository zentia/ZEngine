# ZSlate Native Dock Space -- design & phased rollout

Goal: replace **ImGui's `DockSpace`** (the editor's docking layer) with a native
ZSlate docking system, the same way the menu bar, playback toolbar, and every
editor panel were already moved off ImGui widgets. This is the last large ImGui
surface in the editor shell.

This is a **multi-phase** effort gated behind a CVar so the working ImGui dock
stays the default until the native dock reaches parity -- mirroring the
`r.ZSlate.NativeBackend` / `r.ZSlate.NativeMenuBar` rollout pattern.

---

## 1. What ImGui's DockSpace gives us today (the surface to replace)

Per-frame flow (`EditorUI::ShowEditorUI`):

1. `MenuController::BeginGUI()` opens a full-viewport, `NoBackground` ImGui host
   window ("Editor menu").
2. `MenuController::OnGUI()` paints the (ZSlate) menu bar + playback toolbar, then
   `ShowEditorWindowDock()` -> `DefaultLayout::OnGUI()` -> **`ImGui::DockSpace(...)`**
   builds the dock node tree as a child region of the host window.
3. Each `EditorWindow::BeginGUI()` calls `ImGui::Begin(title, ...)`. With
   `ImGuiConfigFlags_DockingEnable` + `ConfigDockingAlwaysTabBar`, ImGui auto-docks
   the window into the node assigned by the ini / `DockBuilder`.

So ImGui currently owns: the dock-node **tree**, **splitters**, **tab bars**,
window **title bars**, **drag-to-dock** with directional preview overlays,
**resize**, **multi-viewport** (drag a panel out to a separate OS window), and
**`imgui.ini`** persistence. `DefaultLayout` drives `DockBuilder*` to author the
5 builtin layouts and serializes `imgui.ini` text into `.zlayout.json` snapshots.

## 2. Target architecture

A native dock host that owns a `EditorDock::DockTree` and, each frame:

- solves geometry for every node (split rectangles, splitter gaps, tab strips,
  panel content rects);
- paints splitters, per-leaf tab strips, and panel frames through the ZSlate
  `BatchedUIRenderer` (same overlay batch the rest of the editor chrome uses);
- positions each **visible** panel's content into its leaf `ContentRect`. Panels
  keep using ImGui *internally as a clipped canvas at a fixed rect* (NoTitleBar |
  NoResize | NoMove | NoDocking | NoBringToFrontOnFocus) -- ImGui docking itself
  (`ImGuiConfigFlags_DockingEnable`) is turned **off**. A leaf shows only its
  `ActiveTab` panel; the other panels in that leaf are simply not begun.
- routes input: splitter drag (resize), tab click (activate / start panel drag),
  drag-to-dock with a directional drop preview, tab close.

Persistence moves from `imgui.ini`'s `[Docking][Data]` to `DockTree`'s own JSON
(`SerializeToJson` / `DeserializeFromJson`), which slots into the existing
`.zlayout.json` snapshot system in `DefaultLayout`.

## 3. Model layer (PHASE 1 -- DONE)

`engine/Source/Editor/EditorLayout/ZSlateDock/`

- `DockNode.h` -- binary tree node. A node is a **split** (`ChildA`+`ChildB`,
  `Orientation`, `SplitRatio`) or a **leaf** (`Tabs` + `ActiveTab`). Per-frame
  computed rects: `NodeRect`, `ContentRect`, `TabStripRect`, `SplitterRect`.
- `DockTree.{h,cpp}` -- GPU/ImGui-free layout + state:
  - `ComputeGeometry(rect)` recursively solves all rects (splitter thickness +
    tab-strip height come from `Metrics`).
  - `AddTab` / `RemovePanel` (empty leaves collapse: the sibling subtree is
    promoted into the parent's slot) / `SplitLeaf` (Left/Right/Top/Bottom/Center)
    / `MovePanel` (detach + re-dock, the drag-to-dock primitive).
  - `SetSplitRatioFromDrag` (clamped so neither child drops below
    `Metrics.MinNodeExtent`).
  - `HitTestSplitter` / `LeafAt` / `FindPanelLeaf` for input.
  - `SerializeToJson` / `DeserializeFromJson`
    (`{"version":1,"root":{"type":"leaf|split",...}}`).

This phase is **not wired into the live editor** -- it compiles into `ZEditor`
but nothing calls it yet, so the running editor is byte-identical. That keeps the
risky integration (which can only be smoke-tested with a GUI) out of a
non-interactive build-only change.

## 3b. Phase 2 done -- render host (CVar-gated preview)

The render layer is implemented and wired as a **read-only preview**:

- `EditorDock::DockHost` (`engine/Source/Editor/EditorLayout/ZSlateDock/DockHost.{h,cpp}`)
  walks a geometry-solved `DockTree` and paints, through the shared
  `BatchedUIRenderer`: per-leaf panel background + 1px frame, the tab strip with one
  tab per docked panel (active / inactive / hover states + top accent on the active
  tab), and per-split splitter bars. Hover highlight is driven by the mouse position
  but performs NO state change or structural edit -- input lands in P4.
- `r.ZSlate.NativeDock` CVar added (default **0**) in `ZSlateEditorOverlay`
  (registered alongside the other `r.ZSlate.*` CVars; gated by `NativeBackend`).
- `DefaultLayout::OnGUI` captures the dock-host origin and, when the CVar is on,
  calls `RenderNativeDockPreview()` AFTER `ImGui::DockSpace`. The preview builds a
  `DockTree` mirroring the Default arrangement from the *currently open* editor
  windows (`BuildNativeDockTreeFromOpenWindows`, rebuilt only when the open set
  changes), solves geometry over the real dock-host rect, and paints through the
  overlay using `ImGui::GetForegroundDrawList()` as the z-order key so the native
  chrome composites above the live ImGui panels.

This still leaves ImGui's `DockSpace` hosting the real panels underneath -- P2 is a
visual parity preview only, so `r.ZSlate.NativeDock 0` (the default) is byte-identical
to before. Panel hosting moves to the native host in P3.

## 3c. Phase 3 done -- native panel hosting

The native tree now OWNS panel placement when `r.ZSlate.NativeDock` is on:

- `DefaultLayout::OnGUI` early-branches when the CVar is on: it computes
  `dock_host_avail` + origin, calls `RenderNativeDockPreview()` (which builds the tree
  + solves geometry + paints chrome), and `return`s **without ever creating an ImGui
  `DockSpace`**. The entire ImGui dock-repair block (stale-ini repair, `DockBuilder`
  node resizing) is skipped because there is no DockSpace node to repair. The off path
  is unchanged (`ImGui::DockSpace` as before).
- `DefaultLayout::QueryNativeDockPanel(title, out_rect[4], out_is_active)` exposes the
  live tree: it `FindPanelLeaf(title)`s, reports the leaf `ContentRect` (screen px) and
  whether `title` is the leaf's active tab.
- `EditorView::BeginGUI` (all three platform impls -- Win/macOS/Linux) consults the
  layout when the CVar is on: an **inactive** tab returns `Closed` (not begun this
  frame -- only the active tab of each leaf draws); the **active** tab gets
  `SetNextWindowPos/Size(ContentRect)` + `NoTitleBar | NoResize | NoMove | NoCollapse |
  NoDocking | NoBringToFrontOnFocus`. Panels the tree does not place (e.g. ZSlate / UMG
  Designer, not in the builtin arrangement) fall through to the normal floating path.
- `EditorWindow::BeginGUI` skips its 200x150 min-size constraint under native dock so a
  small leaf is not forced to overflow its neighbours (the tree's `MinNodeExtent` is the
  floor instead).

Deviation from the original P3 sketch: we do **not** toggle the global
`ImGuiConfigFlags_DockingEnable` flag on/off with the CVar. Flipping that mid-session
makes ImGui undock everything and is awkward to reverse when the CVar is toggled back.
The per-window `NoDocking` flag achieves the same functional result (native-hosted
panels never participate in ImGui docking) with no global-state churn. `DockSpace` is
simply never called on the native path, so there is no dock node for stray windows to
re-attach to. (Splitter resize / tab-click activation landed in P4; drag-to-dock is
still P5. The arrangement starts as the fixed Default built from the open-window set.)

## 3d. Phase 4 done -- dock input (resize / activate)

Splitter resize and tab activation are live on the native path:

- `DefaultLayout::HandleNativeDockInput(host_rect)` runs once per frame at the tail of
  `RenderNativeDockPreview` (after `DockHost::Render`, so it can read the per-tab rects
  the host just filled). Edits apply on the next frame's `ComputeGeometry` -- a 1-frame
  latency that is imperceptible at 60 FPS and saves a second geometry solve.
- **Splitter drag**: `HitTestSplitter(mouse)` -> on `IsMouseClicked` stores the split
  node in `m_DraggingSplitter`; subsequent frames map the cursor offset within the
  node's `NodeRect` to a ChildA fraction and feed `DockTree::SetSplitRatioFromDrag`
  (which clamps so neither child shrinks below `MinNodeExtent`). The drag branch runs
  BEFORE the hover gate so it keeps tracking even when the cursor leaves the thin bar or
  passes over a panel. Releasing the left button ends the drag.
- **Splitter double-click reset**: `IsMouseDoubleClicked` on a splitter resets its ratio
  to `0.5`.
- **Tab click**: the host stores each painted tab's screen rect in `DockNode::TabRects`
  (index-aligned with `Tabs`); a left click inside a leaf's `TabStripRect` finds the hit
  tab and sets `ActiveTab`. The active-tab change flows through `QueryNativeDockPanel`
  so `EditorView::BeginGUI` begins the newly-selected panel next frame.
- **Resize cursor**: `ImGuiMouseCursor_ResizeEW` / `ResizeNS` is set while hovering or
  dragging a splitter.
- **Suppression**: input is gated on `ImGui::IsWindowHovered()` of the MenuController
  host window -- true only over the dock chrome (strips / splitter gaps), because panels
  float on top (hovered instead) and the menu's modal capture window rises to front when
  open. An extra `IsPopupOpen(AnyPopup)` guard covers ImGui-native menu popups. This is
  the same "menu owns the mouse" rule the native playback toolbar uses.

Deviation from the original P4 sketch: input is hit-tested directly against the tree's
geometry rects rather than threaded through a `SlateInputRouter`. The router exists to
dispatch into a retained `SWidget` tree (buttons / text boxes with focus + capture
semantics); dock splitters and tabs are stateless rectangles, so a direct
`HitTestSplitter` / `TabRects` test is simpler and has no widget tree to feed. The
"tab/title drag to start a panel move" half of the original P4 line is the front end of
drag-to-dock and is folded into P5 (it needs the drop-preview + `MovePanel` machinery).

## 3e. Phase 5 done -- drag-to-dock

Tabs can now be dragged to re-dock panels:

- **Arming**: a left press on a tab activates it AND records `m_TabDragId` + start pos
  (still inside the chrome gate, so a drag can only originate on a tab strip). Once the
  cursor moves past a 6px threshold, `m_TabDragActive` flips and the drag branch (which
  runs BEFORE the chrome gate) takes over -- so it keeps tracking even as the cursor
  passes over panel windows.
- **Drop target**: each drag frame, `DockTree::LeafAt(mouse)` finds the hovered leaf and
  `ComputeDropDir(leaf.NodeRect, mouse)` classifies the cursor into Left / Right / Top /
  Bottom (outer 30% edge zones) or Center (interior).
- **Preview**: `DockHost::DrawDropPreview` paints a translucent accent fill + border over
  the region the panel would occupy -- half of `NodeRect` for an edge, the whole
  `ContentRect` for Center -- drawn last so it sits above the chrome.
- **Commit**: on release over a valid target, `DockTree::MovePanel(id, target, dir, 0.5)`
  detaches the panel (collapsing an emptied source leaf by promoting its sibling) and
  re-docks it. `RenderNativeDockPreview` re-solves geometry + tab layout in the same
  frame (`HandleNativeDockInput` returns `true` on a structural change) so the new
  arrangement paints immediately with no one-frame flash.
- The moved arrangement persists: as of P6 the tree is the persistent source of truth
  (`ReconcileNativeTreeWithOpenWindows` only adds/removes panels on open/close, never
  rebuilds), and the live arrangement is autosaved to `native_dock.json` -- so a manual
  move survives both panel open/close and an editor restart.

Deviation from the original P5 sketch: the drag uses **local `DefaultLayout` state**, not
the global `ZSlate::SetActiveDragOperation` channel. That channel exists to carry a
payload ACROSS independent ZSlate windows (e.g. Hierarchy -> Inspector); the dock drag is
entirely intra-host (one tree, one host rect, hit-tested every frame), so threading it
through the global single-slot channel would add coupling with no benefit. If V2 ever
supports dragging a tab OUT to a separate OS window (explicitly cut for V1, see Open
decisions), that is when the global channel would come into play.

## 3f. P6 -- Persistence (DONE)

The native `DockTree` is now the **persistent source of truth**, no longer re-derived
from the open-window set every frame. Three things changed:

- **Lifecycle: reconcile, not rebuild.** `BuildNativeDockTreeFromOpenWindows` (full
  signature-keyed rebuild) is replaced by `EnsureNativeDockTree` ->
  `ReconcileNativeTreeWithOpenWindows`. On the first native frame the tree is restored
  from the session file (else built from the Default builtin); thereafter each frame
  only *diffs* the tree against the open set -- newly-opened panels are docked into the
  largest leaf (`DockNativePanelSomewhere`), closed panels are `RemovePanel`'d (emptied
  leaves collapse). Manual docking + splitter ratios survive open/close indefinitely.
- **5 builtin layouts as `DockTree` builders.** `BuildNativeBuiltin(type)` authors
  `Default` / `2 by 3` / `4 Split` / `Tall` / `Wide` directly via `SplitLeaf`/`AddTab`
  (Scene is the stable center anchor, re-found via `FindPanelLeaf` after each split).
  `ApplyBuiltinLayout` calls it (CVar-gated) so a `Window > Layouts` switch rebuilds the
  native tree; `ApplyBuiltinWindowStates` then sets the open set and the next reconcile
  trims it. The ImGui `DockBuilder*` path is left intact for `r.ZSlate.NativeDock 0`.
- **JSON persistence.**
  - *User layouts* (`.zlayout.json`): `LayoutSnapshot` grew a `dock_tree_json` field.
    `CaptureCurrentLayout` serialises the live tree (when the CVar is on) alongside the
    existing `imgui_ini`; `SaveSnapshotToFile` writes a `"dock_tree"` member (omitted
    when empty so legacy readers are unaffected). `ApplySnapshot` deserialises it; a
    legacy snapshot with no `dock_tree` rebuilds the Default native tree and lets
    reconcile match it to the snapshot's open set. `LoadSnapshotFromFile` now accepts a
    file with *either* `imgui_ini` or `dock_tree`.
  - *Live session* (`<saved>/config/native_dock.json`, the DockTree analogue of
    `imgui.ini`): `TickNativeSessionAutosave` (tail of `RenderNativeDockPreview`)
    serialises the tree each frame, debounces ~1s, and writes atomically (tmp+rename) on
    change. `LoadNativeSession` restores it on the first native frame -- guarded so a
    returning user's manual arrangement wins over the cold startup Default, but an
    explicit builtin/snapshot queued this run (which moves `m_CurrentLayoutName` off
    `"Default"`) is not clobbered. `~DefaultLayout` does a best-effort final flush.

## 3g. P7 -- Parity + default flip (DONE, pending interactive sign-off)

The parity features landed and `r.ZSlate.NativeDock` now defaults to **1**, so the
native dock is the default editor experience. The ImGui `DockSpace` path is **kept**
as the `r.ZSlate.NativeDock 0` fallback (NOT deleted yet) -- per section 6, the
fallback stays until the native dock is verified interactively, since these features
can only be smoke-tested in a GUI. Full removal of the `DockSpace` / `DockBuilder`
code is the one remaining follow-up once a human confirms parity in-editor.

Parity features:

- **Tab overflow scrolling.** `DockNode` gained transient `TabScrollOffset` +
  `ScrollLeftRect`/`ScrollRightRect`. `DockHost::LayoutLeafTabs` now lays out *every*
  tab (scrolled by `TabScrollOffset`), reserves a two-button arrow zone (`<` `>`) on
  the right of the strip when the tabs overflow, and clamps the scroll to
  `[0, total - usable]`. Tabs are clipped to the usable region so they never paint over
  the arrows. Input: mouse-wheel over an overflowing strip scrolls it; clicking an arrow
  pages it (`HandleNativeDockInput`). The scroll offset is transient (not serialized).
- **Panel close button.** The active tab carries an `x` button (`ActiveTabCloseRect`,
  reserved width in the tab). Clicking it calls `CloseNativePanel(id)` -> clears the
  window's `EditorView::m_Open` (so the next reconcile won't re-dock it) and
  `RemovePanel`s it immediately (chrome updates same frame, emptied leaves collapse).
- **Empty-dock placeholder.** When the tree is a single empty leaf (all panels closed),
  `RenderNativeDockPreview` paints a centered "No panels open -- open one from the Window
  menu." hint over the dock region instead of returning blank.
- **Maximize / restore.** Double-clicking a tab maximizes that panel via
  `m_MaximizedPanelId`: `RenderNativeMaximized` paints a full-host single-panel frame +
  one-tab strip + a restore button, and `QueryNativeDockPanel` reports the maximized
  panel active at the full content rect while every other docked panel reports inactive
  (so `EditorView::BeginGUI` returns `Closed` and hides them). The restore button or a
  second double-click on the strip exits. Cleared automatically if the panel is closed /
  undocked, and on any builtin/snapshot/reset (`ResetNativeInteractionState`).
- **DPI re-layout.** Already inherent: every metric in `RenderNativeDockPreview` derives
  from `ui_scale` (`ImGui::GetFontSize()/16`) and geometry is solved fresh each frame, so
  a DPI / UI-scale change re-lays out the whole dock automatically. Split ratios are
  fractional, so they survive the change. No extra code -- documented here for closure.

## 3h. P8 -- Retire the NativeDock toggle (DONE, scoped)

P8 was deliberately scoped (the user picked the lower-risk option over a full ImGui
fallback removal). The `r.ZSlate.NativeDock` CVar is **retired**: the native dock is no
longer a separate toggle -- it is the only dock whenever the native backend is on. What
changed:

- `r.ZSlate.NativeDock` CVar + `ZSlateEditorOverlay::IsNativeDockEnabled()` deleted.
- Every former `IsNativeDockEnabled()` call site now keys off `IsNativeBackendEnabled()`:
  `DefaultLayout::OnGUI` (native vs ImGui branch), `ApplyBuiltinLayout` / `ApplySnapshot` /
  `CaptureCurrentLayout` (native-tree build/restore/capture), `EditorView::BeginGUI` (all
  three platform impls -- native panel placement), and `EditorWindow::BeginGUI` (min-size
  skip). This mirrors the native menu bar, which already keyed off `IsNativeMenuBarEnabled
  () = NativeBackend && NativeMenuBar`.

**What was deliberately NOT removed** (and why): the ImGui `DockSpace` / `DockBuilder`
authoring + stale-ini repair + the 5 `Build*Layout` builders + the `imgui_ini` snapshot
field all stay, because the `r.ZSlate.NativeBackend 0` path is still a *maintained*
full-ImGui fallback (`MenuController::OnGUI` keeps its ImGui `BeginMenuBar` branch for the
same condition). The `DockSpace` block in `OnGUI` is now reached ONLY when the native
backend is off; deleting it would have left that fallback half-broken (native menu-bar
fallback but no dock fallback). Retiring the whole ImGui editor fallback (removing
`r.ZSlate.NativeBackend` / `NativeMenuBar` and all ImGui menu+dock code) is a separate,
larger decision tracked below.

## 3i. P9 -- Retire the ImGui editor fallback (DONE)

The `r.ZSlate.NativeBackend 0` full-ImGui fallback is gone. The native RHI backend
(`BatchedUIRenderer`) is now the **sole** renderer for the menu bar, dock, and every
ZSlate panel. There is no longer any runtime switch back to ImGui-drawn editor UI.

What changed:

- **`ZSlateEditorOverlay`**: `r.ZSlate.NativeBackend` + `r.ZSlate.NativeMenuBar` CVars and
  their statics deleted. `IsNativeBackendEnabled()` / `IsNativeMenuBarEnabled()` now return
  `true` unconditionally (kept as trivial accessors so the ~25 call sites compile without a
  sweeping signature change; they document "native is the only backend"). The dev-only
  `r.ZSlate.NativeSelfTest` canary CVar is retained (still lazily registered on first call).
- **`MenuController::OnGUI`**: the ImGui `BeginMenuBar` / `BeginMenu` fallback loop is
  deleted; the menu bar is always `RenderNativeMenuBar()` and the toolbar always
  `DrawPlaybackToolbarNative()`.
- **`DefaultLayout`**: native-only `OnGUI` -- the ImGui `DockSpace` call, the
  `DockBuilder` authoring, the 5 `Build*Layout` builders (`BuildDockSpaceRoot` /
  `BuildTwoByThree` / `BuildFourSplit` / `BuildDefault` / `BuildTall` / `BuildWide`), the
  whole stale-on-disk-ini repair machinery (`hasDockingDataInMemory`,
  `isLargeSizeMismatch`, `isDockHostSizeMismatch`, `parseIniSizeValue`,
  `findIniSectionSize`, `shouldAutoRepairSavedLayout`) and the `m_MainDockingId` /
  `m_LastWindowSize` / `m_PreviousStableWindowSize` / `m_SavedLayoutStartupCheckDone`
  members are all gone. `LayoutSnapshot::imgui_ini` is removed: snapshots persist only the
  native `dock_tree_json` (+ window-open map). `LoadSnapshotFromFile` ignores `imgui_ini`
  and accepts a snapshot with either a `dock_tree` or a `windows` map; legacy ini-only
  snapshots fall back to rebuilding the Default native tree. `DockLayoutUtility.h` (the
  ImGui `DockBuilderDockWindow` helper) was deleted.
- **`EditorView` (Win/Linux/macOS) + `EditorWindow::BeginGUI`**: the backend gate is gone
  (native panel placement + no ImGui min-size constraint are unconditional).
- **Per-panel renderers**: the `SlateImGuiRenderer` "bootstrap backend" (the painter that
  drew ZSlate widgets into an ImGui `ImDrawList`) is deleted, along with the `else` fallback
  branch + `m_Renderer` member in all 14 ZSlate windows. Each panel now paints
  unconditionally through `overlay.GetRenderer()` (`BatchedUIRenderer`).

**Deliberately KEPT** (NOT ImGui-fallback, despite living in `SlateImGuiRenderer.h`):
`SlateImGuiTextMeasurer`. It is the layout-time text measurer over the **active ImGui font
atlas**, which the native `BatchedUIRenderer` also draws glyphs from -- so it stays the
single source of truth for ZSlate text metrics and is installed unconditionally by every
window via `SlateApplication::SetTextMeasurer`. The file keeps its historical name.

**Still ImGui, by design** (structural, not "the UI we replaced"): panel hosting
(`ImGui::Begin` positions each native panel at its dock-leaf rect) and ALL input
(`ImGui::GetIO`, `IsMouseClicked`, `IsKeyPressed`, mouse cursor). Removing these needs a
native windowing + input layer and is out of scope.

### 3j. P9 follow-up -- delete the dead ImGui menu + toolbar fallback (DONE)

The dead-code cluster flagged at the end of P9 has now been removed:

- `MenuController::DrawPlaybackToolbarWindow` + its ImGui draw helpers
  (`drawPlaybackToolbarButton` / `drawPlaybackToolbarBackground` /
  `drawPlaybackToolbarIcon` / `beginDisabledButton` / `endDisabledButton`) and the
  ImGui-only `PlaybackToolbarSegment` enum. The shared `PlaybackToolbarIcon` enum and the
  native `SPlaybackIcon` leaf widget stay (live toolbar path).
- The ImGui menu fallback methods orphaned when the menu-bar loop was deleted:
  `Menu::BeginMenu` / `Menu::EndMenu` (ImGui wrappers), `FileMenu::OnGUI` /
  `WindowMenu::OnGUI` (ImGui menu bodies), `WindowMenu::OnDock(ImGuiID)`, and
  `EditorUI::ShowEditorWindowMenu` (ImGui submenu builder). `Menu::OnGUI` is now a no-op
  concrete override (satisfies `EditorView`'s pure virtual; menus only contribute via
  `BuildZSlateMenu`). The native counterparts -- `BuildZSlateMenu` /
  `BuildEditorWindowZSlateMenu` -- are the sole live menu path.
- `EditorUI::DrawAxisToggleButton` (uncalled ImGui button helper) removed.

`Menu.cpp` and `FileMenu.cpp` no longer include `<imgui.h>`.

## 4. Remaining phases (TODO / optional)

| Phase | Scope |
|-------|-------|
| (none) | All planned native-dock phases (P1-P9) plus the P9 dead-fallback cleanup (3j) are landed. The only remaining ImGui is structural and intentional: a native windowing/input layer to drop `ImGui::Begin` panel hosting + input is the one larger follow-up, and is out of scope. |

## 5. Open decisions

- **Multi-viewport drag-out** (`ImGuiConfigFlags_ViewportsEnable`, dragging a panel
  to a separate OS window). ImGui gives this for free; a native dock would need
  per-panel OS windows + a second render surface. **Proposed:** drop it for V1
  (most Unity/UE users dock within one window; it is rarely used here) and revisit
  only if requested. Documented as a deliberate scope cut, not a regression.
- **Composited 3D viewports** (Scene / Game): they composite UNDER a transparent
  panel and report their work-rect to `RenderSystem`. The native host must report
  the same `ContentRect` so `PlayModeView::UpdateViewport` keeps working unchanged.
- **Tab-bar + title-bar styling**: match the current ImGui look initially to avoid
  a jarring visual diff during rollout.

## 6. Why CVar-gated / incremental

The dock layer is the editor's structural backbone; a regression makes the whole
editor unusable. Each phase above is independently buildable and the live swap is
gated, so `r.ZSlate.NativeDock 0` always falls back to the proven ImGui dock until
parity is verified interactively.
