// Shadow RenderResourceBase.h for the headless WASM VT harness.
//
// WHY: VirtualTextureResource.h and VirtualTextureManager.h both
// #include "Runtime/Function/Render/RenderResourceBase.h". The real header pulls
// RenderScene.h + RenderSwapContext.h, which drag in the full Object system
// (PPtr -> Object -> ObjectDefines -> Macro -> Platform.h, plus Component.h ->
// Component.generated.h). That codegen + Object machinery is not available to a
// bare standalone TU, so the include chain fails to compile under emcc.
//
// The VT files use RenderResourceBase ONLY as an opaque pointer (Initialize()
// stores it, Shutdown() nulls it -- never dereferenced) and need TextureData
// (defined in the real RenderType.h). So this shadow provides a forward
// declaration of RenderResourceBase and forwards TextureData from the real
// RenderType.h, dropping the RenderScene/RenderSwapContext includes entirely.
// It is NEVER linked into the real engine; it lives only in
// scripts/vt_wasm_shadow/ and is placed on the include path BEFORE engine/Source.

#pragma once

#include "Runtime/Function/Render/RenderType.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// Opaque: the VT manager only stores/passes this pointer, never dereferences it.
class RenderResourceBase;
