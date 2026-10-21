#pragma once

// =====================================================================
// Legacy HLSL output-semantic rewrite
// ---------------------------------------------------------------------
// Unity shaders are written in Cg, whose output semantics predate Shader
// Model 4. A pixel shader ends with
//
//     float4 frag (VertexOutput i) : COLOR { ... }
//
// and DXC rejects that outright on any modern target:
//
//     error: invalid semantic 'COLOR' for ps 6.0
//
// The tempting one-line fix -- `#define COLOR SV_Target` in the UnityCG
// shim -- does NOT work, and it fails in the worst possible way. `COLOR`
// is *also* a legal INPUT semantic, and Spine uses it exactly that way:
//
//     struct VertexInput { float4 vertexColor : COLOR; };   // VS input
//     struct VertexOutput { float4 vertexColor : COLOR; };  // VS output
//     float4 frag (VertexOutput i) : COLOR { ... }          // PS output
//
// A global macro rewrites all three. The two struct members then carry
// SV_Target, which is illegal on a vertex shader signature; the shader
// stops compiling for a reason that has nothing to do with the original
// error. (Fixtures: unity_compat/diag_semantic_macro.shader proves the
// macro *does* expand in semantic position -- it just cannot be used
// selectively.)
//
// So the rewrite has to be surgical: only the semantic attached to the
// ENTRY FUNCTION's own return value, i.e. the `: NAME` that sits between
// the closing paren of the parameter list and the opening brace (or the
// semicolon of a forward declaration). Everything else is left alone.
//
// Only the fragment stage is touched. `COLOR` is a perfectly valid input
// semantic for a vertex shader, so rewriting there would break correct
// shaders rather than fix broken ones.
//
// Deliberately free of any render-backend dependency -- no Vulkan, no DX12
// -- so it compiles and is unit-tested on every configuration.
// =====================================================================

#include "Runtime/Function/Render/RenderType.h"

#include <string>

namespace ZEngine::ShaderLab
{

    struct SemanticRewriteResult
    {
        // Rewritten source. Identical to the input (same length, same line
        // count) when nothing matched, so downstream diagnostics still point
        // at the author's lines.
        std::string code;

        // How many output semantics were replaced. 0 means untouched.
        int rewrites = 0;
    };

    // Replaces legacy Cg output semantics on `entry_point` with their
    // Shader-Model-4+ equivalents:
    //
    //     COLOR   -> SV_Target      (Spine's `float4 frag (...) : COLOR`)
    //     COLOR0  -> SV_Target0     COLOR1 -> SV_Target1   ... COLOR7
    //
    // Returns the source unchanged when `entry_point` is empty, when `stage`
    // is not Fragment, or when the entry already uses an SV_* semantic.
    SemanticRewriteResult RewriteLegacyOutputSemantics(const std::string& hlsl,
                                                      const std::string& entry_point,
                                                      ShaderStage stage);

}  // namespace ZEngine::ShaderLab
