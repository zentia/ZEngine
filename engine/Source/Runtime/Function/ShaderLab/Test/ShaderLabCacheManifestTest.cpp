// =====================================================================
// ShaderLab SPIR-V cache dependency-manifest smoke-test
// ---------------------------------------------------------------------
// Pins down the invalidation rule for on-disk SPIR-V blobs.
//
// The rule this replaces -- "reuse the blob while it is newer than the
// .shader" -- has one hole that bites constantly in a real shader project:
// editing a SHARED header (UnityCG.cginc, common.h, ...) does not touch the
// .shader, so every cached blob still looked fresh and the renderer kept
// replaying SPIR-V compiled from the previous content of that header.
//
// Each case below maps to one way a cache entry must be rejected:
//
//   1. A dependency edited after the blob was written.          -> [edit]
//   2. A dependency moved BACKWARDS in time (git checkout of an
//      older revision). An "is the blob newer?" test passes here;
//      comparing recorded-vs-current mtime does not.             -> [revert]
//   3. A dependency deleted.                                    -> [delete]
//   4. No manifest at all (entry from an older build).          -> [missing]
//   5. Manifest unreadable / malformed / empty.                 -> [malformed]
//                                                                  [empty]
//
// ...and two ways it must be KEPT: nothing changed ([fresh]), and a file
// that was never an input changed ([unrelated]).
//
// Opt in with:
//   cmake -B build -DZENGINE_BUILD_SHADERLAB_CACHE_TEST=ON
//   cmake --build build --target ShaderLabCacheManifestTest
//
// Exit code is the verdict: 0 = every check passed.
// =====================================================================

#include "Runtime/Function/ShaderLab/ShaderLabCacheManifest.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

using namespace ZEngine::ShaderLab;

namespace
{

int g_checks = 0;
int g_failures = 0;

std::filesystem::path ScratchRoot()
{
    // Under the system temp dir: the test writes, edits and deletes files,
    // and must leave nothing behind in the source tree.
    return std::filesystem::temp_directory_path() / "zengine_shaderlab_cache_manifest_test";
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

void CheckContains(const std::string& haystack, const std::string& needle, const std::string& label)
{
    Check(haystack.find(needle) != std::string::npos,
          label + " (expected to find '" + needle + "')",
          "--- text ---\n" + haystack);
}

void WriteText(const std::filesystem::path& p, const std::string& text)
{
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
    ofs << text;
}

std::string ReadText(const std::filesystem::path& p)
{
    std::ifstream ifs(p, std::ios::binary);
    if (!ifs.is_open())
    {
        return {};
    }
    std::string out((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    return out;
}

size_t CountLines(const std::string& text)
{
    size_t n = 0;
    for (const char c : text)
    {
        if (c == '\n')
        {
            ++n;
        }
    }
    return n;
}

// Moves a file's mtime by `delta` and returns true when the move actually
// took effect (guards against a filesystem with coarse timestamps silently
// making the test vacuous).
bool ShiftMtime(const std::filesystem::path& p, std::chrono::hours delta)
{
    std::error_code ec;
    const auto before = std::filesystem::last_write_time(p, ec);
    if (ec)
    {
        return false;
    }
    std::filesystem::last_write_time(p, before + delta, ec);
    if (ec)
    {
        return false;
    }
    const auto after = std::filesystem::last_write_time(p, ec);
    return !ec && after != before;
}

}  // namespace

int main()
{
    std::cout << "=== ShaderLab SPIR-V cache manifest smoke test ===\n";

    const std::filesystem::path root = ScratchRoot();
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    std::cout << "scratch dir: " << root.generic_string() << "\n";

    // ------------------------------------------------------------------
    // [path] Manifest sits next to the blob, appended to its filename.
    // ------------------------------------------------------------------
    std::cout << "\n---- [path] manifest path derivation ----\n";
    {
        const std::filesystem::path blob = root / "abc_0_0_deadbeef_vertex.spv";
        const std::filesystem::path dep = CacheManifestPathFor(blob);
        Check(dep == std::filesystem::path(root / "abc_0_0_deadbeef_vertex.spv.dep"),
              "blob 'x.spv' -> manifest 'x.spv.dep'",
              "got: " + dep.generic_string());
        Check(dep.filename().string().find(".spv.") != std::string::npos,
              "suffix is appended, not turned into a directory level");
    }

    // ------------------------------------------------------------------
    // [fresh] / [edit] / [revert] / [delete] / [unrelated]
    // ------------------------------------------------------------------
    const std::filesystem::path shader = root / "proj" / "example.shader";
    // A path with a space, on purpose: the manifest is a line format and
    // must survive it.
    const std::filesystem::path header = root / "inc dir" / "common.h";
    const std::filesystem::path bystander = root / "proj" / "notes.txt";
    const std::filesystem::path blob = root / "cache" / "aaaa_0_0_bbbb_fragment.spv";

    WriteText(shader, "Shader \"Example\" { SubShader { Pass { } } }\n");
    WriteText(header, "float4 CommonVal() { return 0; }\n");
    WriteText(bystander, "not an input\n");

    std::cout << "\n---- [write] manifest records the full input set ----\n";
    {
        const bool ok = WriteCacheManifest(blob, {shader, header});
        Check(ok, "WriteCacheManifest succeeds");
        std::error_code exists_ec;
        Check(std::filesystem::exists(CacheManifestPathFor(blob), exists_ec),
              "manifest file exists next to the blob");

        const std::string text = ReadText(CacheManifestPathFor(blob));
        Check(CountLines(text) == 2,
              "manifest has exactly one line per dependency (got " +
                  std::to_string(CountLines(text)) + ")");
        CheckContains(text, "example.shader", "manifest records the .shader");
        CheckContains(text, "common.h", "manifest records the included header");
        CheckContains(text, "inc dir/common.h", "path with a space round-trips intact");
    }

    std::cout << "\n---- [dedupe] a dependency listed twice is recorded once ----\n";
    {
        const std::filesystem::path dup_blob = root / "cache" / "dup.spv";
        Check(WriteCacheManifest(dup_blob, {shader, header, shader, header}),
              "WriteCacheManifest tolerates duplicates");
        const std::string text = ReadText(CacheManifestPathFor(dup_blob));
        Check(CountLines(text) == 2,
              "duplicate dependencies collapse (got " + std::to_string(CountLines(text)) + " lines)");
    }

    std::cout << "\n---- [fresh] nothing changed -> reuse ----\n";
    {
        std::string reason;
        const CacheManifestStatus status = ValidateCacheManifest(blob, &reason);
        Check(status == CacheManifestStatus::Fresh, "unchanged inputs validate as Fresh", reason);
        Check(ShouldReuseBlob(blob), "ShouldReuseBlob agrees");
    }

    std::cout << "\n---- [unrelated] a non-input file changed -> still reuse ----\n";
    {
        WriteText(bystander, "edited, but nobody included this\n");
        std::string reason;
        Check(ValidateCacheManifest(blob, &reason) == CacheManifestStatus::Fresh,
              "changing a file that was never an input does not invalidate",
              reason);
    }

    std::cout << "\n---- [edit] included header edited -> rebuild ----\n";
    {
        // This is the defect being fixed: the .shader is untouched, so an
        // "is the blob newer than the .shader?" test says reuse.
        Check(ShiftMtime(header, std::chrono::hours(1)), "header mtime moved forward");
        std::string reason;
        Check(ValidateCacheManifest(blob, &reason) == CacheManifestStatus::Stale,
              "editing an included header invalidates the blob",
              reason);
        Check(ShouldReuseBlob(blob) == false, "ShouldReuseBlob says no");
        CheckContains(reason, "changed", "reason names the cause");

        // Restoring the header's content is not enough -- the blob really is
        // rebuilt, which is what we want; prove it by re-writing the manifest
        // and seeing the entry go Fresh again.
        Check(WriteCacheManifest(blob, {shader, header}), "manifest rewritten after rebuild");
        Check(ValidateCacheManifest(blob) == CacheManifestStatus::Fresh,
              "re-validates as Fresh once rebuilt");
    }

    std::cout << "\n---- [revert] dependency moved BACKWARDS in time -> rebuild ----\n";
    {
        // An mtime-ordering test cannot see this: the blob is still newer
        // than every input, yet the inputs are not the ones it was built
        // from. Recorded-vs-current catches it.
        Check(ShiftMtime(header, std::chrono::hours(-5)), "header mtime moved backwards");
        std::string reason;
        Check(ValidateCacheManifest(blob, &reason) == CacheManifestStatus::Stale,
              "a dependency reverted to an older revision invalidates the blob",
              reason);
    }

    std::cout << "\n---- [delete] dependency deleted -> rebuild ----\n";
    {
        Check(WriteCacheManifest(blob, {shader, header}), "manifest rewritten");
        std::error_code remove_ec;
        std::filesystem::remove(header, remove_ec);
        Check(!remove_ec && !std::filesystem::exists(header, remove_ec), "header deleted");
        std::string reason;
        Check(ValidateCacheManifest(blob, &reason) == CacheManifestStatus::Stale,
              "a vanished dependency invalidates the blob",
              reason);
        CheckContains(reason, "vanished", "reason names the cause");
        WriteText(header, "float4 CommonVal() { return 0; }\n");  // restore
    }

    std::cout << "\n---- [missing] no manifest (entry from an older build) -> rebuild ----\n";
    {
        const std::filesystem::path legacy = root / "cache" / "legacy.spv";
        WriteText(legacy, "\x03\x02\x23\x07not-really-spirv");  // blob, no manifest
        std::string reason;
        Check(std::filesystem::exists(legacy), "legacy blob written without a manifest");
        Check(ValidateCacheManifest(legacy, &reason) == CacheManifestStatus::Missing,
              "a blob with no manifest reports Missing",
              reason);
        Check(ShouldReuseBlob(legacy) == false,
              "Missing is not reusable -- we cannot prove freshness");
    }

    std::cout << "\n---- [malformed] unreadable manifest content -> rebuild ----\n";
    {
        const std::filesystem::path bad = root / "cache" / "bad.spv";
        WriteText(bad, "blob");
        WriteText(CacheManifestPathFor(bad), "no tab on this line\n");
        std::string reason;
        Check(ValidateCacheManifest(bad, &reason) == CacheManifestStatus::Stale,
              "a malformed manifest is treated as stale",
              reason);
    }

    std::cout << "\n---- [empty] zero-byte manifest -> rebuild ----\n";
    {
        const std::filesystem::path empty = root / "cache" / "empty.spv";
        WriteText(empty, "blob");
        WriteText(CacheManifestPathFor(empty), "");
        std::string reason;
        Check(ValidateCacheManifest(empty, &reason) == CacheManifestStatus::Stale,
              "a manifest recording no dependencies is treated as stale",
              reason);
    }

    std::cout << "\n---- [empty-deps] writing an empty dependency set is refused ----\n";
    {
        std::string error;
        const bool ok = WriteCacheManifest(root / "cache" / "nodeps.spv", {}, &error);
        Check(!ok, "WriteCacheManifest rejects an empty dependency set");
        Check(!error.empty(), "and explains why", error);
    }

    // ------------------------------------------------------------------
    std::filesystem::remove_all(root, ec);
    if (ec)
    {
        std::cout << "warning: could not remove scratch dir " << root.generic_string() << "\n";
    }

    std::cout << "\n=== " << (g_checks - g_failures) << "/" << g_checks
              << " checks passed ===\n";
    return g_failures == 0 ? 0 : 1;
}
