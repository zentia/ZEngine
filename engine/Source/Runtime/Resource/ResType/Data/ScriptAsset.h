#pragma once

#include "Runtime/BaseClasses/Object.h"

#include <string>

/**
 * @brief A logical asset that wraps a TypeScript / JavaScript source file.
 *
 * Design notes (see doc/TYPESCRIPT_SCRIPTING_DESIGN.md Phase 2 + AGENTS.md
 * 2.1 "No .meta sidecar files"):
 *
 *  - ZEngine does NOT use Unity-style `.cs.meta` sidecar files. Identity for
 *    a .ts/.js file lives in a single per-project registry JSON managed by
 *    ScriptRegistry; this Object is a pure in-memory view of one entry in
 *    that registry.
 *  - ScriptAsset is therefore NOT serialised to disk on its own (no .zasset).
 *    The fields below are still declared via Transfer() so that scenes /
 *    components which reference a script via PPtr<ScriptAsset> can serialise
 *    the GUID portion in the usual way; at load time the registry is
 *    consulted to resolve guid -> live ScriptAsset*.
 *  - All paths are relative to the project root, forward-slash separated.
 *    On case-insensitive platforms (Windows) ScriptRegistry stores them
 *    lower-cased so that lookups are stable across user typing.
 *
 * Lifetime: owned by ScriptRegistry. Lookup is via
 * ScriptRegistry::FindByGuid() / FindByPath(); never `new ScriptAsset` from
 * user code.
 */
class ScriptAsset : public Object
{
    REGISTER_CLASS(ScriptAsset);
    DECLARE_OBJECT_SERIALIZE(ScriptAsset);

public:
    // 128-bit GUID encoded as a 32-char lowercase hex string (no dashes).
    // Generated deterministically from Hash(rel_source_path) on first scan,
    // so that a clone whose script_registry.json was lost rebuilds identical
    // GUIDs and pre-existing references survive.
    eastl::string m_Guid;

    // Project-relative path to the user-authored source, e.g.
    // "Scripts/Player/PlayerController.ts". Forward slashes, lower-cased on
    // case-insensitive platforms.
    eastl::string m_SourceRelPath;

    // Project-relative path to the tsc compile output, e.g.
    // "Intermediate/Scripts/Player/PlayerController.js". Empty until Phase 3
    // wires up the TypeScriptCompiler.
    eastl::string m_CompiledRelPath;

    // Parsed from the first `export (default )?class X extends (Behaviour|
    // Component)` declaration in the source. Empty for utility modules that
    // don't declare a Behaviour subclass; the inspector will skip those.
    eastl::string m_DefaultClassName;

    // Source-file mtime as nanoseconds-since-epoch. Used by Phase 3 to skip
    // unchanged files in the incremental compile, and by Phase 6 hot-reload
    // to detect external edits. We store an integer (not std::filesystem::
    // file_time_type) because that type isn't serialisable and time-zone
    // independent comparison is what we actually need.
    int64_t m_SourceMtimeNs = 0;

    // 64-bit FNV-1a hash of the most-recently-scanned source content. This
    // is the cold-start equivalent of UE's UObjectRedirector: when ZEditor
    // restarts after the user has renamed a .ts in their IDE, ScriptRegistry
    // sees one path vanish and another appear; matching them by content
    // hash recovers the original GUID instead of forging a new one. Stored
    // as hex so the JSON stays human-diffable. Empty for entries from older
    // registry versions; rescan() backfills on the next pass.
    eastl::string m_SourceContentHash;
};
