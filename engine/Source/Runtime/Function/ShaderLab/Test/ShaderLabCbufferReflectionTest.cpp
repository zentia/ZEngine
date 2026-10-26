// =====================================================================
// ShaderLabCbufferReflectionTest
// ---------------------------------------------------------------------
// Pins down the "property name -> UBO byte offset" reflection that the dynamic
// property block needs. This is the piece that makes
// `SetFloat("_WhateverYourShaderDeclared", v)` possible: until now the engine
// only knew the names that are hard-coded in MaterialCustomParamNames.h.
//
// Why a text parser and not SPIR-V reflection: spirv-cross is linked only on
// Apple (3rdparty/CMakeLists.txt:252) and SPIRV_CROSS_ENABLE_REFLECT is OFF,
// so on Windows / Android there is nothing to reflect with. A text parser is
// also pure CPU, which means it can actually be tested on a machine with no
// GPU and no shader compiler -- which is how this test runs.
//
// The single most important thing asserted here is CASES D/E: Std140 and
// HlslCbuffer give **different offsets for the same declaration**. Getting
// that wrong silently writes property values into the wrong bytes, so the
// packing rule must follow the shader's source language, never be hard-coded.
//
// Cases:
//   A the real UnityPerMaterial block from engine/shader/example_standard.shader
//   B both block spellings (CBUFFER_START macro and plain `cbuffer X {`)
//   C comments, preprocessor lines, packoffset/register suffixes are ignored
//   D `float _A; float2 _B;`  -> Std140 packs _B at 8, HLSL packs it at 4
//   E `float _A; float3 _B;`  -> Std140 packs _B at 16, HLSL packs it at 4
//   F arrays take a full 16-byte stride per element under both rules
//   G matrices: float4x3 is 48 bytes under Std140 but 64 under HLSL
//     (HLSL is row-major-named floatRxC, GLSL is matCxR -- naming is inverted)
//   H a missing cbuffer reports found=false instead of an empty success
//   I duplicate declarations keep the first offset; nested structs are skipped
//   J total size is rounded up to a multiple of 16
//   K GLSL's `layout(std140, binding = 0) uniform Name { ... };` spelling
//   L a bare `uniform sampler2D x;` is not a block and must not shadow one
//
// Backend-free and ZRuntime-free: only ShaderCbufferReflection.cpp is compiled.
// =====================================================================

#include "Runtime/Function/ShaderLab/ShaderCbufferReflection.h"

#include <cmath>
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

    /// 期望值：名字 + offset + size。
    void CheckAt(const ShaderCbufferLayout& layout,
                 const char* name,
                 uint32_t expected_offset,
                 uint32_t expected_size,
                 const char* label)
    {
        const ShaderPropertyLayout* property = layout.Find(name);
        if (property == nullptr)
        {
            std::fprintf(stderr, "  [FAIL] %s: '%s' not found in the layout\n", label, name);
            ++g_Checks;
            ++g_Failures;
            return;
        }
        const bool ok = property->offset == expected_offset && property->size == expected_size;
        if (!ok)
        {
            std::fprintf(stderr,
                         "  [FAIL] %s: '%s' is at %u (size %u), expected %u (size %u)\n",
                         label,
                         name,
                         property->offset,
                         property->size,
                         expected_offset,
                         expected_size);
        }
        ++g_Checks;
        if (!ok)
        {
            ++g_Failures;
        }
        else
        {
            std::fprintf(stdout, "  [ ok ] %s: '%s' @ %u size %u\n", label, name, expected_offset, expected_size);
        }
    }
}  // namespace

int main()
{
    // ---- A: 真实 shader 里的 UnityPerMaterial --------------------------------
    {
        // 抄自 engine/shader/example_standard.shader:75-82
        const std::string source = R"(
            CBUFFER_START(UnityPerMaterial)
                float4 _BaseColor;
                float4 _BaseMap_ST;
                float  _Metallic;
                float  _Smoothness;
                float  _NormalScale;
                float  _Cutoff;
            CBUFFER_END
        )";

        const ShaderCbufferLayout hlsl = ReflectCbuffer(source, "UnityPerMaterial", ShaderCbufferPacking::HlslCbuffer);
        Check(hlsl.found, "A the UnityPerMaterial block is found");
        Check(hlsl.properties.size() == 6, "A b it declares exactly 6 properties");
        CheckAt(hlsl, "_BaseColor", 0, 16, "A");
        CheckAt(hlsl, "_BaseMap_ST", 16, 16, "A");
        CheckAt(hlsl, "_Metallic", 32, 4, "A");
        CheckAt(hlsl, "_Smoothness", 36, 4, "A");
        CheckAt(hlsl, "_NormalScale", 40, 4, "A");
        CheckAt(hlsl, "_Cutoff", 44, 4, "A");
        Check(hlsl.size == 48, "A c total size rounds to 48 (16 * 3)");

        // 这个块全是 16 对齐起步的，两套规则算出来必须一致。
        const ShaderCbufferLayout std140 = ReflectCbuffer(source, "UnityPerMaterial", ShaderCbufferPacking::Std140);
        bool same = std140.size == hlsl.size && std140.properties.size() == hlsl.properties.size();
        for (size_t i = 0; same && i < hlsl.properties.size(); ++i)
        {
            same = std140.properties[i].offset == hlsl.properties[i].offset &&
                   std140.properties[i].size == hlsl.properties[i].size;
        }
        Check(same, "A d a fully 16-aligned block comes out identical under both packing rules");
    }

    // ---- B: 两种块写法 -------------------------------------------------------
    {
        const std::string plain = "cbuffer UnityPerMaterial : register(b0) { float4 _Tint; float _Amount; }";
        const ShaderCbufferLayout layout = ReflectCbuffer(plain, "UnityPerMaterial", ShaderCbufferPacking::HlslCbuffer);
        Check(layout.found, "B a plain `cbuffer X : register(b0) { ... }` block is recognised");
        CheckAt(layout, "_Tint", 0, 16, "B");
        CheckAt(layout, "_Amount", 16, 4, "B");
    }
    {
        // 同名块不是一个：要跳过 UnityPerDraw 找到 UnityPerMaterial。
        const std::string two = R"(
            CBUFFER_START(UnityPerDraw)
                float4x4 unity_ObjectToWorld;
            CBUFFER_END
            CBUFFER_START(UnityPerMaterial)
                float4 _Tint;
            CBUFFER_END
        )";
        const ShaderCbufferLayout layout = ReflectCbuffer(two, "UnityPerMaterial", ShaderCbufferPacking::HlslCbuffer);
        Check(layout.found, "B b the right block is picked when several cbuffers are present");
        Check(layout.properties.size() == 1 && layout.properties[0].name == "_Tint",
              "B c UnityPerDraw's members did not leak into UnityPerMaterial");
    }

    // ---- C: 注释 / 预处理 / 后缀不干扰 ---------------------------------------
    {
        const std::string source = R"(
            CBUFFER_START(UnityPerMaterial)
                // float4 _CommentedOut;
                /* float4 _BlockCommented; */
                #ifdef FOO
                float4 _InsideIfdef;
                #endif
                float4 _Tint : packoffset(c0);
                float  _Amount : register(c4);
            CBUFFER_END
        )";
        const ShaderCbufferLayout layout = ReflectCbuffer(source, "UnityPerMaterial", ShaderCbufferPacking::HlslCbuffer);
        Check(layout.properties.size() == 2, "C comments and #ifdef'd declarations are not counted");
        Check(layout.Find("_CommentedOut") == nullptr, "C b a //-commented declaration is ignored");
        Check(layout.Find("_BlockCommented") == nullptr, "C c a /* */ -commented declaration is ignored");
        Check(layout.Find("_InsideIfdef") == nullptr, "C d a declaration inside #ifdef is ignored");
        CheckAt(layout, "_Tint", 0, 16, "C");
        CheckAt(layout, "_Amount", 16, 4, "C");
    }

    // ---- D: float2 的对齐差异（std140 要 8 字节对齐，HLSL 只要不跨寄存器）------
    {
        const std::string source = R"(
            CBUFFER_START(UnityPerMaterial)
                float  _A;
                float2 _B;
            CBUFFER_END
        )";
        const ShaderCbufferLayout std140 = ReflectCbuffer(source, "UnityPerMaterial", ShaderCbufferPacking::Std140);
        const ShaderCbufferLayout hlsl = ReflectCbuffer(source, "UnityPerMaterial", ShaderCbufferPacking::HlslCbuffer);

        CheckAt(std140, "_B", 8, 8, "D std140");
        CheckAt(hlsl, "_B", 4, 8, "D hlsl");
        Check(std140.Find("_B")->offset != hlsl.Find("_B")->offset,
              "D c the two rules genuinely disagree here (that is the whole point of the enum)");
    }

    // ---- E: float3 的差异更大（std140 要 16 对齐）-----------------------------
    {
        const std::string source = R"(
            CBUFFER_START(UnityPerMaterial)
                float  _A;
                float3 _B;
            CBUFFER_END
        )";
        const ShaderCbufferLayout std140 = ReflectCbuffer(source, "UnityPerMaterial", ShaderCbufferPacking::Std140);
        const ShaderCbufferLayout hlsl = ReflectCbuffer(source, "UnityPerMaterial", ShaderCbufferPacking::HlslCbuffer);

        CheckAt(std140, "_B", 16, 12, "E std140");
        CheckAt(hlsl, "_B", 4, 12, "E hlsl");
        Check(std140.size == 32 && hlsl.size == 16, "E c total size differs too (32 vs 16)");
    }

    // ---- F: 数组每个元素占满一个寄存器 ---------------------------------------
    {
        const std::string source = R"(
            CBUFFER_START(UnityPerMaterial)
                float  _Curve[4];
                float4 _Points[2];
            CBUFFER_END
        )";
        const ShaderCbufferLayout std140 = ReflectCbuffer(source, "UnityPerMaterial", ShaderCbufferPacking::Std140);
        const ShaderCbufferLayout hlsl = ReflectCbuffer(source, "UnityPerMaterial", ShaderCbufferPacking::HlslCbuffer);

        // float[4]：元素只有 4 字节，但 stride 被撑到 16 -> 64 字节
        CheckAt(std140, "_Curve", 0, 64, "F std140");
        CheckAt(hlsl, "_Curve", 0, 64, "F hlsl");
        CheckAt(std140, "_Points", 64, 32, "F std140");
        CheckAt(hlsl, "_Points", 64, 32, "F hlsl");
        Check(std140.Find("_Curve")->array_count == 4 && std140.Find("_Curve")->ElementSize() == 4,
              "F c array_count is remembered separately from the padded stride");
    }

    // ---- G: 矩阵的命名约定在 HLSL 与 GLSL 之间是反的 --------------------------
    {
        const std::string hlsl_source = R"(
            CBUFFER_START(UnityPerMaterial)
                float4x4 _Mat4;
                float4x3 _Mat43;
            CBUFFER_END
        )";
        const std::string glsl_source = R"(
            CBUFFER_START(UnityPerMaterial)
                mat4 _Mat4;
                mat3x4 _Mat34;
            CBUFFER_END
        )";

        // float4x4 / mat4 方阵：两边都是 64
        const ShaderCbufferLayout hlsl = ReflectCbuffer(hlsl_source, "UnityPerMaterial", ShaderCbufferPacking::HlslCbuffer);
        CheckAt(hlsl, "_Mat4", 0, 64, "G hlsl float4x4");
        const ShaderCbufferLayout glsl = ReflectCbuffer(glsl_source, "UnityPerMaterial", ShaderCbufferPacking::Std140);
        CheckAt(glsl, "_Mat4", 0, 64, "G std140 mat4");

        // 非方阵：HLSL float4x3 = 4 行 -> 4 个寄存器 = 64
        //         std140 mat3x4 = 3 列 -> 3 * 16 = 48
        CheckAt(hlsl, "_Mat43", 64, 64, "G hlsl float4x3");
        CheckAt(glsl, "_Mat34", 64, 48, "G std140 mat3x4");
        Check(hlsl.Find("_Mat43")->is_matrix && hlsl.Find("_Mat43")->components == 4,
              "G c matrix rows are reported as components");
    }

    // ---- H: 找不到就如实说 ---------------------------------------------------
    {
        const std::string source = "CBUFFER_START(UnityPerDraw) float4x4 _M; CBUFFER_END";
        const ShaderCbufferLayout layout = ReflectCbuffer(source, "UnityPerMaterial", ShaderCbufferPacking::Std140);
        Check(!layout.found && layout.properties.empty() && layout.size == 0,
              "H a shader with no UnityPerMaterial block reports found=false (not an empty success)");

        const ShaderCbufferLayout empty = ReflectCbuffer("", "UnityPerMaterial", ShaderCbufferPacking::Std140);
        Check(!empty.found, "H b empty source reports found=false");
    }

    // ---- I: 重复声明取第一次；嵌套 struct 跳过 -------------------------------
    {
        const std::string source = R"(
            CBUFFER_START(UnityPerMaterial)
                struct Nested { float4 _InStruct; };
                float4 _Tint;
                float4 _Tint;
                float  _Amount;
            CBUFFER_END
        )";
        const ShaderCbufferLayout layout = ReflectCbuffer(source, "UnityPerMaterial", ShaderCbufferPacking::HlslCbuffer);
        Check(layout.Find("_InStruct") == nullptr, "I members of a nested struct are not picked up");
        Check(layout.properties.size() == 2, "I b a duplicate declaration is not counted twice");
        CheckAt(layout, "_Tint", 0, 16, "I");
        CheckAt(layout, "_Amount", 16, 4, "I");
    }

    // ---- J: 大小补齐 ---------------------------------------------------------
    {
        const std::string source = R"(
            CBUFFER_START(UnityPerMaterial)
                float _OnlyOne;
            CBUFFER_END
        )";
        const ShaderCbufferLayout layout = ReflectCbuffer(source, "UnityPerMaterial", ShaderCbufferPacking::Std140);
        Check(layout.size == 16, "J a 4-byte cbuffer still reports a 16-byte size");
    }

    // ---- K: GLSL 的 uniform block 写法 ---------------------------------------
    {
        const std::string source = R"(
            #version 310 es
            layout(std140, set = 1, binding = 0) uniform UnityPerMaterial
            {
                float  _Amount;
                float4 _Tint;
            };
        )";
        const ShaderCbufferLayout layout = ReflectCbuffer(source, "UnityPerMaterial", ShaderCbufferPacking::Std140);
        Check(layout.found, "K a GLSL `layout(...) uniform Name { ... };` block is recognised");
        CheckAt(layout, "_Amount", 0, 4, "K");
        CheckAt(layout, "_Tint", 16, 16, "K");
    }

    // ---- L: 非块的 uniform 声明要跳过，不能把它后面的块挡住 --------------------
    {
        const std::string source = R"(
            #version 310 es
            uniform sampler2D _BaseMap;
            uniform vec3 _CameraPos;
            layout(std140, binding = 0) uniform UnityPerMaterial
            {
                float4 _Tint;
            };
        )";
        const ShaderCbufferLayout layout = ReflectCbuffer(source, "UnityPerMaterial", ShaderCbufferPacking::Std140);
        Check(layout.found, "L a bare `uniform sampler2D x;` does not shadow the real block");
        Check(layout.properties.size() == 1 && layout.properties[0].name == "_Tint",
              "L b the non-block uniforms contributed no members");
    }

    std::fprintf(stdout, "\nShaderLabCbufferReflection: %d checks, %d failed\n", g_Checks, g_Failures);
    return g_Failures == 0 ? 0 : 1;
}
