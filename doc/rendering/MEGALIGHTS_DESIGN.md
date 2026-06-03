# MegaLights (ZEngine)

UE reference: `UnrealEngine/Engine/Source/Runtime/Renderer/Private/MegaLights/`,
`Engine/Shaders/Private/MegaLights/`, SIGGRAPH 2025 "MegaLights: Stochastic Direct Lighting".

## Goal

Scale direct lighting to hundreds of dynamic local lights without per-light shadow maps.
Fixed cost per pixel via stochastic light selection + screen-space visibility (V1).

## V1 (landed)

| Stage | Implementation |
|-------|----------------|
| Light collection | `RenderScene::SyncPointLightsFromLevel` fills `PointLightList`; optional `r.MegaLights.SpawnTestLights` debug ring |
| Tile culling | CPU 8x8 tiles, frustum-sphere test, max 64 lights/tile |
| Shading | `megalights_deferred.frag` - reservoir-style pick + GGX from `mesh_lighting.h` |
| Shadows | Screen-space ray march on depth (no HWRT yet). **DX12**: `Texture2D::Load` at arbitrary pixels along the march. **Vulkan**: subpass depth input has no per-pixel offset API -- off-center samples fall back to center depth (conservative, may miss thin occluders). |
| Temporal denoise | Stochastic direct lighting only (not directional/IBL). Per-viewport ping-pong `R16G16B16A16` history textures; depth-based reprojection + neighborhood clamp. CVars: `r.MegaLights.TemporalDenoise.Enable` (default 1), `.Blend` (default 0.9), `.DisocclusionThreshold` (default 0.02). |
| Spatial denoise | Joint bilateral on temporally filtered direct lighting (depth + normal edge stop). Second fullscreen draw in deferred subpass when enabled. Pass 1 writes `ml_history_out` + `Lo_temporal + stable`; pass 2 filters history as `ml_direct` and overwrites `backup_odd` with `Lo_filtered + stable`. CVars: `r.MegaLights.SpatialDenoise.Enable` (default 1), `.Radius` (1=3x3, 2=5x5), `.DepthSigma`, `.NormalPower`. |
| Unlit sky | Vulkan: `skybox_sampler` (set 2). DX12: reconstruct world ray + `specular_map` cubemap (t5, same env faces as IBL). |
| Directional | Still from legacy `scene_directional_light` + shadow map |
| Toggle | `r.MegaLights.Enable` (default 0) |

## Pipeline hook

When `MegaLights::IsEnabled()` on Vulkan:

1. Skip `PointLightShadowPass` draw (no per-light atlases).
2. `MainCameraPass::DrawDeferredLighting` uses `megalights_deferred` pipeline; when spatial denoise is on, a second `megalights_spatial` draw follows (UAV->SRV barrier on history write buffer between passes).
3. SSBO bindings 8-10 on mesh global set: lights + tile indices + tile ranges.

## Not in V1

- HWRT / Lumen BVH tracing (UE production path)
- Rect/spot textured area lights
- ~~DX12 deferred parity~~ **DX12 RP1 landed** (`megalights_deferred.frag.hlsl` + `MainCameraRp1Pass`)
- Volumetric/translucency injection

## CVars

| CVar | Default | Meaning |
|------|---------|---------|
| `r.MegaLights.Enable` | 0 | Use MegaLights deferred path |
| `r.MegaLights.NumSamplesPerPixel` | 4 | Stochastic samples (2/4/8) |
| `r.MegaLights.ScreenSpaceShadowSteps` | 12 | SS ray march steps |
| `r.MegaLights.MaxLights` | 1024 | GPU light cap |
| `r.MegaLights.TemporalDenoise.Enable` | 1 | Blend stochastic direct with reprojected history |
| `r.MegaLights.TemporalDenoise.Blend` | 0.9 | History weight (0=current frame only) |
| `r.MegaLights.TemporalDenoise.DisocclusionThreshold` | 0.02 | UV reprojection rejection threshold |
| `r.MegaLights.SpatialDenoise.Enable` | 1 | Joint bilateral filter on direct lighting |
| `r.MegaLights.SpatialDenoise.Radius` | 1 | Filter radius in pixels (1=3x3, 2=5x5) |
| `r.MegaLights.SpatialDenoise.DepthSigma` | 0.01 | Depth edge-stop sigma (view-space) |
| `r.MegaLights.SpatialDenoise.NormalPower` | 32 | Normal edge-stop exponent |

## Console commands

| Command | Meaning |
|---------|---------|
| `r.MegaLights.SpawnTestLights [N]` | Spawn N debug lights in a ring (default 16), sets Enable=1 |
| `r.MegaLights.ClearTestLights` | Remove debug lights |

## Smoke test (editor)

Vulkan or DX12 (Windows default):

1. Open demo project, scene view with lit geometry.
2. Console: `r.MegaLights.SpawnTestLights 24`
3. Expect warm point lighting without point-shadow pass cost; toggle `r.MegaLights.Enable 0` to compare legacy loop.

## Files

- `engine/Source/Runtime/Function/Render/MegaLights/*`
- `engine/shader/glsl/megalights_deferred.{vert,frag}`
- `engine/shader/glsl/megalights_spatial.frag`
- `engine/shader/hlsl/rp1/megalights_spatial.frag.hlsl`
- `engine/shader/include/megalights_*.h`
