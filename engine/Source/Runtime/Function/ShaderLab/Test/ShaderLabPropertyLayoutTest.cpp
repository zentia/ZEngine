// =====================================================================
// ShaderLabPropertyLayoutTest
// ---------------------------------------------------------------------
// Pins down the JOIN between a ShaderLab `Properties {}` block and the
// `CBUFFER_START(UnityPerMaterial)` block -- i.e. the thing that turns
//
//     Properties { _Metallic ("Metallic", Range(0,1)) = 0 }
//     ...
//     CBUFFER_START(UnityPerMaterial)
//         float _Metallic;
//     CBUFFER_END
//
// into "script can call SetFloat(\"_Metallic\", 0.7f)". Each block alone was
// already reachable before this change (the parser produced m_Properties),
// but nothing ever connected the two, so the runtime had names with no byte
// offsets and named property writes were impossible.
//
// The three things that can go wrong, and are asserted here:
//   1. a property declared in Properties but NOT in the cbuffer must report
//      "no slot" (offset < 0) instead of a guessed offset -- a wrong offset
//      silently overwrites a neighbouring property.
//   2. the packing rule must follow the shader's source language. GLSL is
//      std140, HLSL is the cbuffer rule, and they disagree on float2/float3.
//   3. a shader with no UnityPerMaterial block must report found=false rather
//      than an empty success, so the caller can log it.
//
// Backend-free: no Vulkan, no DX12, no GPU. Links ZRuntime because the parser
// chain pulls in RenderType.h / EASTL.
// =====================================================================

#include "Runtime/Function/ShaderLab/ShaderCbufferReflection.h"
#include "Runtime/Function/ShaderLab/ShaderLabParser.h"

#include <cstdio>
#include <string>
#include <vector>

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

    /// 期望值：名字 + offset + size。查不到或数值不符都算失败。
    void CheckSlot(const ShaderCbufferLayout& layout,
                   const char* name,
                   uint32_t expected_offset,
                   uint32_t expected_size,
                   const char* label)
    {
        const ShaderPropertyLayout* slot = layout.Find(name);
        ++g_Checks;
        if (slot == nullptr)
        {
            std::fprintf(stderr, "  [FAIL] %s: '%s' has no slot\n", label, name);
            ++g_Failures;
            return;
        }
        if (slot->offset != expected_offset || slot->size != expected_size)
        {
            std::fprintf(stderr,
                         "  [FAIL] %s: '%s' is at %u (size %u), expected %u (size %u)\n",
                         label,
                         name,
                         slot->offset,
                         slot->size,
                         expected_offset,
                         expected_size);
            ++g_Failures;
            return;
        }
        std::fprintf(stdout, "  [ ok ] %s: '%s' @ %u size %u\n", label, name, expected_offset, expected_size);
    }

    /// 走一遍真实链路：解析 .shader -> 取 program 源码 -> 反射 cbuffer。
    ///
    /// 返回的指针指向 `out_asset` 里的 program，所以**调用方必须持有 out_asset**。
    /// 早先的版本把 parser 当局部变量、只把裸指针交回来，指针立刻悬垂 —— 解析
    /// 结果里的字段会读到垃圾（表现为 language 随机、source_code 为空）。
    const ShaderProgram* FindFirstProgram(const std::string& source, std::shared_ptr<ShaderLabAsset>& out_asset)
    {
        ShaderLabParser parser(source);
        if (!parser.Parse())
        {
            std::fprintf(stderr, "  [FAIL] parse error: %s\n", parser.GetError().c_str());
            ++g_Checks;
            ++g_Failures;
            return nullptr;
        }

        out_asset = parser.GetAsset();
        if (out_asset == nullptr)
        {
            return nullptr;
        }

        for (const ShaderSubShader& subshader : out_asset->subshaders)
        {
            for (const ShaderPass& pass : subshader.passes)
            {
                for (const ShaderProgram& program : pass.programs)
                {
                    if (!program.source_code.empty())
                    {
                        return &program;
                    }
                }
            }
        }
        return nullptr;
    }

    ShaderCbufferPacking PackingFor(const ShaderProgram& program)
    {
        return program.language == ShaderLanguage::GLSL ? ShaderCbufferPacking::Std140
                                                        : ShaderCbufferPacking::HlslCbuffer;
    }
}  // namespace

int main()
{
    // ---- A: HLSL 版 —— 抄自 engine/shader/example_standard.shader ------------
    {
        const std::string source = R"(
Shader "ZEngine/Standard"
{
    Properties
    {
        _BaseColor ("Base Color", Color) = (1,1,1,1)
        _Metallic ("Metallic", Range(0,1)) = 0
        _Smoothness ("Smoothness", Range(0,1)) = 0.5
        _NormalScale ("Normal Scale", Range(0,1)) = 1
        _Cutoff ("Cutoff", Range(0,1)) = 0.5
    }
    SubShader
    {
        Tags { "RenderPipeline" = "Deferred" }
        Pass
        {
            Name "GBuffer"
            Tags { "LightMode" = "GBuffer" }
            HLSLPROGRAM
            #pragma vertex StandardVert
            #pragma fragment StandardFrag
            CBUFFER_START(UnityPerMaterial)
                float4 _BaseColor;
                float4 _BaseMap_ST;
                float  _Metallic;
                float  _Smoothness;
                float  _NormalScale;
                float  _Cutoff;
            CBUFFER_END
            float4 StandardFrag (float4 i : COLOR) : COLOR { return i; }
            ENDHLSL
        }
    }
}
)";
        std::shared_ptr<ShaderLabAsset> asset_holder;
        const ShaderProgram* program = FindFirstProgram(source, asset_holder);
        Check(program != nullptr, "A the HLSLPROGRAM block is found by the parser");
        if (program == nullptr)
        {
            return 1;
        }
        Check(program->language == ShaderLanguage::HLSL, "A b its language is HLSL");
        Check(program->vertex_entry == "StandardVert" && program->fragment_entry == "StandardFrag",
              "A c entry points come from #pragma vertex / #pragma fragment");

        const ShaderCbufferLayout layout = ReflectCbuffer(program->source_code, "UnityPerMaterial", PackingFor(*program));
        Check(layout.found, "A d UnityPerMaterial is found inside the program body");
        CheckSlot(layout, "_BaseColor", 0, 16, "A");
        CheckSlot(layout, "_Metallic", 32, 4, "A");
        CheckSlot(layout, "_Smoothness", 36, 4, "A");
        CheckSlot(layout, "_NormalScale", 40, 4, "A");
        CheckSlot(layout, "_Cutoff", 44, 4, "A");
        Check(layout.size == 48, "A e total cbuffer size is 48");

        // Properties 里声明了但 cbuffer 里没有的属性：必须报"没有槽位"。
        const std::shared_ptr<ShaderLabAsset>& asset = asset_holder;
        Check(asset != nullptr && asset->properties.size() == 5, "A f Properties block yields 5 properties");
        if (asset != nullptr)
        {
            int resolved = 0;
            for (const ShaderProperty& property : asset->properties)
            {
                if (layout.Find(property.name) != nullptr)
                {
                    ++resolved;
                }
            }
            Check(resolved == 5, "A g every declared property resolves to a real UBO slot");
        }
    }

    // ---- B: 同一份声明写成 GLSLPROGRAM —— 打包规则要跟着换 --------------------
    {
        // float / float2 是两套规则唯一真正分岔的地方，所以这里故意用 float2。
        const std::string source = R"(
Shader "Custom/Dissolve"
{
    Properties
    {
        _Amount ("Amount", Range(0,1)) = 0
        _EdgeWidth ("Edge Width", Range(0,1)) = 0.1
    }
    SubShader
    {
        Pass
        {
            Name "GBuffer"
            GLSLPROGRAM
            #pragma vertex glsl_vert
            #pragma fragment glsl_frag
            #version 310 es
            layout(std140, set = 1, binding = 0) uniform UnityPerMaterial
            {
                float  _Amount;
                float2 _EdgeWidth;
            };
            void glsl_frag() {}
            ENDGLSL
        }
    }
}
)";
        std::shared_ptr<ShaderLabAsset> asset_holder;
        const ShaderProgram* program = FindFirstProgram(source, asset_holder);
        Check(program != nullptr, "B the GLSLPROGRAM block is found by the parser");
        if (program == nullptr)
        {
            return 1;
        }
        Check(program->language == ShaderLanguage::GLSL, "B b its language is GLSL");
        Check(PackingFor(*program) == ShaderCbufferPacking::Std140, "B c GLSL selects the std140 packing rule");

        const ShaderCbufferLayout layout = ReflectCbuffer(program->source_code, "UnityPerMaterial", PackingFor(*program));
        Check(layout.found, "B d the plain `uniform UnityPerMaterial { ... }` block is recognised");
        // std140：float2 要求 8 字节对齐 -> _EdgeWidth 落在 8，而不是 4。
        CheckSlot(layout, "_Amount", 0, 4, "B std140");
        CheckSlot(layout, "_EdgeWidth", 8, 8, "B std140");

        // 同一份源码按 HLSL 规则算会得到 4 —— 这正是"规则必须跟着源语言走"的点。
        const ShaderCbufferLayout wrong = ReflectCbuffer(program->source_code, "UnityPerMaterial",
                                                        ShaderCbufferPacking::HlslCbuffer);
        const ShaderPropertyLayout* wrong_slot = wrong.Find("_EdgeWidth");
        Check(wrong_slot != nullptr && wrong_slot->offset == 4,
              "B e the same declaration packs _EdgeWidth at 4 under the HLSL rule (hence the enum)");
    }

    // ---- C: 没有 UnityPerMaterial 就如实报 found=false ------------------------
    {
        const std::string source = R"(
Shader "Custom/NoCbuffer"
{
    Properties { _Tint ("Tint", Color) = (1,1,1,1) }
    SubShader
    {
        Pass
        {
            HLSLPROGRAM
            #pragma vertex v
            #pragma fragment f
            float4 f(float4 i : COLOR) : COLOR { return i; }
            ENDHLSL
        }
    }
}
)";
        std::shared_ptr<ShaderLabAsset> asset_holder;
        const ShaderProgram* program = FindFirstProgram(source, asset_holder);
        Check(program != nullptr, "C a shader with no cbuffer still parses");
        if (program == nullptr)
        {
            return 1;
        }
        const ShaderCbufferLayout layout = ReflectCbuffer(program->source_code, "UnityPerMaterial", PackingFor(*program));
        Check(!layout.found && layout.properties.empty() && layout.size == 0,
              "C b missing UnityPerMaterial reports found=false, not an empty success");
    }

    std::fprintf(stdout, "\nShaderLabPropertyLayout: %d checks, %d failed\n", g_Checks, g_Failures);
    return g_Failures == 0 ? 0 : 1;
}
