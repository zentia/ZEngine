# Per-Platform Texture Cook Pipeline

This document describes ZEngine's texture cook pipeline: one source image per
texture, per-platform compressed + mipped variants (BC on desktop/WebGL, ASTC
LDR on mobile; **no ETC2**), cached in a Derived Data Cache (DDC) and emitted as
per-platform cooked `.zasset` files.

It mirrors UE's model on top of ZEngine's existing conventions (no `.meta`,
GUID-in-header, `AssetRegistry/*.json` for settings, `Intermediate/` for derived
data). It is the engine equivalent of Unity's `Library/` + per-platform texture
import overrides, expressed without sidecar files.

---

## 1. Source vs. cooked model

| Concept | Unity | UE | ZEngine |
|---------|-------|----|---------|
| Per-asset import settings | `.meta` | `UTexture` import settings | `<Project>/AssetRegistry/texture_import_settings.json` (VCS) |
| Derived/cooked bytes | `Library/` | DDC + `Cooked/<Platform>/` | `<Project>/Intermediate/DDC/` (LMDB) + `<Project>/Intermediate/Cooked/<Platform>/` |
| Stable identity | GUID in `.meta` | package GUID | GUID in the `.zasset` `AssetFileHeader` (176-byte prefix) |

- **Source image** (`.png` / `.jpg` / `.jpeg` / `.tga` / `.bmp`) is the
  authoring artifact. It is a transient input to import; per the Project window
  rules it is never surfaced in the asset tree.
- **Editor-platform `.zasset`** lives next to the source as
  `<dir>/<stem>.zasset`. It holds the cooked variant for the editor's preview
  build target (`Standalone` -> BC7 on the Windows DX12 editor). This is the
  file the editor preview and scene materials consume.
- **Per-platform cooked `.zasset`** lives under
  `<Project>/Intermediate/Cooked/<Platform>/<rel>.zasset`. Produced by the cook
  step (Build menu / `asset.cook`). Reuses the **source asset's GUID** so player
  builds resolve references identically.

---

## 2. Texture2D schema (Phase 1)

`engine/Source/Runtime/Function/Render/Texture/Texture2D.{h,cpp}` stores a full
mip chain and a real GPU format:

- `uint32_t m_Width / m_Height`
- `uint32_t m_Format` -- an `RHIFormat` ordinal, including the BC*/ASTC block
  formats (`RHI_FORMAT_BC1_RGB_UNORM_BLOCK` .. `RHI_FORMAT_ASTC_12x12_SRGB_BLOCK`).
- `std::vector<uint8_t> m_Pixels` -- **all mips concatenated**, mip0 first.
- `std::vector<uint32_t> m_MipOffsets` -- byte offset of each mip within
  `m_Pixels`. Empty on legacy single-mip assets; helpers synthesize `[0]`.
- Helpers: `GetMipCount()`, `GetMipSpan(i)`, `IsCompressed()`.

`Transfer()` appends the mip/format fields after the legacy `"pixels"` node, so
old `.zasset` files still load: SafeBinaryRead returns `kNotFound` for the
missing nodes and leaves the new fields at their defaults (single uncompressed
RGBA8 mip0). Covered by the T1/T2 scenarios in
`engine/Source/Runtime/Core/Serialize/Test/SchemaEvolutionSmokeTest.cpp`.

---

## 3. Encoders + TextureCompressor (Phase 2)

`engine/Source/Editor/AssetPipeline/TextureImporter/TextureCompressor.{h,cpp}`
(editor-only) takes RGBA8 -> sRGB-aware box-filter mip chain -> block encode:

- **BC1 / BC3 / BC7** via `bc7enc_rdo` (vendored under `engine/3rdparty/`, MIT).
  Desktop + WebGL.
- **ASTC LDR** via ARM `astc-encoder` (Apache-2.0). Mobile (Android / iOS).
- Output `CompressedTexture { width, height, rhi_format, mip_offsets, pixels }`
  matches the Phase 1 `Texture2D` layout exactly.
- `EncoderVersion()` is folded into the DDC cache key so an encoder bump
  invalidates stale variants.

Encoders link `PRIVATE` into ZEditor only (cooking is editor-side), but compile
on the host regardless of target so the Windows editor can cross-encode ASTC for
a mobile cook.

---

## 4. Derived Data Cache (Phase 3)

`engine/Source/Runtime/Resource/Cache/` (`LMDBDerivedDataCache`) is a process-
wide LMDB cache opened lazily at `<Project>/Intermediate/DDC/` through
`Runtime::GetDerivedDataCache()`
(`DerivedDataCacheAccessor.{h,cpp}`).

- Key: `DDCKey { cache_type="Texture", asset_guid, cache_key }`.
- `cache_key = MakeDDCCacheKey(platform_tag, settingsHash, encoderVersion)`
  where `settingsHash` = FNV-1a 64 over the cook-affecting settings
  (format / mips / sRGB / max_size / quality).
- Value blob: a compact `'TXDC'`-tagged serialisation of the cooked variant
  (independent of the `.zasset` SerializedFile format -- the DDC stores raw cook
  artifacts; the `.zasset` stores the engine `Object`).

Because the key includes platform + settings + encoder version, the same source
image cooked for two platforms produces two cache entries that never collide.

---

## 5. Import + preview (Phase 4)

`TextureImporter::Import`
(`engine/Source/Editor/AssetPipeline/TextureImporter/TextureImporter.cpp`):

1. Decode source to RGBA8 (stb_image), downscale to `max_size`.
2. `GetEffective(EditorPreviewBuildTarget())` -> cook options (default BC7).
3. DDC `get`; on miss, `TextureCompressor::Compress` then DDC `Put`.
4. Build a `Texture2D` (mips + BC format) and write `Assets/<stem>.zasset`.

The default per-platform format is `BC7` (`TextureImporterSettings::PlatformSettings::format`),
so freshly imported desktop textures are compressed + mipped out of the box.

Editor preview / UI upload the compressed mip chain:
`UiGpuResources::EnsureTexture2D` / `CreateFromPixels`
(`engine/Source/Runtime/UI/Render/UiGpuResources.{h,cpp}`) pass `miplevels` and
the BC format to `RHI::CreateGlobalImage`. Verified on the DX12 BC sampling path.

---

## 6. Material consumption (Phase 5)

- `MaterialRes` (`engine/Source/Runtime/Resource/ResType/Data/Material.{h,cpp}`)
  gained `PPtr<Texture2D>` shadow fields beside each `m_*TextureFile` string
  (write both, read prefer PPtr, fall back to path string -- same pattern as
  `m_shader_pptr`). `Get*TextureFile()` accessors return the cooked `.zasset`
  path when the PPtr is valid, else the legacy source-path string. The PPtr
  fields are appended last in `Transfer()`, so old material `.zasset` files read
  them back null and behave byte-identically.
- A startup pass `TextureImporter::ImportProjectTextures()` (called from
  `EditorAssetManager::Initialize`, after `ImportProjectShaders`) walks
  `<Project>/Assets/` and cooks an `Assets/<stem>.zasset` for any source image
  lacking one (A2 first-time seeding -- idempotent, O(stat) on warm restarts).
- `RenderResourceBase::LoadTexture`
  (`engine/Source/Runtime/Function/Render/RenderResourceBase.cpp`) tries
  `TryLoadCookedTexture(file)` first: it resolves the `<source>.zasset` sibling,
  loads the cooked `Texture2D`, and copies its compressed + mipped payload into a
  `TextureData`. On a miss it falls through to the legacy `stb_image` source
  decode, so projects without cooked variants render unchanged.
- RHI upload threads the real mip count: `RenderResource.cpp` populates the new
  `*_miplevels` fields on `TextureDataToUpdate` and passes them to
  `RHI::CreateGlobalImage`. The DX12 backend (`DX12RHI.cpp`) uploads packed mip
  chains for both linear and block-compressed formats
  (`UploadPackedMipChain` / `CreateMipUploadStaging`).

### Backend gating

DX12 can sample BC*; the Vulkan `CreateGlobalImage` path here does not yet
decode block-compressed uploads. `TryLoadCookedTexture` therefore returns
`nullptr` for block-compressed cooked variants when the active backend is not
DX12, forcing the uncompressed `stb_image` fallback (Vulkan then auto-generates
mips via `miplevels=0`). ASTC is not sampleable on the DX12 editor at all, so
ASTC variants are verified by file inspection, not rendering -- matching UE (you
don't preview Android ASTC on a D3D editor).

---

## 7. Cook step + per-platform output (Phase 6)

### Explicit-GUID write API

`AssetManager::WriteObjectToDiskWithGuid(path, object, guid)` (and the new
`explicit_guid` parameter on `WriteFile`) stamps a caller-supplied GUID into the
`AssetFileHeader` instead of the path-derived `DeterministicGuidFromPath`. The
cook step uses it so a cooked `.zasset` under `Intermediate/Cooked/<Platform>/`
keeps the **source** GUID -- otherwise its path-derived GUID would differ and
references would not resolve in a player build.

### Cook walk

`TextureImporter::CookProjectTextures(BuildTarget)`:

1. Walk `<Project>/Assets/` for source images.
2. Read the source GUID from the editor `Assets/<stem>.zasset` header
   (`GetAssetGuidAndType`); skip with a warning if the texture isn't imported.
3. `GetEffective(target)` -> cook options; decode + downscale + DDC-keyed encode
   (BC on desktop/WebGL, ASTC on mobile).
4. Write the cooked `Texture2D` to
   `<Project>/Intermediate/Cooked/<Platform>/<rel>.zasset` via
   `WriteObjectToDiskWithGuid(out_path, texture, source_guid)`.

`ProjectInfo::EnsureScriptsScaffold` creates `Intermediate/Cooked/` (gitignored);
the per-platform subdir is made on demand. Paths come from
`ProjectInfo::GetIntermediateCookedRoot()` / `GetIntermediateDDCRoot()`.

### Entry points

- **Build menu** (`MenuController.cpp`): `Build -> Cook Textures for
  Standalone | Android | iOS | WebGL`.
- **Console** (`EditorConsoleCommands.cpp`): `asset.cook <standalone|android|ios|webgl>`.

---

## 8. Verification

- **Standalone (BC7)**: import a PNG, observe the `Assets/<stem>.zasset` is
  compressed + mipped, and the scene material renders through it on DX12
  (`TryLoadCookedTexture` hit). `asset.cook standalone` writes a renderable
  cooked variant under `Intermediate/Cooked/Standalone/`.
- **Android (ASTC)**: `asset.cook android` writes cooked variants under
  `Intermediate/Cooked/Android/` with the source GUID. Verified by header/size
  inspection (not rendered on the DX12 editor).
- Full Vulkan compressed-upload verification is blocked by the pre-existing
  `MainCameraPass` crash; the Vulkan path takes the uncompressed fallback in the
  meantime.

---

## 9. The `.meta` / `Library` mapping (summary)

| Unity artifact | ZEngine equivalent |
|----------------|--------------------|
| `Foo.png.meta` (import settings + GUID) | `texture_import_settings.json` (settings) + GUID in `.zasset` header |
| `Library/.../<hash>` (cooked bytes cache) | `Intermediate/DDC/` (LMDB) |
| Platform build output | `Intermediate/Cooked/<Platform>/<rel>.zasset` |

No sidecar files; the settings JSON is checked into VCS, all derived/cooked data
lives under the gitignored `Intermediate/`.
