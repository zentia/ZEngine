#pragma once

#include "Runtime/Core/Base/EngineSystem.h"
#include "Runtime/Resource/ResType/Data/ScriptAsset.h"

#include <EASTL/unordered_map.h>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <typeindex>
#include <vector>

class ProjectInfo;

/**
 * @brief Centralised path<->GUID registry for all .ts/.js source files in
 *        a project.
 *
 * Why this class exists
 * ---------------------
 * ZEngine intentionally does NOT use Unity-style `.cs.meta` sidecars for
 * text-source assets (see AGENTS.md 2.1). The replacement strategy is
 * UE-inspired: a single per-project JSON, checked into VCS, that maps
 * relative-path <-> 128-bit GUID. ScriptRegistry owns this map.
 *
 * The GUID-by-deterministic-hash design guarantees that a fresh `git clone`
 * whose `script_registry.json` happens to be missing rebuilds *identical*
 * GUIDs from the rel-paths alone, so any pre-existing PPtr<ScriptAsset>
 * references survive even if the registry file is lost.
 *
 * On a rename:
 *   - On-disk:      <Scripts>/Old.ts -> <Scripts>/New.ts
 *   - Registry:     entry's `path` field updates in place; `guid` unchanged.
 *   - References:   PPtr<ScriptAsset> stores the GUID, so nothing breaks.
 *
 * This is the structural equivalent of a UE `UObjectRedirector` - the
 * identity is preserved across moves - just centralised in one JSON instead
 * of one redirector asset per move.
 *
 * Lifecycle
 * ---------
 * ScriptRegistry is an IEngineSystem registered after ProjectInfo (it
 * depends on ProjectInfo to resolve the project root). On Initialize() it:
 *   1. Loads `<Project>/AssetRegistry/script_registry.json` if present.
 *   2. Walks `<Project>/Scripts/` recursively, reconciling on-disk files
 *      with the loaded entries (additions, removals, renames detected via
 *      content hash for the rare manual-edit case).
 *   3. Rewrites the JSON if anything changed.
 *
 * Phase 6 will add FileSystemWatcher integration for live add/remove/rename
 * events; in P2 we only handle the cold-start scan.
 *
 * Threading: all public mutators take the internal mutex. Read-only lookups
 * also take a shared lock since FileSystemWatcher (Phase 6) writes from a
 * background thread.
 */
class ScriptRegistry : public IEngineSystem
{
public:
    // ---- IEngineSystem -----------------------------------------------------

    std::string GetName() const override { return "ScriptRegistry"; }
    SystemInitPhase GetInitPhase() const override { return SystemInitPhase::Resource; }
    std::vector<std::type_index> GetDependencies() const override;
    bool Initialize() override;
    void Shutdown() override;

    // ---- Lookup ------------------------------------------------------------
    //
    // All lookups return raw pointers owned by ScriptRegistry. The pointers
    // are stable for the lifetime of the registry entry; if the underlying
    // .ts is deleted, the pointer becomes invalid on the next reconcile pass.
    // Callers that hold pointers across reconciles must re-look-up.

    /// Find by 32-char lowercase hex GUID. Returns nullptr if no entry exists.
    ScriptAsset* FindByGuid(const eastl::string& guid) const;

    /// Find by project-relative source path (forward-slash, lower-cased on
    /// case-insensitive platforms). Returns nullptr if no entry exists.
    ScriptAsset* FindByPath(const eastl::string& source_rel_path) const;

    /// Snapshot of all currently-known scripts. Returned by value; cheap
    /// because each element is a single pointer.
    std::vector<ScriptAsset*> GetAll() const;

    // ---- Mutation (used by Phase 6 FileSystemWatcher; exposed for tests) ---

    /// Re-walk <Project>/Scripts/ and reconcile with the in-memory map.
    /// Entries for files that no longer exist on disk are removed.
    /// New files get a deterministic GUID and a fresh ScriptAsset.
    /// On any change, schedules a debounced registry JSON save (see
    /// TickDeferredSave / FlushPendingSave).
    /// Also re-pairs each entry with its compiled `.js` mirror under
    /// `Intermediate/Scripts/` (sets/clears `m_CompiledRelPath`).
    void rescan();

    /// Live notification from `TypeScriptCompiler` that a compiled `.js`
    /// just appeared / changed at `abs_js_path` (under the project's
    /// Intermediate/Scripts/ tree). We pair it with the matching ScriptAsset
    /// (by mirroring the relative path back to Scripts/<...>.ts) and update
    /// `m_CompiledRelPath` so subsequent component bind attempts succeed.
    /// No-op if the path doesn't sit under Intermediate/Scripts/ or no
    /// matching .ts source exists. Idempotent; cheap to call from a per-
    /// frame Tick path.
    void OnCompiledJsChanged(const std::filesystem::path& abs_js_path);

    /// Live notification that a `.js` was removed from Intermediate/Scripts/
    /// (e.g. user nuked Intermediate/, or tsc deleted the output for a .ts
    /// it stopped emitting for). Clears `m_CompiledRelPath` on the matching
    /// ScriptAsset so the Inspector accurately reports "waiting for tsc".
    void OnCompiledJsDeleted(const std::filesystem::path& abs_js_path);

    // ---- Persistence (also used by smoke tests) ----------------------------

    /// Absolute path where the registry JSON lives. Empty if no project is
    /// loaded.
    std::filesystem::path GetRegistryFilePath() const;

    /// Editor tick: Flush debounced JSON when the quiet window elapsed.
    void TickDeferredSave();

private:
    bool LoadFromDisk();
    bool SaveToDisk() const;
    void ScheduleSave();
    void FlushPendingSave() const;

    /// Walk GetScriptsRoot() recursively, returning project-relative paths
    /// (forward-slash) of every supported source file (.ts / .tsx / .js).
    static std::vector<eastl::string>
    EnumerateScriptFiles(const std::filesystem::path& project_root,
                         const std::filesystem::path& scripts_root);

    /// Deterministic 128-bit GUID -> 32-char lowercase hex.
    /// Splits a 128-bit FNV-1a hash of the rel-path into two 64-bit halves.
    /// Stable across runs / machines / clones.
    static eastl::string DeterministicGuidFromPath(const eastl::string& rel_path);

    /// Best-effort parse of `export (default )?class X extends (Behaviour|
    /// Component)`. Reads at most the first few KB; not a real TS parser.
    static eastl::string ParseDefaultClassName(const std::filesystem::path& abs_path);

    /// File mtime as nanoseconds since epoch, or 0 on failure.
    static int64_t FileMTimeNs(const std::filesystem::path& abs_path);

    /// Normalise a project-relative path to forward-slash, lower-cased on
    /// Windows (which is the only platform that matters for case stability
    /// today; the macOS/Linux build keeps original case).
    static eastl::string NormaliseRelPath(const std::filesystem::path& rel_path);

    // Owning storage. Keys in m_ByPath mirror m_ByGuid->m_SourceRelPath.
    // We use eastl::unordered_map (not std::unordered_map) because
    // std::hash<eastl::string> is not specialised in this codebase; every
    // existing hash-keyed-by-eastl::string container uses EASTL throughout
    // (see PreloadManager, WorldManager).
    eastl::unordered_map<eastl::string, std::unique_ptr<ScriptAsset>> m_ByGuid;
    eastl::unordered_map<eastl::string, ScriptAsset*> m_ByPath;
    mutable std::mutex m_Mutex;
    mutable bool m_SavePending {false};
    mutable std::chrono::steady_clock::time_point m_SaveDeadline {};

    // Cached project paths captured at Initialize(). Re-resolved on rescan()
    // in case the user opened a different project mid-session.
    std::filesystem::path m_ProjectRoot;
    std::filesystem::path m_ScriptsRoot;
    std::filesystem::path m_IntermediateScriptsRoot;
    std::filesystem::path m_RegistryFile;
};
