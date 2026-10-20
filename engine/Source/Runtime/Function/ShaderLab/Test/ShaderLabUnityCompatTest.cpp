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

// <repo>/engine -- the include root for engine/shader/*.shader, which write
// project-relative paths (`#include "shader/include/common.h"`).
#ifndef ZENGINE_SHADERLAB_ENGINE_SHADER_DIR
#define ZENGINE_SHADERLAB_ENGINE_SHADER_DIR "."
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

// Regression guard: a named property must survive parsing.
//
// The lexer used to dispatch on the first character, so digit-leading
// property types (2D / 3D) were split into a number plus an identifier and
// never matched the keyword table -- the whole property was dropped. For
// Spine that meant _MainTex (the atlas sampler) silently vanished.
void RequireProperty(const std::filesystem::path& shader,
                     const std::string& name,
                     int& checks,
                     int& failures)
{
    ++checks;

    ShaderLabDx12Compiler compiler;
    if (!compiler.LoadFromFile(shader.generic_string()))
    {
        std::cout << "  [FAIL] " << name << " -- shader failed to parse\n";
        ++failures;
        return;
    }

    for (const auto& p : compiler.GetAsset()->properties)
    {
        if (p.name == name)
        {
            std::cout << "  [ OK ] " << name << " (" << PropertyTypeToString(p.type) << ")\n";
            return;
        }
    }

    std::cout << "  [FAIL] " << name << " -- property missing after parse\n";
    ++failures;
}

// Regression guard: a named Pass must survive parsing, with its CGPROGRAM.
//
// SkipToNextStatement() used to swallow everything up to and INCLUDING the next
// '}'. An unrecognised line-oriented command at SubShader level ("Cull Off")
// therefore ate the following Pass block whole -- the Spine Caster pass came
// out as passes=0 / programs=0.
void RequirePass(const std::filesystem::path& shader,
                 const std::string& pass_name,
                 int& checks,
                 int& failures)
{
    ++checks;

    ShaderLabDx12Compiler compiler;
    if (!compiler.LoadFromFile(shader.generic_string()))
    {
        std::cout << "  [FAIL] pass '" << pass_name << "' -- shader failed to parse\n";
        ++failures;
        return;
    }

    const auto asset = compiler.GetAsset();
    for (const auto& sub : asset->subshaders)
    {
        for (const auto& pass : sub.passes)
        {
            if (pass.name == pass_name)
            {
                if (pass.programs.empty())
                {
                    std::cout << "  [FAIL] pass '" << pass_name << "' parsed but has no program\n";
                    ++failures;
                    return;
                }
                std::cout << "  [ OK ] pass '" << pass_name << "' (" << pass.programs.size()
                          << " program(s), light_mode='" << pass.light_mode << "')\n";
                return;
            }
        }
    }

    std::cout << "  [FAIL] pass '" << pass_name << "' -- missing after parse\n";
    ++failures;
}

// Regression guard: Compile(subshader, pass, stage) must compile THAT pass.
//
// Compile() used to hand the whole .shader file to
// ExtractHlslFromShaderLabFile(), which returns the first CGPROGRAM block it
// finds -- so every pass of a multi-pass shader produced pass 0's DXIL.
void RequirePassSelection(const std::filesystem::path& shader,
                          const std::string& pass0_entry,
                          const std::string& pass1_entry,
                          int& checks,
                          int& failures)
{
    ++checks;

    ShaderLabDx12Compiler compiler;
    if (!compiler.LoadFromFile(shader.generic_string()))
    {
        std::cout << "  [FAIL] pass selection -- shader failed to parse\n";
        ++failures;
        return;
    }

    const CompiledShaderModuleDx12 p0 = compiler.Compile(0, 0, "vertex", {});
    const CompiledShaderModuleDx12 p1 = compiler.Compile(0, 1, "vertex", {});

    if (!p0.success || !p1.success)
    {
        std::cout << "  [FAIL] pass selection -- compile failed\n";
        if (!p0.success)
        {
            std::cout << "         pass 0: " << p0.error_message << "\n";
        }
        if (!p1.success)
        {
            std::cout << "         pass 1: " << p1.error_message << "\n";
        }
        ++failures;
        return;
    }

    if (p0.entry_point != pass0_entry || p1.entry_point != pass1_entry)
    {
        std::cout << "  [FAIL] pass selection -- expected entries '" << pass0_entry << "' / '"
                  << pass1_entry << "', got '" << p0.entry_point << "' / '" << p1.entry_point
                  << "'\n";
        ++failures;
        return;
    }

    if (p0.dxil_code == p1.dxil_code)
    {
        std::cout << "  [FAIL] pass selection -- pass 0 and pass 1 produced identical DXIL\n";
        ++failures;
        return;
    }

    std::cout << "  [ OK ] pass 0 -> '" << p0.entry_point << "' (" << p0.dxil_code.size()
              << " B), pass 1 -> '" << p1.entry_point << "' (" << p1.dxil_code.size() << " B)\n";
}

// Regression guard: a project-relative #include must resolve through
// SetIncludeDirectory().
//
// Unity resolves `#include "Foo/Bar.h"` against the project's shader include
// root (Assets/Shaders), NOT against the .shader's own directory. ZEngine's
// Compile() only ever searched the .shader's own directory -- and the
// SetIncludeDirectory() that would have fixed it had zero callers, while
// PrecompileAll()'s `include_paths` parameter was discarded outright
// (`(void)include_paths;`). So any shader using a project-relative include
// path failed with "fatal error: ... file not found".
//
// Both halves matter: without the root it MUST fail (otherwise this test
// proves nothing), and with it it MUST succeed.
void RequireIncludeRoot(const std::filesystem::path& shader,
                        const std::filesystem::path& include_root,
                        int& checks,
                        int& failures)
{
    // --- negative: no include root -> header must not be found ------------
    ++checks;
    {
        ShaderLabDx12Compiler compiler;
        if (!compiler.LoadFromFile(shader.generic_string()))
        {
            std::cout << "  [FAIL] include root -- shader failed to parse\n";
            ++failures;
            return;
        }

        const CompiledShaderModuleDx12 r = compiler.Compile(0, 0, "vertex", {});
        if (r.success)
        {
            std::cout << "  [FAIL] include root -- compiled WITHOUT an include root; "
                         "expected the project-relative #include to be unresolvable\n";
            ++failures;
        }
        else
        {
            std::cout << "  [ OK ] without include root: rejected as expected\n";
        }
    }

    // --- positive: SetIncludeDirectory() -> header must resolve -----------
    ++checks;
    {
        ShaderLabDx12Compiler compiler;
        compiler.SetIncludeDirectory(include_root);
        if (!compiler.LoadFromFile(shader.generic_string()))
        {
            std::cout << "  [FAIL] include root -- shader failed to parse\n";
            ++failures;
            return;
        }

        const CompiledShaderModuleDx12 r = compiler.Compile(0, 0, "vertex", {});
        if (!r.success)
        {
            std::cout << "  [FAIL] include root -- compile failed with the root set: "
                      << r.error_message << "\n";
            ++failures;
        }
        else
        {
            std::cout << "  [ OK ] with include root: compiled (" << r.dxil_code.size()
                      << " B)\n";
        }
    }
}

// Regression guard: EVERY pass of EVERY subshader must compile.
//
// RunShaderFile()/TryCompile() only ever exercise pass 0, so a broken pass 1
// (a different entry point, a missing texture declaration, an unported
// include) would pass the suite unnoticed. example_standard.shader has two
// passes (GBuffer + ForwardLit) with different fragment entry points and
// different include sets -- exactly the shape that hides that class of bug.
//
// `expected_passes` is checked too: a parser regression that silently drops a
// pass must not be masked by the loop simply not running.
void RequireEveryPass(const std::filesystem::path& shader,
                      const std::filesystem::path& include_root,
                      size_t expected_passes,
                      int& checks,
                      int& failures)
{
    ShaderLabDx12Compiler compiler;
    compiler.SetIncludeDirectory(include_root);

    if (!compiler.LoadFromFile(shader.generic_string()))
    {
        ++checks;
        ++failures;
        std::cout << "  [FAIL] parse: " << compiler.GetParseError() << "\n";
        return;
    }

    auto asset = compiler.GetAsset();
    if (!asset)
    {
        ++checks;
        ++failures;
        std::cout << "  [FAIL] no asset produced\n";
        return;
    }

    size_t total_passes = 0;
    for (const auto& sub : asset->subshaders)
    {
        total_passes += sub.passes.size();
    }

    ++checks;
    if (total_passes != expected_passes)
    {
        ++failures;
        std::cout << "  [FAIL] parsed " << total_passes << " passes, expected "
                  << expected_passes << "\n";
        return;
    }
    std::cout << "  [ OK ] parsed " << total_passes << " passes\n";

    for (size_t i = 0; i < asset->subshaders.size(); ++i)
    {
        for (size_t j = 0; j < asset->subshaders[i].passes.size(); ++j)
        {
            const auto& pass = asset->subshaders[i].passes[j];
            const std::string tag = "SubShader[" + std::to_string(i) + "] Pass[" +
                                    std::to_string(j) + "] '" + pass.name + "'";

            for (const char* stage : {"vertex", "fragment"})
            {
                ++checks;
                const CompiledShaderModuleDx12 r = compiler.Compile(i, j, stage, {});
                if (r.success)
                {
                    std::cout << "  [ OK ] " << tag << " " << stage << " -> DXIL "
                              << r.dxil_code.size() << " B, entry '" << r.entry_point
                              << "'\n";
                }
                else
                {
                    ++failures;
                    std::cout << "  [FAIL] " << tag << " " << stage << "\n         "
                              << r.error_message << "\n";
                }
            }
        }
    }
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

    // ------------------------------------------------------------------
    // 0. Regression: digit-leading property types must survive the lexer.
    // ------------------------------------------------------------------
    std::cout << "\n---- [0] Regression: 2D/3D property types ----\n";
    RequireProperty(Fixture("Spine-Skeleton-Tint.shader"), "_MainTex", checks, failures);
    {
        const std::filesystem::path props = Fixture("diag_props2.shader");
        static const char* const names[] = {
            "_T2D_black", "_T2D_empty", "_T2D_white", "_T2D_nospace", "_T3D_empty", "_TCube_empty"};
        for (const char* n : names)
        {
            RequireProperty(props, n, checks, failures);
        }
    }

    // ------------------------------------------------------------------
    // 0b. Regression: unrecognised SubShader-level commands must not eat the
    //     Pass that follows them.
    // ------------------------------------------------------------------
    std::cout << "\n---- [0b] Regression: Pass survives unknown SubShader commands ----\n";
    RequirePass(Fixture("Spine-Skeleton-Tint-CasterOnly.shader"), "Caster", checks, failures);

    // ------------------------------------------------------------------
    // 0c. Regression: Compile(subshader, pass, stage) must honour `pass`.
    // ------------------------------------------------------------------
    std::cout << "\n---- [0c] Regression: pass_index selects the right program ----\n";
    RequirePassSelection(Fixture("diag_two_passes.shader"), "vertA", "vertB", checks, failures);

    // ------------------------------------------------------------------
    // 0d. Regression: a project-relative #include must resolve through
    //     SetIncludeDirectory() (and must NOT resolve without it).
    // ------------------------------------------------------------------
    std::cout << "\n---- [0d] Regression: project include root ----\n";
    RequireIncludeRoot(
        Fixture("inc_root/shaders/inc_root_proj.shader"),
        std::filesystem::path(ZENGINE_SHADERLAB_UNITY_COMPAT_TEST_DIR),
        checks,
        failures);

    // ------------------------------------------------------------------
    // 0e. Regression: the engine's own HLSL sample shader must compile, and
    //     EVERY pass must compile -- not just pass 0.
    //
    // This is what pins down the HLSL `shader/include/common.h` (Unity-style
    // transform/buffer/texture shims) and the dual-language gbuffer.h: if
    // either regresses, or if a later pass stops compiling, this fails.
    // ------------------------------------------------------------------
    std::cout << "\n---- [0e] Regression: engine/shader/example_standard.shader (all passes) ----\n";
    RequireEveryPass(std::filesystem::path(ZENGINE_SHADERLAB_ENGINE_SHADER_DIR)
                         / "shader" / "example_standard.shader",
                     std::filesystem::path(ZENGINE_SHADERLAB_ENGINE_SHADER_DIR),
                     2,
                     checks,
                     failures);

    // Ad-hoc mode: `ShaderLabUnityCompatTest <file> [<file>...] [--include-root=<dir>]`
    // parses and compiles pass 0 of each file. Lets you drop a diagnostic fixture in
    // and re-run without rebuilding.
    //
    // `--include-root=<dir>` sets the project include root, which is what a shader
    // writing a project-relative path (`#include "shader/include/common.h"`) needs.
    // engine/shader/example_standard.shader is exactly that case -- its root is the
    // engine/ directory.
    if (argc > 1)
    {
        std::filesystem::path include_root;
        for (int a = 1; a < argc; ++a)
        {
            const std::string arg = argv[a];
            const std::string prefix = "--include-root=";
            if (arg.rfind(prefix, 0) == 0)
            {
                include_root = arg.substr(prefix.size());
                continue;
            }
            if (arg.rfind("--", 0) == 0)
            {
                continue;
            }

            std::filesystem::path p = argv[a];
            if (!p.is_absolute())
            {
                p = std::filesystem::path(ZENGINE_SHADERLAB_UNITY_COMPAT_TEST_DIR) / "unity_compat" / p;
            }

            std::cout << "\n---- " << p.filename().generic_string() << " ----\n";
            std::cout << "  file: " << p.generic_string() << "\n";

            ShaderLabDx12Compiler compiler;
            if (!include_root.empty())
            {
                compiler.SetIncludeDirectory(include_root);
            }
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
