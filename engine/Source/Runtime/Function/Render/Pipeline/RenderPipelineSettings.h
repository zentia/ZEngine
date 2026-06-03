#pragma once

class ConsoleManager;

// Selection layer for the active render path (which RenderPipelineModule runs).
// Mirrors the MegaLights settings pattern: a CVar-backed int with thin accessors,
// plus a deferred-apply "dirty" flag so a path switch requested from the console /
// editor (on the game thread) is applied by the RenderSystem at a safe frame
// boundary (GPU-idle), never mid-frame.
namespace RenderPipelineSettings
{

enum class RenderPath
{
    Auto    = 0,  // resolve from the running platform (Desktop on PC, Mobile on phone)
    Desktop = 1,  // deferred + forward (full-fidelity)
    Mobile  = 2,  // forward skeleton (lightweight)
};

// The path the user configured via r.RenderPath (may be Auto).
RenderPath GetConfiguredPath();

// Platform default for Auto: Desktop on Windows/macOS/Linux, Mobile on Android/iOS/OHOS.
RenderPath ResolveAuto();

// The concrete path to instantiate (GetConfiguredPath() with Auto resolved).
RenderPath GetEffectivePath();

const char* ToString(RenderPath path);

// Registers the r.RenderPath CVar on the given console. Its on-changed callback
// flips the dirty flag. Called from RegisterRuntimeConsoleCommands.
void RegisterConsoleVariables(ConsoleManager& console);

// Programmatic switch (editor menu / console command). Updates the CVar storage
// and flips the dirty flag. Pass Auto/Desktop/Mobile.
void SetConfiguredPath(RenderPath path);

// Returns true exactly once after the configured path changed; the RenderSystem
// calls this each frame and, on true, performs RenderPipeline::SetActiveModule.
bool ConsumePathDirty();

}  // namespace RenderPipelineSettings
