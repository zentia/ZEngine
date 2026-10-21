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

    std::cout << "\n---- [pragma-unknown] an unsupported model is an error, not silence ----\n";
    {
        const SurfaceShaderDirective d = ParseSurfaceDirective("#pragma surface surf Toon\n");
        Check(!d.valid, "directive rejected");
        Check(!d.error.empty(), "explains why", "(no error message)");
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
        Check(r.passes.size() == 1, "one pass (no addshadow)",
              std::to_string(r.passes.size()));
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
            Check(r.warnings.empty(), "no warnings for a fully supported Input");
        }
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
        Check(r.passes.size() == 2, "two passes", std::to_string(r.passes.size()));
        if (r.passes.size() == 2)
        {
            Check(r.passes[0].light_mode == "ForwardBase", "first is ForwardBase",
                  r.passes[0].light_mode);
            Check(r.passes[1].light_mode == "ShadowCaster", "second is ShadowCaster",
                  r.passes[1].light_mode);
            Check(r.passes[1].fragment_entry != r.passes[0].fragment_entry,
                  "and has its own fragment entry");
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
