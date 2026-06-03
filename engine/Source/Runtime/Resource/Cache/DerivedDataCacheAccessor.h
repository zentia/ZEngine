#pragma once

// =============================================================================
// DerivedDataCacheAccessor
// -----------------------------------------------------------------------------
// Process-wide, lazily-opened Derived Data Cache. The cache is an LMDB store
// rooted at <Project>/Intermediate/DDC/ (resolved from the active ProjectInfo
// system). It holds platform-specific cooked artifacts -- compressed+mipped
// texture blobs today -- keyed by (cache_type, asset GUID, settings/platform/
// encoder-version hash). See DerivedDataCache.h for the key/value contract and
// AGENTS.md for the source-vs-cooked / DDC keying convention.
//
// Lifetime: the backing LMDB env is opened once on first GetDerivedDataCache()
// and reused for the process. ShutdownDerivedDataCache() closes it (called at
// engine shutdown; also handy for tests). Reopening after shutdown is allowed.
//
// Returns nullptr when no project is loaded (project path unknown) or the LMDB
// env could not be opened -- callers MUST null-check and treat a null cache as
// "always a miss" (cooking still works, just uncached).
// =============================================================================

#include "DerivedDataCache.h"

#include <cstdint>
#include <filesystem>

namespace Runtime
{
    // Lazily open (if needed) and return the process-wide DDC. May be nullptr
    // (no project / open failure). Thread-safe.
    IDerivedDataCache* GetDerivedDataCache();

    // Explicit open at a caller-chosen directory (used by tests and tools that
    // run without a ProjectInfo). Idempotent: a second call with the same path
    // is a no-op; a call with a different path closes and reopens. Thread-safe.
    IDerivedDataCache* OpenDerivedDataCacheAt(const std::filesystem::path& dir, size_t max_size_mb = 1024);

    // Close the backing LMDB env. Safe to call when not open. Thread-safe.
    void ShutdownDerivedDataCache();

    // Compose a stable cache_key string from the inputs that, when changed,
    // must invalidate a cooked variant: the platform tag, an opaque settings
    // hash (e.g. FNV-1a of the effective TextureImporterSettings), and the
    // encoder version. Returns a compact lowercase-hex token.
    std::string MakeDDCCacheKey(const std::string& platform_tag,
                                uint64_t settings_hash,
                                uint32_t encoder_version);
}  // namespace Runtime
