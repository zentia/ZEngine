# ZEngine Bindless Texture Path

> **Status banner (2026-05, ImGui cleanup)**: the two **editor-side**
> consumer widgets described in the PR8b / PR8c / PR-V3-part-2 sections
> below -- `BindlessBlitSmoke` (`bindless_blit_smoke.{h,cpp}`) and
> `BindlessTexturePreview` (`bindless_texture_preview.{h,cpp}`) -- have
> been **deleted**. They were dead ImGui code once the editor Inspector
> became fully native ZSlate: their `DrawWidget` / `DrawPreview` entry
> points were only ever dispatched from the now-removed legacy ImGui
> `InspectorWindow`, so they had zero call sites. The PR sections about
> them are kept below as **historical landing notes** (they document how
> the bindless blit pipeline was first exercised), not current code.
> What survives, unchanged: the `bindless_texture_blit_pipeline.{h,cpp}`
> RHI pipeline (DX12 + Vulkan) and the gated smoke-TEST harness under
> `dx12/test/`. The only live remnant of the preview module is its
> `texture2d` type predicate, relocated to
> `InspectorAssetCommon::IsTexture2DInspectorAssetType`. Texture assets
> are previewed through the native ZSlate Preview window.

## 概述 / Scope

This document is the **single source of truth** for ZEngine's bindless
texture descriptor path -- the cross-backend `RHIBindlessTextureManager`
abstraction, its Vulkan and DX12 backends, the SM 6.6 / `VK_EXT_descriptor_indexing`
toolchain plumbing, the smoke-test harness, and the per-PR open items.

It was extracted from `AGENTS.md` §2.9 once the section grew past 400 lines
and started to overshadow the rest of the project conventions file. `AGENTS.md`
now keeps a short pointer block; this file holds the full design rationale and
the per-PR landing notes (PR1 through PR8c plus the route-B retrofit
PR9-PR11, plus the helper / smoke-test follow-ups).

If you are an AI agent that landed here from `AGENTS.md`, you are in the
right place -- read on. If you are a human looking for the high-level
"how does ZEngine pick descriptor paths" summary, see
`doc/MULTI_API_RHI_GUIDE.md` first; this file is the deep dive.

Cross-references back into the codebase use forward slashes and the
case-as-on-disk; on case-insensitive filesystems either form works.

---

## Bindless texture path (PR1 / PR2 / PR3)

The Vulkan backend ships a global bindless texture descriptor table.
The cross-backend touch points are:

- `RHI::supportsBindlessTextures()` / `maxBindlessSampledImages()` /
  `maxBindlessStorageBuffers()` -- capability queries on the base
  class. Default `false / 0 / 0`. Vulkan overrides them after probing
  `VK_EXT_descriptor_indexing` + the required feature bits at
  device-init time. DX12 / WebGL2 still inherit the `false / 0 / 0`
  default (DX12 will gain its own override in PR4).
- `RHI::getBindlessTextureManager()` -- returns
  `RHIBindlessTextureManager*` or `nullptr`. Pointer is owned by the
  RHI; lifetime equals `RHI::clear()`. Callers MUST guard on
  `supportsBindlessTextures()` before dereferencing.
- `RHIBindlessTextureManager` interface (`rhi.h`) exposes:
  - `capacity()` -- slot count provisioned at init, already clamped
    against the driver-reported
    `maxPerStageDescriptorUpdateAfterBindSampledImages`.
  - `allocate(view, sampler) -> uint32_t` -- reserves a slot and writes
    the descriptor immediately. Returns `kInvalidBindlessIndex`
    (0xFFFFFFFF) on failure (table full, manager unusable, null args).
  - `free(uint32_t)` -- returns slot to a LIFO free list. **Slot 0 is
    reserved as the "default / missing texture" placeholder** and
    cannot be freed.
  - `update(idx, view, sampler)` -- in-place re-write for streaming
    systems that swap mip pyramids.

Vulkan-specific impl (`vulkan_bindless_texture_manager.{h,cpp}`):

- Single `VkDescriptorSet`, single binding (binding=0,
  `COMBINED_IMAGE_SAMPLER`, count=capacity), flagged with
  `UPDATE_AFTER_BIND` + `PARTIALLY_BOUND` +
  `VARIABLE_DESCRIPTOR_COUNT`.
- Pool created with
  `VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT_EXT`; layout with
  `VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT_EXT`.
- Set index reserved for bindless: see
  `VulkanBindlessTextureManager::kBindlessDescriptorSet` (=0). Any
  pipeline layout that consumes the bindless path MUST use that set
  index -- keep it as the single source of truth, do not hard-code 0
  in shader-side / pipeline-side code.
- `allocate / free / update` are mutex-protected; concurrent draw
  recording that reads previously-bound slots is permitted thanks to
  `UPDATE_AFTER_BIND`.

Lifecycle hooks:

- `VulkanRHI::Initialize()` constructs the manager only when
  `m_bindless_supported && m_max_bindless_sampled_images > 0`. If
  `initialize()` reports failure at runtime (e.g. driver rejects pool
  creation), the manager is reset and `m_bindless_supported` is
  demoted to `false` so callers fall back gracefully.
- `VulkanRHI::clear()` calls `shutdown()` on the manager BEFORE the
  logical device is destroyed (the pool / layout are children of
  `m_device`).

Shader-side conventions (not yet wired; consumers will arrive in
PR5+):

- GLSL: `texture(g_BindlessTextures[nonuniformEXT(idx)], uv)`. Requires
  `#extension GL_EXT_nonuniform_qualifier : require`. The binding
  declaration uses `layout(set = 0, binding = 0) uniform
  sampler2D g_BindlessTextures[];` (unsized).
- HLSL (DX12 SM 6.6, future PR4):
  `ResourceDescriptorHeap[NonUniformResourceIndex(idx)]`. No layout
  declaration is needed.

Capacity defaults (from PR2): PC desktop 16384 sampled images / 1024
storage buffers; mobile (Android / OHOS) 4096 / 256. Both clamped at
runtime against `maxPerStageDescriptorUpdateAfterBind*`.

DX12-specific impl (PR4 -- `dx12_bindless_texture_manager.{h,cpp}`):

- DX12 has at most one CBV/SRV/UAV heap and one Sampler heap bound at
  a time. The manager therefore owns a **dedicated**
  `SHADER_VISIBLE`, `CBV_SRV_UAV` `ID3D12DescriptorHeap` whose entire
  range is the bindless SRV table. The legacy
  `DX12RHI::m_cbv_srv_uav_heap` (used by ImGui SRV / generic
  per-material SRV) is intentionally left untouched -- existing call
  sites are unaffected, at the cost of a future `SetDescriptorHeaps`
  swap when both paths coexist.
- Heap descriptor index N == bindless slot N == shader-side
  `ResourceDescriptorHeap[N]` index. No root-signature descriptor
  table mapping; consumers will use SM 6.6 dynamic resource binding.
- SRV ingress: source `RHIImageView` is expected to already hold a
  CPU-side SRV (created by the engine's standard view path on the
  non-shader-visible heap). `allocate / update` perform a single
  `ID3D12Device::CopyDescriptorsSimple(1, dst, src, CBV_SRV_UAV)` --
  host operation, no GPU sync required. If the source view's
  `getCpuHandle().ptr == 0` the slot is left stale and a warning is
  logged; reusing the slot via `update()` later is fine.
- **Sampler caveat**: DX12 keeps samplers in a separate
  `D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER` heap, which the SRV-side
  manager cannot describe. The `sampler` parameter to `allocate()` /
  `update()` is **silently ignored** on DX12; PR5 shader bindings
  will rely on a small static-sampler array declared in the root
  signature. A parallel sampler-bindless table lands in a future PR
  if/when a real consumer needs heterogeneous samplers.
- Capacity default: 8192 SRVs (a few MB of heap memory). Not clamped
  against any driver-reported maximum because Resource Binding
  Tier 2/3 already guarantees >=1M descriptors per shader-visible
  heap; if `CreateDescriptorHeap` ever fails the manager logs and
  demotes to legacy.
- Capability probe (`DX12RHI::Initialize`): the bindless path is
  enabled iff
  `D3D12_FEATURE_SHADER_MODEL.HighestShaderModel >= D3D_SHADER_MODEL_6_6`
  AND `D3D12_FEATURE_D3D12_OPTIONS.ResourceBindingTier >=
  D3D12_RESOURCE_BINDING_TIER_2`. Either probe failing -> manager not
  constructed, `supportsBindlessTextures()` stays false, and
  `getBindlessTextureManager()` returns `nullptr`.

Lifecycle hooks (DX12):

- `DX12RHI::Initialize()` constructs the manager after fence creation
  but before `prepareContext()`. Init failure resets the manager and
  keeps `m_bindless_supported = false`.
- `DX12RHI::clear()` calls `shutdown()` + reset BEFORE `clearSwapchain`
  (and obviously before `m_device.Reset()`); the heap is a child of
  the device.

Things still **NOT done** after PR5c (call out so future PRs plan
correctly):

1. No production material / runtime pipeline currently uses
   `getBindlessRootSignatureFlags()`. The smoke-test (PR5c) is the
   only consumer; it proves the toolchain works, but the engine's
   actual draw path still uses legacy descriptor-table root
   signatures. The next PR in this thread (let's call it PR6) will
   migrate the first real material -- probably the post-process
   tonemap pass, since it has a single texture input -- to the
   bindless root-signature template.
2. The `(texture_index, sampler_index)` packing convention from the
   smoke-test (uint16:uint16, low half = bindless table index, high
   half = static-sampler array slot) is now a first-class engine API:
   `BindlessIndex::pack / unpackTexture / unpackSampler` +
   `kTextureIndexBits / kSamplerIndexBits / kTextureIndexMask /
   kSamplerIndexMask / kMaxTextureIndex / kMaxSamplerIndex` live in
   `runtime/function/render/interface/rhi.h` directly below
   `RHIBindlessTextureManager`. All four backends and the HLSL
   shader-side unpack code (currently only `bindless_smoke.hlsl`)
   MUST go through this helper -- the smoke-test
   `static_assert`s round-trip identities and pins
   `kTextureIndexMask == 0xFFFFu` so any width drift breaks the
   build before it can ship a silent texture-vs-sampler swap.
   Truncation behavior is contractual (oversized halves silently
   mask, do NOT overflow into the neighboring half); the smoke-test
   asserts this too.
3. Sampler-bindless (`SAMPLER_HEAP_DIRECTLY_INDEXED` + a per-sampler
   table similar to the SRV one) is intentionally deferred.
   Static-sampler array covers every shipping use case and avoids a
   second descriptor heap on the hot path.

Slot-0 placeholder (PR5a):

Both backends now own and upload a 1x1 RGBA8 opaque-white texture
into bindless slot 0 during their bindless manager's
`initialize()`. This guarantees that any shader sampling an
unbound bindless index sees defined data (white) instead of stale
descriptor memory, which is required behaviour under Vulkan's
`PARTIALLY_BOUND` (and is just good hygiene on DX12). Resources
are owned by the manager (image / memory / view / sampler on
Vulkan, `ID3D12Resource` + 1-slot non-shader-visible SRV heap on
DX12) and freed in `shutdown()`. The upload runs synchronously on
the engine's graphics queue with a dedicated fence -- no
dependency on the higher-level texture pipeline, so it is safe to
call before any of those systems exist. `initialize()` therefore
now requires the physical device + graphics queue + queue-family
on Vulkan, and the command queue on DX12.

Sampler note (Vulkan-only): the slot-0 placeholder is shipped with
a default linear-clamp `VkSampler` owned by the manager. DX12
keeps samplers in a separate heap (see "Sampler caveat" above) and
its slot 0 is therefore SRV-only -- the shader-side sampler is
expected to come from the root signature's static-sampler array
(landing in PR5b/c).

DX12 toolchain hooks (PR5b):

PR5b lands the DX12-side compiler / root-signature plumbing required
to actually emit a bindless HLSL pixel shader. **No consumer is
wired yet** -- this is purely the toolchain. The first consumer is
the smoke-test in PR5c.

- `DX12ShaderCompiler::compileFromFile` / `compileFromSource` now take
  two extra optional trailing args: `target_profile` and
  `hlsl_version`. Both default to `""` so every existing call site
  (preview renderer, inspector shader-validate, runtime
  `createShaderModuleFromFile`) compiles byte-for-byte unchanged --
  empty `target_profile` falls through to the legacy stage-derived
  default (`vs_6_0` / `ps_6_0` / ...), and empty `hlsl_version` omits
  the `-HV` flag (DXC default = HV 2018). Bindless-aware shaders pass
  e.g. `target_profile="ps_6_6"`, `hlsl_version="2021"` to opt in to
  SM 6.6 + HLSL 2021 (required for
  `ResourceDescriptorHeap[NonUniformResourceIndex(idx)]`).
- The DXIL on-disk cache key was extended to include both
  `target_profile` and `hlsl_version`. Two compiles of the same
  `.hlsl` under different profiles (e.g. `ps_6_0` vs `ps_6_6`)
  therefore land in distinct cache slots; existing legacy cache
  entries continue to work because their key resolves with the empty
  strings, matching what every legacy call site passes today.
- `DX12RHI::getBindlessRootSignatureFlags()` returns the
  `D3D12_ROOT_SIGNATURE_FLAGS` set required by SM 6.6
  `ResourceDescriptorHeap[]`. Bindless-aware materials OR this into
  their existing flag set unconditionally:
  ```cpp
  desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
             | rhi->getBindlessRootSignatureFlags();
  ```
  - When `supportsBindlessTextures()` is `true` (SM 6.6 + Resource
    Binding Tier 2+), returns
    `D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED`.
  - When bindless is unsupported, returns
    `D3D12_ROOT_SIGNATURE_FLAG_NONE` so the OR is a no-op and legacy
    materials keep their existing flag set verbatim.
  - `_SAMPLER_HEAP_DIRECTLY_INDEXED` is **deliberately NOT set** even
    on Tier 3 hardware: PR5c expresses samplers via root-signature
    static-sampler array, not a sampler-bindless table. The hook to
    flip it on lives as a comment in the implementation, ready for
    a future sampler-bindless PR.

DX12 bindless smoke-test (PR5c):

PR5c lands the first end-to-end consumer of the PR4 + PR5a/b
toolchain: a standalone executable that compiles a SM 6.6 + HLSL
2021 pixel shader using `ResourceDescriptorHeap[]`, builds a
bindless-aware root signature via `getBindlessRootSignatureFlags()`,
and validates that `CreateGraphicsPipelineState` accepts the
combination. **No production code path consumes bindless yet** --
this is purely a CI / developer smoke-test that the toolchain is
healthy.

- Lives at
  `engine/Source/Runtime/Function/Render/Interface/dx12/test/`:
  - `bindless_smoke.hlsl` -- pixel shader (`ps_6_6` + `-HV 2021`).
    Uses `ResourceDescriptorHeap[NonUniformResourceIndex(idx)]`
    for textures and four root-signature static samplers
    (linear-wrap / linear-clamp / point-wrap / point-clamp,
    s0..s3) selected via a four-way switch on the upper half of a
    32-bit packed root constant. Indices unpack as
    `tex = packed & 0xFFFF; samp = (packed >> 16) & 0xFFFF`.
  - `bindless_smoke_vs.hlsl` -- trivial fullscreen-triangle VS
    (`vs_6_0`, default HV) driven by `SV_VertexID`. Stays at SM 6.0
    on purpose to also validate that mixing a legacy-profile VS
    with a bindless-profile PS in the same PSO works -- this is
    the exact configuration future bindless materials will use.
  - `dx12_bindless_smoke_test.cpp` -- standalone main():
    1. Creates a bare `ID3D12Device` (hardware adapter; WARP
       fallback for headless / CI hosts).
    2. Reproduces the bindless probe inline (`SM >= 6.6 &&
       ResourceBindingTier >= 2`); on a fail it returns exit
       code **77** (autotools / CTest "skip" convention) so CI
       buckets unsupported hosts as skipped, not failed.
    3. Drives `DX12ShaderCompiler::compileFromFile` twice -- VS
       with the legacy default args (validates PR5b's "empty
       params == byte-for-byte unchanged" guarantee), PS with
       `target_profile="ps_6_6", hlsl_version="2021"`.
    4. Builds a `D3D12_ROOT_SIGNATURE_DESC` with
       `ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
       CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED` (the latter pulled
       from a local mirror of `getBindlessRootSignatureFlags`,
       deliberately duplicated so the test stays
       RHI-class-independent), one 32-bit root constant at b0,
       and the four static samplers.
    5. Calls `CreateGraphicsPipelineState`. Success == toolchain
       OK. Any failure here gets logged and bubbled up as exit
       code 1.
- Build hook: a `ZENGINE_BUILD_BINDLESS_SMOKE_TEST` CMake option
  (default **OFF**) gates `add_executable(DX12BindlessSmokeTest)`
  in the test/CMakeLists.txt subdirectory. The subdirectory is
  always `add_subdirectory`'d on Windows builds (so the option
  shows up in cmake-gui) but the inner CMakeLists early-returns
  when the option is OFF or when the platform isn't Windows --
  zero configuration cost on default builds.
- Source-file gating: `dx12_bindless_smoke_test.cpp` is removed
  from ZRuntime's GLOB_RECURSE explicitly (alongside the
  pre-existing `shader_lab_test.cpp` REMOVE_ITEM). This is
  required because ZRuntime is built with /WHOLEARCHIVE for the
  reflection auto-registration system, which would otherwise
  pull the test's `main()` into every consumer .exe and hit
  LNK2005.
- The test target links `ZRuntime` PRIVATE to consume
  `DX12ShaderCompiler` + the engine's logging macros, plus the
  system libraries `d3d12 dxgi`. DXC itself is loaded at
  runtime via LoadLibrary by the compiler class -- no extra
  link-time dxcompiler dependency.
- The test target also PRIVATE-links `ZEnginePCH` (and adds
  `engine/Source/CommonPCH` to its include path) so EASTL
  headers are resolvable -- ZRuntime PRIVATE-links the PCH and
  therefore does not propagate its include directories.
  Mirrors the pattern used by Editor / Launcher / WebLauncher /
  profiler. The PCH is **not** REUSE_FROM'd; the smoke-test is
  a one-TU executable and a fresh PCH compile costs <1s.
- Running: `cmake -B build -DZENGINE_BUILD_BINDLESS_SMOKE_TEST=ON
  && cmake --build build --target DX12BindlessSmokeTest && \
  ./build/.../DX12BindlessSmokeTest`. Exit 0 = pass, 1 = fail,
  77 = skipped (bindless not supported on host).

DX12 bindless RHI wiring (PR6):

PR6 lifts the toolchain from PR5b/c into the cross-backend
`createPipelineLayout` path so that any future material can declare a
bindless descriptor set through plain RHI structs (no DX12-specific
calls) and get a working bindless-aware root signature on the DX12
backend. Vulkan was already consuming the same `bindingFlags` field
since PR4; PR6 is the symmetrical DX12 side. **No production
consumer is wired yet** -- this is purely the cross-backend plumbing.
The first consumer is intentionally deferred to a later PR (most
likely the editor's inspector texture-preview path, see PR6 design
discussion in chat history).

- Bindless detection in `DX12RHI::createPipelineLayout`: a
  descriptor set is treated as bindless iff **at least one** binding
  carries `RHI_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT` in
  its `RHIDescriptorSetLayoutBinding::bindingFlags`. (Mixing
  bindless and bindful bindings inside the **same** set is not
  supported on either backend; the ZEngine
  `BindlessTextureManager` uses a single `binding=0` slot in
  space0.)
- Effects when at least one bindless set is present:
  1. Bindless sets contribute **zero** descriptor-table ranges /
     root parameters. Their bindings are **not** added to
     `DX12PipelineLayout::m_bindings`, so the existing
     `cmdBindDescriptorSetsPFN` path naturally skips them with zero
     further branching -- descriptors are sourced directly from the
     bindless heap via `ResourceDescriptorHeap[NonUniformResourceIndex(idx)]`
     in HLSL.
  2. The root signature gains exactly **one** trailing 32-bit root
     constant at `b0` / `space0` (one DWORD), used to push the
     packed `BindlessIndex::pack(tex, sampler)` value at draw time.
     The parameter index is recorded on `DX12PipelineLayout` via
     `setBindlessInfo()` and read back through
     `getBindlessRootConstantParameterIndex()` /
     `usesBindless()`.
  3. `desc.Flags |= getBindlessRootSignatureFlags()` -- today this
     is `CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED` only; the
     `SAMPLER_HEAP_DIRECTLY_INDEXED` hook lives as a comment in
     `DX12RHI::getBindlessRootSignatureFlags()` for a future
     sampler-bindless PR (PR5c-style static samplers remain the
     current model).
  4. **Hard fail** if any set is bindless but
     `m_bindless_supported == false` (SM<6.6 or
     ResourceBindingTier<2). Falling back to bindful is unsafe --
     the shader DXIL would still reference
     `ResourceDescriptorHeap[]` -- so we surface a clear
     `LOG_ERROR(ZRender)` and return `false` from
     `createPipelineLayout`.
- New RHI command: `RHI::cmdSetBindlessIndexPFN(commandBuffer,
  pipelineBindPoint, layout, packed_index)`.
  - DX12 implementation:
    `SetGraphicsRoot32BitConstant` /
    `SetComputeRoot32BitConstant` on the parameter index recorded
    by `DX12PipelineLayout`. **Does NOT** call
    `SetGraphicsRootSignature` itself -- doing so would silently
    invalidate any descriptor tables set in between on the same
    root signature (D3D12 spec: changing the bound root signature
    resets all root parameters). `cmdBindDescriptorSets` remains
    the single source of truth for "active root signature is now
    this layout's"; this PFN just pushes 32 bits onto it.
  - Default base implementation: empty no-op (Vulkan / Metal /
    WebGL2 inherit it). This is one of the few PFNs that is **not**
    pure-virtual: promoting it to `=0` would force four backends
    to ship throwaway stubs, and the contract is "calling this on
    a non-bindless layout is a silent no-op everywhere" -- which
    `if (!layout->usesBindless()) return;` enforces in the DX12
    override and the empty default trivially satisfies elsewhere.
  - Calling on a non-bindless DX12 layout is also a silent no-op
    (defensive guard inside the DX12 override), so legacy
    materials that mis-call this degrade safely instead of
    blowing away their root parameters.
- Smoke-test (PR5c) gained PR6 API-compat canaries: a
  `static_assert` block + a never-called function probe that take
  the address of `&RHI::cmdSetBindlessIndexPFN` with the agreed
  signature. Any drift in the bindless RHI surface area
  (`bindingFlags` field type / name,
  `RHI_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT` enumerator,
  `cmdSetBindlessIndexPFN` signature) breaks the smoke-test
  compile. Runtime phases of the smoke-test still go through raw
  D3D12 -- wiring `DX12RHI::Initialize` into the test would
  require a headless RHI variant or a hidden GLFW window, both out
  of scope for PR6.

`BindlessIndex` packing helper (PR6 follow-up):

Lifted out of the smoke-test ad-hoc `(tex | samp << 16)` literal
into a first-class engine API so every backend, every material,
and every HLSL shader speaks the same packing format.

- Location: `runtime/function/render/interface/rhi.h`, in
  `namespace BindlessIndex`, immediately below
  `RHIBindlessTextureManager`.
- API surface (all `inline constexpr noexcept`):
  - `pack(uint32_t texture_index, uint32_t sampler_index) -> uint32_t`
  - `unpackTexture(uint32_t packed) -> uint32_t`
  - `unpackSampler(uint32_t packed) -> uint32_t`
  - Constants: `kTextureIndexBits` / `kSamplerIndexBits` (16 each),
    `kTextureIndexMask` / `kSamplerIndexMask` (`0xFFFFu` each),
    `kMaxTextureIndex` / `kMaxSamplerIndex` (== mask values).
- Layout: bits `[0..15]` = texture index (slot in the bindless
  CBV/SRV/UAV heap); bits `[16..31]` = sampler index (slot in the
  per-pipeline static-sampler array). 65 535 simultaneously live
  texture slots is several orders of magnitude above any ZEngine
  demo's working set; 65 536 sampler permutations is far past D3D12's
  hard cap of 2 032 static samplers per root signature.
- Truncation contract: out-of-range halves silently mask down to
  their N-bit window. No bounds-check on the draw hot path; the
  smoke-test's `static_assert` block catches drift, and debug
  builds catch upstream mis-allocations elsewhere.
- HLSL twin: `bindless_smoke.hlsl` does
  `texture_index = g_packed_indices & 0xFFFFu;` and
  `sampler_index = (g_packed_indices >> 16) & 0xFFFFu;`. The
  smoke-test pins `kTextureIndexMask == 0xFFFFu` and
  `kTextureIndexBits == 16u` via `static_assert`, so widening
  the texture half in `rhi.h` without updating the HLSL breaks
  the build at compile time.
- Sampler-bindless (`SAMPLER_HEAP_DIRECTLY_INDEXED` + a sampler
  descriptor heap) is intentionally NOT modeled here -- the high
  half is a static-sampler-array slot, not a heap index. See
  "Things still NOT done" item 3 / 4 below.

Things still **NOT done** after PR6 (call out so future PRs plan
correctly):

> **Status note (post-PR8c)**: items 1, 2, and parts of 4 below have
> since been addressed. See the PR7 / PR8a / PR8b / PR8c sections
> further down for landing notes; the canonical current-state list
> lives at the end of the PR8c section.

1. ~~Still no production material / runtime pipeline that declares a
   bindless set through `RHIDescriptorSetLayoutBinding::bindingFlags`.~~
   **PR7 partially addresses this**: a real RHI-level bindless
   pipeline (`BindlessTextureBlitPipeline`) now exists and goes
   through `createDescriptorSetLayout` -> `createPipelineLayout` ->
   `createGraphicsPipelines`, exercising the entire bindless code
   path in `DX12RHI` for the first time. **NOT yet done**: an actual
   editor / runtime consumer that *calls* `recordBlit` on every
   frame -- the Inspector texture-preview UI integration is the
   PR8 follow-up (see PR7 section below for the explicit deferral
   reasoning).
2. `DX12RHI::cmdBindDescriptorSetsPFN` still calls
   `SetDescriptorHeaps(m_cbv_srv_uav_heap, m_sampler_heap)` --
   the bindless heap (owned by `DX12BindlessTextureManager`) is
   **not** bound on the command list yet. A bindless draw needs
   `SetDescriptorHeaps` to reference the bindless heap instead of
   `m_cbv_srv_uav_heap`. This is deferred because the heap is
   shared with the ImGui SRV slot allocator today; unifying or
   separating the two heaps is a non-trivial refactor that should
   happen at the same time as the first real bindless consumer
   lands (so we have a tested call site to validate against).
   **Status after PR7**: still NOT done. PR7 builds the pipeline
   but does not invoke a draw against it; PR8 (Inspector
   integration) is the right place to address the heap binding,
   because that's the first PR that actually issues a bindless
   draw on a real command list.
3. Sampler-bindless (`SAMPLER_HEAP_DIRECTLY_INDEXED`) remains
   off; static-sampler array is still the model.
4. Vulkan / Metal `cmdSetBindlessIndexPFN` overrides are
   not implemented (default empty no-op suffices until a real
   bindless consumer lands on those backends; Vulkan will most
   likely route the index through push-constants when it does).
   **WebGL2 is permanently no-op** -- the backend has no bindless
   support and never will (see Conventions in `webgl2_rhi.h`).

---

DX12 bindless texture-blit pipeline (PR7):

PR7 is the first PR to instantiate a real, RHI-abstracted bindless
graphics pipeline. The PR4-6 toolchain (DXC SM 6.6 -> bindless root
signature -> bindless descriptor set layout) and the PR5c smoke-test
were both validated independently of the engine's `DX12RHI`; PR7
finally drives `DX12RHI::createPipelineLayout`'s bindless branch with
a real input. The deliverables are:

1. **Static-sampler bank, baked into every bindless pipeline layout.**
   `DX12RHI::createPipelineLayout` now appends 4 static samplers at
   `s0..s3 / RegisterSpace 0` whenever the layout contains at least
   one bindless descriptor set:
   - s0 = `MIN_MAG_MIP_LINEAR` + `ADDRESS_WRAP`  (LinearWrap)
   - s1 = `MIN_MAG_MIP_LINEAR` + `ADDRESS_CLAMP` (LinearClamp)
   - s2 = `MIN_MAG_MIP_POINT`  + `ADDRESS_WRAP`  (PointWrap)
   - s3 = `MIN_MAG_MIP_POINT`  + `ADDRESS_CLAMP` (PointClamp)
   All four are `D3D12_SHADER_VISIBILITY_PIXEL` -- matches PR5c's
   smoke-test inline definition byte-for-byte. The count is exposed
   as `DX12RHI::kBindlessStaticSamplerCount` (`= 4u`) so a single
   constexpr is the SSOT for both the RHI implementation and the
   smoke-test's `static_assert` canary.

   Decision: hard-code the bank in the RHI rather than letting each
   bindless layout declare its own. Reason: bindless HLSL must pick
   a sampler at runtime via the `samp_idx = (packed_index >> 16)`
   field of the bindless root constant; a descriptor-table sampler
   binding cannot vary per-draw without descriptor-bindless
   (`SAMPLER_HEAP_DIRECTLY_INDEXED`), which is intentionally still
   off. Hard-coded bank == zero-config for the shader author and
   one failure mode pinned by `static_assert`.

2. **`BindlessTextureBlitPipeline`** at
   `runtime/function/render/interface/dx12/utility/bindless_texture_blit_pipeline.{h,cpp}`.
   First real RHI-level bindless graphics pipeline:
   - Single-attachment R8G8B8A8_UNORM color pass, no depth, no MSAA.
   - Fullscreen-triangle VS (`SV_VertexID` only, no vertex buffer).
   - Bindless PS reads `ResourceDescriptorHeap[NonUniformResourceIndex(tex_idx)]`
     and selects one of the 4 static samplers by `(packed_index >> 16)`.
   - `initialize` calls
     `createDescriptorSetLayout` (1 binding, `descriptorCount=0`,
     `bindingFlags = RHI_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
     RHI_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT`) ->
     `createPipelineLayout` (which now emits the s0..s3 bank +
     OR's in `getBindlessRootSignatureFlags()`) ->
     `createGraphicsPipelines`.
   - `recordBlit` issues `cmdBindPipeline -> cmdSetViewport ->
     cmdSetScissor -> cmdSetBindlessIndexPFN(packed) -> cmdDraw(3,1,0,0)`.
     Caller wraps the call in a `cmdBeginRenderPass` /
     `cmdEndRenderPass` pair; the pipeline does not own the render
     pass / framebuffer (those are caller-supplied so different
     consumers can target different RTs).

   File location rationale: under `dx12/utility/` (peer of
   `dx12/test/`) rather than `interface/utility/`. The .cpp pulls
   in `dx12_shader_compiler.h` for the SM 6.6 + HLSL 2021
   compileFromFile overload, which makes it DX12-aware. The
   existing `Runtime/CMakeLists.txt` regex EXCLUDE patterns
   (`dx12/.*\\.cpp` excluded on Apple/Mobile/Web) automatically
   keep this file out of non-Windows builds with zero CMake edits.
   When PR8 wants Vulkan parity, it can either (a) add
   `interface/utility/` with an RHI-only shim that selects per
   backend at link time, or (b) drop a sibling
   `vulkan/utility/bindless_texture_blit_pipeline.{h,cpp}` --
   decision deferred until the actual cross-platform editor needs
   it.

   HLSL files: `dx12/utility/shaders/bindless_blit_vs.hlsl`,
   `dx12/utility/shaders/bindless_blit_ps.hlsl`. Compiled at
   runtime by the same `DX12ShaderCompiler` cache used by the
   editor's preview / inspector paths -- first call hits the
   compiler, subsequent calls hit the on-disk cache.

3. **Smoke-test canary upgraded to PR7.**
   `dx12_bindless_smoke_test.cpp` now includes
   `runtime/function/render/interface/dx12/dx12_rhi.h` and
   `dx12/utility/bindless_texture_blit_pipeline.h`, then
   `static_assert`s:
   - `DX12RHI::kBindlessStaticSamplerCount == 4u` -- pins the bank
     size against future regressions (e.g. someone adding s4
     `AnisotropicWrap` without updating the HLSL).
   - `BindlessBlitSampler::LinearWrap == 0`, `LinearClamp == 1`,
     `PointWrap == 2`, `PointClamp == 3` -- pins the C++
     enum -> HLSL `register(s0..s3)` mapping.
   The smoke-test still does **NOT** instantiate a `DX12RHI` -- the
   "headless DX12RHI bringup" is still out of scope (would require
   either a hidden GLFW window or a `DX12RHI::InitializeHeadless`
   variant; both are bigger than this PR's contract). The
   compile-time canaries are sufficient to catch the relevant drift
   modes (bank size, sampler order, packed-index bit layout) at
   build time. Phases 4 and 5 of the smoke-test continue to verify
   that the same root-signature shape (4 static samplers + 1 root
   constant + bindless flag) survives `D3D12SerializeRootSignature`
   and `CreateGraphicsPipelineState`; PR7's `static_assert`s ensure
   the RHI emits exactly that shape too.

What PR7 explicitly does NOT do (handed to PR8):

- **Inspector / editor UI integration.** PR7 wired up the pipeline
  but does not call `recordBlit` from any UI code path. The Project
  Window's "selected texture asset" -> "show 256x256 preview"
  feature, plus the off-screen RT lifecycle (`RHIRenderPass` +
  `RHIImage` + `RHIFramebuffer`) and the `ImTextureID` bridge
  needed to draw the RT into ImGui, all live in PR8. Reason for
  the split: each piece (pipeline / inspector route / RT lifecycle
  / ImGui bridge) is reviewable on its own; bundling them turned
  PR7 into a >1000-line patch that mixed RHI-core changes
  (sampler bank, kBindlessStaticSamplerCount) with editor-side UX,
  hurting reviewability.
- **`SetDescriptorHeaps` swap** (PR6 NOT-done #2). PR7 builds the
  pipeline but never executes a draw against it. PR8's first
  `recordBlit` call from a real frame is the right time to surface
  / fix the heap-binding gap, because the failure (if any) would
  be observable as a black preview rather than a silent
  correctness issue.
- **Vulkan / Metal / WebGL2 parity.** Same reasoning as the file
  location decision above -- defer until a cross-platform editor
  actually needs the preview tile.

Things still **NOT done** after PR7:

> **Status note (post-PR8c)**: items 1 and 2 have been addressed by
> PR8b (smoke widget, first `recordBlit` consumer) + PR8c (production
> Inspector preview) and PR8a (scoped `SetDescriptorHeaps` swap).
> The canonical current-state list lives at the end of the PR8c
> section below.

1. Inspector UI integration (described above) -- PR8 candidate.
2. `cmdBindDescriptorSetsPFN` heap-swap to the bindless heap --
   PR8 candidate, scoped to land alongside the first `recordBlit`
   consumer so failure modes are observable on a real draw.
3. `BindlessTextureBlitPipeline` parity for Vulkan / Metal /
   WebGL2 (none of those backends have a real bindless consumer
   yet, so default-empty `cmdSetBindlessIndexPFN` is fine).
4. Sampler-bindless (`SAMPLER_HEAP_DIRECTLY_INDEXED`) -- still
   intentionally off; the static-sampler bank baked in by PR7 is
   the explicit alternative.

---

DX12 bindless heap swap (PR8a):

PR8a closes the heap-binding gap that PR6 and PR7 explicitly deferred:
a real bindless draw requires `SetDescriptorHeaps` to point at the
manager-owned bindless heap (the one
`ResourceDescriptorHeap[NonUniformResourceIndex(idx)]` indexes into),
not at the engine's legacy `m_cbv_srv_uav_heap` which holds ImGui SRVs
and per-material descriptors. Doing the swap inside
`DX12RHI::cmdBindDescriptorSetsPFN` was rejected because that PFN runs
on every legacy draw too -- a global swap-on-bind would penalise every
non-bindless caller and risk descriptor-table churn against no
benefit. PR8a takes the **scoped-swap** approach instead: the
`recordBlit` caller swaps in the bindless heap immediately before
`BindlessTextureBlitPipeline::recordBlit`, then the editor's existing
`editor_ui_pass.cpp:195-200` block swaps back to
`m_cbv_srv_uav_heap` before `ImGui_ImplDX12_RenderDrawData` so
`ImTextureID` SRV slots resolve correctly.

- Swap site (call-side, not RHI-side): both
  `bindless_blit_smoke.cpp` and `bindless_texture_preview.cpp` issue
  `command_list->SetDescriptorHeaps(1, &bindless_heap)` immediately
  before `BindlessTextureBlitPipeline::recordBlit`, inside the
  `cmdBeginRenderPass` / `cmdEndRenderPass` pair that targets the
  off-screen 256x256 RT. The bindless heap pointer comes from
  `DX12BindlessTextureManager::getDescriptorHeap()`.
- Swap-back site (engine-owned bookend): the existing block at
  `editor_ui_pass.cpp:195-200` was already binding
  `getCbvSrvUavDescriptorHeap()` before
  `ImGui_ImplDX12_RenderDrawData`. PR8a relies on this as the
  closing bookend -- no edit was needed there. The contract is now:
  any caller that swaps to the bindless heap MUST be inside the
  inspector onGUI tick (so the editor UI pass's swap-back is the
  next thing on the command list).
- Sampler heap is **not** swapped -- bindless on DX12 uses
  root-signature static samplers (s0..s3, baked into the bindless
  pipeline layout by PR7), not a sampler-bindless heap. The
  `m_sampler_heap` binding from `cmdBindDescriptorSetsPFN` is left
  untouched.

PR8a is a zero-cost change for any caller that does NOT use bindless:
the SRV-side swap only runs from inside `recordBlit`-issuing widgets,
and the swap-back was already in editor_ui_pass for ImGui's benefit.

DX12 bindless smoke widget (PR8b):

PR8b lands the **first end-to-end bindless draw inside the editor**.
Up to PR7 the bindless toolchain (PR4-7) and pipeline
(`BindlessTextureBlitPipeline`) had no production caller; the
PR5c / PR7 smoke-tests validated build-time canaries only and never
instantiated a `DX12RHI`. PR8b is a dev-only ImGui widget that drives
the entire path with an engine-generated 64x64 procedural
checkerboard, so any first-frame failure (heap swap, descriptor
write, RT lifecycle, ImGui SRV bridge) is observable on a real
command list before a user-facing feature depends on it.

- Lives at
  `engine/Source/Editor/editor_window/inspector_window/bindless_blit_smoke.{h,cpp}`.
  Single function `BindlessBlitSmoke::drawWidget()` rendered into
  the current ImGui window.
- Activation: dev-only `[Dev] Bindless Blit Smoke` collapsing
  header at the top of `InspectorWindow::onGUI`. Off by default
  (gated by a checkbox to also avoid lazily allocating GPU
  resources on every editor session). Stays in the codebase as a
  permanent canary against regressions in the bindless path -- it
  catches drift cheaper than a blank texture-preview window would.
- Vertical slice exercised, in order:
  1. CPU-side: synthesise 64x64 RGBA8 checker pixels.
  2. RHI: `createImage` (SAMPLED_BIT) + `createImageView` +
     `createSampler` -- the engine's standard texture path.
  3. RHI: `writeImageData` to upload pixels (one-shot, on
     `RHIBindlessTextureManager` allocation).
  4. RHI: `RHIBindlessTextureManager::allocate(view, sampler)` ->
     bindless slot N.
  5. Per-frame: lazy-init off-screen 256x256 R8G8B8A8_UNORM
     `RHIRenderPass` + `RHIImage` + `RHIFramebuffer`.
  6. Per-frame: `SetDescriptorHeaps({bindless_heap})`
     (the swap from PR8a) inside the off-screen render pass.
  7. Per-frame:
     `BindlessTextureBlitPipeline::recordBlit(packed_index)` --
     this is the first time `recordBlit` runs against a real
     command list anywhere in the engine.
  8. Per-frame: `cmdEndRenderPass` (RT now contains the blitted
     checker).
  9. Per-frame: ImGui SRV bridge -- `DX12RHI::allocateImGuiSrvDescriptor`
     gives a slot in `m_cbv_srv_uav_heap`,
     `CreateShaderResourceView` writes the RT's `ID3D12Resource`
     into it, and `ImGui::Image` references the resulting
     `ImTextureID`.
  10. The editor UI pass's existing swap-back
      (`editor_ui_pass.cpp:195-200`) re-binds
      `m_cbv_srv_uav_heap` before
      `ImGui_ImplDX12_RenderDrawData` so the SRV slot resolves.
- Failure modes are all latched: any one-time init failure
  (RHI / pipeline / RT / shader) flips the widget into a "Failed"
  label and renders nothing further. Resources are
  intentionally never freed -- editor session lifetime ==
  widget lifetime.
- Non-DX12 backends: the widget renders an "unavailable" label
  and exits. Same fallback as PR8c's production preview.

DX12 bindless texture preview (PR8c):

PR8c is the **first user-facing feature** that consumes the bindless
path. It replaces the inspector's "generic .zasset" rendering for
recognised image extensions with an actual 256x256 GPU preview of
the on-disk pixels. The plumbing it relies on is identical to PR8b's
smoke widget, only the input is "real `.png` on disk" instead of an
engine-synthesised checker.

- Lives at
  `engine/Source/Editor/editor_window/inspector_window/bindless_texture_preview.{h,cpp}`.
  API:
  - `bool drawPreview(const std::filesystem::path&)` -- returns
    `true` if the preview rendered (caller short-circuits the
    generic asset inspector), `false` otherwise.
  - `bool isSupportedTextureExtension(const std::filesystem::path&)`
    -- pure extension match, no I/O. Lists must stay in sync with
    `inspector_window.cpp:363-369` (the dispatch site).
- Dispatch site: `inspector_window.cpp:5735-5747` -- when
  `ResolveInspectorAssetType` returns the texture pseudo-type,
  `BindlessTexturePreview::drawPreview` is called first; on
  success the inspector returns immediately, otherwise it falls
  through to the generic .zasset inspector. Mirrors the
  shader / material dispatch pattern used elsewhere in the same
  file.
- Supported source extensions (V1):
  `.png`, `.jpg`, `.jpeg`, `.tga`, `.bmp`. Loaded via
  `stbi_load` + 4-channel force, fed through the same RHI
  texture path as PR8b.
- Per-asset cache: keyed on the absolute asset path. Each entry
  owns:
  - One `RHIImage` + `RHIImageView` + `RHISampler` (the source
    texture, allocated one bindless slot).
  - One persistent `UPLOAD` buffer holding the most recent
    pixel snapshot (pre-warmed at allocation, refreshed on
    file mtime change via `LoadPreviewTextureImage`'s cache
    contract).
  - One off-screen 256x256 RT (`RHIImage` + `RHIRenderPass` +
    `RHIFramebuffer`) and one ImGui SRV slot (allocated via
    `DX12RHI::allocateImGuiSrvDescriptor`).
  Entries are created lazily on first `drawPreview` call and
  **never freed**. A future LRU cap (e.g. 64 entries) is a
  one-line change against the cache struct; the current
  trade-off matches PR8b's "permanent" model and assumes a
  realistic inspector working set of <100 textures per session.
- Per-frame work, when an entry is shown:
  1. If the source mtime changed since last frame, re-upload
     pixels into the persistent UPLOAD buffer and queue an
     inline `CopyTextureRegion` against the source `RHIImage`
     ahead of the bindless draw.
  2. `SetDescriptorHeaps({bindless_heap})` (PR8a swap).
  3. `cmdBeginRenderPass(off-screen RT)` ->
     `BindlessTextureBlitPipeline::recordBlit(packed_index)` ->
     `cmdEndRenderPass`.
  4. `ImGui::Image(srv_slot, {256, 256})`.
  5. The editor UI pass's existing swap-back
     (`editor_ui_pass.cpp:195-200`) handles ImGui's heap
     restoration.
- Failure modes:
  - Non-DX12 backend: returns `false` after rendering a
    "Bindless preview unavailable on this backend" label.
    The caller's generic-asset fallback then runs.
  - stbi_load failure: returns `false` and lets the generic
    .zasset inspector handle the file.
  - One-time pipeline / RT init failure: latches the entry
    into a permanent error state, returns `false` so the
    fallback inspector still produces something useful.

What PR8c explicitly does NOT do (handed to a future PR):

- **Texture `.zasset` preview.** Today the inspector flags
  post-import binary textures as generic `.zasset`. Once the
  `Texture2D` asset class plumbs through `AssetRegistry` and
  exposes its `RHIImage*`, the same `BindlessTexturePreview`
  service can be invoked for `.zasset` whose underlying class
  is `Texture2D`. `bindless_texture_preview.h:35-37` is the
  hook point.
- **Cross-backend parity.** Vulkan / Metal / WebGL2 still fall
  back to the "unavailable" label. A Vulkan implementation
  would need: (a) a sibling
  `vulkan/utility/bindless_texture_blit_pipeline.{h,cpp}` using
  GLSL `texture(g_BindlessTextures[nonuniformEXT(idx)], uv)` +
  push-constant for the packed index, (b) a non-empty
  `VulkanRHI::cmdSetBindlessIndexPFN` override
  (`vkCmdPushConstants`), and (c) the editor's preview pipeline
  to be backend-agnostic above the blit step.
- **LRU cap on the per-asset cache.** Acknowledged in
  `bindless_texture_preview.cpp:59-63`; one-line change when a
  real workload reveals the need.

Things still **NOT done** after PR8c:

1. ~~**`Texture2D` (`.zasset`) preview** -- the highest-priority
   remaining bindless work. PR8c only consumes raw image
   sources. Wire `BindlessTexturePreview::drawPreview` from the
   `.zasset` inspector path once `Texture2D` exposes a stable
   `RHIImage*` through `AssetRegistry`.~~ **Addressed by PR9-PR11
   (route B); see the section below for the actual landing.**
2. **Production draw consumer beyond the inspector preview.**
   The engine's forward / deferred / shadow / post-process
   pipelines still use legacy descriptor-table root signatures.
   Migrating the first one (probably the tonemap pass, single
   texture input) to `getBindlessRootSignatureFlags()` +
   `cmdSetBindlessIndexPFN` is the next strategic milestone.
   This is the original PR6-NOT-done #1 hand-off; PR7-PR8c
   built the toolchain + the editor consumer, but the **runtime**
   draw path is still bindful.
3. **Vulkan / Metal `BindlessTextureBlitPipeline` parity.**
   Required if (and only if) the editor's bindless preview needs
   to run on Linux / macOS. Today the PR8c widget shows
   "unavailable" on those backends.
   **WebGL2 is permanently excluded** from bindless: GLSL ES 3.00
   has no `GL_ARB_bindless_texture`, no `nonuniformEXT`, and
   `sampler2D` arrays are capped at `MAX_TEXTURE_IMAGE_UNITS`
   (16-32 in browsers) with dynamically-uniform index requirements.
   The WebGL2RHI overrides `supportsBindlessTextures() → false`
   explicitly and will never grow a `BindlessTextureManager`.
4. **Vulkan / Metal `cmdSetBindlessIndexPFN` overrides.**
   Default empty no-op until item 3 (or a non-DX12 production
   bindless caller) lands. Vulkan will most likely route the
   index through `vkCmdPushConstants`. **WebGL2 is permanently
   no-op** (see item 3 for rationale).
5. **LRU cap on `BindlessTexturePreview`'s per-asset cache.**
   Low priority; current behaviour is "never free", which is
   fine until someone scrolls through thousands of distinct
   textures in a single session.
6. **Sampler-bindless (`SAMPLER_HEAP_DIRECTLY_INDEXED`).**
   Permanently deferred -- the four static samplers baked into
   bindless pipeline layouts cover every shipping use case.
   Re-open only if a material legitimately needs runtime-varying
   `Mirror` / `MirrorOnce` / anisotropic samplers.

---

## PR9 / PR10 / PR11 -- Route B: UE-style imported preview

### Problem statement

PR8c shipped a **Unity-style** preview path: the Project window
surfaced raw `.png` / `.jpg` / `.tga` / `.bmp` files and the
inspector `stbi_load`'d them on selection. This worked end-to-end
but conflicted with two design directives that surfaced after PR8c:

1. **UE-style asset surfacing.** The user-visible asset in the
   Project window should be the *imported product* (`.zasset`
   authored by `Texture2D`), not the raw source image. Source
   images become inputs to the importer, not assets in their
   own right.
2. **Editor / cooker symmetry.** A headless cooker / build farm
   never sees `.png` files -- only `.zasset`. If the inspector
   stbi-loads raw images and the runtime can only consume
   `Texture2D::m_pixels`, the two paths disagree on what "this
   texture's pixels" means and on the format pivots in between.

### Decision: route B (minimal viable migration)

Three options were considered:

- **Route A** -- full UE-strict: GUID persistence in
  `SerializedFileHeader`, `AssetFile::saveAsset` half-implementation
  cleanup, Project-window source hiding, drag-and-drop
  auto-import, `Texture` base class reflection. ~5-7 PRs.
- **Route B** -- minimal viable: introduce `Texture2D` runtime
  asset class, wire the importer to write it via
  `AssetManager::WriteObjectToDiskThreadSafe`, retrofit
  `BindlessTexturePreview` to consume it. Skip GUID persistence
  and Project-window source hiding for now. ~3 PRs.
- **Route C** -- abandon UE-style, lean into Unity-style. Keep
  PR8c as-is, give up on cooker symmetry.

Route B was chosen. The remaining route-A items are tracked in
the backlog at the end of this section.

### PR9 -- `Texture2D` runtime asset class

`runtime/function/render/texture/Texture2D.{h,cpp}`. Pure CPU
state, deliberately backend-agnostic:

- Inherits `Texture` (placeholder base; see header comment for
  why we do NOT call `Super::Transfer`).
- Reflection: `REGISTER_CLASS(Texture2D)` +
  `IMPLEMENT_OBJECT_SERAILIZE` (note macro spelling) +
  `INSTANTIATE_TEMPLATE_TRANSFER_EXPORTED`. Same template
  shape as `MaterialRes` / `PrefabAsset`.
- Serialised state: `m_width` / `m_height` / `m_format`
  (uint32 ordinal; stable across `RHIFormat` enum reorder) /
  `m_pixels` (`std::vector<uint8_t>`, NOT `eastl::vector` --
  `SerializeTraits` is only specialised for `std::vector`,
  see `Runtime/Core/Serialize/SerializeTraits.h`).
- **No RHI state** (no `RHIImage*`, no bindless slot). GPU
  upload + descriptor allocation is the consumer's
  responsibility (currently `BindlessTexturePreview`). This
  keeps `Texture2D` free of any DX12 / Vulkan / Metal include
  leakage and makes it cheap to deserialise in a headless /
  cooker context.

If a future "every Texture2D auto-uploads itself once" policy is
needed (e.g. for material binding), introduce a runtime
`GPUResourceManager` that observes asset-load events and pulls
`m_pixels` into RHI state on demand. That is a route-A
follow-up, not in scope for route B.

### PR10 -- TextureImporter rewrite

`Editor/asset_pipeline/texture_importer/texture_importer.cpp`
switched from the half-implemented `AssetFile::saveAsset(...)`
path (whose body serialisation was entirely commented out;
existing `.zasset` files on disk are 176-byte header stubs)
to the working `AssetManager::WriteObjectToDiskThreadSafe(path,
*texture)` API used by `MaterialRes` / `PrefabAsset` /
`XlsxImporter`.

The flow is now:

```
ObjectManager::Produce(TypeOf<Texture2D>(), 0)
  -> stbi_load(force 4-channel RGBA8)
  -> texture->m_pixels.assign(image_data, image_data + bytes)
  -> texture->m_width / m_height / m_format
  -> AssetManager::WriteObjectToDiskThreadSafe(zasset_path, *texture)
  -> MemoryManager::DestroyObject(produced)
```

`reimport()` is a temporary no-op + `LOG_WARNING` -- the new
`SerializedFile` zasset has no metadata layer to recover the
original source path from. Restoring `reimport()` is a route-A
follow-up that requires either a per-asset metadata sidecar or
augmenting `SerializedFileHeader` with importer-config fields.

`getSupportedExtensions()` was also corrected (`.tag` -> `.tga`)
and gained `.bmp`.

### PR11 -- BindlessTexturePreview retrofit

The inspector preview path is now driven by the imported product,
not the raw source. Concretely:

- `bindless_texture_preview.h`: removed
  `isSupportedTextureExtension(path&)`, replaced with
  `isSupportedAssetType(const std::string& resolved_asset_type)`.
  The new function takes the already-normalised
  (lowercased + `Res` suffix stripped) class-name token from
  `ResolveInspectorAssetType` and matches it against
  `"texture2d"`. Pure string compare; no I/O.
- `bindless_texture_preview.cpp`: deleted `loadPixels` (stbi)
  and the `<stb_image.h>` include. Pixel ingestion is now
  `AssetManager::ReadObject<Texture2D>(path&) ->
  Texture2D::m_pixels`. The rest of the pipeline
  (`stagePixelsToUploadBuffer` -> persistent UPLOAD ->
  deferred CopyTextureRegion -> bindless slot ->
  off-screen RT -> ImGui SRV bridge) is unchanged.
- `inspector_window.cpp` `ResolveInspectorAssetType`: deleted
  the `.png / .jpg / .jpeg / .tga / .bmp -> "texture"` shortcut.
  Texture dispatch now goes through the same `.zasset` ->
  `AssetManager::getAssetTypeName` -> normalise pipeline that
  Material and DataTable already use. The dispatch site below
  was updated from `resolved_asset_type == "texture"` to
  `BindlessTexturePreview::isSupportedAssetType(resolved_asset_type)`,
  isolating the asset-type token from the dispatch site.

The cache key is still `path.lexically_normal().generic_string()`,
which is now the `.zasset` path (one cache slot per imported
texture, same shape as before).

### Backlog -- route A items NOT addressed

These are deliberately deferred; opening them requires more than
a one-PR slice each. Listed in priority order so the next
iteration knows where to start.

1. **GUID persistence in `SerializedFileHeader`. (LANDED -- P2 #6)**
   `SerializedFile::WriteHeaderAndMetadata` now stamps a 176-byte
   `AssetFileHeader` prefix (magic = "ZASS", carrying GUID +
   asset_type + metadata/data byte spans) at file offset 0 of every
   `.zasset`. `SerializedFile::ReadHeader` peeks the magic and, on a
   match, advances `m_ReadOffset` past the prefix so the inner
   format stays unchanged -- legacy zassets written before the
   change keep loading. The GUID itself is derived deterministically
   from the absolute output path (FNV-1a 64x2 over a lower-cased
   POSIX-style path on Windows), mirroring `ScriptRegistry`'s
   path-hash policy. AssetRegistry's `scanSingleAsset` already
   reads the prefix via `ifstream::read`, so freshly-written
   zassets now produce real GUIDs instead of the `"legacy:" + path`
   synthetic fallback. Renames still break references because
   AssetRegistry indexes by absolute output path; widening the
   path-rename layer to use the embedded GUID is the natural
   follow-up but out of scope here.
2. ~~**Project window source hiding.**~~ **Done (AGENTS.md 2.10)**:
   `EditorFileService::shouldDisplayInProjectWindow` whitelists
   `Assets/` to `.zasset` / `.json` only; raw `.png` / `.jpg` are not
   surfaced even when misplaced under `Assets/`.
3. **Auto-import on drop / on file watcher.** Currently the user
   has to invoke the importer manually. UE / Unity both
   auto-import on file create / modify. Wire this into the
   existing file watcher used by `ScriptRegistry` /
   `DataTableImporter::compileProject`.
4. **`AssetFile::saveAsset` half-implementation cleanup.**
   `engine/source/Runtime/Asset/asset_file.{h,cpp}` still
   contains the dead "old" zasset write path with its body
   serialisation entirely commented out (see `asset_file.cpp`
   ~lines 116-141). Now that route B's `WriteObjectToDiskThreadSafe`
   is the de-facto path for every asset type, delete the
   `AssetFile` header struct + the importer call sites that
   used it (none remain after PR10) and drop the file.
5. **`Texture` base class reflection.** Today `Texture2D` skips
   `Super::Transfer` because the base has no reflection and its
   two legacy fields are private. Lift the fields to protected,
   register the base, give it a `Transfer<TF>`, then remove the
   skip. Required before sibling texture asset classes
   (TextureCube, Texture3D, RenderTexture) can share serialised
   state.
6. **`reimport()` plumbing.** **Partial (PR-AI3 + 2026-05)**:
   AutoReimport and `EditorAssetManager::reimportAsset` use
   `SourceAssetRegistry` + 3-arg `AssetImporter::Reimport`.
   `TextureImporter::Reimport` (2-arg) delegates to that API.
   Still open: dedicated Inspector **Reimport** button on
   `Texture2D` zassets (DataTable already has one for `.csv`).

The route-B slice (PR9-PR11) intentionally does NOT block on any
of these; the inspector preview works correctly today through
imported `Texture2D` zassets, and every backlog item above is
about widening the path, not fixing it.

---

## PR-V3 -- Vulkan `BindlessTextureBlitPipeline` parity (landed)

PR-V3 closes item 3 of the PR8c backlog (above) for the Vulkan
backend: the editor's bindless preview path now has a Vulkan
sibling of the DX12-only `BindlessTextureBlitPipeline` that
shipped in PR7. This is the **first cross-backend bindless
production-shape consumer** in ZEngine; PR1-PR8c built the
toolchain and the DX12 editor consumer, PR-V1/PR-V2 wired the
Vulkan descriptor-table plumbing, and PR-V3 is the first place
all three layers meet on Vulkan.

### Scope pivot: tonemap pass -> blit pipeline

The original PR-V3 description in earlier drafts of this
document targeted "the Vulkan tonemap pass as the first real
bindless production path." That target was **rejected** after
reading the existing Vulkan render graph:

- The current Vulkan tonemap pass (`tone_mapping_pass.cpp`)
  reads its input via `subpassInput` / `subpassLoad()` --
  i.e. as a **Vulkan input attachment** bound through the
  render-pass dependency graph, NOT as a `sampler2D`. Input
  attachments are fundamentally a per-fragment fixed-function
  binding -- they cannot be replaced by a bindless
  descriptor-indexed array load without simultaneously
  flattening the pass's tile-memory subpass-merge optimisation
  on tilers (Adreno / Mali / Apple).
- Migrating tonemap would therefore require a **second**
  refactor (subpass -> standalone pass, with the cost of an
  extra resolve / tile-memory eviction on mobile) before the
  bindless migration could even start. Two coupled pivots in
  one PR violates the per-PR-slice rule.
- Meanwhile PR8c backlog item 3 already named the Vulkan
  `BindlessTextureBlitPipeline` parity as the natural Vulkan
  first consumer -- the DX12 sibling at
  `interface/dx12/utility/bindless_texture_blit_pipeline.{h,cpp}`
  is precisely the "single texture, fullscreen blit, push-
  constant index" template the migration was looking for, and
  its semantics translate to Vulkan with zero subpass surgery.

So PR-V3 ships the Vulkan blit pipeline. Tonemap migration is
**re-deferred** behind a prerequisite "tonemap subpass
flattening" PR, tracked in the backlog at the bottom of this
section.

### Files landed

```
engine/source/Runtime/Function/Render/Interface/vulkan/utility/
  bindless_texture_blit_pipeline.h
  bindless_texture_blit_pipeline.cpp
  shaders/
    bindless_blit.vert     # GLSL 4.50 fullscreen-triangle VS
    bindless_blit.frag     # GLSL 4.50 + GL_EXT_nonuniform_qualifier

engine/source/Runtime/Function/Render/Interface/vulkan/test/
  vulkan_bindless_smoke_test.cpp   # extended (PR-V3 section 5)
```

The `shaders/*.{vert,frag}` files are kept on disk **only for
IDE syntax highlighting and future glslangValidator offline
compile**. The `.cpp` inlines the GLSL source as raw-string
literals and feeds them to `ShaderCompiler::compileFromSource`
directly, mirroring how the runtime hot path compiles its
shaders. There is therefore no CMake registration for the
`.vert` / `.frag` files (CMake `GLOB_RECURSE` does NOT match
them; only the new `.cpp` is auto-discovered into `ZRuntime`,
and the existing per-platform `EXCLUDE REGEX
".*/Function/Render/interface/vulkan/.*\\.cpp$"` rules
correctly exclude it on Apple / WebGL2 / Win-DX12-only builds).

### API shape (1:1 with DX12 sibling)

```cpp
class VulkanBindlessTextureBlitPipeline {
public:
    VulkanBindlessTextureBlitPipeline() = default;
    ~VulkanBindlessTextureBlitPipeline();

    bool initialize(RHI* rhi, RHIRenderPass* render_pass);
    void shutdown();

    void recordBlit(RHICommandBuffer* cmd,
                    const BindlessIndex& index,
                    uint32_t viewport_w, uint32_t viewport_h);

    bool isReady() const;
    RHIPipeline*       getPipeline()       const;
    RHIPipelineLayout* getPipelineLayout() const;
};
```

This signature is byte-for-byte compatible with the DX12
sibling at `interface/dx12/utility/bindless_texture_blit_pipeline.h`.
The editor-side `bindless_blit_smoke.cpp` widget (DX12-only
today) can therefore be ported to Vulkan with a single
`#if`-gated `#include` swap once the editor-side widget is
itself made backend-agnostic. That widget port is the natural
"PR-V3 part 2" and is listed in the backlog below.

### Vulkan-specific implementation notes

These are the non-trivial places where the Vulkan port
**diverges semantically** from DX12, captured here so the next
agent does not re-derive them.

1. **Push-constant range is supplied explicitly.** Unlike DX12's
   `createPipelineLayout` -- which auto-injects a 4-byte root
   constant when `RHIRootSignatureFlags::CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED`
   is set -- `VulkanRHI::createPipelineLayout` is a **passthrough**
   with no bindless detection. The blit pipeline must therefore
   hand-supply the push-constant range, which it pulls from
   `VulkanBindlessTextureManager::getBindlessPushConstantRange()`
   (size 4, offset 0, stages = `VK_SHADER_STAGE_ALL`). This
   keeps the descriptor-set-layout-compatibility rule
   trivially satisfied (we reuse the manager's exact
   `VkDescriptorSetLayout` non-owningly).

2. **Sampler half of `BindlessIndex` is deliberately ignored on
   Vulkan.** The `BindlessIndex::pack(tex, sampler)` ABI exposes
   a 16-bit sampler index in the high half of the 32-bit packed
   key; on DX12 this dispatches into one of four static
   samplers (`SAMPLER_HEAP_DIRECTLY_INDEXED` is permanently
   deferred per PR8c backlog item 6, so static samplers are the
   path). On Vulkan, the bindless table is declared as
   `uniform sampler2D u_bindless[]` -- `COMBINED_IMAGE_SAMPLER`
   -- meaning the sampler is **fixed at descriptor allocation
   time** by `VulkanBindlessTextureManager::allocate(...)` and
   cannot be re-dispatched per-draw. The blit fragment shader
   masks the high half off and uses `texture_index` only.
   Both halves remain in the ABI for symmetry with DX12; the
   sampler half is simply a no-op on Vulkan.

   If a future use case legitimately needs runtime-varying
   sampler dispatch on Vulkan (e.g. a material that toggles
   anisotropic vs. point sampling on the same texture), the
   answer is **separate-image-and-sampler** descriptor sets
   (`VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE` + a separate sampler
   array), which is a manager-level surgery, not a per-pipeline
   one. This is intentionally out of scope for PR-V3.

3. **Fullscreen-triangle convention matches DX12 1:1.** Both
   shaders drive the three NDC corners off
   `gl_VertexIndex` / `SV_VertexID` 0..2 with the same
   `(uv.x, 1-uv.y)` flip, so the Y-axis-flipped Vulkan clip
   space and the standard D3D clip space produce the same
   on-screen orientation. `cmdDraw(3, 1, 0, 0)`, no vertex
   buffer.

4. **Descriptor set is bound from the manager, not owned.**
   `recordBlit` stack-wraps the manager's raw `VkDescriptorSet`
   in a `VulkanDescriptorSet` and passes it to
   `cmdBindDescriptorSetsPFN(cmd, GRAPHICS, layout, set=0,
   {ds}, {})`. The pipeline never holds a pool / set of its
   own; the manager remains the single owner. This matches the
   "one global bindless table per device" invariant that PR1
   established and PR-V1 preserved.

5. **`createShaderModuleFromSource` stage parameter footgun.**
   The second argument is declared `int` but is read internally
   as `enum class ShaderStage` (Vertex=0, Fragment=1). Passing
   `RHI_SHADER_STAGE_VERTEX_BIT` (=1) silently selects fragment.
   The blit pipeline therefore uses
   `static_cast<int>(ShaderStage::Vertex)` /
   `ShaderStage::Fragment`, matching the convention already
   established in `main_camera_pass.cpp`. Fixing this signature
   to take the enum directly is a separate cross-cutting
   cleanup -- worth doing, not in this PR's slice.

### Smoke-test scope: link-and-shape, not GPU end-to-end

`vulkan_bindless_smoke_test.cpp` section 5 ("PR-V3:
VulkanBindlessTextureBlitPipeline link-and-shape sniff") covers:

- **Compile-time**: `static_assert(!std::is_copy_constructible_v<...>)`,
  `static_assert(!std::is_copy_assignable_v<...>)`, plus a
  `_prv3_check_blit_pipeline_api_shape()` function whose body
  takes member-function pointers of the exact PR-V3 signatures.
  Any silent ABI break breaks this TU at compile time.
- **Link-time**: `main()` instantiates the class, asserts
  default-constructed `isReady() == false`,
  `getPipeline() == nullptr`, `getPipelineLayout() == nullptr`,
  and that `shutdown()` is a safe no-op on a never-initialised
  instance.

It explicitly does **NOT** call `initialize()`. The reason:
`VulkanRHI::Initialize()` hard-depends on `WindowSystem` +
swapchain setup, and the smoke-test target runs on CI hosts
that lack a Vulkan ICD entirely (the test is gated by
`ZENGINE_BUILD_VK_BINDLESS_SMOKE_TEST=ON` and only links / runs
on Vulkan-capable build agents -- but even those agents are
headless and have no display).

End-to-end runtime verification (real `VkDevice`, real
descriptor-set bind, real `vkCmdDraw`) belongs in the
**editor-side** `bindless_blit_smoke.cpp` widget, which today
is DX12-only. The Vulkan port of that widget is the natural
follow-up; it has a real device, a real swapchain, and a real
render-pass to plug PR-V3 into.

### Cross-backend parity table (updated)

| Capability                                  | DX12 | Vulkan | Metal | WebGL2 |
|---------------------------------------------|------|--------|-------|--------|
| `RHIBindlessTextureManager`                 | ✅    | ✅      | ❌     | ❌      |
| `cmdSetBindlessIndexPFN` non-empty override | ✅    | ✅ (PR-V1) | ❌  | ❌      |
| `BindlessTextureBlitPipeline` utility       | ✅ (PR7) | ✅ (PR-V3) | ❌ | ❌      |
| Editor preview widget end-to-end            | ✅    | ✅ (PR-V3 part 2) | ❌ | ❌ |
| Production draw consumer (non-preview)      | ❌    | ❌      | ❌     | ❌      |

### Backlog -- items NOT addressed by PR-V3

Listed in the order the next iteration should pick them up.

1. **Vulkan tonemap migration to bindless**. Requires a
   prerequisite refactor: lift tonemap out of its current
   subpass / `subpassInput` shape into a standalone render
   pass that consumes its input as a `sampler2D`. After that
   prerequisite lands, switching the input bind to bindless is
   a one-line change (pipeline layout flag + the manager's
   descriptor set + `cmdSetBindlessIndexPFN`). Cost on tilers
   (Adreno / Mali / Apple): one extra tile-memory eviction per
   frame; measure before committing.
   **Status update (post-PR-V4 part 1)**: the bindless
   pipeline class itself (`VulkanBindlessTonemapPipeline`) +
   the bindless fragment shader (`tone_mapping_bindless.frag`)
   have landed as a pure-additive utility in PR-V4 part 1
   (see section below). The main_camera render-pass split is
   still pending, tracked there as "PR-V4 part 2".
2. **Metal `BindlessTextureBlitPipeline` parity; WebGL2
   permanently excluded.** Metal needs an
   `argument_buffer` + `texture2d<...>` array path; deferred
   until a real consumer arrives. WebGL2 **cannot** support
   bindless at all (no `GL_ARB_bindless_texture`, no
   `nonuniformEXT`, `sampler2D` arrays capped at 16-32 units)
   and will permanently fall back to the bindful preview.
   `WebGL2RHI::supportsBindlessTextures()` returns `false`
   explicitly; no `WebGL2BindlessTextureManager` will ever
   exist.
3. ~~**`createShaderModuleFromSource` stage-parameter cleanup.**~~
   **Landed as PR-SS1** (see section below). `ShaderStage` enum class
   moved from `vulkan/shader_compiler.h` to `render_type.h` (shared
   across all backends); `int` parameters replaced with `ShaderStage`
   across RHI base class, all 4 backend overrides, DX12/Vulkan/WebGL2
   shader compilers, and all call sites.
4. **Sampler-bindless on Vulkan** (separate-image-and-sampler
   descriptor sets). Permanently deferred unless a real
   workload demands runtime-varying sampler dispatch. See
   semantic divergence note 2 above for the design sketch.

---

## PR-V3 part 2 -- Editor `bindless_blit_smoke` Vulkan port (landed)

PR-V3 part 1 shipped the Vulkan `BindlessTextureBlitPipeline`
class itself plus a compile-time / link-time / API-shape smoke
test. It deliberately stopped short of running the pipeline
against a live `VkDevice`, because the link-and-shape gate runs
in headless CI (no ICD).

Part 2 closes that gap by porting the editor-side
`bindless_blit_smoke.cpp` widget -- previously DX12-only --
into a backend-dispatching shape that also lights up on Vulkan.
Both backends now drive **the same** `drawWidget()` entry point,
which the inspector window already gates behind a dev-only
checkbox.

### File touched

```
engine/source/Editor/editor_window/inspector_window/
  bindless_blit_smoke.h    # comment refresh, public API unchanged
  bindless_blit_smoke.cpp  # added Vulkan implementation block +
                           # multi-backend dispatch in drawWidget()
```

The header's surface (`namespace BindlessBlitSmoke { void drawWidget(); }`)
is byte-for-byte unchanged. All call sites
(`InspectorWindow::onGUI` checkbox + invocation) keep working
without modification.

### Single-file vs split-file decision

Considered: split into
`bindless_blit_smoke_dx12.cpp` + `bindless_blit_smoke_vulkan.cpp`,
the way `editor_ui_pass.cpp` and `editor_ui_pass_macos.mm` are
split. Rejected because:

- The DX12 block was already wrapped in `#if defined(_WIN32)` and
  the Vulkan block is naturally `#if defined(Z_HAS_VULKAN)`. The
  two `#if` regions are textually independent inside one TU; a
  split would only buy file-level separation without reducing
  textual entanglement (`drawWidget()` still has to know about
  both backends to dispatch).
- The DX12 block has accumulated non-trivial DX12-frame-aware
  invariants in inline comments (the deferred-upload-via-editor-
  cmd-list dance, the SetDescriptorHeaps bookend nuance with
  `editor_ui_pass.cpp`). Splitting risks those comments
  drifting; keeping both halves in one file makes the
  cross-backend asymmetry inspectable at a glance.

The Vulkan block uses a nested **named** sub-namespace
`namespace { namespace vk_impl { ... } }` so its own
`Resources` / `tryGetVulkanRHI` / `ensureInitialized` /
`recordFrame` symbols cannot collide with the DX12 anonymous-
namespace siblings of the same names. `drawWidget()` then
dispatches:

```cpp
#if defined(_WIN32)
    if (DX12RHI* dx12 = tryGetDX12RHI())   { /* DX12 path */ return; }
#endif
#if defined(Z_HAS_VULKAN)
    if (VulkanRHI* vk = vk_impl::tryGetVulkanRHI()) { /* Vulkan path */ return; }
#endif
    ImGui::TextDisabled("BindlessBlitSmoke: unavailable on this backend ...");
```

`Z_HAS_VULKAN` is the same configure-time macro the editor's
ImGui-Vulkan backend in `editor_ui_pass.cpp:106` uses, lit up
by `ZENGINE_USE_VULKAN=ON`.

### Vulkan-side simplifications vs the DX12 sibling

The Vulkan block ended up materially simpler in three places:

1. **No `SetDescriptorHeaps` analogue.** Vulkan pipelines bind
   their own `VkDescriptorSet`s through `vkCmdBindDescriptorSets`,
   and `VulkanBindlessTextureBlitPipeline::recordBlit` already
   does that internally for set 0 (the manager's bindless set).
   The DX12 block's "manually swap to the bindless heap, trust
   `editor_ui_pass.cpp:196-200` to swap back" dance has no
   equivalent. ImGui's own descriptor pool is independent and
   coexists trivially.

2. **Texture upload is synchronous and safe inside `onGUI`.**
   `VulkanRHI::createGlobalImage` allocates a staging buffer and
   issues a self-contained
   `beginSingleTimeCommands` + `vkQueueSubmit` +
   `vkQueueWaitIdle` sequence on a throwaway primary command
   buffer (see `vulkan_rhi.cpp:675-712`). It does **not** touch
   `VulkanRHI::m_current_command_buffer`, so it is safe to call
   from inside `InspectorWindow::onGUI` mid-frame. The DX12
   block had to:
     - allocate a persistent UPLOAD buffer in `ensureInitialized()`,
     - memcpy the pixels into it,
     - defer the actual `CopyTextureRegion` + barriers to
       `recordFrame()`'s first invocation,
   because `DX12RHI::uploadTextureData` resets the per-frame
   command pool and would discard upstream-recorded passes.
   The Vulkan block collapses all of that into a single
   `vk_rhi->createGlobalImage(...)` call inside
   `ensureInitialized()`. The
   `queueWaitIdle` cost (one-time, on first widget show) is
   acceptable for a dev-only smoke widget.

3. **ImGui texture bridge is one helper call.**
   `ImGui_ImplVulkan_AddTexture(view, layout)` allocates a
   `VkDescriptorSet` from the ImGui Vulkan backend's own
   descriptor pool, writes the
   `(view, sampler-from-imgui-init, layout)` triple into it,
   and returns the set. The opaque handle goes straight into
   `ImGui::Image` as an `ImTextureID` (cast through
   `uintptr_t` to satisfy the strictly-64-bit-opaque ABI rule
   on 32-bit platforms; on 64-bit `VkDescriptorSet` IS a
   pointer, so the cast is a no-op). Compare to the DX12
   block's manual `allocateImGuiSrvDescriptor` +
   `CreateShaderResourceView` plumbing.

### Render-pass attachment-layout trick

The Vulkan RT's render-pass attachment uses
`initialLayout = VK_IMAGE_LAYOUT_UNDEFINED` and
`finalLayout   = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL`,
combined with `LOAD_OP_CLEAR`. This pairing is intentional:

- On the very first frame the image really is in `UNDEFINED`
  (`createImage` does not transition).
- On every subsequent frame the previous frame left it in
  `SHADER_READ_ONLY_OPTIMAL`. The spec lets `LOAD_OP_CLEAR`
  pair with **any** initial layout (the previous contents are
  thrown away anyway), so the attachment description does not
  need a per-frame switch between two layouts. One
  `RHIRenderPass` allocated once at init time covers both the
  first frame and the steady state.

The matching cleanup-side wisdom: ImGui consumes the SRV later
in the same primary command buffer, after our
`cmdEndRenderPassPFN`. The render-pass-end's implicit
subpass-end barrier already targets ALL_GRAPHICS / SHADER_READ
for any consumer in the same submit, so no explicit
`vkCmdPipelineBarrier` is needed between our blit and ImGui's
draw.

### Backlog -- items NOT addressed by PR-V3 part 2

Same list as PR-V3 part 1's backlog, minus the now-landed
"Editor `bindless_blit_smoke.cpp` Vulkan port" entry.

---

## PR-V4 part 1 -- Vulkan bindless tonemap pipeline (landed)

PR-V4 part 1 lands the **first half** of PR-V3 backlog item 1
("Vulkan tonemap migration to bindless"): the bindless pipeline
class + bindless fragment shader, as a pure-additive utility.
**No consumer is wired** -- the `main_camera_pass` still runs
the legacy `subpassInput`-based tonemap subpass. Wiring the
new pipeline as the production tonemap consumer is **PR-V4
part 2**, deliberately deferred (see "Why split into two
parts" below).

### Files landed

```
engine/source/Runtime/Function/Render/Interface/vulkan/utility/
  bindless_tonemap_pipeline.h
  bindless_tonemap_pipeline.cpp
  shaders/
    tone_mapping_bindless.frag   # GLSL 4.50 + GL_EXT_nonuniform_qualifier
                                 # Uncharted2 + Gamma 2.2 -- byte-identical
                                 # math to the legacy `tone_mapping.frag`
```

The vertex shader is intentionally **inlined into the .cpp** as
a raw-string literal -- same source as `bindless_blit.vert` --
so the tonemap TU is self-contained. If the blit sibling's
vertex shader ever drifts (e.g. a future Y-flip change for
headless capture), the tonemap version stays pinned to its
tested form. The `shaders/*.frag` file is kept on disk for IDE
syntax highlighting + future glslangValidator offline compile;
the .cpp keeps an inlined copy of the same source. No CMake
registration is needed (existing `GLOB_RECURSE` rules in
`Runtime/CMakeLists.txt` auto-discover the new `.cpp`, and the
per-platform `EXCLUDE REGEX
".*/Function/Render/interface/vulkan/.*\\.cpp$"` rules already
exclude it on Apple / WebGL2 / Win-DX12-only builds).

### API shape (1:1 with VulkanBindlessTextureBlitPipeline)

```cpp
class VulkanBindlessTonemapPipeline {
public:
    VulkanBindlessTonemapPipeline()  = default;
    ~VulkanBindlessTonemapPipeline() = default;

    bool initialize(RHI* rhi, RHIRenderPass* render_pass);
    void shutdown();

    void recordTonemap(RHICommandBuffer* cmd,
                       uint32_t          viewport_w,
                       uint32_t          viewport_h,
                       uint32_t          bindless_texture_index,
                       uint32_t          sampler_index = 0) const;

    bool isReady() const;
    RHIPipeline*       getPipeline()       const;
    RHIPipelineLayout* getPipelineLayout() const;
};
```

The signature only differs from `VulkanBindlessTextureBlitPipeline::recordBlit`
in the verb (`recordBlit` -> `recordTonemap`); the parameter list,
backend gate (`dynamic_cast<VulkanRHI*>` + `supportsBindlessTextures()`),
descriptor-set-layout reuse trick, push-constant range, fullscreen-
triangle topology, and shutdown semantics are byte-identical. A
caller that already knows how to drive the blit pipeline can drive
the tonemap pipeline by swapping the type and the verb.

### Bindless wiring (matches PR-V3 conventions)

- **Set 0, binding 0** = `sampler2D u_bindless[]` -- the global
  bindless descriptor table owned by `VulkanBindlessTextureManager`.
  Reused as the **same** `VkDescriptorSetLayout`-by-identity to
  satisfy Vulkan's set-layout-compat rule at bind time.
- **Push constant**: 4 bytes at offset 0, stages =
  `VK_SHADER_STAGE_ALL`. Pulled from
  `VulkanBindlessTextureManager::getBindlessPushConstantRange()`
  -- same SSOT as the blit sibling.
- **Shader-side unpack**: low 16 bits = bindless slot index;
  high 16 bits ignored (sampler is fixed at allocate() time on
  Vulkan -- see PR-V3 semantic divergence note 2).

### Tone-mapping math: pixel-equivalent to legacy

`tone_mapping_bindless.frag` ships the exact same math as the
legacy `engine/shader/glsl/tone_mapping.frag`:

- Same Uncharted2 curve coefficients (A=0.15, B=0.50, C=0.10,
  D=0.20, E=0.02, F=0.30).
- Same exposure pre-multiply (`color * 4.5f`).
- Same white-point normalisation against `Uncharted2Tonemap(11.2)`.
- Same in-shader Gamma 2.2 correction (i.e. NOT relying on a
  `_SRGB` swapchain format).

The intent is that PR-V4 part 2 (which swaps the production
shader from legacy to bindless) introduces **zero pixel delta**
beyond the unavoidable consequence of the topology change
(`subpassInput` vs `sampler2D` differ only in the bind path,
not in the math). Any visual delta surfaced during PR-V4 part 2
real-device regression is therefore attributable to the
topology change alone, not to a tone-curve drift.

### Why split into two parts

The end goal is "Vulkan tonemap is the first runtime production
draw to consume the bindless path". Reaching that goal requires
two large structural changes that are independent in code but
need to land in series:

- **Part 1 (this PR)**: the bindless pipeline class + shader,
  as a pure-additive utility. Zero risk of regressing the
  default render path -- nothing is wired yet.
- **Part 2 (next PR)**: split `main_camera_pass.cpp`'s
  8-subpass render pass into two render passes around a
  standalone `BindlessTonemapPass`. Touches the
  `_main_camera_subpass_*` enum in `render_pass.h`, the subpass
  index references in `color_grading_pass.cpp`,
  `fxaa_pass.cpp`, `combine_ui_pass.cpp`, and the ui_pass
  definition in `main_camera_pass.cpp` itself; rewires the
  attachment chain so that `backup_even` between the two
  render passes is `initialLayout = SHADER_READ_ONLY_OPTIMAL`
  + `loadOp = LOAD`; per-swapchain-image gets a second
  framebuffer; the draw scheduler issues two
  begin/endRenderPass cycles with the standalone tonemap
  draw between them. This is a real-device-regression-grade
  change.

Doing both halves in one PR would conflate "is the bindless
shape correct?" (compile-time / link-time / API-shape gate) with
"does the new render-pass topology work on Adreno / Mali /
Apple?" (real-device gate). Splitting them keeps each PR
reviewable and gives the topology change a clean rollback
target if mobile-tier validation surfaces a layout-transition
or tile-memory bug.

### Smoke-test scope

PR-V4 part 1 inherits PR-V3 part 1's link-and-shape gate
philosophy: a future `vulkan_bindless_smoke_test.cpp` extension
can `static_assert` non-copyability + take member-function
pointers of the exact PR-V4 signatures, plus
default-constructed `isReady() == false` /
`getPipeline() == nullptr` / `getPipelineLayout() == nullptr` /
shutdown-no-op-on-uninitialised checks. It deliberately does
NOT call `initialize()` -- same headless-CI rationale as
PR-V3 part 1 (no Vulkan ICD on the build agents).

End-to-end runtime verification belongs to PR-V4 part 2, which
will exercise the pipeline against the real `VkDevice` /
swapchain owned by the editor's main render loop.

### Cross-backend parity table (updated)

| Capability                                  | DX12 | Vulkan | Metal | WebGL2 |
|---------------------------------------------|------|--------|-------|--------|
| `RHIBindlessTextureManager`                 | ✅    | ✅      | ❌     | ❌      |
| `cmdSetBindlessIndexPFN` non-empty override | ✅    | ✅ (PR-V1) | ❌  | ❌      |
| `BindlessTextureBlitPipeline` utility       | ✅ (PR7) | ✅ (PR-V3) | ❌ | ❌      |
| `BindlessTonemapPipeline` utility           | ✅ (PR-DX2) | ✅ (PR-V4 part 1) | ❌ | ❌  |
| Editor preview widget end-to-end            | ✅    | ✅ (PR-V3 part 2) | ❌ | ❌ |
| Production draw consumer (non-preview)      | ✅ (PR-DX1) | ✅ (PR-V4 part 2) | ❌ | ❌  |

### Backlog -- items NOT addressed by PR-V4 part 1

> **Status note (post-PR-V4 part 2)**: backlog item 1 below
> ("`MainCameraPass` render-pass split") has since landed -- see the
> PR-V4 part 2 section immediately below for the actual landing
> notes. The remaining items (DX12 / Metal / WebGL2 tonemap parity,
> editor-side `BindlessTonemapSmoke` widget) are still open and the
> canonical current-state list lives at the end of the PR-V4 part 2
> section.

Listed in priority order for PR-V4 part 2 onwards.

1. **`MainCameraPass` render-pass split**. The single
   8-subpass render pass at
   `engine/source/Runtime/Function/Render/passes/main_camera_pass.cpp`
   needs to be split into two render passes:
   - **RP_main_scene** (3 subpasses):
     `basepass` -> `deferred_lighting` -> `forward_lighting`,
     terminating with `backup_odd` at
     `finalLayout = SHADER_READ_ONLY_OPTIMAL`.
   - **standalone tonemap pass** (PR-V4 part 1 pipeline +
     standalone `RHIRenderPass` + `RHIFramebuffer`):
     reads `backup_odd` via the bindless table, writes
     `backup_even`.
   - **RP_post_ui** (4 subpasses):
     `color_grading` -> `fxaa` -> `ui` -> `combine_ui`,
     starting with `backup_even` at
     `initialLayout = SHADER_READ_ONLY_OPTIMAL`,
     `loadOp = LOAD`.

   Code touch points:
   - `engine/source/Runtime/Function/Render/render_pass.h`:
     drop `_main_camera_subpass_tone_mapping`, renumber the
     remaining subpass enumerators, decrement
     `_main_camera_subpass_count` from 8 to 7 across two
     enums.
   - `main_camera_pass.cpp`: split `setupRenderPass()`
     into `setupRenderPasses()` (note the plural);
     split `setupSwapchainFramebuffers()` to allocate two
     framebuffers per swapchain image; rewrite the
     attachment-loadOp / initialLayout for `backup_even`
     in the post-UI render pass.
   - `color_grading_pass.cpp` / `fxaa_pass.cpp` /
     `combine_ui_pass.cpp` / `main_camera_pass.cpp`'s ui
     subpass definition: `pipelineInfo.subpass` indices
     all decrement by 1 (or by their position relative to
     the new RP_post_ui).
   - `tone_mapping_pass.{h,cpp}`: deleted in favour of
     a new `BindlessTonemapPass : RenderPass` that owns
     the standalone render pass, framebuffer, and a
     `VulkanBindlessTonemapPipeline` instance, plus the
     bindless-slot allocation for `backup_odd`'s view
     (allocated in `updateAfterFramebufferRecreate`,
     freed in the destructor).
   - `render_pipeline.cpp`: insert the standalone tonemap
     pass between the two main_camera draw calls.

   The work is mechanical but **must** be regression-tested
   on Adreno / Mali / Apple before merge: the layout-
   transition for `backup_even` (UNDEFINED -> COLOR_OUTPUT
   on tonemap pass, COLOR_OUTPUT -> SHADER_READ_ONLY_OPTIMAL
   on tonemap pass end, SHADER_READ_ONLY_OPTIMAL -> ditto
   on RP_post_ui begin via `loadOp=LOAD`) has multiple ways
   to silently misbehave on tilers if the dependency masks
   are wrong. The validation layer catches most cases on
   desktop, but tile-memory eviction asymmetry between
   "subpass-internal IA dependency" and "external layout
   transition" is mobile-only.

2. **DX12 `BindlessTonemapPipeline` parity**. Same template
   as `dx12/utility/bindless_texture_blit_pipeline.{h,cpp}`,
   only the HLSL pixel shader differs (Uncharted2 + Gamma
   2.2 instead of passthrough). Currently DX12 has no
   tonemap-pass equivalent because the editor uses ImGui's
   gamma-aware path; if a future "DX12 production tonemap"
   feature lands, this is the natural follow-up.

3. **Metal `BindlessTonemapPipeline` parity; WebGL2 permanently
   excluded.** Permanently deferred until a real consumer arrives.
   Metal would need an `argument_buffer` +
   `texture2d<...>` array path; WebGL2 **cannot** support bindless
   (no `GL_ARB_bindless_texture`, no `nonuniformEXT`, `sampler2D`
   arrays capped at 16-32). `WebGL2RHI::supportsBindlessTextures()`
   returns `false` explicitly; no `WebGL2BindlessTextureManager`
   will ever exist.

4. **Editor-side `BindlessTonemapSmoke` widget**. Optional;
   PR-V4 part 1 deliberately does NOT ship one, because
   the per-pixel difference vs the existing blit smoke
   widget is purely the tone-curve, which can be eyeballed
   from any HDR test image without a dedicated widget. If
   PR-V4 part 2 reveals that real-device regression needs
   a finer-grained validation surface than the production
   tonemap pass's own output, the widget can be added in a
   follow-up by copy-pasting `bindless_blit_smoke.cpp`'s
   Vulkan block and swapping the pipeline type + the verb.

---

## PR-V4 part 2 -- MainCameraPass split + BindlessTonemapPass (landed)

PR-V4 part 2 lands the **second half** of PR-V3 backlog item 1
("Vulkan tonemap migration to bindless"): the legacy
`subpassInput`-based tonemap subpass that lived inside
`MainCameraPass`'s 8-subpass render pass is gone. In its place
sits a **standalone `BindlessTonemapPass`** between two new
half-render-passes (RP1 / RP2) of the main camera, and the
PR-V4-part-1 `VulkanBindlessTonemapPipeline` is now wired as the
**first runtime production draw consumer** of the bindless
descriptor table on Vulkan.

This is the PR that flipped the parity table's "Production draw
consumer (non-preview)" Vulkan column from `⏳` to `✅`. Up to
PR-V4 part 1 every bindless caller in the engine was either an
editor-only smoke widget (`bindless_blit_smoke.cpp`), an editor
inspector preview (`bindless_texture_preview.cpp`), or
compile-time canaries; PR-V4 part 2 puts a bindless draw on the
real per-frame command buffer of every Vulkan-running ZEngine
build.

### Files landed

```
engine/source/Runtime/Function/Render/passes/
  bindless_tonemap_pass.h         # NEW: standalone RenderPass wrapper
  bindless_tonemap_pass.cpp       # NEW: setupRenderPass + setupFramebuffer
                                  # + bindless slot allocate/update + draw()

engine/source/Runtime/Function/Render/passes/
  tone_mapping_pass.{h,cpp}       # DELETED: legacy subpassInput-based pass

engine/source/Runtime/Function/Render/
  render_pass.h                   # split _main_camera_subpass_* enum into
                                  # RP1 (3 subpasses) + RP2 (4 subpasses);
                                  # _main_camera_subpass_tone_mapping is
                                  # gone, _main_camera_subpass_count
                                  # decremented from 8 to (3 + 4)
  render_pipeline.cpp             # owns BindlessTonemapPass, sequences
                                  # RP1.draw -> tonemap.draw -> RP2.draw

engine/source/Runtime/Function/Render/passes/
  main_camera_pass.cpp            # setupRenderPass() now calls
                                  # setupRenderPass1() + setupRenderPass2();
                                  # setupSwapchainFramebuffers() allocates
                                  # TWO framebuffers per swapchain image;
                                  # subpass index references in
                                  # color_grading_pass.cpp / fxaa_pass.cpp /
                                  # combine_ui_pass.cpp / ui subpass
                                  # definition all renumbered into RP2's
                                  # 0..3 range
```

### Render pass topology after the split

```
RP1 = main scene  (3 subpasses, fb1)
  attachments:  [gbuffer_a, gbuffer_b, gbuffer_c, backup_odd, depth]
  subpass 0:    basepass            (writes gbuffers + depth)
  subpass 1:    deferred_lighting   (reads gbuffers, writes backup_odd)
  subpass 2:    forward_lighting    (skybox + particle, writes backup_odd)
  finalLayout:  backup_odd -> SHADER_READ_ONLY_OPTIMAL  (storeOp = STORE)

(between RP1 and RP2, on the same primary command buffer:)

BindlessTonemapPass.draw()  -- standalone
  RP:           1 attachment [backup_even]
                initialLayout = UNDEFINED, finalLayout =
                SHADER_READ_ONLY_OPTIMAL, loadOp = CLEAR (UNDEFINED-OK
                because LOAD_OP_CLEAR throws away contents anyway).
  binds:        bindless descriptor set (set 0, binding 0)
                [backup_odd's view occupies a stable bindless slot
                 allocated on first updateAfterFramebufferRecreate;
                 every recreate calls .update(slot, view, sampler)
                 instead of free()/allocate(), so downstream
                 consumers that cached the slot index never need to
                 invalidate].
  push const:   BindlessIndex::pack(slot, 0)  -- sampler half ignored
                on Vulkan per PR-V3 semantic divergence note 2.
  draw:         cmdDraw(3, 1, 0, 0)  -- fullscreen triangle, no VB.

RP2 = post-FX + UI  (4 subpasses, fb2)
  attachments:  [backup_odd, backup_even, post_process_odd, swapchain[i]]
  initialLayout backup_odd  = SHADER_READ_ONLY_OPTIMAL  (loadOp = LOAD)
  initialLayout backup_even = SHADER_READ_ONLY_OPTIMAL  (loadOp = LOAD)
  subpass 0:    color_grading       (subpassInput: backup_even,
                                     writes post_process_odd)
  subpass 1:    fxaa                (writes backup_odd)
  subpass 2:    ui                  (writes backup_even, preserves backup_odd)
  subpass 3:    combine_ui          (combines backup_odd + backup_even
                                     into swapchain[i])
```

The bindless tonemap output (`backup_even`) lands in
SHADER_READ_ONLY_OPTIMAL, which is exactly the layout RP2 needs
to use it as `color_grading`'s `subpassInput`. The downstream
post-FX chain therefore consumes the bindless tonemap result via
its existing tile-memory-friendly subpass-input path; only the
tonemap *step itself* is bindless. This preserves the tile-merge
optimisation on Adreno / Mali / Apple for the four post-FX
subpasses and pays the tile-memory eviction cost only at the
RP1->BindlessTonemap and BindlessTonemap->RP2 boundaries (one
eviction in / out, the same as if `backup_odd` and `backup_even`
were a normal external read/write).

### Why the legacy ToneMappingPass had to be deleted, not retrofitted

The legacy `ToneMappingPass` (now removed from disk) bound its
input via `subpassInput` / `subpassLoad()`. Three things prevented
in-place bindless retrofit:

1. **Input-attachment vs sampled-image are different binding
   models.** `subpassInput` is a per-fragment fixed-function bind
   declared in the render pass's input-attachment reference list;
   `sampler2D u_bindless[]` is a descriptor-set 0 binding 0 array
   resolved at draw time. Bindless can't be a `subpassInput` --
   the descriptor type alone differs (`INPUT_ATTACHMENT` vs
   `COMBINED_IMAGE_SAMPLER`).

2. **Layout transition timing differs.** The legacy subpass
   relied on the parent render pass mediating
   `backup_odd: COLOR_ATTACHMENT -> SHADER_READ_ONLY_OPTIMAL` for
   free between forward_lighting and tonemap. The bindless path
   needs `backup_odd` already in `SHADER_READ_ONLY_OPTIMAL` at
   tonemap's draw time -- which only an *external* layout
   transition (i.e. the end of a separate render pass) can
   produce.

3. **Descriptor-set conflict.** The bindless table claims set 0,
   binding 0 globally. Inside the legacy 8-subpass render pass,
   set 0 was already allocated for the gbuffer / depth /
   backup-attachment binding scheme that the lighting subpasses
   share with tonemap. Carving out set 0 for bindless inside the
   same render pass would force every other subpass to renumber
   its descriptor sets.

The standalone-pass split addresses all three at once: bindless
tonemap gets its own render pass (own attachment chain, own
descriptor-set partitioning), and the layout transition for
`backup_odd` is the natural finalLayout of RP1.

### Subpass index renumbering

`render_pass.h` previously had a single 8-entry
`_main_camera_subpass_*` enum (basepass / deferred_lighting /
forward_lighting / **tone_mapping** / color_grading / fxaa / ui /
combine_ui). PR-V4 part 2 split this into two enums in the same
header to keep "subpass index in the parent RP" type-safe at the
call sites:

```cpp
enum {
    _main_camera_subpass_basepass = 0,
    _main_camera_subpass_deferred_lighting,
    _main_camera_subpass_forward_lighting,
    _main_camera_subpass_count_rp1
};

enum {
    _main_camera_subpass_color_grading = 0,
    _main_camera_subpass_fxaa          = 1,
    _main_camera_subpass_ui            = 2,
    _main_camera_subpass_combine_ui    = 3,
    _main_camera_subpass_count_rp2
};
```

`color_grading_pass.cpp`, `fxaa_pass.cpp`,
`combine_ui_pass.cpp`, and `main_camera_pass.cpp`'s ui subpass
definition all consume the RP2 enum; their `pipelineInfo.subpass`
indices therefore now read `0..3` against RP2 instead of `4..7`
against the old single render pass. The old
`_main_camera_subpass_tone_mapping` value is gone -- the standalone
`BindlessTonemapPass` does not occupy a subpass slot in either
enum.

### Bindless slot lifecycle for `backup_odd`

`BindlessTonemapPass::updateAfterFramebufferRecreate(view, ...)`
keeps the bindless slot stable across framebuffer recreates:

- First call: `allocate(view, sampler) -> slot N`, stored on
  `m_bindless_slot`.
- Subsequent calls: `update(slot, new_view, sampler)` -- in-place
  re-write of slot N. The slot index does NOT change.

This matches the contract the manager already advertises for
streaming systems that swap mip pyramids
(`vulkan_bindless_texture_manager.cpp`'s
`update()` is mutex-protected and `UPDATE_AFTER_BIND`-safe). The
slot is freed only in the pass destructor; mid-session swapchain
resizes leak nothing.

`render_pipeline.cpp` calls
`BindlessTonemapPass::updateAfterFramebufferRecreate(...)` from
two places:
- `initializeUIRenderBackend` (first init), and
- the swapchain-recreate path that follows
  `MainCameraPass::updateAfterFramebufferRecreate`. The order is
  important: main_camera must rebuild `backup_odd` /
  `backup_even` *first*, then tonemap rebinds against the new
  views.

### Cross-backend parity table (updated)

| Capability                                  | DX12 | Vulkan | Metal | WebGL2 |
|---------------------------------------------|------|--------|-------|--------|
| `RHIBindlessTextureManager`                 | ✅    | ✅      | ❌     | ❌      |
| `cmdSetBindlessIndexPFN` non-empty override | ✅    | ✅ (PR-V1) | ❌  | ❌      |
| `BindlessTextureBlitPipeline` utility       | ✅ (PR7) | ✅ (PR-V3) | ❌ | ❌      |
| `BindlessTonemapPipeline` utility           | ✅ (PR-DX2) | ✅ (PR-V4 part 1) | ❌ | ❌  |
| Editor preview widget end-to-end            | ✅    | ✅ (PR-V3 part 2) | ❌ | ❌ |
| Production draw consumer (non-preview)      | ✅ (PR-DX1) | ✅ (PR-V4 part 2) | ❌ | ❌  |

### Backlog -- items NOT addressed by PR-V4 part 2

Listed in priority order. Items 2-5 below are restated from the
PR-V4 part 1 backlog with item 1 ("MainCameraPass render-pass
split") removed because it is now landed.

1. ~~**DX12 production draw consumer.**~~ **Landed as PR-DX1** (see
   section below). The skybox cubemap draw and scene_grid infinite
   grid draw in `DX12MainCameraPass` now use a shared bindless
   production root signature with `ResourceDescriptorHeap[]` in
   SM 6.6 + HLSL 2021 pixel shaders. ImGui SRVs also migrated to
   the same bindless heap, eliminating `SetDescriptorHeaps` switching
   at runtime.

2. **DX12 `BindlessTonemapPipeline` parity.** Same template as
   `dx12/utility/bindless_texture_blit_pipeline.{h,cpp}`, only
   the HLSL pixel shader differs (Uncharted2 + Gamma 2.2 instead
   of passthrough). Lands the day a DX12 host needs a runtime
   tonemap pass (e.g. swapchain change to RGB10A2 / FP16).

3. **Metal `BindlessTonemapPipeline` parity; WebGL2 permanently
   excluded.** Permanently deferred until a real consumer arrives.
   Metal would need an `argument_buffer` + `texture2d<...>` array
   path; WebGL2 **cannot** support bindless (no
   `GL_ARB_bindless_texture`, no `nonuniformEXT`, `sampler2D`
   arrays capped at 16-32 units) and will permanently fall back to
   the legacy bindful tonemap if/when WebGL2 ever gets a tonemap
   pass. `WebGL2RHI::supportsBindlessTextures()` returns `false`
   explicitly.

4. **Editor-side `BindlessTonemapSmoke` widget.** Optional --
   only worth doing if real-device regression on Adreno / Mali
   / Apple surfaces a finer-grained validation need than the
   production tonemap output itself. Cheap copy-paste of
   `bindless_blit_smoke.cpp`'s Vulkan block when needed.

5. ~~**`createShaderModuleFromSource` stage-parameter cleanup.**~~
   **Landed as PR-SS1.** See PR7 backlog item 3 for details.

6. **Sampler-bindless on Vulkan / DX12.** Permanently deferred
   unless a real workload demands runtime-varying sampler
   dispatch. See PR-V3 semantic divergence note 2 (Vulkan side)
   and PR8c backlog item 6 (DX12 side) for the design sketch.


## PR-DX1: DX12 first production bindless consumer (skybox + scene_grid)

### Motivation

Vulkan has had a production bindless draw since PR-V4 part 2
(`VulkanBindlessTonemapPipeline`). DX12 still used bindless only
on the editor inspector preview path (`BindlessTexturePreview`) and
the dev-only smoke widget (`BindlessBlitSmoke`). All runtime
production passes on DX12 used legacy descriptor-table root
signatures. PR-DX1 migrates the two production draws in
`DX12MainCameraPass` (skybox cubemap + scene_grid infinite grid)
to a shared bindless root signature, making them the first
non-preview DX12 consumers of `DX12BindlessTextureManager`.

### Design

**Root signature template** — shared between skybox and scene_grid:

| Root param | Type               | Register    | Purpose                     |
|-----------|--------------------|-------------|-----------------------------|
| 0         | 32-bit constants   | b0, space0  | packed bindless index       |
| 1         | Root CBV           | b1, space0  | per-draw UBO (camera etc.) |
| s0..s3    | Static samplers    | s0..s3      | LinearWrap/Clamp, Point    |

Flag: `CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT`

This mirrors the smoke test's Phase 4 root signature and the RHI's
`createPipelineLayout` bindless path, but adds the Root CBV for
per-draw constants (the smoke test only has the bindless index).

**Unified heap strategy** — `DX12BindlessTextureManager` owns the
sole active CBV/SRV/UAV heap. ImGui SRVs are allocated from this
same heap via `allocateRawSlot()`, eliminating all
`SetDescriptorHeaps` switching between production draws and ImGui
rendering. The heap is bound once at the top of each draw call
with `setBindlessDescriptorHeaps()`.

**Skybox** — fullscreen triangle VS + SM 6.6 / HLSL 2021 PS.
The PS reads the cubemap via
`ResourceDescriptorHeap[NonUniformResourceIndex(texture_index)]`.
IBL specular cubemap occupies a single bindless slot; sampler is
`g_linear_clamp : register(s1)` from the static sampler bank.
Per-viewport UBO (3 slots: game, scene, preview) uploaded each
frame via `mapMemory` / root CBV.

**Scene grid** — same root signature template, same fullscreen
triangle topology, but the PS ignores the bindless index (slot 0
= white placeholder pushed for consistency). Alpha blend enabled.
Single UBO uploaded per frame.

**ImGui migration** — `ImGui_ImplDX12_InitInfo::SrvDescriptorHeap`
now points at the bindless heap when supported.
`allocateImGuiSrvDescriptor()` routes through
`DX12BindlessTextureManager::allocateRawSlot()` instead of the
legacy `m_cbv_srv_uav_heap`. `editor_ui_pass.cpp` uses
`setBindlessDescriptorHeaps()` before `ImGui_ImplDX12_RenderDrawData`.

### Files landed

```
engine/Source/Runtime/Function/Render/passes/
  dx12_main_camera_pass.h           # REWRITE: bindless root signature,
                                    # PSOs, constant buffers, draw helpers
  dx12_main_camera_pass.cpp         # REWRITE: inline HLSL for skybox +
                                    # scene_grid, buildBindlessProductionRootSignature,
                                    # setupSkyboxResources, setupSceneGridResources,
                                    # drawSkyboxWithCamera, drawSceneGrid

engine/Source/Runtime/Function/Render/interface/dx12/
  dx12_bindless_texture_manager.h   # +allocateRawSlot / +freeRawSlot
  dx12_bindless_texture_manager.cpp # allocateRawSlot impl, refactored free()

  dx12_rhi.h                        # +getSamplerDescriptorHeap,
                                    # +setBindlessDescriptorHeaps,
                                    # +cmdSetRootConstantBufferView,
                                    # kBindlessStaticSamplerCount promoted to public
  dx12_rhi.cpp                      # implementations of the above,
                                    # allocateImGuiSrvDescriptor now uses bindless heap

engine/Source/Editor/render/pass/
  editor_ui_pass.cpp                # ImGui SrvDescriptorHeap -> bindless heap,
                                    # setBindlessDescriptorHeaps() before RenderDrawData
```

### Static asserts

In `dx12_main_camera_pass.cpp` (anonymous namespace, after struct
definitions):

- `sizeof(DX12SkyboxConstants) == 3 * sizeof(Vector4)` — pins the
  skybox UBO layout against the HLSL `cbuffer SkyboxConstants : register(b1)`
  that expects exactly 3 `float4` rows.
- `sizeof(DX12SceneGridConstants) == 6 * sizeof(Vector4)` — pins the
  scene_grid UBO layout against the HLSL
  `cbuffer SceneGridConstants : register(b1)` that expects 6 `float4` rows.
- Alignment checks on both structs (`alignof == alignof(Vector4)`).

### Cross-backend parity table (updated)

| Capability                                  | DX12 | Vulkan | Metal | WebGL2 |
|---------------------------------------------|------|--------|-------|--------|
| `RHIBindlessTextureManager`                 | ✅    | ✅      | ❌     | ❌      |
| `cmdSetBindlessIndexPFN` non-empty override | ✅    | ✅ (PR-V1) | ❌  | ❌      |
| `BindlessTextureBlitPipeline` utility       | ✅ (PR7) | ✅ (PR-V3) | ❌ | ❌      |
| `BindlessTonemapPipeline` utility           | ✅ (PR-DX2) | ✅ (PR-V4 part 1) | ❌ | ❌  |
| Editor preview widget end-to-end            | ✅    | ✅ (PR-V3 part 2) | ❌ | ❌ |
| Production draw consumer (non-preview)      | ✅ (PR-DX1) | ✅ (PR-V4 part 2) | ❌ | ❌  |

### Backlog -- items NOT addressed by PR-DX1

1. ~~**DX12 `BindlessTonemapPipeline` parity.**~~ **Landed as PR-DX2** (see
   section below). Same template as
   `dx12/utility/bindless_texture_blit_pipeline.{h,cpp}`, with the
   HLSL pixel shader applying Uncharted2 + Gamma 2.2 instead of
   passthrough. Pure-additive utility; no consumer wired yet (will be
   driven by a future standalone tonemap pass when a DX12 host needs
   runtime tonemap, e.g. swapchain change to RGB10A2 / FP16).

2. **Metal `BindlessTonemapPipeline` parity; WebGL2 permanently
   excluded.** Metal needs an `argument_buffer` + `texture2d<...>`
   array path; deferred until a real consumer arrives. WebGL2
   **cannot** support bindless (no `GL_ARB_bindless_texture`, no
   `nonuniformEXT`, `sampler2D` arrays capped at 16-32 units).
   `WebGL2RHI::supportsBindlessTextures()` returns `false`
   explicitly; no `WebGL2BindlessTextureManager` will ever exist.

3. **Editor-side `BindlessTonemapSmoke` widget.** Optional --
   only worth doing if real-device regression surfaces a finer-
   grained validation need.

4. ~~**`createShaderModuleFromSource` stage-parameter cleanup.**~~
   **Landed as PR-SS1.** See PR7 backlog item 3 for details.

5. **Sampler-bindless on Vulkan / DX12.** Permanently deferred
   unless a real workload demands runtime-varying sampler dispatch.


## PR-DX2: DX12 BindlessTonemapPipeline parity

### Motivation

Vulkan has had a `BindlessTonemapPipeline` since PR-V4 part 1. DX12 was
the only backend missing this utility. While no DX12 consumer is wired
yet (the editor does not have a standalone tonemap pass), landing the
pipeline class now closes the DX12/Vulkan parity gap and provides a
ready-made utility for the future standalone tonemap pass (triggered
when a DX12 host needs runtime tonemap, e.g. swapchain change to
RGB10A2 / FP16).

### Design

The class is structurally identical to its sibling
`BindlessTextureBlitPipeline` (PR7) -- same descriptor-set-layout
trick (VARIABLE_DESCRIPTOR_COUNT binding that triggers
DX12RHI::createPipelineLayout's bindless path), same pipeline state
(no depth, no blend, no MSAA, dynamic viewport+scissor), same
fullscreen-triangle VS (`bindless_blit_vs.hlsl`). The ONLY difference
is the pixel shader (`bindless_tonemap_ps.hlsl`), which applies the
Uncharted2 tone curve + Gamma 2.2 correction instead of a passthrough
texture fetch.

The tonemap math is byte-identical to the Vulkan GLSL sibling
(`tone_mapping_bindless.frag`) and the legacy `tone_mapping.frag`:
same Uncharted2 coefficients (A=0.15, B=0.50, C=0.10, D=0.20,
E=0.02, F=0.30), same exposure (* 4.5), same white-point
normalization (Uncharted2Tonemap(11.2)), same Gamma 2.2 per-channel
pow. This ensures that swapping between bindless and legacy tonemap
paths is a pixel-equivalent change.

**Why copy rather than refactor a shared base** (same rationale as the
Vulkan sibling):
1. The pipeline-state struct count is high (~12 RHI structs per
   pipeline) and a shared base would either require a virtual hook or
   an awkward config struct.
2. Keeping the DX12 files symmetrical with the Vulkan layout (one
   .h+.cpp pair per bindless pipeline kind) eases cross-backend review.
3. The duplication is ~250 lines of validated boilerplate; a future
   cross-cutting cleanup PR can extract a helper builder once a third
   bindless pipeline arrives.

**Index-pack contract** (same as `bindless_blit_ps.hlsl`):
- bits [0..15] = texture_index (slot in the bindless heap)
- bits [16..31] = sampler_index (0..3, indexes static samplers s0..s3)

### Files landed

```
engine/Source/Runtime/Function/Render/Interface/dx12/utility/
  bindless_tonemap_pipeline.h        # NEW: class declaration
  bindless_tonemap_pipeline.cpp      # NEW: implementation (mirrors blit sibling)
  shaders/
    bindless_tonemap_ps.hlsl          # NEW: Uncharted2 + Gamma 2.2 PS (SM 6.6 + HV 2021)
    bindless_blit_vs.hlsl             # REUSED: same fullscreen-triangle VS
```

### Cross-backend parity table (updated)

| Capability                                  | DX12 | Vulkan | Metal | WebGL2 |
|---------------------------------------------|------|--------|-------|--------|
| `RHIBindlessTextureManager`                 | ✅    | ✅      | ❌     | ❌      |
| `cmdSetBindlessIndexPFN` non-empty override | ✅    | ✅ (PR-V1) | ❌  | ❌      |
| `BindlessTextureBlitPipeline` utility       | ✅ (PR7) | ✅ (PR-V3) | ❌ | ❌      |
| `BindlessTonemapPipeline` utility           | ✅ (PR-DX2) | ✅ (PR-V4 part 1) | ❌ | ❌  |
| Editor preview widget end-to-end            | ✅    | ✅ (PR-V3 part 2) | ❌ | ❌ |
| Production draw consumer (non-preview)      | ✅ (PR-DX1) | ✅ (PR-V4 part 2) | ❌ | ❌  |

### Backlog -- items NOT addressed by PR-DX2

1. ~~**DX12 standalone tonemap pass consumer.**~~ **Done**: `BindlessTonemapPass`
   is wired from `DX12MainCameraPass::Draw` (RP1 -> tonemap -> RP2), same
   utility pipeline as PR-DX2.

2. **Metal `BindlessTonemapPipeline` parity; WebGL2 permanently
   excluded.** Metal needs an `argument_buffer` + `texture2d<...>`
   array path; deferred until a real consumer arrives. WebGL2
   **cannot** support bindless (no `GL_ARB_bindless_texture`, no
   `nonuniformEXT`, `sampler2D` arrays capped at 16-32 units).
   `WebGL2RHI::supportsBindlessTextures()` returns `false`
   explicitly; no `WebGL2BindlessTextureManager` will ever exist.

3. **Editor-side `BindlessTonemapSmoke` widget.** Optional -- only
   worth doing if real-device regression surfaces a finer-grained
   validation need.

4. ~~**`createShaderModuleFromSource` stage-parameter cleanup.**~~
   **Landed as PR-SS1.** See PR-SS1 section below.

5. **Sampler-bindless on Vulkan / DX12.** Permanently deferred
   unless a real workload demands runtime-varying sampler dispatch.


## PR-SS1: `createShaderModuleFromSource` stage-parameter cleanup

### Motivation

The `createShaderModuleFromFile` and `createShaderModuleFromSource` virtual
methods in the RHI base class accepted `int shader_stage`, which conflated
two incompatible value spaces: the sequential `ShaderStage` enum (0=Vertex,
1=Fragment, ...) and the Vulkan-style `RHIShaderStageFlagBits` bitflags
(1=Vertex, 16=Fragment, ...). Call sites had to `static_cast<int>()` or
pass raw `RHI_SHADER_STAGE_*_BIT` constants, and the DX12 compiler's
`getShaderProfile()` had to try both interpretations in sequence.

### Design

- **`ShaderStage` enum class promoted to `render_type.h`** (alongside
  `RHIShaderStageFlagBits`), so all RHI backends can include it without
  depending on `vulkan/shader_compiler.h`.
- **`vulkan/shader_compiler.h`** now `#include`s `render_type.h` instead of
  defining `ShaderStage` locally.
- **RHI base class** `int shader_stage` → `ShaderStage shader_stage` in
  both `createShaderModuleFromFile` and `createShaderModuleFromSource`.
- **All 4 backend overrides** (DX12, Vulkan, Metal, WebGL2) updated.
- **All shader compiler classes** accept `ShaderStage` instead of `int`.
  `DX12ShaderCompiler::getShaderProfile()` simplified: single
  `switch (ShaderStage)` replaces the old dual-path.
- **All call sites** updated: `static_cast<int>(ShaderStage::Vertex)` →
  `ShaderStage::Vertex`; `RHI_SHADER_STAGE_VERTEX_BIT` → `ShaderStage::Vertex`.
- **Disk cache compatibility**: cache filenames embed
  `static_cast<int>(shader_stage)`. Values unchanged, so existing
  cache entries remain valid.






