# Changelog

All notable changes to ZEngine will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added - 2026-10-20 (editor undo/redo transaction system)
- **The editor can undo and redo for the first time.** Every scene edit (move /
  rotate / scale with the gizmo, inspector fields, reparent, create empty,
  place a prefab or a mesh, delete, component enable, GameObject active) was
  previously one-way -- a mis-drag could only be fixed by hand or by reloading
  the level.
  - `Editor/Undo/UndoSystem.{h,cpp}` -- engine-agnostic transaction stack
    (UE `FTransBuffer` + Unity `Undo` hybrid), standard library only: swap-based
    `IUndoRecord` (one `Apply()` serves undo *and* redo), `Finalize()` discards
    no-ops, coalesce keys collapse repeated snapshots, nesting transactions,
    a per-frame implicit transaction (Unity's per-event undo group), an
    `IUndoContext` for side state, redo-branch clearing, and 200-step / 256 MB
    budgets.
  - `Editor/Undo/EditorUndo.{h,cpp}` -- `EditorUndoSystem` (an `IEngineSystem`
    in the PostInit phase) owns one `UndoSystem` and wires it to the editor:
    selection is captured and restored with every step, committed edits mark the
    level dirty, undoing back to the saved state clears the dirty flag again
    (save point), the history is dropped when the level is switched or
    reloaded, and recording is suspended in play mode.
  - `Editor/Undo/EditorUndoRecords.{h,cpp}` -- the records. Components are
    snapshotted as a YAML DOM taken through the normal `Transfer()` path with a
    null `PPtr` resolver, so PPtrs round-trip as raw instance IDs and undo
    restores floats bit-exact; `TransformStateRecord` restores local TRS,
    parent, sibling index and the active flag through the hierarchy APIs;
    `GameObjectLifetimeRecord` *owns* the removed subtree, so undo puts back the
    very same objects with the same instance IDs instead of clones.
  - `EditorUndo::` facade for call sites: `ScopedTransaction`,
    `RecordComponent`, `RecordTransform`, `RegisterCreatedGameObject`,
    `DestroyGameObjects`, `Undo`/`Redo`, `CanUndo`/`CanRedo`.
  - UI: Ctrl+Z / Ctrl+Y / Ctrl+Shift+Z (`EditorInputManager`), and a real
    **Edit** menu -- `Undo <title>` / `Redo <title>` with live titles, an
    `Undo History` submenu (20 entries, each undoing N steps) and
    `Clear Undo History` (`MenuController`).
  - Console: `undo [steps]`, `redo [steps]`, `undo.history [max rows]`,
    `undo.clear`.
  - `UndoSystemSmokeTest` (`-DZENGINE_BUILD_UNDO_SMOKE_TEST=ON`): 70 headless
    checks over swapping, nesting, ordering, no-op discard, coalescing, redo
    branch clearing, implicit grouping, cancel, re-entrancy guards, budgets,
    context, listeners and titles.
  - New `ZUndo` log category.
- Supporting runtime changes:
  - `Component::PostEditUndo()` hook; `AnimationComponent` overrides it to
    re-resolve its skeleton / controller (they are only bound in
    `PostLoadResource`).
  - `Level`: `GetLoadSerial()` (bumped on load and unload, so the undo system
    can detect a reload) and `RestoreGObject()` (re-inserts a detached object
    with its original ID).
  - `RenderSwapContext::CancelDeleteGameObject()` -- the swap drain runs upserts
    before deletes, so an undo that resurrects an object in the same frame has
    to un-delete it.
  - `TransformSceneRoots`: fixed two latent bugs that corrupted the scene root
    list (re-adding the last root, and unlinking a transform that is not linked
    at all, which wiped the whole list on every `PostLoadResource`).

### Added - 2026-10-19
- **Audio system (first vertical slice)** -- the engine previously had no audio
  at all; this lands the minimum needed to play sound in a project.
  - `Runtime/Function/Audio/`: `AudioCommon.h` (dependency-free primitives),
    `AudioDecoder` (WAV / RIFF: 8/16/24/32-bit PCM + 32/64-bit float, any
    channel count, WAVE_FORMAT_EXTENSIBLE), `AudioMixer` (voice pool with
    priority stealing, buses, master, 3D distance attenuation, equal-power
    panning, linear resampling for pitch, fade in/out, tanh soft clip).
  - `IAudioDevice` platform layer: WASAPI shared-mode backend on Windows
    (float32, with a 16-bit PCM fallback when an endpoint refuses float), null
    backend everywhere else for now.
  - `AudioSystem` (an `IEngineSystem` in the Gameplay phase) owns the clip
    cache, the mixer and the device. `ZENGINE_AUDIO_DISABLE=1` keeps every API
    working with no device (headless CI), `ZENGINE_AUDIO_MASTER_VOLUME` sets
    the master gain.
  - Components: `AudioSourceComponent` (clip path, volume, pitch, loop, play on
    awake, spatial blend, min/max distance, bus name) and
    `AudioListenerComponent`.
  - New `ZAudio` log category.
  - `AudioMixerSmokeTest` (`-DZENGINE_BUILD_AUDIO_SMOKE_TEST=ON`): 30 checks
    over decode/encode, panning, attenuation, looping, voice cap, buses,
    fades and pitch. Runs headless with no audio device.

### Added - 2026-10-19 (animator state machine, PR-SK3)
- **Characters can react to gameplay for the first time.** Previously
  `AnimationComponent` mixed a fixed clip list at a static weight, always
  looping, with no speed control and no events -- there was no state concept at
  all.
  - `Runtime/Resource/ResType/Data/AnimatorController.{h,cpp}`: states (one clip
    each, per-state speed + wrap mode + events), parameters
    (Float/Int/Bool/Trigger) and transitions (conditions, crossfade duration,
    exit time, priority). Enums are persisted as `int` -- the serializer has no
    enum trait.
  - `Runtime/Function/Animation/AnimatorInstance.{h,cpp}`: the evaluator --
    per-state playhead with Loop / Once / ClampForever / PingPong, speed
    scaling, condition + exit-time + priority resolution, trigger consumption,
    once-per-pass events, and `Play()` / `CrossFade()`.
  - **It does not sample poses.** `GetBlendState()` emits at most two clips with
    weights (1-w, w) and feeds the existing
    `AnimationSystem::GetBlendStateWithClipData()` -> `Skeleton::ApplyAnimation`
    path, so the engine still has exactly one sampler.
  - `AnimationComponentRes::controller_file_path` (optional, empty in every
    pre-existing asset): empty == legacy blend-state playback, non-empty == the
    state machine drives. Schema-evolution safe via SafeBinaryRead.
- `AnimatorSmokeTest` (`-DZENGINE_BUILD_ANIMATOR_SMOKE_TEST=ON`): 50 headless
  checks -- parameter transitions, crossfade weights, back-transitions, trigger
  consumption, priority, all four wrap modes, once-per-pass events, exit-time
  gating, `Play`/`CrossFade`, misuse tolerance, and a real `.zasset`
  write/read round-trip of the controller.
- `Function/Animation/Test/CMakeLists.txt` now guards EACH test target with its
  own option instead of `return()`-ing early, so enabling one no longer
  silently disables the other.

### Added - 2026-10-19 (Animator editor panel, PR-SK3 follow-up)

- **`ZSlateAnimatorWindow`** (dock title "Animator", Window > Animator,
  Animation category): the authoring surface for `AnimatorController`, which
  until now could only be built in code. Three editable ZSlate tables over the
  live controller:
  - **Parameters** -- name, default, and a Float / Int / Bool / Trigger cycler
    (the default editor follows the type).
  - **States** -- name, clip `.zasset` path, anim->skeleton map, wrap-mode
    cycler (Loop / Once / Clamp / PingPong), speed, length in seconds, a
    default-state marker, and an event list (normalized time + function).
  - **Transitions** -- from/to state cyclers, fade duration, exit time +
    enable, priority, and per-condition parameter / mode / threshold rows.
- Toolbar **New / Open / Save** against a path box (absorbed from the UMG
  designer pattern); relative paths resolve against the project content root.
  Save goes through `AssetManager::WriteObjectToDiskThreadSafe`, so the Content
  Browser picks the asset up on the next watcher tick.
- Right-clicking a `*.controller.zasset` in the Content Browser now offers
  **"Open in Animator"**
  (`ContentBrowserHelpers::IsAnimatorControllerNode`).
- Deleting a state renumbering transitions, and deleting a parameter dropping
  the conditions that named it, are handled in the panel -- the two ways the
  index / name asymmetry in the controller schema can silently corrupt an asset.

### Added - 2026-10-19 (Animator graph view)

- The right-hand pane is now tabbed (**Graph** / States / Transitions) instead
  of squeezing three tables side by side; Parameters stays visible on every
  tab, because a transition is meaningless without the parameters it tests.
- **Graph tab** (default): a hand-painted canvas with one node per state -- name,
  clip file name (red when a state has no clip), the default-state marker, and
  arrows for transitions. Unconditioned transitions ("always take") are drawn
  green so they read differently from guarded ones.
  - **Drag a node body** to move it; **drag its bottom port onto another node**
    to create a transition (duplicates rejected). Click selects a state and
    highlights its edges; clicking empty canvas clears the selection.
  - `Auto Layout` re-arranges on a 3-column grid.
  - Node positions are persisted on the asset (`AnimatorState::pos_x` /
    `pos_y`, LOGICAL units so a layout survives a DPI change). Controllers
    written before this existed have every state at (0,0); the window treats
    "all zero" as "not laid out yet" and auto-arranges once on load.
- The canvas paints into the rect a placeholder `SBox` was arranged into, so the
  hand-painted region can never drift from the widget layout; arrows are
  `DrawConvexPoly` shafts + heads (the renderer has no `DrawLine`).

### Added - 2026-10-19 (Animator canvas: selectable / editable arrows)

- **Arrows are selectable**: click one and it turns cyan with a grab dot at its
  midpoint. Hit-testing is point-to-segment distance (arrows run at any angle),
  and nodes are tested first so an arrow passing under a node cannot steal its
  clicks. A self-transition (A -> A) is drawn as a stub under the node so it
  stays visible and clickable.
- **In-canvas transition editing**: selecting an arrow opens a strip under the
  canvas with from/to, fade, exit time, priority and conditions, plus
  `Delete (or right-click the arrow)` and `Deselect`. The strip reuses
  `BuildTransitionBody` -- the same function the Transitions tab calls per row --
  so the two editors cannot drift apart.
- **Right-click an arrow deletes it.** Dragging a new connection now also selects
  the arrow it just created, so its conditions can be set immediately.
- Selection is mutually exclusive (node vs arrow) and only a *changed* selection
  rebuilds the chrome: a rebuild would drop a text box's keyboard focus.

### Added - 2026-10-19 (Animator canvas: in-canvas state editing)

- Clicking a **node** now opens the same kind of strip under the canvas: name,
  clip `.zasset` path, anim-skel-map path, wrap cycler, speed, length in
  seconds, and the event list (time + function), plus `Set Default`, `Delete`
  and `Deselect`. It reuses `BuildStateBody` -- the function the States tab calls
  per row -- so the two editors cannot drift apart.
- `+ State` selects the new node immediately, so its clip can be typed without
  a second click.
- `Delete` is a button, not a right-click gesture: removing a state renumbers
  every transition that referenced it, which is too destructive for a bare
  right-click.
- Fix: a newly added state was left at graph position (0,0), stacking on any
  other un-arranged node and becoming unclickable (later nodes win the hit
  test). New states now drop onto the next free grid slot.

### Added - 2026-10-19 (Animator clip / anim-skel-map picker)

- `Browse` next to a state's **clip** and **anim->skeleton map** fields opens an
  asset picker over the right pane: filter box + a scrollable list of the
  project's assets + `Cancel`. Picking one writes the path and closes.
- The list comes from the **Content Browser's own asset tree**
  (`ZSlateContentBrowserWindow::GetInstance()->GetFileService()`), filtered by
  `EditorFileNode::m_AssetType` -- `"animationasset"` for a clip,
  `"animskelmap"` for the map -- so the picker shows exactly what the browser
  shows. If the tree is not built yet it scans the content root instead (then
  unfiltered), so the picker is never empty.
- Picked paths are stored **content-root-relative**, the form
  `AssetManager::loadAsset` resolves, so a controller keeps working when the
  project moves.
- The picker never calls `BuildEngineFileTree()` itself: that clears the node
  array the Content Browser holds raw pointers into (`m_SelectedNode`,
  `m_BrowsedFolderNode`) and would leave it dangling.
- The filter does not rebuild the widget tree (a rebuild drops keyboard focus);
  rows are filtered per frame by toggling `SWidget::Visibility` to `Collapsed`,
  which both `SBoxPanel` and `SScrollBox` honour in layout.

### Added - 2026-10-19 (Cross-window drag-drop: Content Browser -> canvas / Inspector)

- **Content Browser -> Animator canvas**: drag a `.zasset` onto a state node and
  an `AnimationAsset` becomes its clip, an `AnimSkelMap` its anim->skeleton map;
  onto blank canvas an `AnimatorController` opens. The hovered node is outlined
  cyan and a label follows the cursor (`-> clip`, `(unsupported here)`). Canvas
  gestures are suppressed while a drag is in flight, so a drop cannot also drag
  the node.
- **Content Browser -> Inspector fields**: the BaseRenderer section's **Mesh**
  and **Material** rows now accept drops (filtered to `meshdata` / `material`).
  The drop writes the path and runs the box's own commit handler, so it is the
  same code path as typing it.
- The Content Browser already published the payload (`FAssetDragDropOp`); this
  change adds the consumers, not a second payload type.
- Path form is now single-sourced: drags carry ABSOLUTE paths, serialized fields
  store content-root-relative, and `EditorDragDrop::MakeDroppedAssetStoredPath`
  is the one conversion every drop target uses.
- Fix (ZSlate): a cross-window widget drop could be lost to window draw order --
  the publishing router clears the process-wide channel on its own release
  frame, which may run before the target's tick. `SlateInputRouter` now keeps
  `m_LastObservedDragOp` and delivers from it. Also clears `m_DragOverTarget`
  after a drop, otherwise a repeated drag onto the same widget skipped
  `OnDragEnter` and never lit its highlight.

### Added - 2026-10-19 (Animator as drag source: state clip -> component field)

- The **clip line inside a node** is now a drag handle: press it and move and the
  canvas publishes the clip as an `FAssetDragDropOp`, which any window can drop
  (Inspector's AnimationComponent Clip row, Scene viewport, ...). A press without
  travel still just selects the node; the handle is highlighted on hover.
- Because the canvas is hand-painted it has no widget and therefore no router
  owns the op -- `EndAssetDragOut()` unpublishes it on release. (Skipping that
  would leave the process-wide channel hot: every window would keep believing a
  drag was in flight.)
- **AnimationComponent gets a dedicated Inspector section**: Controller,
  Skeleton, Clip, Anim Skel Map and Clip Length (s), each path row an asset drop
  target of the matching type. Previously the generic type-tree walk rendered
  `blend_clip_file_path` as `<array> (not editable)` -- precisely the slot a
  dragged clip needs to reach.
- New `AnimationComponent` slot accessors (Get/Set Clip, Anim Skel Map, Clip
  Length, Controller, Skeleton). They keep the parallel `BlendState` vectors
  consistent: a clip only plays together with its bone map, and `Tick()` returns
  early without length / ratio / weight, so a "clip assigned" component would
  otherwise stand perfectly still.

### Added - 2026-10-19 (drag-out generalized: any asset path row)

- **anim-skel-map can now be dragged out too.** An Animator node shows two asset
  lines -- clip and anim->skeleton map -- and both are drag handles; the node grew
  from 56 to 74 px tall to fit them. `AssetSlotPath` / `AssetChipRect` are shared
  by painting, hit-testing and drag-out, so a handle cannot carry something other
  than what it displays.
- **States tab rows are drag sources.** Both path rows (clip, anim skel map) can be
  dragged onto any asset field. The States tab and the in-canvas strip share
  `BuildStateBody`, so the strip got the same behaviour for free.
- **Inspector path rows are now two-way slots**: drop a path in, drag this one out
  (onto another object's field, ...). Same row, both directions.
- New `ZSlate::SDragSource` (the mirror of `SDropTarget`) plus
  `EditorDragDrop::MakeAssetPathDragSource` / `MakeAssetDragOperation` /
  `MakeDroppedAssetAbsolutePath`, so every source builds the SAME op and converts
  the stored relative path to the absolute one the payload contract requires. An
  empty path starts no gesture at all.
- Behaviour note: a drag inside a focused path text box now wins over selecting
  its text (the router polls `OnDragDetected` over the whole press path). Accepted
  for path rows; do not apply this wrapper to prose fields.

### Added - 2026-10-19 (packaging / AssetBundle, PR-PAK1)
- **The engine can produce a shippable build for the first time.** There was no
  pak, no manifest and no dependency pruning, so "shipping" meant copying the
  whole `Assets/` tree. `AssetBundle` was a 16-line stub whose `GetImpl()` body
  was literally `return nullptr`.
  - `Runtime/AssetBundle/AssetBundleFormat.{h,cpp}`: the `.zbundle` container --
    header (magic "ZBND", version, compression) +     length-prefixed entry table +
    16-byte-aligned blob section. Stores cooked `.zasset` FILE BYTES rather than
    re-serialising objects, so there is still exactly one binary format in the
    engine. Per-entry CRC-32 with no zlib dependency.
  - `Runtime/AssetBundle/AssetBundle.{h,cpp}`: mount (header + entry table only,
    no blob read), on-demand CRC-checked blob reads cached for the bundle's
    lifetime, `Get<T>()`.
  - `Runtime/AssetBundle/AssetBundleManifest.{h,cpp}`: the build's dependency
    table, stored as a normal `.zasset` (Unity `.manifest` equivalent).
  - `Runtime/AssetBundle/AssetBundleManager.{h,cpp}`: `IEngineSystem` that
    mounts bundles, auto-mounts their dependencies from the manifest
    (cycle-guarded), and answers "does any mounted bundle have this asset?".
  - `SerializedFile::InitializeRead()` now serves bundle-resident assets from
    memory. This single hook is what lets `loadAsset<T>()`, PPtr resolution and
    `Level::load` work unchanged with no `Assets/` tree on disk.
  - `Editor/Build/AssetBundleBuilder.{h,cpp}`: roots (level / world / prefab,
    or explicit) -> BFS over AssetRegistry's dependency graph -> one bundle per
    root plus a `shared` bundle for anything >=2 roots reach -> cooked bytes
    when available, source otherwise -> bundles + manifest under
    `<project>/Build/<Platform>/`.
  - Entry points: Build menu `Build Asset Bundles (Standalone/Android/WebGL)`
    and console command `bundle.build <platform>`.
- New `ZPak` log category.
- `AssetBundleSmokeTest` (`-DZENGINE_BUILD_ASSET_BUNDLE_SMOKE_TEST=ON`): 25
  headless checks -- pack, mount, dependency auto-mount, byte-identical blobs,
  **loading an asset after its source file has been deleted** (the whole point),
  CRC corruption detection, unmount.

### Added - 2026-10-19 (FBX skinning import, PR-SK2)
- **The engine can now author a skinned character.** Previously `MeshImporter`
  cleared `MeshData::bind` and never filled it, nothing ever wrote a
  `SkeletonData`, and `AnimationImporter.{h,cpp}` were 0 bytes -- there was no
  way to get skeletal data into the engine at all.
  - `Editor/AssetPipeline/FbxSkeleton.h`: shared FBX bone extraction (one
    numbering for both importers, parents resolved to the nearest bone
    ancestor, bind pose from the node's local transform).
  - `Editor/AssetPipeline/FbxAnimation.h`: bakes every FBX animation stack into
    an `AnimationAsset` + `AnimSkelMap` pair, at the file's own FPS (or an
    override), with a frame cap.
  - `MeshImporter` now writes `Models/x.skeleton.zasset` (SkeletonData), fills
    per-vertex `bind[]` (top-4 influences via
    `AnimationSkinning::PackVertexInfluence`), records the skeleton in
    `AssetMetadata::dependencies`, and bakes clips alongside the mesh. Skinned
    meshes keep their authored coordinates (no recentring / Y flip) so the mesh
    stays inside its own skeleton.
  - `AnimationImporter` + `AnimationImporterSettings` implemented (not
    registered: `FindImporter` keys on extension and `.fbx` belongs to
    `MeshImporter`).
- `AnimationSkinning::PackVertexInfluence()` (raw (bone, weight) list -> 4
  slots, renormalised, shifted to GPU palette slots) + smoke-test scenario S12.

### Fixed - 2026-10-19 (animation runtime crashes)
- `AnimationSystem::GetBlendStateWithClipData()` dereferenced
  `blend_masks[0]` unconditionally: **any** animation setup without blend masks
  crashed every frame. Masks are now optional (no mask = every bone takes every
  clip), and missing clip / skel-map assets are skipped with a `ZAnimation`
  error instead of dereferencing null.
- `AnimationSystem::TryLoadAnimationSkeletonMap()` always returned `nullptr` on
  a first lookup (emplaced the map, then returned null), so every clip silently
  lost its bone mapping.
- `AnimationLoader::LoadAnimationClipData()` dereferenced a null
  `AnimationAsset` when a clip was missing or renamed.

### Added - 2026-10-19 (skeletal skinning, PR-SK1)
- **Skinning core** `Runtime/Function/Animation/Skinning.{h,cpp}`: topological
  sort of bone hierarchies, bind-pose global matrices and their inverses,
  skinning-palette build, time-based clip sampling with `Clamp` / `Loop` /
  `PingPong` wrap, N-clip weighted pose blending, delta composition (parent
  space, matching `Node`'s Translate/Rotate/Scale semantics) and CPU skinning of
  positions / normals.
- **`Skeleton` fixes**: no longer bails out silently on unordered skeletons;
  inverse bind matrices are derived from `binding_pose` instead of the never-written
  `RawBone::tpose_matrix`; `ApplyAnimation()` now blends every clip in
  `BlendStateWithClipData` (previously only clip 0 with a hard-coded weight) and
  tolerates a null `AnimSkelMap`; `AnimationComponent` no longer dereferences
  `[0]` on an empty clip list (crash on load) and no longer dereferences a null
  skeleton.
- **DX12 GPU skinning**: `RenderResourceDx12` gained a real skinned path
  (per-vertex joint-binding SRV + `GpuMesh::enable_vertex_blending`) instead of
  the "skinned mesh uses rigid upload path for now" fallback; `mesh.vert.hlsl`
  now skins in the vertex shader, mirroring `mesh.vert` (Vulkan). DX12 structured
  buffer SRVs can now carry an explicit `RHIDescriptorBufferInfo::stride`
  (previously an 80-byte guess that mis-sized the 32-byte joint-binding buffer).
- New `ZAnimation` log category.
- `AnimationSkinningSmokeTest`
  (`-DZENGINE_BUILD_ANIMATION_SMOKE_TEST=ON`): 36 headless checks over hierarchy
  sorting, bind pose, rest pose, rotation, blending, influence normalisation,
  sampling/wrap, GPU palette layout and normals.

### Fixed - 2026-10-19
- **README**: the Key Features list still advertised Lua and C# scripting. Both
  backends were removed (see AGENTS.md 2.7): Lua survives only as a loader
  stub and C# only as memory-profiler hooks. TypeScript / puerts is the single
  supported scripting path.

### Added - 2026-02-11
- **Memory Profiler Enhancements**
  - Added C# memory profiling support (`CSharpMemoryProfiler`)
  - Enhanced Lua memory profiling with better tracking and statistics
  - Improved UI for memory profiler with detailed memory breakdown
  - Added memory tracking utilities in `Utility.h`
  - New memory profiler APIs in ZEngineApi for external integration
  - Added EASTL visualization support (EASTL.natvis)

- **Console System**
  - New ConsoleWindow component for Editor (`engine/Source/Editor/editor_window/ConsoleWindow/`)
  - Integrated console manager functionality

- **File System**
  - Extended FileSystem API with new file operations
  - Windows-specific file system enhancements in `LocalFileSystemWindowsShared`

- **Scripting**
  - Lua loader improvements with better module management
  - Puerts integration enhancements for JavaScript/TypeScript support
  - Native scripting API additions (`PuertsNative.h`)
  - ScriptObject and ObjectPool for better script object lifecycle management
  - New BackendLua implementation for Lua scripting backend

- **Build System**
  - Added CommonPCH (precompiled headers) for faster compilation
  - New `generate_engine_log.cmake` for automated log generation
  - Improved CMake configuration for output directories
  - Better dependency management for 3rd party libraries

- **Documentation**
  - Added `LINK_ORDER_MEMORY_MANAGER.md` documenting memory manager linking strategies

### Changed - 2026-02-11
- **Profiler Refactor**
  - Reorganized profiler architecture with better separation of concerns
  - Updated ProfilerRuntime with new profiling hooks
  - Enhanced MemoryProfiler base class for extensibility

- **Resource Management**
  - AssetManager API improvements
  - ConfigManager refactoring for better configuration handling
  - AssetBundle optimizations

- **Serialization System**
  - Enhanced TypeTree cache system
  - Improved serialize traits for better type handling
  - JSON serialization improvements

- **Editor Updates**
  - EditorApplication initialization improvements
  - Better resource management in editor
  - Improved project window file handling

- **Code Quality**
  - Removed deprecated container implementations (HashSet, HashMap alternatives)
  - Cleaned up unused includes and dependencies
  - Better EASTL integration
  - Reduced PCH overhead by moving to CommonPCH

### Fixed
- Memory manager linking order issues (see `doc/LINK_ORDER_MEMORY_MANAGER.md`)
- VirtualFileSystem path handling
- Encoding utilities edge cases

## [Previous Changes] - 2026-01-01 to 2026-02-10

### Added
- **Serialization System** (Multiple commits Jan 1-30)
  - Complete binary serialization/deserialization framework
  - TypeTree-based reflection system
  - JSON serialization support
  - Type registration and metadata system

- **Resource Management** (Jan 2, 21)
  - ResourceManager implementation
  - AssetManager for asset loading and caching
  - Asset bundle support

- **Profiling Infrastructure** (Jan 10, 18)
  - Memory profiling foundation
  - Performance profiling utilities
  - Memory analysis tools

- **File Operations** (Jan 14, 17)
  - Binary file read/write support
  - File operation utilities
  - Virtual file system improvements

- **Third-party Libraries** (Jan 2)
  - Integrated xxHash for fast hashing

### Changed
- Repository reinitialized with clean history (Jan 22)

---

## Version History Notes

### Migration from Previous Repository
On 2026-01-22, the repository was reinitialized with a clean history. Previous history before this date has been archived.

### Commit Conventions
- Commits in Chinese indicate internal development work
- Major features are documented in this CHANGELOG
- See git history for detailed commit messages

---

## Contributing
When making changes:
1. Update this CHANGELOG under the [Unreleased] section
2. Follow the format: Added/Changed/Deprecated/Removed/Fixed/Security
3. Include relevant file paths or component names
4. Reference issue numbers where applicable

