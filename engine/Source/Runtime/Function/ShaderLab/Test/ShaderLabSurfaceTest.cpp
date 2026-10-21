// =====================================================================
// ShaderLab surface-shader desugaring smoke-test
// ---------------------------------------------------------------------
// Pins down the two halves of ShaderLabSurface.cpp:
//
//   1. ParseSurfaceDirective() -- read `#pragma surface <fn> <model> [opts]`
//   2. GenerateSurfacePasses() -- expand one CGPROGRAM body into the passes
//      a Unity surface shader stands for
//
// Both are deliberately free of any render-backend dependency, so this
// target configures and runs on every configuration -- including
// ZENGINE_USE_VULKAN=OFF, where ShaderLabCompiler.cpp is excluded from
// ZRuntime and would otherwise be completely unverified.
//
// Every case below is one way desugaring goes wrong:
//
//   - a commented-out pragma being picked up                 -> [comment]
//   - Unity's implicit Lambert when no model is written      -> [default]
//   - the author naming the Input struct something else       -> [custom-input]
//   - tiling/offset uniform already declared by the shader   -> [st-declared]
//   - a pragma model that disagrees with the surf signature  -> [mismatch]
//   - an Input member ZEngine cannot generate yet            -> [unsupported]
//
// Opt in with:
//   cmake -B build -DZENGINE_BUILD_SHADERLAB_SURFACE_TEST=ON
//   cmake --build build --target ShaderLabSurfaceTest
//
// Exit code is the verdict: 0 = every check passed.
// =====================================================================

#include "Runtime/Function/ShaderLab/ShaderLabSurface.h"

#include <iostream>
#include <string>

using namespace ZEngine::ShaderLab;

namespace
{

int g_checks = 0;
int g_failures = 0;

void Check(bool ok, const std::string& label, const std::string& detail = {})
{
    ++g_checks;
    if (!ok)
    {
        ++g_failures;
    }
    std::cout << "  [" << (ok ? " OK " : "FAIL") << "] " << label << "\n";
    if (!ok && !detail.empty())
    {
        std::cout << "         " << detail << "\n";
    }
}

bool Contains(const std::string& haystack, const std::string& needle)
{
    return haystack.find(needle) != std::string::npos;
}

int CountOccurrences(const std::string& haystack, const std::string& needle)
{
    int count = 0;
    size_t at = haystack.find(needle);
    while (at != std::string::npos)
    {
        ++count;
        at = haystack.find(needle, at + needle.size());
    }
    return count;
}

// Unity's Standard Surface Shader template body, minus the ShaderLab wrapper.
const char* kStandardBody =
    "#pragma surface surf Standard fullforwardshadows\n"
    "#pragma target 3.0\n"
    "\n"
    "sampler2D _MainTex;\n"
    "\n"
    "struct Input\n"
    "{\n"
    "    float2 uv_MainTex;\n"
    "};\n"
    "\n"
    "half _Glossiness;\n"
    "half _Metallic;\n"
    "fixed4 _Color;\n"
    "\n"
    "void surf (Input IN, inout SurfaceOutputStandard o)\n"
    "{\n"
    "    fixed4 c = tex2D (_MainTex, IN.uv_MainTex) * _Color;\n"
    "    o.Albedo = c.rgb;\n"
    "    o.Metallic = _Metallic;\n"
    "    o.Smoothness = _Glossiness;\n"
    "    o.Alpha = c.a;\n"
    "}\n";

}  // namespace

int main()
{
    std::cout << "=== ShaderLab surface-shader desugaring smoke test ===\n";

    // ==================================================================
    // ParseSurfaceDirective
    // ==================================================================
    std::cout << "\n---- [pragma-basic] full directive ----\n";
    {
        const SurfaceShaderDirective d = ParseSurfaceDirective(kStandardBody);
        Check(d.valid, "directive is valid");
        Check(d.function == "surf", "function == surf", d.function);
        Check(d.model == SurfaceLightingModel::Standard && d.model_name == "Standard",
              "model == Standard", d.model_name);
        Check(d.HasOption("fullforwardshadows"), "carries fullforwardshadows");
        Check(!d.HasOption("addshadow"),
              "fullforwardshadows is NOT addshadow (it must not add a shadow pass)");
    }

    std::cout << "\n---- [pragma-default] no lighting model means Lambert ----\n";
    {
        const SurfaceShaderDirective d = ParseSurfaceDirective("#pragma surface surf\n");
        Check(d.valid, "directive is valid");
        Check(d.model == SurfaceLightingModel::Lambert && d.model_name == "Lambert",
              "defaults to Lambert", d.model_name);
    }

    std::cout << "\n---- [pragma-space] '# pragma surface' ----\n";
    {
        const SurfaceShaderDirective d = ParseSurfaceDirective("# pragma surface surf BlinnPhong\n");
        Check(d.valid, "directive is valid");
        Check(d.model == SurfaceLightingModel::BlinnPhong, "model == BlinnPhong", d.model_name);
        Check(d.function == "surf", "function == surf", d.function);
    }

    std::cout << "\n---- [pragma-custom] a non-built-in model name is Lighting<Name> ----\n";
    {
        const SurfaceShaderDirective d = ParseSurfaceDirective("#pragma surface surf Toon\n");
        Check(d.valid, "directive accepted");
        Check(d.model == SurfaceLightingModel::Custom, "classified as a custom model");
        Check(d.model_name == "Toon", "name kept verbatim, including case", d.model_name);
    }

    std::cout << "\n---- [pragma-bad-model] a model name that cannot be a function ----\n";
    {
        // `3D` cannot name a `Lighting3D()` function, so this is an error --
        // accepting it would only defer the failure to a confusing
        // "Lighting3D not found" later.
        const SurfaceShaderDirective d = ParseSurfaceDirective("#pragma surface surf 3D\n");
        Check(!d.valid, "directive rejected");
        Check(Contains(d.error, "3D"), "explains why", "(no error message)");
    }

    std::cout << "\n---- [pragma-no-fn] '#pragma surface' alone ----\n";
    {
        const SurfaceShaderDirective d = ParseSurfaceDirective("#pragma surface\n");
        Check(!d.valid, "directive rejected");
        Check(!d.error.empty(), "explains why", "(no error message)");
    }

    std::cout << "\n---- [pragma-absent] a plain CGPROGRAM is not an error ----\n";
    {
        const SurfaceShaderDirective d =
            ParseSurfaceDirective("#pragma vertex vert\n#pragma fragment frag\n");
        Check(!d.valid, "no surface directive");
        Check(d.error.empty(),
              "empty error: 'not a surface shader' must be distinguishable from 'bad directive'",
              d.error);
    }

    std::cout << "\n---- [pragma-comment] a commented-out pragma is not a directive ----\n";
    {
        const SurfaceShaderDirective d =
            ParseSurfaceDirective("// #pragma surface oldFn Standard\nvoid surf() {}\n");
        Check(!d.valid, "commented-out pragma ignored");
        Check(d.error.empty(), "and this is not an error either", d.error);
    }

    // ==================================================================
    // GenerateSurfacePasses
    // ==================================================================
    std::cout << "\n---- [gen-standard] Unity Standard template ----\n";
    {
        const SurfaceShaderDirective d = ParseSurfaceDirective(kStandardBody);
        const SurfaceGenerateResult r = GenerateSurfacePasses(kStandardBody, d);
        Check(r.ok, "generation succeeded", r.error);
        Check(r.passes.size() == 2, "two passes: ForwardBase + ForwardAdd (no addshadow)",
              std::to_string(r.passes.size()));
        if (r.passes.size() == 2)
        {
            Check(r.passes[0].light_mode == "ForwardBase", "first is ForwardBase",
                  r.passes[0].light_mode);
            Check(r.passes[1].light_mode == "ForwardAdd", "second is ForwardAdd",
                  r.passes[1].light_mode);
        }
        if (!r.passes.empty())
        {
            const SurfacePass& pass = r.passes[0];
            Check(pass.name == "ForwardBase" && pass.light_mode == "ForwardBase",
                  "pass is ForwardBase", pass.name);
            Check(pass.vertex_entry == "zSurf_vert" && pass.fragment_entry == "zSurf_frag",
                  "generated entry points", pass.vertex_entry + " / " + pass.fragment_entry);

            const std::string& src = pass.source_code;
            Check(Contains(src, "#include \"UnityCG.cginc\""), "UnityCG.cginc is pulled in");
            Check(src.find("#include") < src.find("sampler2D _MainTex;"),
                  "the include comes BEFORE the body (macros expand in source order)");
            Check(Contains(src, "float4 _MainTex_ST;"),
                  "declares the tiling/offset uniform for uv_MainTex");
            Check(Contains(src, "o.uv_MainTex = v.texcoord * _MainTex_ST.xy"),
                  "vertex shader fills uv_MainTex through _MainTex_ST");
            Check(Contains(src, "surf (zSurfIN, o);"), "fragment shader calls surf");
            Check(Contains(src, "o.Metallic = 0.0;"), "zero-initialises SurfaceOutputStandard");
            Check(Contains(src, "o.Smoothness = 0.0;"), "...including Smoothness");
            Check(Contains(src, "#pragma surface"),
                  "the body is kept verbatim -- including the #pragma surface line, "
                  "which ShaderLabHlslExtract strips later like any other pragma");
            Check(!Contains(src, "zWorldTangent"),
                  "no tangent varying: the template never writes o.Normal");
            Check(Contains(src, "zColor4.a = 1.0;"),
                  "an opaque surface shader forces alpha to 1.0 (UNITY_OPAQUE_ALPHA) -- "
                  "the template writes o.Alpha and Unity ignores it");
        }
        // `fullforwardshadows` only affects shadows in the add pass, which
        // ZEngine does not have. It must be reported, not silently dropped.
        Check(r.warnings.size() == 1 && Contains(r.warnings[0], "fullforwardshadows"),
              "fullforwardshadows is reported as unimplemented, not silently ignored",
              r.warnings.empty() ? "(no warning at all)" : r.warnings[0]);
    }

    std::cout << "\n---- [gen-include-once] a body that already includes UnityCG ----\n";
    {
        const std::string body =
            std::string("#include \"UnityCG.cginc\"\n") + kStandardBody;
        const SurfaceShaderDirective d = ParseSurfaceDirective(body);
        const SurfaceGenerateResult r = GenerateSurfacePasses(body, d);
        Check(r.ok, "generation succeeded", r.error);
        if (!r.passes.empty())
        {
            Check(CountOccurrences(r.passes[0].source_code, "UnityCG.cginc") == 1,
                  "UnityCG.cginc appears exactly once",
                  std::to_string(CountOccurrences(r.passes[0].source_code, "UnityCG.cginc")));
        }
    }

    std::cout << "\n---- [gen-st-declared] shader declares _MainTex_ST itself ----\n";
    {
        const std::string body = std::string("float4 _MainTex_ST;\n") + kStandardBody;
        const SurfaceShaderDirective d = ParseSurfaceDirective(body);
        const SurfaceGenerateResult r = GenerateSurfacePasses(body, d);
        Check(r.ok, "generation succeeded", r.error);
        if (!r.passes.empty())
        {
            // One from the author, none from us: a second one would be a
            // redefinition error.
            Check(CountOccurrences(r.passes[0].source_code, "float4 _MainTex_ST;") == 1,
                  "_MainTex_ST declared once",
                  std::to_string(CountOccurrences(r.passes[0].source_code, "float4 _MainTex_ST;")));
        }
    }

    std::cout << "\n---- [gen-custom-input] the Input struct is not named Input ----\n";
    {
        std::string body = kStandardBody;
        const size_t at = body.find("struct Input");
        Check(at != std::string::npos, "fixture has a struct to rename");
        body.replace(at, std::string("struct Input").size(), "struct MySurfData");
        body.replace(body.find("Input IN"), std::string("Input IN").size(), "MySurfData IN");

        const SurfaceShaderDirective d = ParseSurfaceDirective(body);
        const SurfaceGenerateResult r = GenerateSurfacePasses(body, d);
        Check(r.ok, "generation succeeded", r.error);
        if (!r.passes.empty())
        {
            Check(Contains(r.passes[0].source_code, "MySurfData zSurfIN;"),
                  "reads the input type from the surf signature, not from the name 'Input'");
        }
    }

    std::cout << "\n---- [gen-addshadow] addshadow adds a shadow caster ----\n";
    {
        const std::string body =
            "#pragma surface surf Lambert addshadow\n"
            "struct Input { float2 uv_MainTex; };\n"
            "void surf (Input IN, inout SurfaceOutput o) { o.Albedo = 1; }\n";
        const SurfaceShaderDirective d = ParseSurfaceDirective(body);
        const SurfaceGenerateResult r = GenerateSurfacePasses(body, d);
        Check(r.ok, "generation succeeded", r.error);
        Check(r.passes.size() == 3, "three passes (base, add, shadow)",
              std::to_string(r.passes.size()));
        if (r.passes.size() == 3)
        {
            Check(r.passes[0].light_mode == "ForwardBase", "first is ForwardBase",
                  r.passes[0].light_mode);
            Check(r.passes[1].light_mode == "ForwardAdd", "second is ForwardAdd",
                  r.passes[1].light_mode);
            Check(r.passes[2].light_mode == "ShadowCaster", "third is ShadowCaster",
                  r.passes[2].light_mode);
            Check(r.passes[2].fragment_entry != r.passes[0].fragment_entry,
                  "and has its own fragment entry");
        }
    }

    std::cout << "\n---- [gen-noforwardadd] noforwardadd drops the add pass ----\n";
    {
        const std::string body =
            "#pragma surface surf Lambert noforwardadd\n"
            "struct Input { float2 uv_MainTex; };\n"
            "void surf (Input IN, inout SurfaceOutput o) { o.Albedo = 1; }\n";
        const SurfaceShaderDirective d = ParseSurfaceDirective(body);
        const SurfaceGenerateResult r = GenerateSurfacePasses(body, d);
        Check(r.ok, "generation succeeded", r.error);
        Check(r.passes.size() == 1, "one pass", std::to_string(r.passes.size()));
        Check(r.warnings.empty(), "noforwardadd is implemented, not warned about",
              r.warnings.empty() ? "" : r.warnings[0]);
    }

    std::cout << "\n---- [gen-additive] ForwardAdd is additive and does not write depth ----\n";
    {
        const std::string body =
            "#pragma surface surf Lambert\n"
            "struct Input { float2 uv_MainTex; };\n"
            "void surf (Input IN, inout SurfaceOutput o) { o.Albedo = 1; }\n";
        const SurfaceShaderDirective d = ParseSurfaceDirective(body);
        const SurfaceGenerateResult r = GenerateSurfacePasses(body, d);
        Check(r.ok, "generation succeeded", r.error);
        if (r.passes.size() >= 2)
        {
            const SurfacePass& add = r.passes[1];
            Check(add.render_state.has_blend &&
                      add.render_state.blend_src == SurfaceBlendFactor::One &&
                      add.render_state.blend_dst == SurfaceBlendFactor::One,
                  "ForwardAdd blends One/One");
            Check(add.render_state.has_zwrite && !add.render_state.zwrite,
                  "ForwardAdd does not write depth");
            Check(r.passes[0].render_state.has_zwrite == false,
                  "ForwardBase does not force zwrite -- a SubShader-level 'ZWrite Off' "
                  "must survive");
            Check(Contains(add.source_code, "_LightColor1"),
                  "and is lit by the second light, not the base one");
            Check(r.passes[0].render_state.has_blend == false,
                  "ForwardBase stays opaque");
        }
    }

    std::cout << "\n---- [gen-alpha] `alpha` turns on alpha blending ----\n";
    {
        const std::string body =
            "#pragma surface surf Lambert alpha\n"
            "struct Input { float2 uv_MainTex; };\n"
            "void surf (Input IN, inout SurfaceOutput o) { o.Albedo = 1; o.Alpha = 0.5; }\n";
        const SurfaceShaderDirective d = ParseSurfaceDirective(body);
        const SurfaceGenerateResult r = GenerateSurfacePasses(body, d);
        Check(r.ok, "generation succeeded", r.error);
        if (!r.passes.empty())
        {
            Check(r.passes[0].render_state.has_blend &&
                      r.passes[0].render_state.blend_src == SurfaceBlendFactor::SrcAlpha &&
                      r.passes[0].render_state.blend_dst == SurfaceBlendFactor::OneMinusSrcAlpha,
                  "SrcAlpha / OneMinusSrcAlpha");
            Check(!Contains(r.passes[0].source_code, "zColor4.a = 1.0;"),
                  "and o.Alpha is kept (no UNITY_OPAQUE_ALPHA clamp)");
        }
    }

    std::cout << "\n---- [gen-lambert] SurfaceOutput fields ----\n";
    {
        const std::string body =
            "#pragma surface surf Lambert\n"
            "struct Input { float2 uv_MainTex; float3 worldPos; float3 viewDir; "
            "float4 screenPos; };\n"
            "void surf (Input IN, inout SurfaceOutput o) { o.Albedo = 1; }\n";
        const SurfaceShaderDirective d = ParseSurfaceDirective(body);
        const SurfaceGenerateResult r = GenerateSurfacePasses(body, d);
        Check(r.ok, "generation succeeded", r.error);
        if (!r.passes.empty())
        {
            const std::string& src = r.passes[0].source_code;
            Check(Contains(src, "o.Specular = 0.0;") && Contains(src, "o.Gloss = 0.0;"),
                  "zero-initialises the legacy SurfaceOutput fields");
            Check(!Contains(src, "o.Metallic"), "and does not touch Metallic");
            Check(Contains(src, "zSurfIN.worldPos = IN.zWorldPos;"), "fills worldPos");
            Check(Contains(src, "zSurfIN.viewDir = normalize(_WorldSpaceCameraPos"),
                  "fills viewDir");
            Check(Contains(src, "zSurfIN.screenPos = IN.zScreenPos;"), "fills screenPos");
        }
    }

    std::cout << "\n---- [gen-uv2] uv2_ uses TEXCOORD1 ----\n";
    {
        const std::string body =
            "#pragma surface surf Standard\n"
            "struct Input { float2 uv2_DetailTex; };\n"
            "void surf (Input IN, inout SurfaceOutputStandard o) { o.Albedo = 1; }\n";
        const SurfaceShaderDirective d = ParseSurfaceDirective(body);
        const SurfaceGenerateResult r = GenerateSurfacePasses(body, d);
        Check(r.ok, "generation succeeded", r.error);
        if (!r.passes.empty())
        {
            const std::string& src = r.passes[0].source_code;
            Check(Contains(src, "float4 _DetailTex_ST;"), "declares _DetailTex_ST");
            Check(Contains(src, "o.uv2_DetailTex = v.texcoord1"), "reads texcoord1");
        }
    }

    std::cout << "\n---- [gen-unsupported] an Input member ZEngine cannot fill ----\n";
    {
        const std::string body =
            "#pragma surface surf Standard\n"
            "struct Input { float3 worldRefl; };\n"
            "void surf (Input IN, inout SurfaceOutputStandard o) { o.Albedo = 1; }\n";
        const SurfaceShaderDirective d = ParseSurfaceDirective(body);
        const SurfaceGenerateResult r = GenerateSurfacePasses(body, d);
        Check(r.ok, "still generates (a gap in Input is not a hard failure)", r.error);
        Check(r.warnings.size() == 1, "one warning", std::to_string(r.warnings.size()));
        if (!r.warnings.empty())
        {
            Check(Contains(r.warnings[0], "worldRefl"), "names the member", r.warnings[0]);
        }
    }

    std::cout << "\n---- [gen-mismatch] Standard pragma, legacy output struct ----\n";
    {
        const std::string body =
            "#pragma surface surf Standard\n"
            "struct Input { float2 uv_MainTex; };\n"
            "void surf (Input IN, inout SurfaceOutput o) { o.Albedo = 1; }\n";
        const SurfaceShaderDirective d = ParseSurfaceDirective(body);
        const SurfaceGenerateResult r = GenerateSurfacePasses(body, d);
        Check(!r.ok, "rejected");
        Check(Contains(r.error, "SurfaceOutputStandard"),
              "says which struct the model expects", r.error);
    }

    std::cout << "\n---- [gen-unknown-out] an output struct that is neither ----\n";
    {
        const std::string body =
            "#pragma surface surf Standard\n"
            "struct Input { float2 uv_MainTex; };\n"
            "void surf (Input IN, inout MyOwnOutput o) { }\n";
        const SurfaceShaderDirective d = ParseSurfaceDirective(body);
        const SurfaceGenerateResult r = GenerateSurfacePasses(body, d);
        Check(!r.ok, "rejected");
        Check(Contains(r.error, "MyOwnOutput"), "names the offending type", r.error);
    }

    std::cout << "\n---- [gen-missing-fn] surf is not defined ----\n";
    {
        const std::string body =
            "#pragma surface surf Standard\n"
            "struct Input { float2 uv_MainTex; };\n"
            "void somethingElse (Input IN, inout SurfaceOutputStandard o) { }\n";
        const SurfaceShaderDirective d = ParseSurfaceDirective(body);
        const SurfaceGenerateResult r = GenerateSurfacePasses(body, d);
        Check(!r.ok, "rejected");
        Check(Contains(r.error, "surf"), "names the missing function", r.error);
    }

    std::cout << "\n---- [gen-no-directive] generating without a directive ----\n";
    {
        const SurfaceGenerateResult r = GenerateSurfacePasses(kStandardBody,
                                                              SurfaceShaderDirective{});
        Check(!r.ok, "rejected");
        Check(!r.error.empty(), "explains why", r.error);
    }

    std::cout << "\n---- [gen-tangent] o.Normal brings in a tangent frame ----\n";
    {
        const std::string body =
            "#pragma surface surf Standard\n"
            "sampler2D _BumpMap;\n"
            "struct Input { float2 uv_BumpMap; };\n"
            "void surf (Input IN, inout SurfaceOutputStandard o)\n"
            "{\n"
            "    o.Normal = UnpackNormal(tex2D(_BumpMap, IN.uv_BumpMap));\n"
            "    o.Albedo = 1;\n"
            "}\n";
        const SurfaceShaderDirective d = ParseSurfaceDirective(body);
        const SurfaceGenerateResult r = GenerateSurfacePasses(body, d);
        Check(r.ok, "generation succeeded", r.error);
        if (!r.passes.empty())
        {
            const std::string& src = r.passes[0].source_code;
            Check(Contains(src, "float4 zWorldTangent : TEXCOORD"),
                  "carries a world-space tangent varying (with handedness in .w)");
            Check(Contains(src, "o.zWorldTangent = float4(UnityObjectToWorldDir(v.tangent.xyz)"),
                  "the vertex shader transforms it to world space");
            Check(Contains(src, "cross(zVertexNormal, zT)"), "builds the bitangent");
            Check(Contains(src, "zN = normalize(o.Normal.x * zT"),
                  "and rotates o.Normal out of tangent space");
            Check(Contains(src, "dot(zT, zT) < 1e-8"),
                  "guards the degenerate/untangent case instead of producing NaN");
        }
    }

    std::cout << "\n---- [gen-notangent] no o.Normal, no tangent varying ----\n";
    {
        const std::string body =
            "#pragma surface surf Standard\n"
            "struct Input { float2 uv_MainTex; };\n"
            "void surf (Input IN, inout SurfaceOutputStandard o) { o.Albedo = 1; }\n";
        const SurfaceShaderDirective d = ParseSurfaceDirective(body);
        const SurfaceGenerateResult r = GenerateSurfacePasses(body, d);
        Check(r.ok, "generation succeeded", r.error);
        if (!r.passes.empty())
        {
            Check(!Contains(r.passes[0].source_code, "zWorldTangent"),
                  "an interpolator is not paid for when nothing writes o.Normal");
        }
    }

    std::cout << "\n---- [gen-custom-light] Lighting<Name> (3-arg form) ----\n";
    {
        const std::string body =
            "#pragma surface surf WrapLambert\n"
            "struct Input { float2 uv_MainTex; };\n"
            "void surf (Input IN, inout SurfaceOutput o) { o.Albedo = 1; }\n"
            "half4 LightingWrapLambert (SurfaceOutput s, half3 lightDir, half atten)\n"
            "{\n"
            "    return half4(s.Albedo * _LightColor0.rgb * atten, s.Alpha);\n"
            "}\n";
        const SurfaceShaderDirective d = ParseSurfaceDirective(body);
        Check(d.valid && d.model == SurfaceLightingModel::Custom,
              "an unknown model name is a custom model, not an error", d.error);
        const SurfaceGenerateResult r = GenerateSurfacePasses(body, d);
        Check(r.ok, "generation succeeded", r.error);
        if (!r.passes.empty())
        {
            const std::string& src = r.passes[0].source_code;
            Check(Contains(src, "LightingWrapLambert (o, zL, 1.0)"),
                  "calls the author's lighting function with (s, lightDir, atten)");
            Check(Contains(src, "zColor += o.Emission;"), "and still adds Emission");
            Check(!Contains(src, "const float3 zH"),
                  "no built-in BRDF is emitted alongside it");
        }
    }

    std::cout << "\n---- [gen-custom-light-view] Lighting<Name> (4-arg form) ----\n";
    {
        const std::string body =
            "#pragma surface surf RimLight\n"
            "struct Input { float2 uv_MainTex; };\n"
            "void surf (Input IN, inout SurfaceOutput o) { o.Albedo = 1; }\n"
            "half4 LightingRimLight (SurfaceOutput s, half3 lightDir, half3 viewDir, half atten)\n"
            "{\n"
            "    return half4(s.Albedo * atten, s.Alpha);\n"
            "}\n";
        const SurfaceShaderDirective d = ParseSurfaceDirective(body);
        const SurfaceGenerateResult r = GenerateSurfacePasses(body, d);
        Check(r.ok, "generation succeeded", r.error);
        if (!r.passes.empty())
        {
            Check(Contains(r.passes[0].source_code, "LightingRimLight (o, zL, zV, 1.0)"),
                  "passes the view direction too");
        }
    }

    std::cout << "\n---- [gen-custom-missing] the lighting function does not exist ----\n";
    {
        const std::string body =
            "#pragma surface surf WrapLambert\n"
            "struct Input { float2 uv_MainTex; };\n"
            "void surf (Input IN, inout SurfaceOutput o) { o.Albedo = 1; }\n";
        const SurfaceShaderDirective d = ParseSurfaceDirective(body);
        const SurfaceGenerateResult r = GenerateSurfacePasses(body, d);
        Check(!r.ok, "rejected");
        Check(Contains(r.error, "LightingWrapLambert"),
              "names the function that has to be written", r.error);
    }

    std::cout << "\n---- [gen-custom-arity] the UnityGI form is refused, not mis-called ----\n";
    {
        const std::string body =
            "#pragma surface surf GIModel\n"
            "struct Input { float2 uv_MainTex; };\n"
            "void surf (Input IN, inout SurfaceOutput o) { o.Albedo = 1; }\n"
            "half4 LightingGIModel (SurfaceOutput s, UnityGI gi) { return 0; }\n";
        const SurfaceShaderDirective d = ParseSurfaceDirective(body);
        const SurfaceGenerateResult r = GenerateSurfacePasses(body, d);
        Check(!r.ok, "rejected");
        Check(Contains(r.error, "2 parameters") || Contains(r.error, "UnityGI"),
              "explains which two forms ARE supported", r.error);
    }

    std::cout << "\n---- [gen-custom-mismatch] LightingX disagrees with surf ----\n";
    {
        const std::string body =
            "#pragma surface surf WrapLambert\n"
            "struct Input { float2 uv_MainTex; };\n"
            "void surf (Input IN, inout SurfaceOutput o) { o.Albedo = 1; }\n"
            "half4 LightingWrapLambert (SurfaceOutputStandard s, half3 lightDir, half atten)"
            " { return 0; }\n";
        const SurfaceShaderDirective d = ParseSurfaceDirective(body);
        const SurfaceGenerateResult r = GenerateSurfacePasses(body, d);
        Check(!r.ok, "rejected");
        Check(Contains(r.error, "SurfaceOutputStandard"), "names the mismatch", r.error);
    }

    std::cout << "\n---- [gen-vertex] vertex:<fn> modifier ----\n";
    {
        const std::string body =
            "#pragma surface surf Lambert vertex:myVert\n"
            "struct Input { float2 uv_MainTex; };\n"
            "void myVert (inout appdata_full v) { v.vertex.xyz += v.normal * 0.01; }\n"
            "void surf (Input IN, inout SurfaceOutput o) { o.Albedo = 1; }\n";
        const SurfaceShaderDirective d = ParseSurfaceDirective(body);
        // A value-bearing option must keep the case the author wrote:
        // lower-casing it would make the generated code call a function that
        // does not exist.
        std::string vertex_fn;
        Check(d.OptionValue("vertex:", vertex_fn) && vertex_fn == "myVert",
              "`vertex:` preserves the original case", vertex_fn);
        const SurfaceGenerateResult r = GenerateSurfacePasses(body, d);
        Check(r.ok, "generation succeeded", r.error);
        if (!r.passes.empty())
        {
            const std::string& src = r.passes[0].source_code;
            Check(Contains(src, "appdata_full zApp;"),
                  "the vertex shader works on an appdata_full copy -- that is the type "
                  "Unity hands a vertex modifier");
            Check(Contains(src, "myVert (zApp);"), "and calls the modifier on it");
            Check(Contains(src, "UnityObjectToClipPos(zApp.vertex)"),
                  "using the MODIFIED vertex for the clip position");
            Check(Contains(src, "zApp.texcoord.xy"), "and the modified uv set");
        }
    }

    std::cout << "\n---- [gen-vertex-missing] vertex: names an undefined function ----\n";
    {
        const std::string body =
            "#pragma surface surf Lambert vertex:nope\n"
            "struct Input { float2 uv_MainTex; };\n"
            "void surf (Input IN, inout SurfaceOutput o) { o.Albedo = 1; }\n";
        const SurfaceShaderDirective d = ParseSurfaceDirective(body);
        const SurfaceGenerateResult r = GenerateSurfacePasses(body, d);
        Check(!r.ok, "rejected");
        Check(Contains(r.error, "nope"), "names the missing function", r.error);
    }

    std::cout << "\n---- [gen-finalcolor] finalcolor:<fn> ----\n";
    {
        const std::string body =
            "#pragma surface surf Lambert finalcolor:myFinal\n"
            "struct Input { float2 uv_MainTex; };\n"
            "void myFinal (Input IN, SurfaceOutput o, inout fixed4 color) { color *= 0.5; }\n"
            "void surf (Input IN, inout SurfaceOutput o) { o.Albedo = 1; }\n";
        const SurfaceShaderDirective d = ParseSurfaceDirective(body);
        const SurfaceGenerateResult r = GenerateSurfacePasses(body, d);
        Check(r.ok, "generation succeeded", r.error);
        if (!r.passes.empty())
        {
            Check(Contains(r.passes[0].source_code, "myFinal (zSurfIN, o, zColor4);"),
                  "calls the author's final-colour hook with the lit colour");
        }
    }

    std::cout << "\n---- [gen-alphatest] alphatest:<var> adds a clip ----\n";
    {
        const std::string body =
            "#pragma surface surf Lambert alphatest:_Cutoff addshadow\n"
            "struct Input { float2 uv_MainTex; };\n"
            "void surf (Input IN, inout SurfaceOutput o) { o.Albedo = 1; o.Alpha = 0.5; }\n";
        const SurfaceShaderDirective d = ParseSurfaceDirective(body);
        const SurfaceGenerateResult r = GenerateSurfacePasses(body, d);
        Check(r.ok, "generation succeeded", r.error);
        if (!r.passes.empty())
        {
            const std::string& src = r.passes[0].source_code;
            Check(Contains(src, "float _Cutoff;"),
                  "declares the cutoff variable the body never declared");
            Check(Contains(src, "clip(o.Alpha - _Cutoff);"), "and clips against it");
        }
        if (r.passes.size() == 3)
        {
            Check(Contains(r.passes[2].source_code, "clip(o.Alpha - _Cutoff);"),
                  "the shadow caster clips too -- otherwise cut-out shadows leak");
        }
    }

    std::cout << "\n---- [gen-unknown-option] an unrecognised option is reported ----\n";
    {
        const std::string body =
            "#pragma surface surf Lambert completelyMadeUp\n"
            "struct Input { float2 uv_MainTex; };\n"
            "void surf (Input IN, inout SurfaceOutput o) { o.Albedo = 1; }\n";
        const SurfaceShaderDirective d = ParseSurfaceDirective(body);
        const SurfaceGenerateResult r = GenerateSurfacePasses(body, d);
        Check(r.ok, "still generates", r.error);
        Check(r.warnings.size() == 1 && Contains(r.warnings[0], "completelyMadeUp"),
              "warns instead of silently ignoring it",
              r.warnings.empty() ? "(no warning)" : r.warnings[0]);
    }

    std::cout << "\n---- [gen-instancing] instancing macros are flagged ----\n";
    {
        const std::string body =
            "#pragma surface surf Standard\n"
            "UNITY_INSTANCING_BUFFER_START(Props)\n"
            "    UNITY_DEFINE_INSTANCED_PROP(float4, _Color)\n"
            "UNITY_INSTANCING_BUFFER_END(Props)\n"
            "struct Input { float2 uv_MainTex; };\n"
            "void surf (Input IN, inout SurfaceOutputStandard o)\n"
            "{\n"
            "    o.Albedo = UNITY_ACCESS_INSTANCED_PROP(Props, _Color).rgb;\n"
            "}\n";
        const SurfaceShaderDirective d = ParseSurfaceDirective(body);
        const SurfaceGenerateResult r = GenerateSurfacePasses(body, d);
        Check(r.ok, "still generates (a missing feature is not a build error)", r.error);
        bool flagged = false;
        for (const std::string& warning : r.warnings)
        {
            if (Contains(warning, "instancing"))
            {
                flagged = true;
            }
        }
        Check(flagged, "warns that every instance renders identically");
    }

    // ==================================================================
    std::cout << "\n";
    if (g_failures == 0)
    {
        std::cout << "=== " << g_checks << "/" << g_checks << " checks passed ===\n";
        return 0;
    }
    std::cout << "=== " << (g_checks - g_failures) << "/" << g_checks
              << " checks passed, " << g_failures << " FAILED ===\n";
    return 1;
}
