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
void RegisterPlatform();
