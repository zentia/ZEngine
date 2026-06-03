#pragma once

#include "Runtime/Function/Framework/Component/Behaviour.h"

#include <EASTL/string.h>
#include <string>
#include <tuple>
#include <vector>

/**
 * @brief Engine-side host for a single user-authored TypeScript Behaviour
 *        instance. Phase 5 of the TypeScript scripting integration.
 *
 * Mental model
 * ------------
 * TypeScriptComponent is the C++ counterpart of Unity's `MonoBehaviour`
 * (or UE's `UTypeScriptComponent`):
 *
 *   - It is a regular Behaviour subclass, so the existing GameObject /
 *     scene serialisation pipeline owns its lifetime.
 *   - At Awake (here: postLoadResource) it asks ScriptingManager to
 *     `LoadModule(module_id) -> CreateInstance(class_name)` and stores
 *     the resulting `pesapi_value_ref` as a strong reference.
 *   - On `Tick(dt)` it forwards to the JS side's `OnUpdate(dt)`.
 *   - On destruction it calls `OnDestroy()` and releases the ref.
 *
 * Identity carrier
 * ----------------
 * Instead of `PPtr<ScriptAsset>` (which would require ScriptAsset to be
 * registered in the ObjectManager and round-tripped through .zasset
 * loading), we store the script GUID directly as a string. ScriptRegistry
 * resolves `guid -> ScriptAsset* -> compiled .js path` at component-init
 * time. This is structurally identical to the way Unity's serialised
 * `MonoBehaviour.m_Script.guid` works.
 *
 * Class selection
 * ---------------
 * A single `.ts` source can declare multiple classes; we need to know
 * which one to instantiate. Resolution order:
 *   1. If `m_ClassName` is non-empty, use it verbatim.
 *   2. Otherwise fall back to ScriptAsset::m_DefaultClassName (parsed
 *      out of the source by ScriptRegistry's scanner).
 *   3. If both are empty, log a warning and stay un-instantiated.
 *
 * Hot reload
 * ----------
 * Phase 6 will add: when ScriptingManager fires a "module reloaded"
 * signal, every TypeScriptComponent that points at that module's
 * `module_id` calls OnDestroy on the stale instance and re-runs the
 * Awake/Start chain. P5 doesn't wire this yet (the ReloadModule already
 * happens; what's missing is the per-component re-instantiation), but
 * the data layout is ready.
 *
 * Editor mode
 * -----------
 * Component::Tick is gated by `g_editorTickComponentTypes` when
 * g_isPlaying is false (edit mode). TypeScriptComponent registers itself there in
 * Application::Initialize so OnUpdate fires in Edit mode too -- this
 * matches Unity's `[ExecuteAlways]` and is the simplest way to demo
 * scripting without needing a Play button.
 */
class TypeScriptComponent : public Behaviour
{
    REGISTER_CLASS(TypeScriptComponent);
    DECLARE_OBJECT_SERIALIZE();

public:
    TypeScriptComponent();
    ~TypeScriptComponent() override;

    // Component lifecycle entry points ---------------------------------
    void PostLoadResource(GameObject* parent_object) override;
    void Tick(float delta_time) override;

    // Authoring hooks --------------------------------------------------
    /// 32-char lowercase hex GUID of the source ScriptAsset. Empty until
    /// the component is bound (e.g. by Inspector drag-drop in P5+).
    void SetScriptGuid(const eastl::string& guid);
    const eastl::string& GetScriptGuid() const { return m_ScriptGuid; }

    /// Optional override for which exported class to instantiate. Empty
    /// means "use ScriptAsset::m_DefaultClassName".
    void SetClassName(const eastl::string& class_name);
    const eastl::string& GetClassName() const { return m_ClassName; }

    /// Module id (forward-slash path under <Project>/Intermediate/Scripts/
    /// without the .js suffix). Resolved once at Awake from the GUID;
    /// kept as a member so hot-reload can match against the watcher's
    /// PathToModuleId result without re-doing the GUID lookup.
    const std::string& GetModuleId() const { return m_ModuleId; }

    /// True iff CreateInstance succeeded and OnDestroy has not yet fired.
    bool HasLiveInstance() const { return m_JsInstance != nullptr; }

    /// Editor-only: try to (re-)bind this component to its JS class. Idempotent;
    /// if a live instance already exists this is a no-op (returns true). Used
    /// by the Inspector to retry a bind that may have failed on a previous
    /// frame (e.g. before tsc had emitted the compiled .js, or before the
    /// ScriptRegistry finished its initial scan). Returns the value of
    /// `HasLiveInstance()` after the attempt.
    bool TryBind();

    // -- Phase-7 serialised field overrides ----------------------------
    //
    // Per-instance overrides for the user-authored class's public fields
    // (Unity's `[SerializeField]` analogue). Stored as a flat alternating
    // `[k0,v0,k1,v1,...]` vector of `eastl::string` for two reasons:
    //
    //   1. SerializeTraits<std::vector<eastl::string>> exists and round-
    //      trips fine; pair<eastl::string,eastl::string> doesn't (no
    //      specialisation for the eastl side).
    //   2. The alternating layout keeps the on-disk YAML grep-friendly
    //      (one line per element) without committing to a richer schema.
    //
    // Values are always persisted as their string representation. The
    // ScriptingManager's __zApplyField helper coerces them at apply time
    // by inspecting the live instance's existing field type (number /
    // boolean / string), which is set by the JS class's field-initialiser
    // before any override is applied.
    //
    // Surviving hot reload: on `BindAndAwake` we re-apply this map after
    // CreateInstance and before OnAwake, so Inspector-edited values are
    // preserved across .ts saves. This intentionally differs from the
    // pure stateless rebuild of P6 - field overrides are authoring data,
    // not gameplay state.

    /// Read a single override; returns empty string if not present.
    eastl::string GetSerializedField(const eastl::string& key) const;

    /// Insert or replace an override. Empty value strings are stored as
    /// such (lets user explicitly blank a string field).
    void SetSerializedField(const eastl::string& key, const eastl::string& value);

    /// Drop an override entirely; the next BindAndAwake will fall back to
    /// the JS class's field-initialiser default.
    void RemoveSerializedField(const eastl::string& key);

    /// Number of overrides currently stored.
    size_t GetSerializedFieldCount() const;

    /// Read-only access to the alternating flat vector. Even indices are
    /// keys, odd indices values. Inspector iterates this for rendering.
    const std::vector<eastl::string>& GetSerializedFieldsRaw() const { return m_SerializedFields; }

private:
    // -- Serialised --------------------------------------------------
    // 32-char lowercase hex GUID into the project-level ScriptRegistry.
    eastl::string m_ScriptGuid;

    // Class name to instantiate from the resolved module's exports.
    // Empty -> fall back to ScriptAsset.m_DefaultClassName.
    eastl::string m_ClassName;

    // Phase-7 field overrides: flat alternating [k0,v0,k1,v1,...]. See
    // the GetSerializedField/Set/RemoveSerializedField docs above and
    // the design notes in doc/TYPESCRIPT_SCRIPTING_DESIGN.md (Phase 7).
    std::vector<eastl::string> m_SerializedFields;

    // -- Runtime only ------------------------------------------------
    // Cached after first resolution; not serialised.
    std::string m_ModuleId;

    // pesapi_value_ref. Stored as opaque void* so this header doesn't
    // pull in pesapi.h (which transitively yanks in QuickJS). The .cpp
    // does the cast in one place.
    void* m_JsInstance = nullptr;

    bool m_AwakeCalled = false;
    bool m_StartCalled = false;

    // Phase-6 hot-reload subscription token. 0 means "not subscribed".
    // ScriptingManager::AddModuleReloadObserver hands these out; we drop
    // ours in TearDown / dtor.
    size_t m_ReloadObserverToken = 0;

    // Helpers --------------------------------------------------------
    /// Look up GUID -> module id + class name. Returns false (and logs)
    /// if either piece is missing. On success populates m_ModuleId and
    /// resolved_class_name.
    bool ResolveScript(std::string& resolved_class_name);

    /// (Re-)load the JS module + create a fresh JS instance. Idempotent;
    /// if an instance already exists it's destroyed first. Calls
    /// OnAwake / OnStart in order. Logs and returns false on failure.
    bool BindAndAwake();

    /// Tear down the JS instance (calls OnDestroy on the JS side, then
    /// releases the strong ref). Safe to call multiple times.
    void TearDown();

    /// Phase-6: invoked by ScriptingManager when ANY module reloads. We
    /// filter by module_id and, on a match, do TearDown + BindAndAwake.
    /// The observer is registered in BindAndAwake (so unbound components
    /// don't pay) and removed in TearDown / dtor.
    void OnModuleReloaded(const std::string& module_id);

    /// Phase-7: walk m_SerializedFields and push every (key, value)
    /// pair onto the live JS instance via __zApplyField. No-op if
    /// m_JsInstance is null. Call AFTER CreateInstance and BEFORE
    /// OnAwake so JS-side OnAwake sees Inspector-authored values.
    void ApplySerializedFields();

    /// Phase-7 helper: locate the index of `key` in m_SerializedFields,
    /// or SIZE_MAX if not present. Even-indexed entries only.
    size_t FindSerializedFieldIndex(const eastl::string& key) const;

public:
    /// Phase-7: expose the live JS instance's enumerable own properties
    /// to the Inspector so it can render an editor for each. Returns
    /// triples of (name, type_string, default_value_string) where
    /// type_string is one of "number"/"boolean"/"string"/"object"/etc.
    /// Empty when no instance is live.
    std::vector<std::tuple<std::string, std::string, std::string>> EnumerateLiveFields() const;

    /// Phase-7: push a single field value onto the live instance right
    /// now (used by the Inspector when the user edits a field while the
    /// component is bound). Updates m_SerializedFields and calls into
    /// the JS env. No-op if m_JsInstance is null.
    bool LiveSetField(const eastl::string& key, const eastl::string& value);
};
