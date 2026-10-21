// =====================================================================
// ShaderLab legacy output-semantic rewrite smoke-test
// ---------------------------------------------------------------------
// Pins down RewriteLegacyOutputSemantics() -- the Cg -> Shader-Model-4+
// fix that lets a byte-identical Unity shader compile on a modern target.
//
// DXC rejects the real Spine shader with
//
//     error: invalid semantic 'COLOR' for ps 6.0
//
// and the obvious fix is wrong. `#define COLOR SV_Target` DOES expand in
// semantic position (unity_compat/diag_semantic_macro.shader proves it),
// but `COLOR` is also a legal INPUT semantic and Spine uses it that way:
//
//     struct VertexInput  { float4 vertexColor : COLOR; };
//     struct VertexOutput { float4 vertexColor : COLOR; };
//     float4 frag (VertexOutput i) : COLOR { ... }
//
// A global macro rewrites all three and turns two legal input semantics
// into two illegal output ones. So the rewrite must touch ONLY the
// `: NAME` between the entry function's closing paren and its `{`.
//
// Every case below is one way that targeting can go wrong:
//
//   - struct members / parameter semantics carrying COLOR    -> [input]
//   - a call to the entry function, not its definition      -> [call]
//   - the same text inside a comment or a string            -> [comment]
//                                                              [string]
//   - a member call `obj.frag(...)`                         -> [member]
//   - a longer identifier that merely contains the name     -> [boundary]
//   - a vertex stage, where COLOR is legal as an input      -> [vertex]
//
// Opt in with:
//   cmake -B build -DZENGINE_BUILD_SHADERLAB_SEMANTIC_TEST=ON
//   cmake --build build --target ShaderLabSemanticRewriteTest
//
// Exit code is the verdict: 0 = every check passed.
// =====================================================================

#include "Runtime/Function/ShaderLab/ShaderLabSemanticRewrite.h"

#include <cstring>
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

SemanticRewriteResult Rewrite(const std::string& hlsl,
                              const std::string& entry,
                              ShaderStage stage = ShaderStage::Fragment)
{
    return RewriteLegacyOutputSemantics(hlsl, entry, stage);
}

// The rewrite must be surgical: `expected` is the exact result string.
void Expect(const std::string& hlsl,
            const std::string& entry,
            ShaderStage stage,
            const std::string& expected,
            int expected_rewrites,
            const std::string& label)
{
    const SemanticRewriteResult r = Rewrite(hlsl, entry, stage);
    Check(r.code == expected && r.rewrites == expected_rewrites,
          label + " (expected " + std::to_string(expected_rewrites) + " rewrite(s), got " +
              std::to_string(r.rewrites) + ")",
          "--- expected ---\n" + expected + "\n--- got ---\n" + r.code);
}

// Shorthand: the source must come back byte-identical.
void ExpectUntouched(const std::string& hlsl,
                     const std::string& entry,
                     ShaderStage stage,
                     const std::string& label)
{
    Expect(hlsl, entry, stage, hlsl, 0, label);
}

}  // namespace

int main()
{
    std::cout << "=== ShaderLab legacy output-semantic rewrite smoke test ===\n";

    // ------------------------------------------------------------------
    // [basic] The Spine shape: `float4 frag (VertexOutput i) : COLOR {`
    // ------------------------------------------------------------------
    std::cout << "\n---- [basic] entry return semantic only ----\n";
    {
        const std::string src =
            "struct VertexInput {\n"
            "    float4 vertex : POSITION;\n"
            "    float4 vertexColor : COLOR;\n"
            "};\n"
            "\n"
            "struct VertexOutput {\n"
            "    float4 pos : SV_POSITION;\n"
            "    float4 vertexColor : COLOR;\n"
            "};\n"
            "\n"
            "VertexOutput vert (VertexInput v) {\n"
            "    VertexOutput o;\n"
            "    o.pos = v.vertex;\n"
            "    o.vertexColor = v.vertexColor;\n"
            "    return o;\n"
            "}\n"
            "\n"
            "float4 frag (VertexOutput i) : COLOR {\n"
            "    return i.vertexColor;\n"
            "}\n";

        std::string expected = src;
        const size_t at = expected.find(": COLOR {\n    return i.vertexColor;");
        expected.replace(at, std::strlen(": COLOR"), ": SV_Target");

        Expect(src, "frag", ShaderStage::Fragment, expected, 1,
               "PS entry ': COLOR' -> ': SV_Target'");

        // The two struct members and the VS entry must be untouched. Count
        // them explicitly: they are the reason a global #define cannot work.
        const SemanticRewriteResult r = Rewrite(src, "frag", ShaderStage::Fragment);
        Check(r.code.find("float4 vertexColor : COLOR;") != std::string::npos,
              "VertexInput's input semantic ': COLOR' survives",
              r.code);
        Check(r.code.find("    float4 vertexColor : COLOR;\n}") != std::string::npos,
              "VertexOutput's interpolator ': COLOR' survives",
              r.code);
        Check(r.code.find("VertexOutput vert (VertexInput v) {") != std::string::npos,
              "the vertex entry is untouched");
    }

    // ------------------------------------------------------------------
    // [input] Parameter semantics are inside the parens -- never seen.
    // ------------------------------------------------------------------
    std::cout << "\n---- [input] parameter semantics are not output semantics ----\n";
    {
        const std::string src = "float4 frag (float4 c : COLOR) : COLOR { return c; }\n";
        const std::string expected = "float4 frag (float4 c : COLOR) : SV_Target { return c; }\n";
        Expect(src, "frag", ShaderStage::Fragment, expected, 1,
               "only the post-paren semantic is rewritten");
    }

    // ------------------------------------------------------------------
    // [already] SV_Target is left alone.
    // ------------------------------------------------------------------
    std::cout << "\n---- [already] modern semantics are untouched ----\n";
    {
        ExpectUntouched("float4 frag (float4 i : SV_POSITION) : SV_Target { return i; }\n",
                        "frag",
                        ShaderStage::Fragment,
                        "': SV_Target' unchanged");
        ExpectUntouched("float4 frag (float4 i : SV_POSITION) : SV_Target1 { return i; }\n",
                        "frag",
                        ShaderStage::Fragment,
                        "': SV_Target1' unchanged");
    }

    // ------------------------------------------------------------------
    // [indexed] COLOR0..COLOR7 -> SV_Target0..SV_Target7.
    // ------------------------------------------------------------------
    std::cout << "\n---- [indexed] COLOR0..COLOR7 map to SV_Target0..SV_Target7 ----\n";
    {
        Expect("float4 frag (float4 i : SV_POSITION) : COLOR1 { return i; }\n",
               "frag",
               ShaderStage::Fragment,
               "float4 frag (float4 i : SV_POSITION) : SV_Target1 { return i; }\n",
               1,
               "COLOR1 -> SV_Target1");
        // ps_6_0 has 8 render targets; COLOR8 has no valid substitute and
        // must be left for DXC to reject with its own diagnostic.
        ExpectUntouched("float4 frag (float4 i : SV_POSITION) : COLOR8 { return i; }\n",
                        "frag",
                        ShaderStage::Fragment,
                        "COLOR8 is NOT mapped (no such render target)");
    }

    // ------------------------------------------------------------------
    // [case] Semantics are case-insensitive in HLSL.
    // ------------------------------------------------------------------
    std::cout << "\n---- [case] lower-case ': color' is recognised ----\n";
    {
        Expect("float4 frag (float4 i : SV_POSITION) : color { return i; }\n",
               "frag",
               ShaderStage::Fragment,
               "float4 frag (float4 i : SV_POSITION) : SV_Target { return i; }\n",
               1,
               "': color' -> ': SV_Target'");
    }

    // ------------------------------------------------------------------
    // [newline] Semantic on its own line.
    // ------------------------------------------------------------------
    std::cout << "\n---- [newline] semantic separated by newlines ----\n";
    {
        Expect("float4 frag (float4 i : SV_POSITION)\n    : COLOR\n{\n    return i;\n}\n",
               "frag",
               ShaderStage::Fragment,
               "float4 frag (float4 i : SV_POSITION)\n    : SV_Target\n{\n    return i;\n}\n",
               1,
               "multi-line signature");
    }

    // ------------------------------------------------------------------
    // [decl] A forward declaration is rewritten too, so it stays consistent
    //        with the definition.
    // ------------------------------------------------------------------
    std::cout << "\n---- [decl] declaration and definition both rewritten ----\n";
    {
        const std::string src =
            "float4 frag (float4 i : SV_POSITION) : COLOR;\n"
            "float4 frag (float4 i : SV_POSITION) : COLOR { return i; }\n";
        const std::string expected =
            "float4 frag (float4 i : SV_POSITION) : SV_Target;\n"
            "float4 frag (float4 i : SV_POSITION) : SV_Target { return i; }\n";
        Expect(src, "frag", ShaderStage::Fragment, expected, 2, "2 sites rewritten");
    }

    // ------------------------------------------------------------------
    // [void] No output semantic at all -> nothing to do.
    // ------------------------------------------------------------------
    std::cout << "\n---- [void] no output semantic ----\n";
    {
        ExpectUntouched("void frag (float4 i : SV_POSITION) { }\n",
                        "frag",
                        ShaderStage::Fragment,
                        "void entry untouched");
        ExpectUntouched("VertexOutput frag (float4 i : SV_POSITION) { return (VertexOutput)0; }\n",
                        "frag",
                        ShaderStage::Fragment,
                        "struct-returning entry untouched");
    }

    // ------------------------------------------------------------------
    // [call] A call to the entry function is not a definition.
    // ------------------------------------------------------------------
    std::cout << "\n---- [call] a call site is not a declarator ----\n";
    {
        ExpectUntouched("float4 wrap (float4 i : SV_POSITION) { return frag(i); }\n",
                        "frag",
                        ShaderStage::Fragment,
                        "call site inside another function");
    }

    // ------------------------------------------------------------------
    // [comment] / [string] Text that only looks like a declaration.
    // ------------------------------------------------------------------
    std::cout << "\n---- [comment] / [string] masked-out text ----\n";
    {
        ExpectUntouched("// float4 frag (float4 i : SV_POSITION) : COLOR { return i; }\n",
                        "frag",
                        ShaderStage::Fragment,
                        "line comment is masked");
        ExpectUntouched("/* frag (x) : COLOR */\nfloat4 frag (float4 i : SV_POSITION) : SV_Target { return i; }\n",
                        "frag",
                        ShaderStage::Fragment,
                        "block comment is masked");
        ExpectUntouched("static const char* k = \"frag (x) : COLOR\";\n",
                        "frag",
                        ShaderStage::Fragment,
                        "string literal is masked");
    }

    // ------------------------------------------------------------------
    // [member] `obj.frag(...)` is qualified, not a free function.
    // ------------------------------------------------------------------
    std::cout << "\n---- [member] qualified names are skipped ----\n";
    {
        ExpectUntouched("float4 go (S s) { return s.frag(1) : COLOR; }\n",
                        "frag",
                        ShaderStage::Fragment,
                        "member access skipped");
    }

    // ------------------------------------------------------------------
    // [boundary] Whole-identifier matching.
    // ------------------------------------------------------------------
    std::cout << "\n---- [boundary] whole-identifier matching ----\n";
    {
        ExpectUntouched("float4 myfrag (float4 i : SV_POSITION) : COLOR { return i; }\n",
                        "frag",
                        ShaderStage::Fragment,
                        "'myfrag' is not 'frag'");
        ExpectUntouched("float4 fragColor (float4 i : SV_POSITION) : COLOR { return i; }\n",
                        "frag",
                        ShaderStage::Fragment,
                        "'fragColor' is not 'frag'");
    }

    // ------------------------------------------------------------------
    // [vertex] COLOR is a legal INPUT semantic on a vertex shader.
    // ------------------------------------------------------------------
    std::cout << "\n---- [vertex] vertex stage is never rewritten ----\n";
    {
        ExpectUntouched("float4 vert (float4 v : COLOR) : SV_POSITION { return v; }\n",
                        "vert",
                        ShaderStage::Vertex,
                        "VS with a COLOR input semantic untouched");
        ExpectUntouched("float4 vert (float4 v : POSITION) : COLOR { return v; }\n",
                        "vert",
                        ShaderStage::Vertex,
                        "VS output ': COLOR' left alone (rewriting would break it)");
    }

    // ------------------------------------------------------------------
    // [empty] No entry point -> no rewrite.
    // ------------------------------------------------------------------
    std::cout << "\n---- [empty] degenerate inputs ----\n";
    {
        ExpectUntouched("float4 frag (float4 i : SV_POSITION) : COLOR { return i; }\n",
                        "",
                        ShaderStage::Fragment,
                        "empty entry point");
        ExpectUntouched("", "frag", ShaderStage::Fragment, "empty source");
    }

    std::cout << "\n=== " << (g_checks - g_failures) << "/" << g_checks
              << " checks passed ===\n";
    return g_failures == 0 ? 0 : 1;
}
