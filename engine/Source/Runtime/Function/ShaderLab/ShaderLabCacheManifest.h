#pragma once

// =====================================================================
// On-disk SPIR-V cache dependency manifest ("depfile")
// ---------------------------------------------------------------------
// A cached .spv blob is only valid for the exact set of input files it was
// built from. Those inputs are NOT just the .shader: once #include has been
// resolved, the real input set is
//
//     { the .shader source }  U  { every header ResolveIncludes() pulled in }
//
// The previous invalidation rule compared only the .shader's mtime against
// the .spv's mtime. Editing a shared header (UnityCG.cginc, common.h, ...)
// therefore left every cached blob looking fresh: the blob was reloaded and
// handed to the renderer while the author was staring at stale output, with
// no way to notice short of deleting the cache directory by hand.
//
// The fix is the standard makefile trick: write the input set next to the
// blob, and on load require every recorded file to still exist with the
// exact mtime it had at build time. Comparing recorded-vs-current (rather
// than current-vs-cache-file) also catches the cases an mtime ordering test
// cannot: a file restored to an OLDER revision (mtime moves backwards),
// a dependency that was deleted, and a cache entry whose blob was copied
// around without its inputs.
//
// Deliberately free of any render-backend dependency -- no Vulkan, no DX12
// -- so it compiles and can be unit-tested on ZENGINE_USE_VULKAN=OFF builds
// where ShaderLabCompiler.cpp itself is excluded from ZRuntime.
//
// Wire format: one dependency per line, "<mtime-micros>\t<path>". The mtime
// comes first so the split is unambiguous even if a path contains a tab.
// =====================================================================

#include <filesystem>
#include <string>
#include <vector>

namespace ZEngine::ShaderLab
{

    // "<cache_file>.dep". Written with path concatenation, not operator/,
    // so "abc.spv" becomes "abc.spv.dep" rather than "abc.spv/.dep".
    std::filesystem::path CacheManifestPathFor(const std::filesystem::path& cache_file);

    // Records `dependencies` next to `cache_file`. Overwrites any previous
    // manifest. Returns false (and fills `out_error`) when the manifest
    // could not be written -- in that case the caller should treat the blob
    // as uncached, because a later load has no way to prove freshness.
    bool WriteCacheManifest(const std::filesystem::path& cache_file,
                            const std::vector<std::filesystem::path>& dependencies,
                            std::string* out_error = nullptr);

    enum class CacheManifestStatus
    {
        // Every recorded dependency still exists and still carries the mtime
        // recorded at build time. The blob may be reused.
        Fresh,

        // At least one dependency changed, or the manifest is unreadable /
        // malformed. Recompile.
        Stale,

        // No manifest at all -- an entry written before manifests existed, or
        // someone deleted the file. Treated as Stale by ShouldReuseBlob()
        // because we cannot prove freshness; the recompilation that follows
        // rewrites the manifest and the next run hits.
        Missing,
    };

    CacheManifestStatus ValidateCacheManifest(const std::filesystem::path& cache_file,
                                              std::string* out_reason = nullptr);

    // The only predicate callers actually need: is the blob reusable?
    inline bool ShouldReuseBlob(const std::filesystem::path& cache_file,
                                std::string* out_reason = nullptr)
    {
        return ValidateCacheManifest(cache_file, out_reason) == CacheManifestStatus::Fresh;
    }

}  // namespace ZEngine::ShaderLab
