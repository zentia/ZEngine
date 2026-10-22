# Changelog

All notable changes to ZEngine will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added - 2026-10-22 (Nanite: the main camera's GBuffer pass draws Nanite's clusters)

Nanite's cull runs at `RenderNaniteCmd`, before any pass is open, so it can only
write indirect commands; the draw has to happen while somebody's GBuffer pass is
open. That somebody is now the DX12 main camera.

* **`MainCameraRp1Pass::SetNaniteRenderer`** — fed by `DesktopRenderPipelineModule`
  from the `NaniteRenderPass` slot (DX12 branch: it is the only path with an RP1
  pass). `DrawGBufferPass` hands Nanite the live GBuffer render pass + framebuffer
  and, right after the **game viewport's** mesh draws, appends
  `DrawClustersIndirectInPass`. Game viewport only: culling uses
  `GetRenderCamera(ViewportType::game)`, so reusing its visible set in the editor
  viewport would draw a scene culled against the wrong frustum. Viewport and
  scissor stay whatever `DrawMeshGbuffer` installed — Nanite only appends a draw.
* **The injection is deliberately persistent across frames.** Nanite's own
  `Render()` runs earlier in the frame than this pass; seeing "already injected"
  there is what makes it skip its offscreen target, which is a
  `BeginSingleTimeCommands` + submit + wait — a full GPU sync every frame. The
  cost is that the first frame may draw into the unread offscreen target once.
  It is released in `Shutdown()` (the pass and framebuffer it points at are about
  to be destroyed) and refreshed on resize via
  `DX12MainCameraPass::UpdateAfterFramebufferRecreate` → `RefreshNaniteInjection`.
* **Verification caveat, stated rather than papered over:** `Passes/*.cpp` is
  excluded from the Apple build, so the edited TU is not part of this machine's
  build. `MainCameraRp1Pass.cpp` was instead type-checked explicitly with the
  project's own flags (`compile_commands.json` + `-fsyntax-only`), with a
  deliberately broken copy as a negative control. The
  `DesktopRenderPipelineModule.cpp` change sits inside the `_WIN32` branch and is
  unverified locally — it needs a Windows build.

### Added - 2026-10-22 (NaniteGPUCullingSmokeTest: 85 checks, the release path pinned)

Two more checks: releasing the injection clears it, and Nanite then draws into
its own offscreen GBuffer again. That is exactly what `Shutdown()` relies on —
release the stale pass *and* keep drawing. Mutation: making
`SetRasterRenderPass(nullptr, nullptr)` a no-op → 4 failures.

### Added / Fixed - 2026-10-22 (Nanite: the raster writes a real GBuffer)

The raster target used to be a single colour image that the fragment shader
filled with the LOD debug colour. That was enough to prove "triangles landed",
but useless to the frame loop: whatever Nanite draws has to come out of the
GBuffer pass already in the shape the deferred lighting pass reads.

* **`nanite_cluster.frag` now emits the three GBuffer targets** through
  `EncodeGBufferData` (`shader/include/gbuffer.h`): world normal,
  metallic/specular/roughness + `SHADINGMODELID_DEFAULT_LIT`, base colour. Two
  honest compromises, both written down in the shader: the normal is the flat
  face normal from `cross(dFdx(in_world_position), dFdy(in_world_position))`
  and the base colour is still the per-cluster debug tint — `NaniteMeshResource`
  uploads positions and indices only, so real normals and material ids have to
  come from the cluster builder first.
* **The offscreen target is GBuffer-shaped now**: three colour attachments
  (`R8G8B8A8_UNORM`, `R8G8B8A8_UNORM`, `R8G8B8A8_SRGB`) plus `D32_SFLOAT`, the
  same set `MainCameraFramebufferResources::SetupAttachments` builds. The
  pipeline's blend state covers three attachments and the framebuffer holds four
  views. A PSO built this way can be injected into the main camera's GBuffer
  pass; one built against a single target cannot.
* **The clear values changed from dark grey to all zero**, matching
  `MainCameraRp1Pass::DrawGBufferPass` field for field. Zero is also the only
  safe "nothing here" marker: GBufferC is an SRGB attachment, so a clear of 0.05
  gets linear→sRGB encoded into byte 62 and reads back as "something was drawn".
  (The test found this — the offscreen probe counted every cleared pixel as
  covered.)

### Added - 2026-10-22 (NaniteGPUCullingSmokeTest: 83 checks, GBuffer content pinned)

Counting pixels no longer proves anything: writing one attachment and writing
three both "have pixels". The test now decodes what was written.

* `ProbeGBuffer` reads the three targets back and checks, per covered pixel:
  GBufferA decodes to a +Y world normal (the test mesh is a horizontal plane and
  the camera is above it), GBufferB.a decodes to `SHADINGMODELID_DEFAULT_LIT`,
  GBufferC is non-zero, and every pixel Nanite never touched is still the
  caller's clear colour.
* The caller-owned target became a GBuffer (three colour + depth) so the
  injection path is asserted on **content** too, and a fourth criterion was
  added: the caller's 16×16 viewport must be *partly* covered. The bounding-box
  check alone could not see a hijacked viewport — the caller's 16×16 scissor
  clips the enlarged geometry — but the fill count can (184 covered pixels
  becomes 256, i.e. every pixel).

Mutation-tested six ways: `cross` operands swapped, so the normal flips to −Y (2
failures); shading model id written as 0 (3); GBufferC blanked after the encode
(2); the offscreen clear changed back to dark grey (4); the injected draw
grabbing the viewport (1). **Not caught, written down rather than assumed
covered:** dropping the third blend-state entry. Metal leaves pipeline colour
attachment descriptors the pipeline state never touches at their default
`MTLColorWriteMaskAll`, so GBufferC is still written and the test stays green;
this one can only be pinned on a backend that reads the write mask strictly.

### Added / Fixed - 2026-10-22 (Nanite: the raster can draw into a caller-owned pass)

The offscreen target proved Nanite *can* draw, but a frame loop cannot use it:
`UpdateNanite` runs before any render pass is opened, so the only thing Nanite
could do there was open a pass of its own on a one-shot command buffer, submit
and wait — a full stall every frame, into a target nobody reads. Drawing has to
happen while somebody else's pass is open.

* **`NaniteRenderer::DrawClustersIndirectInPass(command_buffer)`** — the
  injection entry point. It appends the one `CmdDrawIndexedIndirect` into the
  render pass handed to `SetRasterRenderPass` and touches nothing else: no
  `CmdBeginRenderPassPFN`, no `CmdEndRenderPassPFN`, no `CmdSetViewport` /
  `CmdSetScissor`. Those belong to whoever opened the pass, and grabbing them
  would silently change every draw queued after Nanite's in the same pass.
* **The command buffer is an explicit argument on purpose.** Nanite cannot tell
  whether the caller is really inside the pass: `GetCurrentCommandBuffer()` is
  null on the headless, editor-offscreen and pre-`PrepareBeforePass` paths, so
  using it as the predicate would read "not in a pass" as "in a pass". Passing a
  buffer *is* the caller's declaration that the pass is open; `nullptr` draws
  nothing.
* **`DrawClustersIndirect()` is now the offscreen path only**, and returns
  immediately when a pass has been injected — otherwise every frame would draw a
  second copy into the unread offscreen target on top of the injected one.
* `RecordClusterDraw` holds the bind-pipeline / bind-descriptor-set /
  bind-index-buffer / draw sequence, shared by both paths so they cannot drift.

### Added - 2026-10-22 (NaniteGPUCullingSmokeTest: 69 checks, injection semantics pinned)

(Superseded later the same day — 83 checks, and the caller target is a GBuffer;
see the GBuffer entries above.)

The test now builds its own 32×32 target (colour + depth + render pass +
framebuffer), clears it to black, sets a **16×16 viewport** in the corner, and
lets Nanite draw into it. Three assertions, each of which rules out a different
"looks right" implementation:

* covered > 0 — rules out the injection being ignored (or falling back to the
  offscreen target, which the caller never reads);
* every covered pixel inside the 16×16 viewport — rules out Nanite opening the
  pass itself, which would install a full-size viewport and fill the target;
* a control run with the injection refused (pass without a framebuffer) leaves
  the caller's target black.

Plus: while a pass is injected, `DrawClustersIndirect()` must leave the
offscreen target untouched.

Mutation-tested six ways: injected draw grabs the viewport (1 failure),
offscreen path ignores the injection (1), render pass without a framebuffer
accepted (2), `drawCount = 0` (2). **Not caught, and written down here rather
than assumed covered:** (a) dropping the `external_render_pass == nullptr` guard
in `DrawClustersIndirectInPass` — `EnsureRasterPipeline` refuses first, so the
guard is defensive only; (b) `DrawClustersIndirectInPass` calling
`CmdEndRenderPassPFN` and so closing the caller's pass — Metal treats a repeated
`endEncoding` as a no-op, so the test stays green, but Vulkan and DX12 treat an
unbalanced end as an error.

### Added / Fixed - 2026-10-21 (Nanite: the raster half, so the culled clusters are actually drawn)

The GPU cull produced indirect draw commands that **nothing ever consumed**.
`SetRasterRenderPass` had zero callers anywhere in the tree, so
`EnsureRasterPipeline` never succeeded and `DrawClustersIndirect` always took its
no-op branch: the cull ran on the GPU and then no triangle was ever drawn.

* **`NaniteRenderer`** — an offscreen raster target (colour + depth image, view,
  render pass, framebuffer) that Nanite owns when nobody hands it a pass, created
  lazily at the size `SetRasterTargetSize` asks for (or the swapchain extent, or
  512² when there is no swapchain). `DrawClustersIndirect` opens and closes that
  pass itself, falls back to `BeginSingleTimeCommands` when there is no frame
  command buffer, and issues the one `CmdDrawIndexedIndirect`.
  (2026-10-22: the injected-pass case moved out into
  `DrawClustersIndirectInPass` — see the entry above.)
* **The injection point is deliberately two-argument**: `SetRasterRenderPass(pass,
  framebuffer)` with both non-null means "the caller is already inside this pass"
  (how the DX12 main camera GBuffer will plug in); either one null drops back to
  the offscreen target. Passing a render pass with no framebuffer is silently
  ignored rather than guessed at.
* **`DestroyGPUResources`** now releases the offscreen target too. Pipelines,
  layouts and descriptor sets still leak — the RHI has no `DestroyPipeline` /
  `DestroyPipelineLayout` / `DestroyDescriptorSetLayout` entry point at all, and
  that gap is now written down in the source instead of being implied.

### Fixed - 2026-10-21 (Metal RHI: two more defects, both found by drawing a triangle)

* **`stencilAttachmentPixelFormat` was set for depth-only attachments.**
  `D32_SFLOAT` has no stencil component, so Metal aborts at pipeline creation
  with `MTLPixelFormatDepth32Float is not stencil renderable`. Only formats with
  an actual stencil component get it now; the rest get `MTLPixelFormatInvalid`.
  Never hit before because every Metal pipeline in the engine was a UI pass.
* **The front-face winding was mapped the wrong way round.** Metal decides
  winding in framebuffer space (origin top-left, y down) while its NDC has y up,
  so the NDC→framebuffer transform flips the winding. DX12
  (`FrontCounterClockwise`) and Vulkan (`VK_FRONT_FACE_COUNTER_CLOCKWISE`) are
  both defined in NDC terms, so `RHI_FRONT_FACE_COUNTER_CLOCKWISE` has to map to
  `MTLWindingClockwise` to mean the same thing. Empirically: a plane whose normal
  is `+Y`, viewed from above, is CCW in NDC — it was culled entirely (0 pixels)
  under the old mapping and draws 253 pixels under the new one. Every scene pass
  in the engine asks for `COUNTER_CLOCKWISE` + back-face culling, so they were
  all going to cull their front faces on Metal.

### Added - 2026-10-21 (NaniteGPUCullingSmokeTest: it now proves pixels, not just counters)

59 checks (was 52). The new section builds a 64×64 offscreen target on a
headless `MetalRHI`, dispatches the cull, draws, then copies the colour target
back with `CmdCopyImageToBuffer` and counts pixels that the clear colour cannot
explain — plus a control run at the *same* vantage point with the camera turned
away, which must leave the target at the clear colour. Without the control, "the
target has pixels" could just be an uncleared target.

Mutation-tested six ways: winding mapping reverted (1 failure), `drawCount = 0`
(1), descriptor set never bound (1), colour attachment not cleared (1), stencil
format reverted (aborts on Metal's validation). **Not covered:** dropping the
`CmdSetViewport` / `CmdSetScissor` calls is not caught on Metal, whose default
viewport happens to equal the attachment size; those two lines are required by
the declared dynamic state on the other three backends and are commented as such.

### Added / Fixed - 2026-10-21 (Nanite: the cull really runs on the GPU now)

Nanite had never executed a single instruction on a GPU. `NaniteGPUResources`
was eight `void*` members, `UpdateGPUBuffers` / `PerformGPUCulling` were two
TODOs, and the `use_gpu_culling` config flag was printed and nothing else — the
whole chain (cluster build, hierarchy, per-cluster culling, batch assembly) ran
on the CPU, and there was no GPU version of the per-cluster culling at all.

* **`Nanite/NaniteGPUData.h`** — the CPU↔shader byte contract. `NaniteGPUCluster`
  (80 B), `NaniteGPUInstance` (176 B), `NaniteCullConstants` (208 B),
  `NaniteCullCounter` (16 B), every one pinned by `static_assert` on `sizeof` and
  `offsetof`. Vertices and indices are **one big array for the whole scene**, so
  index values are already global vertex indices and `vertexOffset` is always 0 —
  that sidesteps the one thing the four backends disagree about (`gl_VertexIndex`
  and baseVertex). `NaniteGPUPackMatrix` transposes explicitly: `Matrix4x4` is
  row-major, a GLSL `mat4` in a storage buffer is column-major, and a plain
  `memcpy` gives `Mᵀ·p` — wrong positions, no error.
* **`engine/shader/glsl/nanite_cluster_cull.comp`** — one thread per cluster,
  `local_size_x = 64`. Culled clusters write an `indexCount == 0` empty command
  rather than "skipping", so `drawCount` stays the CPU's cluster-count upper
  bound and the CPU never has to read the count back (readback would make the
  whole GPU-driven chain synchronous). `LodKeepsCluster` mirrors
  `NaniteCullingSystem::SelectLOD` + `IsClusterVisible` line by line.
* **`engine/shader/glsl/nanite_cluster.{vert,frag}`** — rasterisation with **no
  vertex attributes**: positions come from a storage buffer via `gl_VertexIndex`,
  the cluster id from `gl_InstanceIndex`. (DX12's `SV_InstanceID` does not
  include `StartInstanceLocation`; that backend has to add it back — the shader
  says so.)
* **`NaniteRenderer`** — the eight `void*`s became real `RHIBuffer*` +
  `RHIDeviceMemory*` pairs (grow-only, host-visible), plus a descriptor set, a
  compute pipeline and a lazily-built raster pipeline. `Render()` ends with
  `PrepareGPUData` → `DispatchClusterCulling` → `DrawClustersIndirect`. When
  there is no frame command buffer (headless, editor offscreen update) the
  dispatch falls back to `BeginSingleTimeCommands` so it still runs.
* **`r.Nanite.GPU`** — a live CVar, wired through
  `NaniteRenderPass::ApplyConsoleVariables`.
* **`NaniteGPUCullingSmokeTest`** (52 checks) — see the entry below.

### Added - 2026-10-21 (NaniteGPUCullingSmokeTest: the test that can prove the kernel ran)

A GPU cull that silently produces zeros looks exactly like a GPU cull that never
ran. This test is what separates them. On Apple it creates a real `MetalRHI`
headlessly (`Initialize` needs only an `MTLDevice` — no window, no
`CAMetalLayer`), uploads the packed scene, builds the compute pipeline,
dispatches, and reads the counter, the visible list and the indirect draw
commands back:

* `visible + culled == cluster_count` — every thread must land in exactly one
  bucket. A kernel that never ran reports `0 + 0`.
* culling and LOD off ⇒ `visible == 2 × leaf_count`, exactly.
* frustum culling on ⇒ **not one** cluster of the instance behind the camera
  produces a draw command; turning the camera away leaves nothing at all.
* a huge LOD error budget keeps only LOD-0 clusters; an impossibly tight one
  keeps every leaf.
* the tail slots the kernel never writes (group-count rounding) are cleared
  before the next dispatch.

Two more sections run everywhere, including CI on Windows with no GPU: the
C++/GLSL byte contract, verified by reading the three shader sources as text
(field order, `binding = N` slots, `local_size_x`), and the
`NaniteBuildSceneGPUData` packing (contiguous per-instance ranges, global index
values, the column-major transpose).

Five mutations were used to confirm it actually goes red: dropping the
workgroup-size read (7 failures), removing the compute-encoder creation from
both the pipeline and the descriptor-set path (9), removing the command-buffer
clear (1), dropping `+ vertex_base` from the packed indices (1), and swapping
two GLSL binding slots (5).

### Fixed - 2026-10-21 (Metal RHI: five more defects, all of them found by that test)

* **The workgroup-size read in the previous entry never worked.** It ran *after*
  `spirv_cross::CompilerMSL compiler(std::move(words))`, and a moved-from vector
  is empty — the scanner found nothing and returned `(1,1,1)`, so a
  `local_size_x = 64` kernel still ran one thread. It now reads the size before
  the move. This one is the reason the entry above claims a fix that did not
  take effect.
* **Compute descriptor sets were bound into a void.** `CmdBindDescriptorSetsPFN`
  guarded on `computeEncoder != nil`, but only `CmdDispatch` ever created the
  encoder — and binding happens *before* the dispatch, so every compute buffer
  binding was dropped. `BeginComputeEncoder` now opens it on demand from
  `CmdBindPipelinePFN` and `CmdBindDescriptorSetsPFN` as well.
* **`EndSingleTimeCommands` did not wait.** Vulkan does `vkQueueWaitIdle` and
  DX12 does `Signal` + `WaitForFences`; Metal only committed and deleted. Any
  caller that maps and reads right after `EndSingleTimeCommands` (PickPass does
  exactly that) was reading a buffer that had not finished. It now calls
  `waitUntilCompleted` and also closes a stray render encoder, which otherwise
  made the commit fail outright.
* **`GetDescriptorPoor()` returned `nullptr` forever** on Metal — the member was
  never assigned. `Initialize` now creates the pool.
* **`DispatchClusterCulling` cleared only `[0, cluster_count)` of the command
  buffer** — the same range the kernel already overwrites — while the slots that
  nobody writes (the group-rounding tail) were left stale. It now clears the
  whole capacity.

### Fixed - 2026-10-21 (Metal RHI: the four defects that made any GPU-driven pass impossible)

Nanite needs compute dispatch, storage-buffer bindings and indirect draws. On
Apple, Metal is the only backend that compiles at all (the build excludes
`Interface/Vulkan` and `Interface/DX12` because no Vulkan headers exist here), so
its state decides whether a GPU-driven Nanite can work on this host. Four of its
paths were wrong, all of them silently:

* **`CmdDispatch` used `threadsPerThreadgroup = (1,1,1)` regardless of the
  kernel.** A `layout(local_size_x = 256)` kernel therefore ran one thread per
  group on Metal and 256 on Vulkan — same shader, different behaviour. The size
  is now read at pipeline creation: from the SPIR-V `OpExecutionMode`
  (`LocalSize` 17 / `LocalSizeId` 38) for compiled shaders, and from
  `[[threads_per_threadgroup(...)]]` for MSL handed in through
  `CreateShaderModuleFromSource`. `CmdDispatchIndirect` picks it up too.
* **`CmdDispatch` ended the compute encoder after every dispatch.** Any subsequent
  `CmdBindDescriptorSets` or second dispatch then found
  `computeEncoder == nil` and did nothing. The encoder's lifetime now belongs to
  `EndCommandBuffer` / `CmdBeginRenderPassPFN` / `QueueSubmit`, and
  `CmdBeginRenderPassPFN` closes it first (Metal forbids two live encoders on one
  command buffer — dispatch-then-draw is exactly Nanite's shape).
* **Compute descriptor buffers were bound at the wrong index.**
  `CreateShaderModule` gives SPIRV-Cross `msl_buffer = slot + 8`, but
  `CmdBindDescriptorSetsPFN` bound at `slot`, so every compute storage buffer
  landed on the wrong `[[buffer(n)]]` and kernels read garbage.
* **`CreateBufferAndInitialize` allocated `datasize` bytes instead of `size`.**
  `newBufferWithBytes:length:` was handed `datasize`, so `size=4096/datasize=16`
  produced a 16-byte buffer on Metal while Vulkan produced 4096. It now always
  allocates `size` and copies `min(datasize, size)`, staging through a blit
  encoder when the storage mode is device-private.

*Caveat, discovered one commit later:* the first bullet did not actually take
effect — the SPIR-V scan ran after `words` had been moved into SPIRV-Cross, so it
always returned `(1,1,1)`. See the "five more defects" entry above.

`CmdDrawIndexedIndirect` was **not** changed: `firstIndex` is deliberately not
applied on the CPU. The GPU reads it out of the indirect buffer into
`MTLDrawIndexedPrimitivesIndirectArguments::indexStart`, whose unit (index
elements) already matches Vulkan's `firstIndex` and D3D12's
`StartIndexLocation`. A comment now says so, because the obvious "fix" of
mirroring `CmdDrawIndexedPFN`'s `firstIndex * index_size` would double-count.

### Added / Fixed - 2026-10-21 (Nanite: wired into the frame loop, and it can finally see a scene)

Phase 31 left Nanite working but uncalled: `NaniteRenderer` had no way to see a
scene (`RegisterInstance()` had **zero callers** in the whole repo),
`NaniteRenderPass` was never constructed anywhere, and its `Draw()` was an empty
TODO behind a null check on a renderer nobody ever set. There was no pipeline
hook, no frame command and no CVar.

* **`NaniteRenderer::UpdateFromScene(scene, render_resource)`** turns a
  `RenderScene` into instances + clusters. Geometry comes from
  `RenderResourceBase::FindMeshGeometry()` (`CpuMeshGeometry`) — the same source
  Lumen's distance field uses, because `RenderMeshData` is dropped the moment
  the GPU buffers are filled. Entities whose asset was never uploaded are
  skipped, not crashed on.
* **A scene-topology hash decides whether to rebuild.** It covers
  `(instance_id, mesh_asset_id)` pairs only: transforms are excluded (they change
  every frame and do not need a rebuild — they get pushed into
  `transform` / `prev_transform` instead), and so are entities without CPU
  geometry (otherwise an asset finishing its upload one frame late would look
  like a topology change). `clear()` resets the hash, or the next call takes the
  "transforms only" branch against an empty instance list and Nanite goes
  silently dead.
* **Cluster building is once per mesh asset.** `EnsureMeshResource` returns
  early when `NaniteClusterManager` already knows the id — the builder is a
  recursive spatial bisection, running it twice on one asset is pure waste.
* **`NaniteRenderPass` mirrors `LumenRenderPass`:** it owns its renderer, reads
  its config from CVars, initialises lazily in `PreparePassData` (the render
  resource often does not exist yet at `Initialize` time), and exposes
  `Render(scene, camera, dt)` + `GetRenderBatches()` / `hasVisibleClusters()`.
  The phantom `Draw()` (a virtual that `RenderPassBase` does not declare and
  nothing calls) is gone.
* **Wiring, following the Lumen precedent:** `RenderPipelineBase::UpdateNanite`
  + an `m_NanitePass` slot, `RenderNaniteCmd` in the frame command list (right
  after `RenderLumenCmd`, before `RenderPreparePassDataCmd`), and the pass is
  constructed in `DesktopRenderPipelineModule::Setup` **before the Apple early
  return**. There is no `Z_HAS_NANITE`: Nanite is never stripped from ZRuntime.
* **`r.Nanite.*` CVars** (`Enable`, `LOD`, `Culling`, `MaxScreenError`,
  `LodBias`), re-applied every frame. Disabling Nanite now clears the previous
  frame's batches instead of leaving them behind.
* `NaniteInstance`'s `instance_id` / `mesh_id` / `transform` / `prev_transform`
  had no default member initialisers — a default-constructed instance was
  garbage.
* New `NaniteSceneWiringSmokeTest` (50 checks), enabled in `smoke_tests.yml`.

### Added / Fixed - 2026-10-21 (Nanite: culling that culls, and a LOD that points the right way)

Every meaningful branch in `NaniteCullingSystem` was a stub:

* **`FrustumCull()` was `return true;` with two TODOs inside** — nothing was ever
  culled. It was also handed a **default-constructed box**
  (`AxisAlignedBox instance_bounds;  // 需要从mesh资源获取`), so even a working
  test would have had nothing to test. `NaniteInstance` now carries
  `local_bounds`, and `NaniteRenderer::RegisterInstance` fills it from the mesh
  resource when the caller leaves it empty.
* **`ExtractFrustumPlanes()` used the Ogre / column-major form** (`m[col][row]`,
  vector on the left) on matrices that are **row-major, `m_Mat[row][col]`,
  `clip = M * v`**. Six planes, all wrong. It was private with zero callers,
  which is why nobody noticed. Now `Left = row3 + row0`, `Right = row3 - row0`,
  `Bottom = row3 + row1`, `Top = row3 - row1`, `Near = row3 + row2`,
  `Far = row3 - row2`.
* **`FrustumCull` and `OcclusionCull` had opposite conventions** in the same
  file: one returned "visible", the other "culled". Both now return
  **true = culled**, matching their names.
* **`SelectLOD()` moved the level the wrong way** — small screen error asked for
  a *finer* level, large error for a *coarser* one, so the farther away
  something was, the more detail it demanded. It also divided by a distance that
  can be zero, and read the member `m_Config` instead of its own `config`
  argument.
* **There was no cluster-level culling at all**, and `BuildRenderBatches()`
  emitted batches with `cluster_count = 0` — the only thing it ever produced.
  It now walks the leaf clusters of each visible instance (per-cluster frustum
  test + LOD test), groups the survivors into one contiguous range per instance,
  and emits no empty batches. `PrepareRenderData` and `BuildRenderBatches` were
  both writing into the same vector, with the second one `clear()`ing it first —
  the first one's output was discarded every frame.

`IsClusterVisible()` is new and is deliberately modest: **without a mesh
simplifier there is no coarser-but-cheaper representation**, so the only honest
LOD is dropping clusters whose projected error is under the budget — the hole
they leave is sub-pixel. Internal hierarchy nodes are never submitted, because
their geometry window is just the aggregate of their subtree.

Also: `NaniteClusterManager` gained `GetMeshResource` / `GetClusterCount` /
`SetVisibleClusters` (its `visible_clusters` field had never been written), and
registered meshes now count as loaded — with no streaming, registering a mesh
means it is resident, and `IsClusterLoaded` used to return false for everything.

**Test**: `NaniteCullingSmokeTest` — 52 checks, 5 consecutive green runs
(`-DZENGINE_BUILD_NANITE_CULLING_SMOKE_TEST=ON`). It builds the view/projection
by hand so the plane extraction is pinned independently of `RenderCamera`, and
checks inside / outside / straddling for both points and boxes. Reverting
`FrustumCull` to `return true;` fails 7 of the 52. ctest 20/20 on Apple.

### Added / Fixed - 2026-10-21 (Nanite: the cluster builder produced nothing)

`NaniteClusterBuilder` did not build a coarse approximation of the mesh — it
built nothing at all. `Nanite` has **zero callers outside its own directory**, so
none of this had ever been executed. Four independent defects:

* **`BuildClustersFromMesh()` cleared `vertex_data`/`index_data` at the top and
  never wrote them again.** The mesh resource walked out with plausible-looking
  cluster metadata (offsets and counts were computed correctly) and **zero
  geometry bytes**. Anything downstream — GPU upload, render batches, visibility
  stats — had nothing to read. The geometry is now written out (uncompressed;
  compression is still a TODO), with the layout documented on
  `NaniteMeshResource`.
* **`RecursiveClusterSplit()` returned early when `depth` reached
  `kMaxHierarchyDepth`**, silently dropping every triangle under that node. Deep
  meshes lost a chunk of geometry with no error. Reaching the depth limit now
  makes the node a leaf (possibly oversized) instead of discarding it.
* **`BuildHierarchy()` was an O(n²) guess.** Each cluster picked as its parent
  the first other cluster with a lower `lod_level` whose centre was within
  `2 * radius` — which cluster that is depends on array order, and because the
  loop `break`s a parent could collect the same child repeatedly. And
  `root_cluster_index` was hard-coded to `0`, but cluster 0 is the first *leaf*
  in depth-first order, never the root. The tree is now built during the split:
  internal nodes are created post-order and claim exactly the two roots their
  recursive calls returned. (Writing the parent pointer over the whole subtree
  *range* instead is the obvious-looking mistake here: it overwrites the pointers
  the deeper levels already wrote, collapsing the tree into "everything hangs
  off one parent".)
* **`error_metric` was never assigned by anyone**, and `NaniteCluster` had no
  default member initialisers, so `SelectLOD()` read uninitialised memory. The
  metric is now `radius` (monotone under spatial subdivision — the previous
  `radius * (1 + lod_level * 0.1)` ran the wrong way, making *finer* clusters
  look worse), floors degenerate radii, and is forced monotone parent ≥ child so
  LOD selection can rely on it.

Also: rejected input is now rejected (`indices.size() % 3 != 0` and out-of-range
indices used to walk off the end of `positions`), `lod_level` is documented as
"0 = root", and internal nodes are marked `is_leaf = false` — without a mesh
simplifier there is no coarser-but-cheaper representation, so only leaves are
renderable and internal nodes exist to make the hierarchy and per-level error
queryable.

**Test**: `NaniteClusterBuilderSmokeTest` — 54 checks, 5 consecutive green runs
(`-DZENGINE_BUILD_NANITE_CLUSTER_SMOKE_TEST=ON`). It pins the byte counts, that
no triangle is lost, that leaf windows tile the geometry exactly once, that leaf
bounds match the vertices actually stored in their window, and the tree
invariants. Removing the geometry write fails 7 of the 54; the range-based
parent write fails the parent/child check. Unlike the Lumen tests this one is
enabled in `smoke_tests.yml`, because `Function/Render/Nanite` is never stripped
from ZRuntime. ctest 19/19 on Apple.

### Added / Fixed - 2026-10-21 (Lumen reflections: from a zero-caller dead end to the shader)

`LumenRenderer::ComputeReflections()` was an empty TODO, so
`LumenReflections::ComputeReflection()` had **zero callers** and
`LumenReflectionData` never left `Function/Render/Lumen/` — no shader, no
`RenderScene` field, no consumer. Reflections were a dead end, and the code
behind them had never been executed.

**Fixed in the computation** (bugs that only exist because nobody called it):

* `BlendReflections()` picked a source using `max_trace_distance` — a *constant*.
  With `max_trace_distance < 100` and `roughness < 0.3` it chose screen-space
  reflections, and `ComputeScreenSpaceReflection()` is a TODO that returns
  nothing, so a distance-field hit that had already been traced was discarded
  and replaced with black. Weighting is now "did this actually hit", via a new
  explicit `LumenReflectionData::valid` flag — the colour cannot answer that,
  because a hit on a black surface is legitimately black.
* `ComputeReflection()` computed the mirror direction into
  `result.reflection_direction` and then overwrote `result` wholesale with
  `BlendReflections()`'s output, which never set that field. The direction was
  silently lost.
* The roughness TODO was never implemented, so a rough surface still traced one
  perfect mirror ray, and the result was then multiplied by `(1 - roughness)` —
  which double-counts roughness (the BRDF lobe in the shader already handles it)
  and sends `roughness = 1` to exactly black. Roughness now widens a cone
  (`reflection_sample_count` rays, averaged) instead of dimming the radiance.

**Wired**: `ComputeReflections()` now runs `UpdateReflections()`, which uses the
surface-cache pages as shading points (they already carry a world position and a
normal), budgeted and round-robin, with the same frame-rate-independent temporal
blend as the GI probe grid. The scene-average result is published into
`RenderScene::m_IndirectLighting.reflection` and reaches the shader as
`MainCameraPerFrame::lumen_reflection`, where `mesh_lighting.inl` blends it into
the specular IBL term (blend, not add). `lumen_indirect` and `lumen_reflection`
carry **separate weights** because `r.Lumen.GlobalIllumination` and
`r.Lumen.Reflections` are independent switches.

**Two caveats, both deliberate.** The published reflection is a *scene average*:
the CPU path has neither screen probes nor a GBuffer, so it cannot get a
per-pixel shading point and view direction — it stands in for a coarse specular
environment term, not for real reflections. And the deferred HLSL pass has no
specular IBL term to blend into, so today only the forward GLSL path consumes it.

New CVars: `r.Lumen.ReflectionBudget`. `r.Lumen.SampleCount` and
`r.Lumen.TemporalBlendRate` now drive reflections as well as probes.

New `LumenReflectionsSmokeTest` (32 checks,
`-DZENGINE_BUILD_LUMEN_REFLECTIONS_SMOKE_TEST=ON`) pins the mirror direction
surviving the blend, that a 50-unit trace range still reflects (the SSR
regression), that `roughness = 1.0` no longer resolves to black, the budgeted
round-robin, and that rate 0 freezes while a large rate catches up.
`LumenPerFrameBridgeSmokeTest` grew to 36 checks — and it caught a real one:
adding `lumen_reflection` broke its "lumen_indirect is last" assertion, which is
exactly what that test is for.

ctest 18/18 on Apple.

### Added - 2026-10-21 (Lumen GI reaches the shader: the per-frame uniform bridge)

Phase 27 ended with Lumen publishing its irradiance into `RenderScene::m_IndirectLighting`
— and with a commit message that said so explicitly: `MainCameraPerFrame` had no GI
field and `deferred_lighting.frag` did not composite it. GI still could not reach a
single pixel. This closes that last metre.

- **`MainCameraPerFrame::lumen_indirect`** (`Vector4`, appended last): rgb = irradiance,
  a = blend weight. It is *weight + colour*, not "is there a value", because a scene
  boxed in by closed geometry legitimately resolves to black — that is data, and only
  the weight distinguishes it from "Lumen is off".
- `SceneIndirectLighting::ToUniform()` (`RenderCommon.h`) is the single definition of
  that packing, called by **both** `UpdatePerFrameBuffer` implementations. See the trap
  below for why it is not written out twice.
- **Shaders composite it**: `shader/include/mesh_lighting.inl` (forward/GLSL) and
  `shader/hlsl/rp1/deferred_lighting.frag.hlsl` (deferred/HLSL) now `mix` / `lerp`
  between the baked irradiance cubemap and `lumen_indirect.rgb` by `lumen_indirect.a`.
  Blending, not adding — both are irradiance, and adding would count the sky twice.
  Weight 0 behaves byte-for-byte as before.
- **New `LumenPerFrameBridgeSmokeTest`** (`-DZENGINE_BUILD_LUMEN_PER_FRAME_BRIDGE_SMOKE_TEST=ON`,
  20 checks): pins the packing, the C++ struct layout (`static_assert` + runtime
  mirror), that both backend call sites publish the field, and that all four shader
  mirrors declare / expose it — including that the consumers blend rather than add.
  Verified to fail when a mirror is deliberately broken.

#### The trap: `UpdatePerFrameBuffer` has two mutually exclusive bodies

`Function/Render/RenderResource.cpp` is wrapped *entirely* in `#if defined(Z_HAS_VULKAN)`
(lines 5–1420); `Function/Render/RenderResourceDx12.cpp` is wrapped in
`#if defined(_WIN32)` + `#if !defined(Z_HAS_VULKAN)`. **Apple (Metal) and Web have no
implementation at all** — the `RenderResource::UpdatePerFrameBuffer` symbol the Apple
smoke tests link is a stub that `abort()`s. This is the same mistake class as phase 27:
"is Lumen compiled in" (`Z_HAS_LUMEN`) and "does anything fill the per-frame buffer"
(`Z_HAS_VULKAN` / `_WIN32`) are two different switches, and on Apple their intersection
is empty.

Practical consequence: any new per-frame field must be filled in **both** files, and any
test that calls `UpdatePerFrameBuffer` directly will only ever run on Vulkan hosts —
which is why the packing was hoisted into a header function the test can link anywhere.

### Added / Fixed - 2026-10-21 (Lumen render pass: wired into the frame loop, and it has CVars now)

The probe grid had real content, but nothing in the engine ever asked for it.
`LumenRenderPass::Render()` — the only function that drives `Update` /
`ComputeGlobalIllumination` / `ComputeReflections` — had **zero callers** (only
`PreparePassData` was invoked, from `RenderPipelineBase`). It also ended in a
TODO, so even if it had run the result went nowhere. And there were no CVars at
all: GI could not be switched off at runtime and probe density was a hard-coded
constant.

- **`Z_HAS_LUMEN`** (new compile definition, `engine/CMakeLists.txt`) now mirrors
  which configurations actually strip `Function/Render/Lumen` from ZRuntime
  (Emscripten, and Windows with `ZENGINE_USE_VULKAN=OFF`). This is *not* the same
  as `Z_HAS_VULKAN`: Apple builds Metal and has no Vulkan, yet does compile
  Lumen — which is exactly why guarding the pass on `Z_HAS_VULKAN` left it
  uncreated there.
- **`m_LumenPass` is now created on every build that compiles Lumen**, in
  `DesktopRenderPipelineModule::Setup()` before the Apple early-return. Lumen is
  pure CPU (distance-field voxelization, surface cache, probe grid) — it needs no
  render pass, framebuffer or swapchain.
- **`RenderPipelineBase::UpdateLumen(scene, camera, delta_time)`** dispatches to
  the pass through a `dynamic_cast` (the slot is shared by all passes, and on
  stripped configurations `LumenRenderPass` does not exist at all). No-op when
  there is no Lumen pass, so the Mobile path is unaffected.
- **New `RenderLumenCmd` frame command**, enqueued right after
  `RenderUpdateSceneCmd`: GI reads scene entities, so it must follow the
  visibility update, and it must precede `PreparePassData` so its result is
  available to this frame's per-frame uniform.
- **`Render()` now publishes**: it writes the probe grid's mean irradiance into
  `RenderScene::m_IndirectLighting` (new POD `SceneIndirectLighting` in
  `RenderCommon.h`, deliberately Lumen-free so stripped configurations still
  compile). The published payload carries a **weight**, not just a colour: an
  enclosed scene legitimately resolves to black, and that is data, not "no data".
- **`LumenRenderPass::Initialize` no longer requires an RHI.** Lumen runs entirely
  on the CPU today; gating it on `m_Rhi` left the whole subsystem uninitialized
  whenever the RHI was not up yet.
- **New `Lumen/LumenSettings.{h,cpp}`** registers `r.Lumen.Enable`,
  `r.Lumen.GlobalIllumination`, `r.Lumen.Reflections`, `r.Lumen.ProbeBudget`,
  `r.Lumen.SampleCount`, `r.Lumen.ProbeSpacing`, `r.Lumen.TemporalBlendRate` and
  `r.Lumen.IndirectIntensity`, following the MegaLights pattern. They are **live**:
  `Render()` re-applies them every frame via the new
  `LumenRenderer::ApplyConfig()`, which pushes the config down to all five
  subsystems without re-initializing them (that is the only way to keep the probe
  grid's history and the surface cache's pages).
- **`LumenRenderPassWiringSmokeTest`** (37 checks,
  `-DZENGINE_BUILD_LUMEN_RENDER_PASS_WIRING_SMOKE_TEST=ON`, registered with
  CTest) pins it headlessly: the pass publishes a red-dominant irradiance for a
  red floor, `r.Lumen.Enable=0` zeroes the weight rather than leaving stale light,
  `r.Lumen.IndirectIntensity` scales the result, `r.Lumen.ProbeSpacing` resizes
  the grid without restarting the pipeline, and `UpdateLumen` drives a Lumen pass
  while ignoring a non-Lumen one.

### Added / Fixed - 2026-10-21 (Lumen global illumination: GI produces something, and it is not noise)

`ComputeGlobalIllumination` was a TODO that returned `true` without touching
anything, so nothing ever wrote `m_LightingCache` — which meant `SpatialReuse`
(a linear scan over that permanently empty cache) always returned zero, which
meant every `GetIndirectLighting` call fell through to a fresh stochastic trace.
`TemporalAccumulation` was likewise an empty TODO body, so the noise had nowhere
to go. Rebuilt around a world-space irradiance probe grid:

- **`ComputeGlobalIllumination` now fills `LumenProbeGrid`** (new struct in
  `LumenTypes.h`): per-probe accumulated irradiance, a hit-rate "visibility"
  term, and the frame each probe was last updated. `ResizeProbeGrid` anchors the
  grid to the distance field's **world box** and derives the per-axis resolution
  from `gi_probe_spacing`, clamped to `LumenConstants::kMaxGiProbeResolution`
  (a probe costs `gi_sample_count` rays, so the probe count has to be bounded).
  No distance field ⇒ no world box ⇒ no grid, and the call now reports failure
  instead of pretending to succeed.
- **The grid deliberately does not follow the camera.** A camera-anchored grid
  would be rebuilt on every camera move, throwing away the history that temporal
  accumulation exists to exploit. The camera only decides which probes get this
  frame's budget (distance filter + a "don't bother with probes far from any
  surface" filter).
- **`UpdateProbes` is budgeted and rotates** (`gi_probe_update_budget` +
  `m_ProbeCursor`), so one frame never traces the whole grid. A probe's first
  update is trusted outright; every later one blends with
  `alpha = 1 - exp(-gi_temporal_blend_rate * dt)`, which is frame-rate
  independent. `gi_temporal_blend_rate = 0` now means "freeze the probe", not
  "update it instantly".
- **`SpatialReuse` is real trilinear interpolation** over the grid, and
  `SampleProbeGrid` gates on `last_updated_frame`, never on the length of the
  value — a probe boxed in by closed geometry legitimately holds black, and
  treating that as "no data" would re-trace forever.
- **`GetIndirectLighting` is intentionally non-const** and does query-and-fill:
  on a miss it traces and writes the result back through `TemporalAccumulation`.
  The write path only fills *empty* probes, so it never fights the per-frame
  accumulation.
- **`LumenGlobalIlluminationSmokeTest`** (49 checks,
  `-DZENGINE_BUILD_LUMEN_GLOBAL_ILLUMINATION_SMOKE_TEST=ON`, registered with
  CTest) pins it headlessly: the grid gets built at the expected resolution,
  moving the camera neither resizes nor discards it, a budgeted frame updates
  exactly `gi_probe_update_budget` probes, sampling at a probe centre returns
  that probe's own value while a point between two probes lands between them,
  and — end to end — recolouring the floor from red to blue flips the probes'
  hue under a large blend rate while a zero blend rate freezes them on red.

### Added / Fixed - 2026-10-21 (Lumen surface cache: it exists now, and rays carry real material)

The surface cache was a dead subsystem: `BuildFromScene` was a TODO, so no page was
ever created, `GetSurfaceInfo` (which had **zero callers**) always returned false,
and `TraceRaySoftware` handed back a hard-coded `albedo = 0.5`. Consequence: every
surface in the scene bounced identical grey light. Rebuilt:

- **`BuildFromScene` now builds a page per scene instance.** Page = one
  `RenderEntity`, covering its world AABB (transformed by `m_ModelMatrix`, falling
  back to the CPU geometry cache when the entity box is invalid — same rule the
  distance field already uses), carrying the entity's real material from
  `m_BaseColorFactor` / `m_RoughnessFactor` / `m_MetallicFactor` /
  `m_EmissiveFactor`.
- **The page key is gone, replaced by a uniform grid.** The old key was
  `(uint64_t(x*1000) << 32) | (uint64_t(y*1000) << 16) | uint64_t(z*1000)` — `x`
  overflows the field it is shifted into, `y`'s 16-bit field overlaps `x`'s low
  bits, `z` is not shifted at all, and a negative coordinate converted to
  `uint64_t` is undefined behaviour. Lookup is now spatial: each page registers
  into every grid cell its AABB covers, and the grid cell size is the largest
  page extent, so a page spans at most 2×2×2 = 8 cells (bounded registration).
  Points inside a page's box win by "smallest box"; otherwise the nearest page
  centre is used, because the distance field stops rays about half a voxel *off*
  the surface and a flat quad's AABB has zero thickness.
- **`CleanupInvalidPages` actually evicts**, by LRU against a frame counter that
  `UpdateSurfaceCache` advances. `GetSurfaceInfo` is deliberately **non-const**:
  a hit refreshes the page's LRU timestamp, which is the only signal eviction
  has. `AllocatePage` falls back to forced LRU eviction when the page pool is
  exhausted, and `FreePage` no longer double-decrements `active_page_count`
  (it is unsigned, so that wrapped to an astronomical number).
- **A scene hash gates the rebuild** (instance id + mesh/material asset id +
  model matrix + material factors), so changing a colour rebuilds the page
  instead of leaving GI bouncing the old albedo. Rebuild also triggers when the
  cache is empty — without that, an evicted cache would stay empty forever.
- **Per-texel arrays are no longer pre-allocated.** 64×64 × 7 arrays × 1024
  pages is several hundred MB, and every texel held the same constant anyway:
  there is no card rasterizer on the CPU path. `page.resolution` is 0 there.
- **`TraceRaySoftware` fills the hit's material from the surface cache**, so a
  ray landing on a red floor now reports red, and reports which instance it hit.
  With no surface cache it falls back to the neutral 0.5 grey.
- **`LumenSurfaceCacheSmokeTest`** (42 checks, `-DZENGINE_BUILD_LUMEN_SURFACE_CACHE_SMOKE_TEST=ON`,
  registered with CTest) pins all of it headlessly, including the cases the old
  key mangled: huge coordinates, negative coordinates, and a 1 mm nudge.

### Added / Fixed - 2026-10-21 (Lumen software ray tracing: it can now hit)

`LumenDistanceField::GenerateFromScene` had a TODO where the geometry was
supposed to be collected, so `m_SceneGeometryCache` stayed empty, every voxel
kept its initial `FLT_MAX`, the software ray marcher could never satisfy
`distance < 0.01f` and **GI was mathematically zero**. Filled in:

- **`RenderResourceBase` now keeps a CPU copy of mesh geometry**
  (`CpuMeshGeometry`: local-space positions + widened indices, keyed by
  `RenderEntity::m_MeshAssetId`). `RenderMeshData` is handed to
  `UploadGameObjectRenderResource` **by value** and freed the moment the GPU
  buffers are filled, so nothing could read vertex positions back afterwards --
  that gap is exactly why the cache had to be invented. Only positions and
  indices are retained (12 + 4 bytes per vertex/index, not the 44-byte
  `MeshVertexDataDefinition`). Populated from `RenderResource::GetOrCreateMesh`
  on both the Vulkan/Metal and the DX12 upload paths.
- **`SceneGeometryCache` is now actually filled**, and extended with a
  per-instance `MeshRange` so it can express what the flat vectors could not:
  geometry is stored **once per mesh asset** and N instances share one range,
  so `ranges` / `transforms` / `bounds` are parallel arrays of length = instance
  count. `bounds` holds world-space AABBs (`RenderEntity::m_BoundingBox` is
  local, exactly as the visibility code already assumes).
- **`geometry_hash`** (FNV-1a over instance ids + mesh asset ids + model
  matrices) lets `GenerateFromScene` skip the whole voxelization when neither
  the scene nor the bounds changed. `UpdateDistanceField` runs every frame, so
  without this the field would be rebuilt every frame.
- **The voxelizer computes real distances** instead of leaving `FLT_MAX`:
  each instance is pre-pruned by its world AABB (distance-to-box is a lower
  bound on distance-to-any-triangle), and the world->local matrix is inverted
  once per instance rather than once per voxel per instance.
- **Fixed `DistanceToTriangle`**: its barycentric solve used
  `edge · (point - v0)` where the algorithm (Ericson, RTCD 5.1.5) requires
  `edge · (v0 - point)`. The whole region test then took the wrong branch --
  a point *inside* the triangle was reported as the distance to `v0` (0.89
  instead of 0.125 in the smoke test). Nothing noticed until now because the
  function had no callers that reached it with geometry.
- **CPU voxelization resolution is capped** at
  `LumenConstants::kMaxCpuDistanceFieldResolution` (64). The default config asks
  for 256, i.e. 16.7M voxels of brute-force triangle distance -- minutes of CPU.
  `Initialize` clamps and warns; the cap disappears once the field is built on
  the GPU.
- **`LumenRenderer`** passes its `RenderResourceBase` down and no longer
  hard-codes a ±1000 world box (that made the voxel size so large the field was
  meaningless); when the bounds are invalid they are derived from the scene.
- **`LumenSceneGeometrySmokeTest`** (36 checks, `-DZENGINE_BUILD_LUMEN_SCENE_GEOMETRY_SMOKE_TEST=ON`)
  covers cache -> voxel -> query end to end headlessly. Not in the Windows
  smoke-test workflow: that job runs with `ZENGINE_USE_VULKAN=OFF`, which strips
  `Function/Render/Lumen/*.cpp` from ZRuntime.

### Added / Fixed - 2026-10-21 (Lumen software ray tracing: it can now hit)

A filled distance field was necessary but not sufficient -- the software marcher
still reported a miss for every single ray, so GI was still exactly zero. Two
independent blockers, both fixed:

- **`TraceRaySoftware` ignored its own parameter.** It took a
  `LumenDistanceField*` and returned "no hit" immediately when it was `nullptr`,
  while all three call sites (`LumenGlobalIllumination` x2, `LumenReflections`
  x1) passed literal `nullptr`. The member installed by `setDistanceField()` was
  never consulted. It is now **argument-first with a member fallback**: the
  callers pass the real field explicitly, `nullptr` still falls back to the
  member so nothing silently regresses, and an unloaded field is rejected up
  front instead of being marched over. The undefined
  `traceRayWithDistanceField` declaration (declared, never defined) is gone.
- **`DistanceFieldMarch` used an unreachable hit threshold.** It treated
  `distance < 0.01f` as a hit, but the field only holds values at voxel centres,
  so the smallest distance any query can return is on the order of half a voxel
  diagonal. Every ray therefore burned all 256 steps and returned a miss. The
  threshold is now `max(min_trace_step, voxel_diagonal * 0.5f)`, computed from
  the new `LumenDistanceField::GetVoxelSize()` / `GetVoxelDiagonal()`.
  Additionally: the ray is **clipped to the field's world box** (outside it
  `QueryDistance` is `FLT_MAX`, which means "no data", not "far away", so an
  origin outside the box used to wander off at maximum step size), a hit is
  **refined by binary search** (8 iterations) so `hit_distance` lands on the
  surface rather than a voxel away, and a zero-length direction is rejected.
- **Shading points are lifted off their own surface.** GI and reflections start
  their rays on a surface, where the field reads ~0 -- without an offset the
  first step "hits" the surface the ray is leaving. Both now offset the origin
  along the shading normal by one voxel diagonal, which is the smallest scale
  the field can resolve.
- **`QueryNearestSurface` no longer produces NaN normals.** Its gradient step
  was hard-coded to `0.1`; on coarse voxels all three samples landed in the same
  voxel, the difference was zero, and `normalisedCopy()` of a zero vector fed
  NaN into the lighting. The epsilon is now voxel-scaled, out-of-bounds samples
  are treated as "no change" rather than `FLT_MAX`, and a degenerate gradient
  falls back to a finite normal.
- **`LumenRayTracingSmokeTest`** (32 checks, `-DZENGINE_BUILD_LUMEN_RAY_TRACING_SMOKE_TEST=ON`,
  registered with CTest) pins all of the above headlessly: a ray hits the floor
  at the right distance with a finite unit normal, empty space misses, an origin
  outside the world box is clipped and still hits, the argument beats the
  member, and -- end to end -- GI returns non-zero indirect light (it was
  identically zero before) while a hemisphere facing empty space returns zero.

### Fixed / Added - 2026-10-21 (RHI groundwork for the GPU-driven path)
- **`VulkanRHI::CreateComputePipelines` / `CreateGraphicsPipelines` read and wrote
  past their own stack** for any `createInfoCount > 1`: only one
  `Vk*PipelineCreateInfo` and one output slot were ever filled, yet
  `createInfoCount` was forwarded verbatim to `vkCreate*Pipelines`. It stayed
  hidden because every caller passes `1` -- and Nanite needs several compute
  pipelines, so it would have been the first thing to trip it.
  Both now build and return exactly one pipeline, and every backend (Vulkan,
  DX12, Metal) **rejects** `createInfoCount != 1` instead of silently dropping
  the rest. `RHI.h` documents the contract: create N pipelines with N calls.
  The out-parameter cannot carry N anyway -- `RHIPipeline` is an empty base, so
  indexing it as an array would stride by `sizeof(RHIPipeline)` (== 1).
- Compute **specialization constants were unconditionally discarded** on Vulkan
  (both branches of the `if/else` assigned `nullptr`), so a compute shader that
  asked for one silently got the default. They are now translated properly.
- **Added `RHI::CmdDrawIndexedIndirect` and `RHI::CmdDrawIndirect`** -- the
  missing primitive for GPU-driven geometry. A compute pass can now write the
  draw arguments and the CPU never learns how many draws there were. Implemented
  on Vulkan (`vkCmdDrawIndexedIndirect`, with a `multiDrawIndirect` feature
  probe so a batch is clamped rather than silently truncated), DX12
  (`ExecuteIndirect` + a new draw command signature; the stride is baked into
  the signature, so a mismatched stride is refused instead of reinterpreted),
  Metal (no multi-draw indirect exists, so a batch loops with
  `offset + i * stride`), and WebGL2 (no-op -- no indirect draw exists there).
- `RHIDrawIndexedIndirectCommand` / `RHIDrawIndirectCommand` are the shared
  argument layout; their size and field offsets are the ones Vulkan, DX12 and
  Metal already agree on, and `RHIIndirectDrawSmokeTest` (35 checks, registered
  with CTest and added to the smoke-test workflow) pins them, because on Apple
  neither the Vulkan nor the DX12 backend is compiled and nothing else would
  catch a field drifting apart.

### Added - 2026-10-21 (packaging is incremental)
- Every `AssetBundleBuilder::Build()` used to redo everything: re-cook every
  texture in the project, then deflate and rewrite every `.zbundle`. Editing
  one material cost a full cook.
- Both halves are now skipped when nothing that goes into them changed:
  - **cook** (`TextureImporter::CookProjectTextures(target, only_stale)`) --
    re-encoding is the expensive part; a texture whose source pixels, editor
    `.zasset` and effective import settings are unchanged, and whose cooked
    output is still on disk, is left alone. The Cook menu items and
    `asset.cook` keep the old "always re-cook" behaviour on purpose -- they
    are the deliberate redo action.
  - **packing** -- a bundle is rewritten only when its fingerprint changed.
- **The verdict is a content hash, never a timestamp.** (size, mtime) is only
  a fast path that lets a no-op build skip *reading* the assets: when the
  stamp is unchanged the hash recorded last time is reused, and when it is not
  the file is re-hashed -- so a `git checkout` that only moved mtimes still
  produces no rebuild, and a changed byte always does.
- State lives next to what it describes, so it can never outlive it:
  `<output_dir>/AssetBundleBuildCache.json` and
  `<Cooked>/<Platform>/cook_cache.json` (the latter is already excluded from
  the install rules). Deleting the output directory is a full rebuild.
- Fails safe: a missing, corrupt or wrong-version cache rebuilds everything and
  logs why; an artifact that was deleted or resized loses its cache entry; a
  bundle nothing produces any more is **deleted**, otherwise a removed level
  would keep shipping.
- `AssetBundleBuildOptions::force_rebuild` (and `bundle.build --force`) is the
  escape hatch for "I don't trust the cache" -- it is NOT needed for "I changed
  something". `AssetBundleBuildResult` now reports `rebuilt_bundle_count`,
  `skipped_bundle_count` and `cooked_texture_count`.
- **A RELEASE build of the editor defaults `force_rebuild` to true**
  (`-DZENGINE_PACKAGING_FORCE_REBUILD=ON`, which `build_windows.bat` /
  `build_linux.sh` / `build_macos.sh` pass for the `release` configuration).
  An incremental package is only reproducible by whoever owns the cache, and
  that is not what a release artifact should depend on. It is a DEFAULT, not a
  lock: `bundle.build --incremental` opts back in on a release editor, and the
  build scripts are the only place the flag is set -- nobody has to remember it.
  The scripts pass it explicitly in both directions (`=OFF` for a non-release
  config) because CMake options are sticky and the Windows presets are
  multi-config: without that, a debug build reusing the cache would inherit a
  previous release's full-rebuild default.
- New `Editor/Build/BuildCache.{h,cpp}` (engine-free on purpose) plus
  `BuildCacheSmokeTest` (44 checks), registered with CTest and added to the
  smoke-test workflow.

### Fixed - 2026-10-21 (zlib is vendored, not looked up on the host)
- **`zlib.h` was not findable on several machines.** Both consumers -- the
  `.zbundle` per-entry DEFLATE in ZRuntime and minizip-ng's `.xlsx` reader --
  depended on whatever zlib the host happened to provide. Where none was
  installed, minizip-ng's fallback was `clone_repo(zlib
  https://github.com/zlib-ng/zlib-ng stable)`, i.e. a **git clone at configure
  time**; when that failed (no GitHub access on the agent) the build silently
  degraded to `MZ_ZLIB=OFF` / no `ZENGINE_BUNDLE_HAS_ZLIB` -- either a hard
  "zlib.h not found" or, worse, a build that quietly stopped compressing.
- zlib is now a **git submodule at `engine/3rdparty/zlib`** (pinned `v1.3.2`),
  built static-only as `zlibstatic` (`ZLIB_INSTALL OFF`, so it never joins the
  install/CPack rules). Same sources and same DEFLATE behaviour on every host,
  no system package and no network access at configure time.
- `Source/Runtime` links `zlibstatic` directly, and minizip-ng is pointed at
  the *same* target (its `find_package(ZLIB)` is pre-seeded with
  `ZLIB_LIBRARY` / `ZLIB_INCLUDE_DIR` + a `ZLIB::ZLIB -> zlibstatic` alias),
  which lets `MZ_FETCH_LIBS` stay OFF. Exactly one zlib is in the link graph;
  `MZ_FETCH_LIBS` is only left ON as a last resort when the submodule is
  missing.

### Fixed - 2026-10-21 (PlayerSettings is a real settings object)
- **`PlayerSettings` was a 7-line stub**, and the empty `m_ProductName` it left
  behind had a real consequence: `UserPreferences` derives its per-user
  directory from that field, so every project on a machine collapsed onto the
  same `ZEngine` folder and shared one `preferences.json`.
  - Fields now filled from the project: `m_ProductName` (defaults to the
    project name), `m_CompanyName`, `m_Version` (project version, else
    `1.0.0`), plus `m_DefaultScreenWidth/Height`.
  - Persisted to `<project>/<config_dir>/player_settings.json` (rapidjson,
    atomic temp+rename). Missing / malformed / partial files fall back to the
    defaults instead of failing; a resolution <= 0 is rejected.
  - `WindowSystem` now resolves the window size as UserPreferences >
    PlayerSettings > built-in fallback, so a project can actually pick a
    default resolution (it was hardcoded to 1280x720 before).
  - `VulkanRHI` no longer dereferences `GET_SYSTEM(PlayerSettings)` blindly --
    `RegisterRuntimeLight()` deliberately does not register it, so that path
    crashed on instance creation.
- New `PlayerSettingsSmokeTest` (32 checks), registered with CTest and added to
  the Windows smoke-test workflow.

### Fixed - 2026-10-20 (CTest actually runs the smoke tests)
- **The opt-in smoke tests were invisible to CTest.** Every
  `option(ZENGINE_BUILD_*_SMOKE_TEST ...)` built an executable but never called
  `add_test()`, and `enable_testing()` was called from `tests/` instead of the
  root `CMakeLists.txt`, so `ctest` registered only the three
  `Platform*Test`s. A CI job that ran `ctest` therefore found the suite empty
  and exited 0 -- a green light that protected nothing.
  - `enable_testing()` moved to the root `CMakeLists.txt`, above
    `add_subdirectory(tests)` / `add_subdirectory(engine)`.
  - `add_test(NAME <t> COMMAND <t>)` added to all 16 smoke-test targets: the
    four serialization tests (via `zengine_add_smoke_test`), `UndoSystem`,
    `RenderGraph`, `AssetBundle`, `Addressables`, `DerivedDataCache`,
    `TextureCompressor`, `AI`, `NavMesh`, `AnimationSkinning`, `Animator`,
    `AudioMixer`, `DX12Bindless`, `VulkanBindless`.
  - Verdict is the exit code (each test returns `EXIT_FAILURE` on its first
    failed check), so no `PASS_REGULAR_EXPRESSION` is needed.
  - Deliberately NOT registered: `SceneGenTool`, `AssetMigrator`,
    `ObjectDefinitionMigrator` (migration tools that write files, not tests).
    `VulkanBindlessSmokeTest` registers only when `ZENGINE_USE_VULKAN` is on.
  - Verified on Windows with every `ZENGINE_BUILD_*_SMOKE_TEST` flag on:
    `ctest -N` lists 19 tests (3 `Platform*Test` + 16 smoke) and
    `ctest -C Release` passes **19/19**.

### Added - 2026-10-20 (the ZEnvironmentCollector tests are reachable again)
- The four `environment_*` tests (`environment_collector_core`,
  `environment_preflight`, `environment_capture_safety`,
  `environment_wer_safety`) under `engine/tools/ZEnvironmentCollector` called
  `add_test()` but could never register: they were gated on `BUILD_TESTING`,
  which is `OFF` in the cache because minizip-ng force-sets it to silence
  zlib-ng's own tests. They had been doubly invisible before that, because
  `enable_testing()` was missing from the root `CMakeLists.txt`.
  - New `option(ZENGINE_BUILD_ENVIRONMENT_CHECK_TESTS ...)` (default `OFF`)
    gates them; the block now runs on
    `if(BUILD_TESTING OR ZENGINE_BUILD_ENVIRONMENT_CHECK_TESTS)`. A dedicated
    option rather than `-DBUILD_TESTING=ON`, which would also switch on every
    3rdparty suite (curl, Jolt, mimalloc, ...) -- none of which this build
    compiles, so `ctest` would just report "Unable to find executable" for all
    of them.
  - Enabled in CI. Verified: `ctest -N` lists 23 tests and `ctest -C Release`
    passes **23/23**.

### Added - 2026-10-20 (the smoke tests run in CI)
- `.github/workflows/smoke_tests.yml` (Windows / Release, on push and PR to
  `main`) turns on every opt-in smoke-test flag, builds the 19 registered
  targets and runs them with
  `ctest --test-dir build_smoke -C Release --output-on-failure`.
  - `ZENGINE_USE_VULKAN=OFF` (runners ship no VULKAN_SDK) and
    `PAPI_TYPE=quickjs` (release would otherwise pull the V8 backend).
  - `DX12BindlessSmokeTest` is included: it falls back to the WARP adapter
    when no hardware adapter exists, so it runs headless.
  - `NavMeshTest` is included: it prints `(skipped: level file not found)`
    and still passes when the shipped level has not been copied into
    `bin/$<CONFIG>/asset/level/`.
  - `ZENGINE_BUILD_WRITE_FAULT_SMOKE_TEST` stays `OFF` -- its plumbing is not
    in the tree yet (`WriteFaultSmokeTest.cpp.pending`).

### Fixed - 2026-10-20 (`RHI::CreateSemaphore` vs. the Win32 macro)
- `synchapi.h` `#define`s `CreateSemaphore` to `CreateSemaphoreA`/`W`, so the
  spelling of `RHI::CreateSemaphore` depended on whether the TU had included
  `<windows.h>` yet. `DX12RHI.h` includes `RHI.h` **before** `<d3d12.h>`, so
  any standalone TU that included `DX12RHI.h` first ended up with a base class
  named `CreateSemaphore` and an override named `CreateSemaphoreA` --
  `error C3668: ... did not override any base class methods`. Production TUs
  never hit it because they pull in `<windows.h>` first and both sides get
  renamed together, which is why it stayed hidden: `DX12BindlessSmokeTest` had
  never compiled.
  - `push_macro` / `#undef` / `pop_macro` now pin the name at the declaration
    (`RHI.h`), both overrides (`DX12RHI.h`, `VulkanRHI.h`) and both
    definitions (`DX12RHI.cpp`, `VulkanRHI.cpp`). The method has no call sites
    yet.

### Added - 2026-10-20 (RenderGraph)
- **A real frame graph exists now.** Before this the renderer was written straight
  through: every pass hand-wrote its `CmdPipelineBarrier` calls, every pass owned a
  full-screen render target for its whole lifetime, the pass order was hard-coded into
  `MainCameraPass::Draw(...)`'s parameter list, and a swapchain resize had to re-wire
  six hard-coded view pointers by hand. There was no transient resource pool anywhere
  in the engine.
  - `Function/Render/Graph/RenderGraphTypes.{h,cpp}` -- the data model: handles
    (index + generation, so a stale handle from a previous frame fails loudly), a small
    RHI-free mirror of the format / usage / state enums, texture and buffer descriptors.
  - `Function/Render/Graph/RenderGraphBuilder.{h,cpp}` -- the Setup-phase API:
    `CreateTexture` / `CreateBuffer`, `ImportTexture` / `ImportBuffer` for
    caller-owned resources, `Read/Write` dependency declaration, `KeepAlive` (UE's
    Extract) and `RGPassFlags::NeverCull`.
  - `Function/Render/Graph/RenderGraph.{h,cpp}` -- the compiler: dependency edges from
    read/write (including write-after-read and write-after-write), topological sort,
    culling of passes whose results nobody consumes, resource lifetimes, greedy memory
    aliasing of transient resources, and barrier derivation (a barrier is emitted only
    when the resource is not already in the state the pass needs). Idempotent and
    deterministic. Validation turns "no format", "zero extent", "read but never
    written", "written with a read state", "read and written in one pass" and
    "texture used as a buffer" into errors instead of UB.
  - `Function/Render/Graph/RenderGraphResources.{h,cpp}` -- the only RHI-aware part:
    `RenderGraphResourcePool` (descriptor-keyed transient pool, the thing the engine
    never had), `RenderGraphExecuteContext` (handle -> `RHIImage`/`RHIImageView`/
    `RHIBuffer` during Execute) and `RenderGraphExecutor`, which records each pass with
    one batched `CmdPipelineBarrier` and `PushEvent`/`PopEvent` around it and hands
    memory back the moment a resource's lifetime ends.
  - `RenderGraphSmokeTest` (`-DZENGINE_BUILD_RENDERGRAPH_SMOKE_TEST=ON`): 78 headless
    checks -- no GPU, no window, links only the three compiler files.
- **Not wired into the render pipeline yet.** `DesktopRenderPipelineModule` and
  `MainCameraPass::Draw(...)` are untouched; converting them needs per-frame visual
  verification. `doc/RENDER_GRAPH.md` lists the three prepared seams, lowest risk
  first: `RenderPassBase::AppendToDrawList`, splitting the post passes out of
  `MainCameraFramebufferResources`, and deleting the six hand-written
  `UpdateAfterFramebufferRecreate` calls.

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

