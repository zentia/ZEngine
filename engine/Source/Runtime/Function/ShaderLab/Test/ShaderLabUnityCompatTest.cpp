// =====================================================================
// ShaderLab Unity-compatibility smoke-test
// ---------------------------------------------------------------------
// QUESTION THIS ANSWERS:
//   Can a real, unmodified Unity CG shader be compiled by ZEngine's
//   ShaderLab -> DX12 (DXC/DXIL) path, given a UnityCG.cginc shim?
//
// It exercises three things at once:
//   1. ShaderLabParser understands real Unity ShaderLab syntax
//      (property attributes, Stencil { Ref[_X] }, Fog/Lighting, CustomEditor).
//   2. The DX12 include handler resolves `#include "UnityCG.cginc"`
//      relative to the .shader's own directory.
//   3. DXC accepts the Cg-flavoured body once the shim supplies the
//      built-ins DXC does not have (fixed, sampler2D, tex2D, ...).
//
// Opt in with:
//   cmake -B build -DZENGINE_BUILD_SHADERLAB_UNITY_COMPAT_TEST=ON
//   cmake --build build --target ShaderLabUnityCompatTest
//
// Exit code is the verdict: 0 = every required stage produced DXIL.
// =====================================================================

#include "Runtime/Function/ShaderLab/ShaderLabDx12Compiler.h"

#include <filesystem>
#include <iostream>
#include <string>

using namespace ZEngine::ShaderLab;

namespace
{

// Injected by CMake so the fixtures are found regardless of CWD.
#ifndef ZENGINE_SHADERLAB_UNITY_COMPAT_TEST_DIR
#define ZENGINE_SHADERLAB_UNITY_COMPAT_TEST_DIR "."
#endif

std::filesystem::path Fixture(const char* name)
{
    return std::filesystem::path(ZENGINE_SHADERLAB_UNITY_COMPAT_TEST_DIR) / "unity_compat" / name;
}

const char* LanguageName(ShaderLanguage lang)
{
    switch (lang)
    {
        case ShaderLanguage::HLSL: return "HLSL";
        case ShaderLanguage::GLSL: return "GLSL";
        case ShaderLanguage::CG:   return "CG";
        default:                   return "?";
    }
}

void DumpAsset(const ShaderLabAsset& asset)
{
    std::cout << "  shader_name   : " << asset.shader_name << "\n";
    std::cout << "  custom_editor : " << asset.custom_editor << "\n";
    std::cout << "  properties    : " << asset.properties.size() << "\n";
    for (const auto& p : asset.properties)
    {
        std::cout << "      - " << p.name << " (" << PropertyTypeToString(p.type) << ")\n";
    }
    std::cout << "  subshaders    : " << asset.subshaders.size() << "\n";

    for (size_t i = 0; i < asset.subshaders.size(); ++i)
    {
        const auto& sub = asset.subshaders[i];
        std::cout << "    SubShader[" << i << "] LOD=" << sub.lod
                  << " passes=" << sub.passes.size() << "\n";
        for (size_t j = 0; j < sub.passes.size(); ++j)
        {
            const auto& pass = sub.passes[j];
            std::cout << "      Pass[" << j << "] name='" << pass.name << "'"
                      << " light_mode='" << pass.light_mode << "'"
                      << " programs=" << pass.programs.size() << "\n";
            for (size_t k = 0; k < pass.programs.size(); ++k)
            {
                const auto& prog = pass.programs[k];
                std::cout << "        Program[" << k << "] lang=" << LanguageName(prog.language)
                          << " vert='" << prog.vertex_entry << "'"
                          << " frag='" << prog.fragment_entry << "'"
                          << " len=" << prog.source_code.length() << "\n";
            }
        }
    }
}

bool TryCompile(ShaderLabDx12Compiler& compiler,
                const std::string& program_type,
                const std::string& label,
                bool require_success,
                int& checks,
                int& failures)
{
    auto result = compiler.Compile(0, 0, program_type, {});
    const bool ok = result.success;

    // Informational checks are reported but never gate the exit code.
    if (require_success)
    {
        ++checks;
        if (!ok)
        {
            ++failures;
        }
    }

    std::cout << "  [" << (ok ? " OK " : "FAIL") << "] " << label
              << (require_success ? "" : " [informational]");
    if (ok)
    {
        std::cout << " -> DXIL " << result.dxil_code.size() << " bytes, entry '"
                  << result.entry_point << "'\n";
    }
    else
    {
        std::cout << "\n         " << result.error_message << "\n";
        if (!require_success)
        {
            std::cout << "         (informational only -- not a hard failure)\n";
        }
    }

    return ok || !require_success;
}

// Loads `file_name`, dumps the parse result, and compiles vertex + fragment.
// Returns false only when a *required* stage failed.
bool RunShaderFile(const char* file_name,
                   const char* title,
                   bool required,
                   int& checks,
                   int& failures)
{
    const std::filesystem::path path = Fixture(file_name);

    std::cout << "\n---- " << title << " ----\n";
    std::cout << "  file: " << path.generic_string() << "\n";

    ShaderLabDx12Compiler compiler;
    if (!compiler.LoadFromFile(path.generic_string()))
    {
        std::cout << "  [FAIL] load/parse: " << compiler.GetParseError()
                  << (required ? "" : " [informational]") << "\n";
        if (required)
        {
            ++checks;
            ++failures;
        }
        return !required;
    }

    auto asset = compiler.GetAsset();
    if (!asset)
    {
        std::cout << "  [FAIL] no asset produced"
                  << (required ? "" : " [informational]") << "\n";
        if (required)
        {
            ++checks;
            ++failures;
        }
        return !required;
    }

    std::cout << "  [ OK ] parse\n";
    DumpAsset(*asset);

    if (asset->subshaders.empty() || asset->subshaders[0].passes.empty())
    {
        std::cout << "  [FAIL] no subshader/pass parsed"
                  << (required ? "" : " [informational]") << "\n";
        if (required)
        {
            ++checks;
            ++failures;
        }
        return !required;
    }

    TryCompile(compiler, "vertex", "vertex stage", required, checks, failures);
    TryCompile(compiler, "fragment", "fragment stage", required, checks, failures);
    return true;
}

}  // namespace

int main(int argc, char** argv)
{
    std::cout << "=== ShaderLab Unity-compat smoke test ===\n";
    std::cout << "fixture dir : "
              << std::filesystem::path(ZENGINE_SHADERLAB_UNITY_COMPAT_TEST_DIR).generic_string()
              << "\n";

    int checks = 0;
    int failures = 0;

    // Ad-hoc mode: `ShaderLabUnityCompatTest <file> [<file>...]` parses and
    // compiles pass 0 of each file. Lets you drop a diagnostic fixture in and
    // re-run without rebuilding.
    if (argc > 1)
    {
        for (int a = 1; a < argc; ++a)
        {
            std::filesystem::path p = argv[a];
            if (!p.is_absolute())
            {
                p = std::filesystem::path(ZENGINE_SHADERLAB_UNITY_COMPAT_TEST_DIR) / "unity_compat" / p;
            }

            std::cout << "\n---- " << p.filename().generic_string() << " ----\n";
            std::cout << "  file: " << p.generic_string() << "\n";

            ShaderLabDx12Compiler compiler;
            if (!compiler.LoadFromFile(p.generic_string()))
            {
                std::cout << "  [FAIL] parse: " << compiler.GetParseError() << "\n";
                ++checks;
                ++failures;
                continue;
            }
            std::cout << "  [ OK ] parse\n";
            DumpAsset(*compiler.GetAsset());
            if (compiler.GetAsset()->subshaders.empty() ||
                compiler.GetAsset()->subshaders[0].passes.empty())
            {
                std::cout << "  [FAIL] no pass parsed\n";
                ++checks;
                ++failures;
                continue;
            }
            TryCompile(compiler, "vertex", "vertex stage", true, checks, failures);
            TryCompile(compiler, "fragment", "fragment stage", true, checks, failures);
        }

        std::cout << "\n=== verdict: " << (failures == 0 ? "PASS" : "FAIL")
                  << " (" << (checks - failures) << "/" << checks << " checks) ===\n";
        return failures == 0 ? 0 : 1;
    }

    // ------------------------------------------------------------------
    // 1. Spine/Skeleton Tint with ONE change: the fragment output semantic
    //    is SV_Target instead of Unity's legacy COLOR (DXC rejects COLOR for
    //    ps 6.0). Everything else is byte-identical to the Unity original.
    //
    // NOTE: ShaderLabDx12Compiler::Compile() hands the whole .shader file to
    // ExtractHlslFromShaderLabFile(), which returns the FIRST CGPROGRAM block
    // it finds -- so pass_index is not honoured. Only pass 0's body is ever
    // compiled here.
    // ------------------------------------------------------------------
    RunShaderFile("Spine-Skeleton-Tint-SemanticFixed.shader",
                  "[1] Spine/Skeleton Tint (only edit: frag : COLOR -> : SV_Target)",
                  true,
                  checks,
                  failures);

    // ------------------------------------------------------------------
    // 1b. The byte-identical Unity original. Expected to FAIL on the fragment
    //     stage until ZEngine rewrites legacy output semantics. Informational
    //     only -- it documents the gap, it is not a regression gate.
    // ------------------------------------------------------------------
    RunShaderFile("Spine-Skeleton-Tint.shader",
                  "[1b] Spine/Skeleton Tint (unmodified, expected to fail)",
                  false,
                  checks,
                  failures);

    // ------------------------------------------------------------------
    // 2. The Caster pass in isolation. Currently the parser drops it
    //    (unknown pass-level commands swallow the block), so informational.
    // ------------------------------------------------------------------
    RunShaderFile("Spine-Skeleton-Tint-CasterOnly.shader",
                  "[2] Spine Caster pass (isolated, exercises shadow macros)",
                  false,
                  checks,
                  failures);

    // ------------------------------------------------------------------
    // 3. Negative control: a missing include must surface as a real DXC
    //    error, not be silently commented out.
    //    (Must go through LoadFromFile -- the DX12 path refuses to compile
    //    a shader loaded from a string because it has no path to resolve
    //    includes against.)
    // ------------------------------------------------------------------
    std::cout << "\n---- [3] Negative control (missing include) ----\n";
    {
        const std::filesystem::path path = Fixture("diag_missing_include.shader");
        ShaderLabDx12Compiler neg;
        if (neg.LoadFromFile(path.generic_string()))
        {
            auto r = neg.Compile(0, 0, "vertex", {});
            std::cout << "  " << (r.success ? "[UNEXPECTED OK]" : "[ OK ] correctly failed")
                      << "\n";
            if (!r.success)
            {
                std::string first_line = r.error_message.substr(0, r.error_message.find('\n'));
                std::cout << "         " << first_line << "\n";
            }
        }
        else
        {
            std::cout << "  (load failed: " << neg.GetParseError() << ")\n";
        }
    }

    std::cout << "\n=== verdict: " << (failures == 0 ? "PASS" : "FAIL")
              << " (" << (checks - failures) << "/" << checks << " checks) ===\n";
    return failures == 0 ? 0 : 1;
}
