#pragma once

// ===========================================================================
// Unity surface-shader desugaring.
// ---------------------------------------------------------------------------
// A Unity surface shader is not a shader: it is a *request* for one. The
// author writes
//
//     CGPROGRAM
//     #pragma surface surf Standard fullforwardshadows
//     struct Input { float2 uv_MainTex; };
//     void surf (Input IN, inout SurfaceOutputStandard o) { ... }
//     ENDCG
//
// directly under SubShader -- with no Pass, no #pragma vertex, no #pragma
// fragment. Unity's own compiler expands that single block into several
// passes (ForwardBase, ForwardAdd, ShadowCaster, Meta, ...), each with a
// generated vertex/fragment pair that fills `Input` from the mesh, calls
// `surf`, and runs the lighting model.
//
// ZEngine does the same thing here, at parse time. The alternative -- teach
// every backend about "surface programs" -- would mean duplicating this in
// the DX12 and Vulkan paths and in the editor's inspection UI. Desugaring
// once in the parser means every consumer keeps seeing ordinary
// `ShaderPass`es with ordinary entry points, and nothing downstream changes.
//
// This TU is deliberately free of any render-backend dependency (no DX12,
// no Vulkan, no engine headers beyond <string>/<vector>) so it can be unit
// tested as plain C++ -- same rule as ShaderLabSemanticRewrite and
// ShaderLabCacheManifest.
// ===========================================================================

#include <string>
#include <vector>

namespace ZEngine::ShaderLab
{

    // Lighting models after `#pragma surface <fn> <model>`.
    enum class SurfaceLightingModel
    {
        Standard,    // inout SurfaceOutputStandard
        Lambert,     // inout SurfaceOutput
        BlinnPhong,  // inout SurfaceOutput
        Unknown
    };

    // The parsed `#pragma surface ...` directive.
    struct SurfaceShaderDirective
    {
        bool valid = false;

        std::string function;     // "surf"
        std::string model_name;   // as written, e.g. "Standard"
        SurfaceLightingModel model = SurfaceLightingModel::Unknown;

        std::vector<std::string> options;  // lower-cased: addshadow, noforwardadd, ...

        // Populated only when `valid` is false AND a `#pragma surface` line
        // was present but unusable. An absent directive is not an error: a
        // plain CGPROGRAM simply has no surface shader in it.
        std::string error;

        bool HasOption(const std::string& option) const;
    };

    // One pass worth of generated shader. The parser turns each of these into
    // an ordinary ShaderPass.
    struct SurfacePass
    {
        std::string name;         // "ForwardBase"
        std::string light_mode;   // "ForwardBase" / "ShadowCaster"
        std::string vertex_entry;
        std::string fragment_entry;

        // The author's CGPROGRAM body verbatim, followed by the generated
        // vertex/fragment pair. `#pragma` lines are stripped later, by
        // ShaderLabHlslExtract, exactly like any other program block.
        std::string source_code;
    };

    struct SurfaceGenerateResult
    {
        bool ok = false;
        std::vector<SurfacePass> passes;

        // Non-fatal gaps: an `Input` member Unity would fill automatically but
        // ZEngine does not understand yet. Surfaced so the caller can report
        // them instead of letting a silently zero-filled member look like a
        // shader bug.
        std::vector<std::string> warnings;

        std::string error;
    };

    // Finds and parses the `#pragma surface` line in a CGPROGRAM body.
    // A body with no such line returns `valid == false` and an empty error.
    SurfaceShaderDirective ParseSurfaceDirective(const std::string& program_source);

    // Expands a surface CGPROGRAM body into passes. Always produces at least
    // the forward pass; `addshadow` adds a shadow-caster pass.
    SurfaceGenerateResult GenerateSurfacePasses(const std::string& program_source,
                                                const SurfaceShaderDirective& directive);

}  // namespace ZEngine::ShaderLab
