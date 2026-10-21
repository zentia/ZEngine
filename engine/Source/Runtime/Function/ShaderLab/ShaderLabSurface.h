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
        Custom,      // not one of Unity's: the shader supplies Lighting<Name>()

        // Only ever the value of a default-constructed directive. A model
        // that is neither built-in nor a usable identifier is an error, not
        // `Unknown`: a `Lighting<Name>` that can never be called is worse
        // than refusing to compile.
        Unknown
    };

    // The parsed `#pragma surface ...` directive.
    struct SurfaceShaderDirective
    {
        bool valid = false;

        std::string function;     // "surf"
        std::string model_name;   // as written, e.g. "Standard"
        SurfaceLightingModel model = SurfaceLightingModel::Unknown;

        // `options` is lower-cased so HasOption() can be case-insensitive --
        // Unity writes `addshadow` and `fullforwardshadows` in whichever case
        // the author felt like. Value-bearing options (`vertex:MyVert`,
        // `alphatest:_Cutoff`) name a HLSL identifier, so those go into
        // `raw_options` as well, untouched: lower-casing them would make the
        // generated code call a function that does not exist.
        std::vector<std::string> options;
        std::vector<std::string> raw_options;

        // Populated only when `valid` is false AND a `#pragma surface` line
        // was present but unusable. An absent directive is not an error: a
        // plain CGPROGRAM simply has no surface shader in it.
        std::string error;

        bool HasOption(const std::string& option) const;

        // Reads a `name:value` option. `prefix` includes the colon and is
        // matched case-insensitively; the returned value keeps the case the
        // author wrote. Returns false when the option is absent.
        bool OptionValue(const std::string& prefix, std::string& value_out) const;
    };

    // Blend factors a surface option can ask for. A small local enum rather
    // than ShaderLab's BlendFunc: this TU includes nothing but <string> and
    // <vector> (see the header comment), and a surface shader only ever needs
    // these four.
    enum class SurfaceBlendFactor
    {
        One,
        Zero,
        SrcAlpha,
        OneMinusSrcAlpha
    };

    // Render-state overrides implied by surface options. `alpha` turns on
    // alpha blending; ForwardAdd is additive and must not write depth. The
    // parser merges this over the states inherited from the SubShader.
    struct SurfaceRenderStateOverride
    {
        bool has_blend = false;
        SurfaceBlendFactor blend_src = SurfaceBlendFactor::One;
        SurfaceBlendFactor blend_dst = SurfaceBlendFactor::One;

        // Separate flag rather than a plain `zwrite`: only ForwardAdd forces
        // depth writes off. Overwriting zwrite unconditionally would stomp a
        // `ZWrite Off` the author wrote at SubShader level.
        bool has_zwrite = false;
        bool zwrite = true;
    };

    // One pass worth of generated shader. The parser turns each of these into
    // an ordinary ShaderPass.
    struct SurfacePass
    {
        std::string name;         // "ForwardBase"
        std::string light_mode;   // "ForwardBase" / "ForwardAdd" / "ShadowCaster"
        std::string vertex_entry;
        std::string fragment_entry;

        SurfaceRenderStateOverride render_state;

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

    // Expands a surface CGPROGRAM body into passes, in Unity's own order:
    // ForwardBase, ForwardAdd (unless `noforwardadd`), ShadowCaster (only
    // with `addshadow`). ZEngine has no Meta/Deferred path, so those are not
    // generated and `nometa` / `exclude_path:deferred` are satisfied by doing
    // nothing.
    SurfaceGenerateResult GenerateSurfacePasses(const std::string& program_source,
                                                const SurfaceShaderDirective& directive);

}  // namespace ZEngine::ShaderLab
