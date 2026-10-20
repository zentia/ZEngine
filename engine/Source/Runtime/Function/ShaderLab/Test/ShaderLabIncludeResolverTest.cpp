// =====================================================================
// ShaderLab #include resolver smoke-test
// ---------------------------------------------------------------------
// Pins down ResolveIncludes() -- the shared (Vulkan-free) expansion used
// by the SPIR-V ShaderLab compiler.
//
// The implementation it replaced had these defects, each of which has a
// case below:
//
//   1. Regex only matched the quoted form, so `#include <foo.h>` was
//      left in the source verbatim.                              -> [angle]
//   2. `#pragma once` was not honoured, so a guarded header pulled
//      in twice was emitted twice (duplicate definition).         -> [once]
//   3. A missing include was replaced by
//          // WARNING: Could not find include: foo.h
//      and compilation continued against a source the author never
//      wrote.                                                    -> [missing]
//                                                                 [no-warn]
//   4. After result.replace(), the code read the now-invalidated
//      `match[0].second` and then restarted the scan from cbegin()
//      -- quadratic, and UB in principle.                        -> [nested]
//   5. `include_content.empty()` was the not-found sentinel, so an
//      EMPTY but existing header was reported as missing.        -> [empty]
//   6. Quoted includes fell back to the process CWD rather than the
//      including file's directory.                               -> [owning]
//   7. No cycle detection: a self/mutually including header
//      recursed until the stack blew.                            -> [cycle]
//
// Opt in with:
//   cmake -B build -DZENGINE_BUILD_SHADERLAB_INCLUDE_TEST=ON
//   cmake --build build --target ShaderLabIncludeResolverTest
//
// Exit code is the verdict: 0 = every check passed.
// =====================================================================

#include "Runtime/Function/ShaderLab/ShaderLabIncludeProcessor.h"

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

using namespace ZEngine::ShaderLab;

namespace
{

#ifndef ZENGINE_SHADERLAB_INCLUDE_TEST_DIR
#define ZENGINE_SHADERLAB_INCLUDE_TEST_DIR "."
#endif

int g_checks = 0;
int g_failures = 0;

std::filesystem::path FixtureRoot()
{
    return std::filesystem::path(ZENGINE_SHADERLAB_INCLUDE_TEST_DIR) / "include_proc";
}

std::filesystem::path LibDir() { return FixtureRoot() / "lib"; }
std::filesystem::path OwningDir() { return FixtureRoot() / "owning"; }

std::vector<std::string> LibOnly() { return {LibDir().generic_string()}; }

int CountOccurrences(const std::string& haystack, const std::string& needle)
{
    int count = 0;
    size_t pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string::npos)
    {
        ++count;
        pos += needle.size();
    }
    return count;
}

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

void CheckContains(const std::string& haystack,
                   const std::string& needle,
                   int expected,
                   const std::string& label)
{
    const int actual = CountOccurrences(haystack, needle);
    Check(actual == expected,
          label + " (expected " + std::to_string(expected) + " occurrence(s) of '" + needle +
              "', got " + std::to_string(actual) + ")",
          "--- output ---\n" + haystack);
}

}  // namespace

int main()
{
    std::cout << "=== ShaderLab include-resolver smoke test ===\n";
    std::cout << "fixture dir : " << FixtureRoot().generic_string() << "\n";

    // ------------------------------------------------------------------
    // [angle] Both #include forms are recognised.
    // ------------------------------------------------------------------
    std::cout << "\n---- [angle] quoted and angled forms ----\n";
    {
        const IncludeResolution r = ResolveIncludes("#include \"once.h\"\n", LibOnly(), {});
        Check(r.success, "quoted form resolves", r.error_message);
        CheckContains(r.code, "once_val", 1, "quoted form inlined");
    }
    {
        const IncludeResolution r = ResolveIncludes("#include <angle.h>\n", LibOnly(), {});
        Check(r.success, "angled form resolves", r.error_message);
        CheckContains(r.code, "angle_val", 1, "angled form inlined");
    }
    {
        // Angled form must NOT search the including file's directory --
        // that is the whole point of <> vs "".
        const IncludeResolution r = ResolveIncludes("#include <local.h>\n", LibOnly(), OwningDir());
        Check(!r.success, "angled form does not search the owning directory");
    }

    // ------------------------------------------------------------------
    // [once] #pragma once suppresses the second inclusion.
    // ------------------------------------------------------------------
    std::cout << "\n---- [once] #pragma once ----\n";
    {
        const IncludeResolution r =
            ResolveIncludes("#include \"once.h\"\n#include \"once.h\"\n", LibOnly(), {});
        Check(r.success, "double inclusion of a guarded header succeeds", r.error_message);
        CheckContains(r.code, "once_val", 1, "guarded header emitted exactly once");
    }
    {
        // Unguarded headers are re-included. That is deliberate: it is what
        // the C preprocessor does, and headers without #pragma once are
        // expected to carry #ifndef guards.
        const IncludeResolution r =
            ResolveIncludes("#include \"plain.h\"\n#include \"plain.h\"\n", LibOnly(), {});
        Check(r.success, "double inclusion of an unguarded header succeeds", r.error_message);
        CheckContains(r.code, "plain_val", 2, "unguarded header emitted twice (by design)");
    }
    {
        // A file that guards itself AND includes itself is legal.
        const IncludeResolution r = ResolveIncludes("#include \"self_once.h\"\n", LibOnly(), {});
        Check(r.success, "self-inclusion under #pragma once is benign", r.error_message);
        CheckContains(r.code, "self_once_val", 1, "self-guarded file emitted once");
    }

    // ------------------------------------------------------------------
    // [missing] / [no-warn] A missing include is a hard error, never a
    // silently commented-out line.
    // ------------------------------------------------------------------
    std::cout << "\n---- [missing] unresolved include is a hard error ----\n";
    {
        const IncludeResolution r = ResolveIncludes("#include \"nope.h\"\n", LibOnly(), {});
        Check(!r.success, "missing include fails");
        Check(!r.error_message.empty(), "failure carries a diagnostic");
        Check(r.error_message.find("could not be resolved") != std::string::npos,
              "diagnostic names the failure",
              "error was: " + r.error_message);
        Check(r.code.find("WARNING") == std::string::npos,
              "no '// WARNING:' placeholder is emitted",
              "output was: " + r.code);
    }

    // ------------------------------------------------------------------
    // [cycle] Cycles are reported, not recursed into.
    // ------------------------------------------------------------------
    std::cout << "\n---- [cycle] cycle detection ----\n";
    {
        const IncludeResolution r = ResolveIncludes("#include \"self_cycle.h\"\n", LibOnly(), {});
        Check(!r.success, "direct self-inclusion fails");
        Check(r.error_message.find("cyclic") != std::string::npos,
              "diagnosed as a cycle",
              "error was: " + r.error_message);
    }
    {
        const IncludeResolution r = ResolveIncludes("#include \"cycle_a.h\"\n", LibOnly(), {});
        Check(!r.success, "mutual inclusion fails");
        Check(r.error_message.find("cyclic") != std::string::npos,
              "diagnosed as a cycle",
              "error was: " + r.error_message);
    }

    // ------------------------------------------------------------------
    // [empty] An empty header is found, not misreported as missing.
    // ------------------------------------------------------------------
    std::cout << "\n---- [empty] empty header ----\n";
    {
        const IncludeResolution r =
            ResolveIncludes("A\n#include \"empty.h\"\nB\n", LibOnly(), {});
        Check(r.success, "empty header resolves (not 'could not be resolved')", r.error_message);
        // The directive's own newline is kept, so an empty header expands to an
        // empty LINE rather than vanishing. That is deliberate: it keeps the
        // expanded source's line count aligned with the original, which is what
        // makes downstream diagnostics point at the line the author wrote.
        Check(r.code == "A\n\nB\n",
              "empty header contributes no text, but keeps its line",
              "got: [" + r.code + "]");
    }

    // ------------------------------------------------------------------
    // [owning] Quoted includes search the including file's directory.
    // ------------------------------------------------------------------
    std::cout << "\n---- [owning] search relative to the owning file ----\n";
    {
        const IncludeResolution r = ResolveIncludes("#include \"local.h\"\n", LibOnly(), OwningDir());
        Check(r.success, "quoted form searches the owning directory", r.error_message);
        CheckContains(r.code, "local_val", 1, "owning-directory header inlined");
    }

    // ------------------------------------------------------------------
    // [nested] Nested includes, and included_files for cache invalidation.
    // ------------------------------------------------------------------
    std::cout << "\n---- [nested] nesting and the included-file list ----\n";
    {
        const IncludeResolution r =
            ResolveIncludes("#include \"nested_parent.h\"\n", LibOnly(), {});
        Check(r.success, "nested include resolves", r.error_message);
        // Match the DEFINITION, not the bare name -- the parent calls
        // nested_child_val(), so the bare name legitimately appears twice.
        CheckContains(r.code, "float nested_parent_val()", 1, "parent inlined");
        CheckContains(r.code, "float nested_child_val()", 1, "child inlined transitively");

        bool saw_child = false;
        for (const auto& f : r.included_files)
        {
            if (f.filename() == "nested_child.h")
            {
                saw_child = true;
            }
        }
        Check(saw_child, "included_files lists the transitive header");
        Check(r.included_files.size() == 2,
              "included_files has one entry per distinct header (got " +
                  std::to_string(r.included_files.size()) + ")");
    }

    // ------------------------------------------------------------------
    // Directive parsing edge cases.
    // ------------------------------------------------------------------
    std::cout << "\n---- [parse] directive parsing ----\n";
    {
        const IncludeResolution r =
            ResolveIncludes("// #include \"nope.h\"\nfloat a;\n", LibOnly(), {});
        Check(r.success, "#include inside a comment is ignored", r.error_message);
        CheckContains(r.code, "// #include \"nope.h\"", 1, "commented directive preserved");
    }
    {
        const IncludeResolution r =
            ResolveIncludes("#includefoo \"nope.h\"\nfloat a;\n", LibOnly(), {});
        Check(r.success, "#includefoo is not an include directive", r.error_message);
    }
    {
        const IncludeResolution r = ResolveIncludes("#  include   \"once.h\"\n", LibOnly(), {});
        Check(r.success, "whitespace after '#' and around the target is allowed", r.error_message);
        CheckContains(r.code, "once_val", 1, "spaced directive inlined");
    }
    {
        const IncludeResolution r = ResolveIncludes("#pragma once\nfloat a;\n", LibOnly(), {});
        Check(r.success, "#pragma once at the root succeeds", r.error_message);
        Check(r.code.find("pragma once") == std::string::npos,
              "#pragma once is not emitted into the expanded source",
              "got: [" + r.code + "]");
    }
    {
        // No trailing newline on the last line must not lose that line.
        const IncludeResolution r = ResolveIncludes("A\n#include \"once.h\"\nB", LibOnly(), {});
        Check(r.success, "source without a trailing newline resolves", r.error_message);
        Check(r.code.find('B') != std::string::npos, "final line survives");
    }

    std::cout << "\n=== verdict: " << (g_failures == 0 ? "PASS" : "FAIL") << " ("
              << (g_checks - g_failures) << "/" << g_checks << " checks) ===\n";
    return g_failures == 0 ? 0 : 1;
}
