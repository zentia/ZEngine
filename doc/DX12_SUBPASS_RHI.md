# DX12 subpass RHI emulation (DX-B0)

Vulkan `MainCameraPass` relies on multi-subpass render passes (`CmdNextSubpass`).
On DX12, `DX12RHI` now **emulates** that contract so the same pass code can run
once the full MainCamera port (path B) lands.

## What changed

| Component | Behavior |
|-----------|----------|
| `DX12RenderPass` | Deep-copies `RHIRenderPassCreateInfo` (attachments, subpasses, dependencies). Legacy `setAttachments()` still builds one implicit subpass. |
| `DX12Framebuffer` | `setAttachmentView(i, view)` / `getAttachmentView(i)` indexed by render-pass attachment index. |
| `CmdBeginRenderPass` | Subpass 0 only: dependencies `EXTERNAL -> 0`, transitions, bind RTVs/DSV, clear per `loadOp`. |
| `CmdNextSubpass` | Apply dependency edges, transition inputs to SRV + outputs to RT/DSV, rebind OM. |
| `CmdEndRenderPass` | All attachments -> `finalLayout` from `RHIAttachmentDescription`. |

## Files

- `engine/Source/Runtime/Function/Render/Interface/DX12/DX12RenderPass.cpp`
- `engine/Source/Runtime/Function/Render/Interface/DX12/DX12RHIResource.h`
- `engine/Source/Runtime/Function/Render/Interface/DX12/DX12RHI.cpp` (CmdBegin/Next/End)

## Limitations (v1)

- Barriers are **conservative** (per-subpass attachment role), not a full Vulkan access-mask decoder.
- `preserveAttachments` and `resolveAttachments` are stored but not acted on.
- `RHISubpassContents` (inline vs secondary) is ignored.
- Input attachments are implemented as **SRV** binds (same as existing `INPUT_ATTACHMENT` descriptor mapping).

## DX-B1: MainCamera framebuffer scaffolding (done)

Backend-neutral resources live in:

- `engine/Source/Runtime/Function/Render/Passes/MainCameraFramebufferResources.{h,cpp}`
- `MainCameraPassInitInfo` moved here (avoids pulling Vulkan-only `MainCameraPass.h` into DX12 TUs)

`DX12MainCameraPass::Initialize` calls `MainCameraFramebufferResources::Initialize` after the bindless
root signature is built. `UpdateAfterFramebufferRecreate` is wired from
`RenderPipeline::passUpdateAfterRecreateSwapchain` on the DX12 branch.

CMake: on `WIN32` + DX12 (non-Vulkan), `MainCameraFramebufferResources.cpp` is allow-listed next to
`Passes/DX12*.cpp`.

Creates (no draws yet):

| Resource | Layout |
|----------|--------|
| RP1 | 5 attachments (gbuffer A/B/C, backup_odd, depth), 3 subpasses |
| RP1 FB | Single framebuffer |
| RP2 | 4 attachments (backup odd/even, post odd, swapchain), 4 subpasses |
| RP2 FB | One per swapchain image |

Log line on success: `MainCameraFramebufferResources: RP1+RP2 ready (...)`.

## DX-B2: shadow passes on DX12 (done)

- `DirectionalLightPass` / `PointLightPass` allow-listed in the Win32 DX12 build; wired into
  `RenderPipeline::Initialize` and `DeferredRender` (directional draw, then point clear, then main camera).
- Built-in directional shadow HLSL: `ShadowPassDx12Shaders.h` (Vulkan keeps SPIR-V blobs).
- `ShadowPassShared` creates the per-mesh descriptor layout (replaces `MainCameraPass::_per_mesh` on DX12).
- `RenderResourceDx12.cpp`: ring-buffer upload + rigid mesh buffers for shadow draws.
- `DX12RHI::IsPointLightShadowEnabled()` returns true (legacy gate for mesh shadow rendering).
- `DX12MainCameraPass` exposes `m_*ShadowColorImageView` for RP1 deferred sampling.

Point-light mesh projection (geometry shader) remains Vulkan-only; DX12 pass still clears its atlas each frame.

## DX-B3: MainCamera RP1 draws (done)

- `MainCameraRp1Pass` + `MainCameraPassShaderCommon`: gbuffer / deferred / forward subpasses via RHI render-pass API.
- HLSL under `engine/shader/hlsl/rp1/`; runtime compile via `ZENGINE_SHADER_ROOT` (CMake define on Win32 non-Vulkan).
- `DX12MainCameraPass::Draw` calls `DrawRP1` when init succeeds; bindless skybox-to-swapchain is fallback only.
- `RenderResourceDx12`: per-material UBO + 1x1 white placeholder textures + material descriptor set.
- `RenderPipeline(DX12)`: wires `m_MaterialDescriptorSetLayout` from `MainCameraRp1Pass` after init.

Skybox inside RP1 forward subpass: `MainCameraRp1Pass::SetSkyboxDrawCallback` -> `DX12MainCameraPass::DrawSkybox` (Vulkan parity, see Next steps item 2).

## DX-B4: Bindless tonemap between RP1 and RP2 (done)

- `BindlessTonemapPass` is backend-agnostic: Vulkan uses `VulkanBindlessTonemapPipeline`, DX12 uses `DX12BindlessTonemapPipeline` (PR-DX2 utility).
- `DX12MainCameraPass::Draw`: RP1 (`DrawRP1`) then `BindlessTonemapPass::Draw` (samples `backup_odd`, writes `backup_even`).
- Bindless slot for `backup_odd` is allocated/refreshed in `UpdateAfterFramebufferRecreate` (swapchain resize).
- `ZENGINE_DX12_UTILITY_SHADER_ROOT` points at `Interface/DX12/Utility/Shaders` for runtime HLSL compile.

## DX-B5: RP2 post chain on DX12 (done)

- `MainCameraRp2Pass`: RP2 render pass (color grading -> FXAA -> UI clear + ImGui callbacks -> combine to swapchain).
- HLSL under `engine/shader/hlsl/rp2/` (`post_process.vert`, `color_grading.frag`, `fxaa.frag`, `combine_ui.frag`).
- `DX12MainCameraPass::Draw` order: RP1 -> tonemap -> RP2 -> scene grid overlay.
- `post_ui_callbacks` run inside RP2 UI subpass (same slot as Vulkan `MainCameraPass`).

## DX-B6: ImGui in RP2 UI subpass (done)

- `EditorUIPass` DX12: `RTVFormat` = `DX12RHI::GetUiLayerRtvFormat()` (`R16G16B16A16`, matches `backup_even`).
- `Draw()` binds `backup_even` via `DX12RHI::BindUiLayerRenderTarget` instead of `RestoreSwapchainRenderState` (swapchain is only written in combine_ui).
- `RenderPipeline::GetUIRenderPass()` / `GetUiLayerColorView()` forward to `DX12MainCameraPass` framebuffer resources.
- Swapchain resize: `registerFramebufferRecreateCallback` -> `EditorUIPass::RecreateBackendDeviceObjects()` (`ImGui_ImplDX12_Invalidate/CreateDeviceObjects`).

## DX-B8: RenderPipeline DX12 parity (done)

- DX12 init keeps shadow + `DX12MainCameraPass` only; color grading / FXAA / UI / combine are embedded in `MainCameraRp2Pass` (documented in `RenderPipeline.cpp`).
- `DX12MainCameraPass::getRp2RenderPass` / `getUiLayerColorView` / `getFramebufferImageViews` mirror Vulkan `MainCameraPass` queries for Editor and resize hooks.

## Path B status

All numbered path-B implementation items (DX-B2 .. DX-B8, RP1 skybox, global IBL/LUT init order,
cubemap mip upload) are **done**. No further code milestones are tracked here; cross-backend bindless
gaps (Metal / WebGL2) live in `doc/BINDLESS_TEXTURE_PATH.md`.

## Manual validation (RenderDoc)

Optional regression pass after renderer changes. Capture one frame on Win64 + DX12 + demo project:

- [ ] RP1 subpass 0: 3 MRT + depth bound.
- [ ] After `CmdNextSubpass`: gbuffer images in `PIXEL_SHADER_RESOURCE`, `backup_odd` as RTV.
- [ ] After RP1 end: `backup_odd` in `finalLayout` shader-read; tonemap pass samples via bindless.
- [ ] Startup log: `DX12 CreateCubeMap: uploaded mip0 + GPU-generated N mips` and
      `DX12CubemapMipGenerator: ready` when IBL HDR cubemap loads. These lines are
      first written during `RenderSystem::Initialize` (before the Console window
      exists); the editor re-emits them right after `EditorUI` init so they also
      show up in the Console. The on-disk log file always has the original lines.
