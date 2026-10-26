// =====================================================================
// ShaderLabProgramExtractTest
// ---------------------------------------------------------------------
// Pins down "which program block does a backend actually compile".
//
// A `.shader` can carry both an HLSLPROGRAM and a GLSLPROGRAM in the same
// Pass: the first is for DX12, the second for Vulkan / Metal. Until now
// ExtractHlslFromShaderLab* took whichever program it stumbled over first,
// so a dual-language shader handed the HLSL body to glslang (or the GLSL
// body to DXC) depending on declaration order -- and every backend paid for
// a compile failure before falling back to the built-in shader.
//
// The rule this test locks in: extraction is **language-filtered**. HLSL +
// Cg go to the DX12 path, GLSL goes to the Vulkan / Metal path, and asking
// for a language the file does not declare is an honest failure rather than
// a silent wrong-language hand-off.
//
// A second, subtler rule: the Cg -> Shader-Model-4 output-semantic rewrite
// (`: COLOR` -> `: SV_Target`) is HLSL-only. GLSL has no output semantics,
// so running the rewrite on it would corrupt correct source.
//
// Cases:
//   A dual-language shader: each extractor picks its own block and entry
//   B the legacy semantic rewrite applies to HLSL and only HLSL
//   C a shader with no GLSLPROGRAM reports failure, not a fallback
//   D IsShaderLabSourcePath is the extension gate every backend shares
//
// Backend-free: no Vulkan, no DX12, no GPU. Links ZRuntime because the
// parser chain pulls in RenderType.h / EASTL.
// =====================================================================

#include "Runtime/Function/ShaderLab/ShaderLabHlslExtract.h"

#include <cstdio>
#include <string>

using namespace ZEngine::ShaderLab;

namespace
{
    int g_Checks = 0;
    int g_Failures = 0;

    void Check(bool condition, const char* name)
    {
        ++g_Checks;
        if (!condition)
        {
            std::fprintf(stderr, "  [FAIL] %s\n", name);
            ++g_Failures;
        }
        else
        {
            std::fprintf(stdout, "  [ ok ] %s\n", name);
        }
    }

    /// 同一份 .shader 里同时写 HLSLPROGRAM 与 GLSLPROGRAM。
    const char* k_DualLanguageShader = R"(
Shader "Dual/Lang"
{
    Properties { _Tint ("Tint", Color) = (1,1,1,1) }
    SubShader
    {
        Pass
        {
            Name "GBuffer"
            Tags { "LightMode" = "GBuffer" }
            HLSLPROGRAM
            #pragma vertex HlslVert
            #pragma fragment HlslFrag
            float4 HlslFrag (float4 i : COLOR) : COLOR { return i; }
            ENDHLSL
            GLSLPROGRAM
            #pragma vertex glsl_vert
            #pragma fragment glsl_frag
            #version 310 es
            void glsl_frag() {}
            ENDGLSL
        }
    }
}
)";

    const char* k_HlslOnlyShader = R"(
Shader "Only/Hlsl"
{
    SubShader { Pass { HLSLPROGRAM
        #pragma vertex v
        #pragma fragment f
        float4 f(float4 i : COLOR) : COLOR { return i; }
    ENDHLSL } }
}
)";
}  // namespace

int main()
{
    // ---- A: 双语言 shader，两条提取路径各取各的 --------------------------------
    {
        const ShaderLabHlslExtractResult hlsl =
            ExtractHlslFromShaderLabSource(k_DualLanguageShader, "dual.shader", ShaderStage::Fragment);
        Check(hlsl.ok, "A a HLSL extract succeeds on a dual-language shader");
        Check(hlsl.entry_point == "HlslFrag", "A b HLSL entry comes from `#pragma fragment HlslFrag`");
        Check(hlsl.hlsl_source.find("#version 310 es") == std::string::npos,
              "A c HLSL extract does not leak the GLSLPROGRAM body");

        const ShaderLabGlslExtractResult glsl =
            ExtractGlslFromShaderLabSource(k_DualLanguageShader, "dual.shader", ShaderStage::Fragment);
        Check(glsl.ok, "A d GLSL extract succeeds on the same shader");
        Check(glsl.entry_point == "glsl_frag", "A e GLSL entry comes from `#pragma fragment glsl_frag`");
        Check(glsl.glsl_source.find("#version 310 es") != std::string::npos,
              "A f GLSL extract keeps the #version directive");
        Check(glsl.glsl_source.find("HlslFrag") == std::string::npos,
              "A g GLSL extract does not leak the HLSLPROGRAM body");
    }

    // ---- B: Cg 输出语义改写只作用在 HLSL 上 ------------------------------------
    {
        const ShaderLabHlslExtractResult hlsl =
            ExtractHlslFromShaderLabSource(k_DualLanguageShader, "dual.shader", ShaderStage::Fragment);
        Check(hlsl.ok && hlsl.hlsl_source.find("SV_Target") != std::string::npos,
              "B HLSL extract still rewrites the legacy COLOR output semantic");

        const ShaderLabGlslExtractResult glsl =
            ExtractGlslFromShaderLabSource(k_DualLanguageShader, "dual.shader", ShaderStage::Fragment);
        Check(glsl.ok && glsl.glsl_source.find("SV_Target") == std::string::npos,
              "B b GLSL extract does NOT apply the HLSL semantic rewrite");
    }

    // ---- C: 没有 GLSLPROGRAM 就如实失败，不能把 HLSL 当 GLSL 交出去 -------------
    {
        const ShaderLabGlslExtractResult missing =
            ExtractGlslFromShaderLabSource(k_HlslOnlyShader, "only_hlsl.shader", ShaderStage::Fragment);
        Check(!missing.ok, "C GLSL extract reports failure when the shader has no GLSLPROGRAM");
        Check(!missing.error_message.empty(), "C b the failure carries a reason");
        Check(missing.glsl_source.empty(), "C c no source is handed back on failure");

        // 反向：HLSL 侧在这个文件上仍然要工作（回归保护）。
        const ShaderLabHlslExtractResult present =
            ExtractHlslFromShaderLabSource(k_HlslOnlyShader, "only_hlsl.shader", ShaderStage::Fragment);
        Check(present.ok && present.entry_point == "f", "C d the HLSL path is unaffected");
    }

    // ---- D: 每个后端共用的扩展名判定 -------------------------------------------
    {
        Check(IsShaderLabSourcePath("a/b/Foo.shader"), "D IsShaderLabSourcePath: .shader");
        Check(IsShaderLabSourcePath("a/b/Foo.SHADER"), "D b IsShaderLabSourcePath: case-insensitive");
        Check(!IsShaderLabSourcePath("a/b/mesh.frag"), "D c IsShaderLabSourcePath: .frag is not ShaderLab");
        Check(!IsShaderLabSourcePath("a/b/mesh.vert"), "D d IsShaderLabSourcePath: .vert is not ShaderLab");
        Check(!IsShaderLabSourcePath("no_extension"), "D e IsShaderLabSourcePath: no extension is not ShaderLab");
    }

    std::fprintf(stdout, "\nShaderLabProgramExtract: %d checks, %d failed\n", g_Checks, g_Failures);
    return g_Failures == 0 ? 0 : 1;
}
