#pragma once

// Selects which RHI implementation `RegisterRuntime` registers.
// On platforms that only support a single RHI (Android/OHOS -> Vulkan,
// macOS/iOS -> Metal, Web -> WebGL2) the value is ignored.
// Only Windows respects this (DX12 vs Vulkan); the default is the
// platform's "primary" backend (DX12 on Windows).
enum class PreferredRHI
{
    Default,
    DX12,
    Vulkan
};

void RegisterCore();
void RegisterRuntime(PreferredRHI preferred_rhi = PreferredRHI::Default);
// Light variant: registers core + rendering/UI, but NO editor/asset systems.
// Used by standalone tools (TexPreview, etc.) that need a window + rendering
// but don't have a project opened.
void RegisterRuntimeLight(PreferredRHI preferred_rhi = PreferredRHI::Default);
// Registers the AssetManager implementation used outside the editor
// (RuntimeAssetManager). Called by RegisterGameRuntime() for the standalone
// game, and by ZEngineApi / standalone tools. Deliberately NOT called by
// RegisterRuntime(): the editor registers its own EditorAssetManager via
// RegisterEditor(), and RegisterSystemAlias() would skip it if this ran first.
void RegisterPlatform();

// Standalone game process (ZGame): Runtime systems + the game framework layer
// (GameEngine -> GameInstance / LocalPlayer / PlayerController).
//
// Why this is a separate entry point rather than being folded into
// RegisterRuntime(): the editor also calls RegisterRuntime(), and GameEngine
// auto-possesses the active character on startup. Running that inside the
// editor would steal the character the editor is driving. Only the standalone
// game asks for the game layer.
void RegisterGameRuntime(PreferredRHI preferred_rhi = PreferredRHI::Default);
