#pragma once

// =============================================================================
// SourceAssetRegistry -- PR-AI3 backbone for AutoReimport.
//
// Records, for every `.zasset` produced by AssetsMenu::ConvertAsset, the
// import-time tuple (source absolute path, source mtime). Persisted as a
// single JSON sidecar at `<Project>/AssetRegistry/source_registry.json`,
// checked into VCS, mirroring the pattern ScriptRegistry uses for
// `script_registry.json` (see AGENTS.md 2.1 -- "no .meta sidecars; one
// per-project JSON, checked in").
//
// Why a sidecar JSON instead of inlining the source path into the
// `.zasset` binary header:
//
//   * `AssetFileHeader` is a fixed 176-byte struct with reserved[4]
//     (32 bytes). A 240-byte source path + 8-byte mtime would not fit,
//     and growing the header would either break the static_assert that
//     pins it to 176 bytes or require a bumped version that every
//     existing reader (`scanSingleAsset`, `SerializedFile::ReadHeader`)
//     would need to special-case. The JSON path adds the data WITHOUT
//     touching the on-disk binary format, so every existing .zasset
//     stays readable bit-for-bit.
//
//   * The JSON is human-diffable, manually fixable, and naturally
//     three-way-merge-able for entries -- same trade we already accepted
//     for script_registry.json.
//
//   * Collateral: textures imported BEFORE PR-AI3 will not have entries
//     here, which means AutoReimport silently does nothing for them
//     (correct behaviour: we don't know their source path). The user
//     re-imports once (which writes a fresh entry) and from then on
//     auto-reimport works.
//
// Threading: Initialize / Shutdown / record / lookup / forEach / removeEntry
// all take an internal mutex. The ProjectWindow drop-import path may call
// `Record()` from the editor main thread; the focus-callback driven
// `EditorAssetManager::tickReimportQueue` calls `ForEach()` from the same
// thread; both are safe under the same lock.
//
// This class is NOT an IEngineSystem. It is owned by EditorAssetManager
// (composition) so its lifetime is bounded by the editor's asset
// subsystem. Runtime code never touches it -- consult AssetRegistry +
// `.zasset` GUID instead. See AGENTS.md 2.10 PR-AI3 section.
// =============================================================================

#include <chrono>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

class SourceAssetRegistry
{
public:
    struct Entry
    {
        // ABSOLUTE path on the originating machine. Stored verbatim
        // (forward-slash via generic_string for cross-platform stability),
        // NOT canonicalised -- canonicalising at record time would resolve
        // symlinks the user might rely on (e.g. an artist's "current
        // project" symlink). Stat'ing this path on focus is what tells
        // us whether the source still exists.
        std::string source_path;

        // ns-since-epoch of the source file's last_write_time AT THE
        // TIME WE RAN THE IMPORT. We compare this against the live
        // last_write_time on focus to decide whether to reimport. Using
        // ns precision matches std::filesystem::file_time_type's clock
        // on every platform we care about (Windows FILETIME = 100ns,
        // ext4 mtime = ns, APFS = ns).
        int64_t source_mtime_ns = 0;
    };

    /// Resolve `<Project>/AssetRegistry/source_registry.json` from
    /// `ProjectInfo` and load whatever entries are already there. Safe
    /// to call when no project is loaded -- becomes a no-op and `record`
    /// / `forEach` will do nothing. Returns false only on hard JSON
    /// parse errors (a missing file is fine and returns true).
    bool Initialize();

    /// Persist any pending entries to disk and clear in-memory state.
    /// Call this from EditorAssetManager::Shutdown(). Like ScriptRegistry,
    /// every mutator already calls SaveToDisk() so this is mostly a
    /// safety net if a session ends without a graceful Shutdown.
    void Shutdown();

    /// Return the absolute on-disk path of the registry JSON. Empty
    /// when no project is loaded.
    std::filesystem::path GetRegistryFilePath() const;

    // ---- Mutation -----------------------------------------------------------

    /// Record (or update) the entry for a `.zasset` file. `zasset_path`
    /// is interpreted as project-relative -- we normalise to forward
    /// slashes and store as-is so the JSON is portable across
    /// machines that share a project via VCS. `source_path` should
    /// be absolute on the recording machine; we stat it once here to
    /// fill source_mtime_ns. Saves the JSON synchronously.
    ///
    /// Idempotent: calling with the same (zasset_path, source_path)
    /// rewrites the mtime to the current value (which is exactly what
    /// we want immediately after a successful `import` -- the new mtime
    /// is the new baseline for the next focus-driven check).
    void Record(const std::filesystem::path& zasset_abs_path,
                const std::filesystem::path& source_abs_path);

    /// Remove the entry for `zasset_path`. Called from
    /// EditorAssetManager::OnFileDeleted (the m_FileWatcher delete
    /// handler) so that a user deleting a `.zasset` from disk also
    /// drops the AutoReimport mapping. No-op if the entry doesn't
    /// exist. Persists.
    void RemoveEntry(const std::filesystem::path& zasset_abs_path);

    // ---- Lookup -------------------------------------------------------------

    /// Find the entry for `zasset_abs_path`. Returns std::nullopt if no
    /// entry exists (= imported before PR-AI3, or import was rolled
    /// back). Does NOT stat the source file.
    std::optional<Entry> Lookup(const std::filesystem::path& zasset_abs_path) const;

    /// Iterate every (zasset_abs_path, entry) pair under the lock.
    /// `visitor` MUST NOT call back into mutators (record / removeEntry)
    /// or it will deadlock; collect targets into a local vector and act
    /// on them after the call returns. This is the same convention
    /// AssetRegistry::ForEach uses.
    void ForEach(const std::function<void(const std::filesystem::path& zasset_abs_path,
                                          const Entry& entry)>& visitor) const;

    /// Convenience: stat `entry.source_path` and report whether the
    /// source has been modified relative to entry.source_mtime_ns.
    /// Returns:
    ///   * `true`  if the file exists and its mtime differs from the
    ///             stored value (newer OR older -- a backup restore
    ///             could legitimately move mtime backwards).
    ///   * `false` if the file does not exist (caller should warn the
    ///             user, NOT trigger reimport) or the mtimes match.
    /// `out_current_mtime_ns` (if non-null) receives the live mtime
    /// even when the function returns false, so the caller can update
    /// the stored entry on a successful reimport without a second stat.
    static bool HasSourceChanged(const Entry& entry, int64_t* out_current_mtime_ns = nullptr);

    /// Update only the stored mtime for a zasset entry. Called from
    /// `tickReimportQueue` after a reimport completes successfully so
    /// the next focus check uses the new mtime as baseline. Persists.
    /// No-op if no entry exists (this matters: if the source file was
    /// the one we removed during shutdown, we don't want to recreate
    /// the entry).
    void UpdateMtime(const std::filesystem::path& zasset_abs_path, int64_t new_mtime_ns);

private:
    // Path normalisation: forward-slash, lower-cased on Windows only
    // (the only platform whose filesystem is case-insensitive in
    // practice). Mirrors ScriptRegistry::NormaliseRelPath, which is
    // private. We can't share the helper across modules (Editor cannot
    // depend on Runtime internals) so we re-implement it.
    static std::string NormaliseKey(const std::filesystem::path& zasset_abs_path);

    static int64_t MtimeNs(const std::filesystem::path& abs_path);

    bool LoadFromDisk();
    bool SaveToDisk() const;

    // Keyed by normalised absolute path of the .zasset file. The key
    // matters for stable lookups across editor sessions; the source
    // path is just data inside the entry.
    std::unordered_map<std::string, Entry> m_Entries;

    std::filesystem::path m_RegistryFile;
    mutable std::mutex m_Mutex;
};
