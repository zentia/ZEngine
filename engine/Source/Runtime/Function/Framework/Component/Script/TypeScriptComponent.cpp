#include "Runtime/Function/Framework/Component/Script/TypeScriptComponent.h"

#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Core/Log/LogSystem.h"
#include "Runtime/Core/Serialize/SerializationMetaFlags.h"  // for TransferMetaFlags::HideInEditorMask
#include "Runtime/Project/ProjectInfo.h"
#include "Runtime/Resource/ResType/Data/ScriptAsset.h"
#include "Runtime/Scripting/ScriptRegistry.h"
#include "Runtime/Scripting/ScriptingManager.h"  // transitively brings pesapi.h via ScriptEnv.h

// =============================================================================
// RTTI / serialisation boilerplate.
//
// Behaviour itself is registered separately (see Behaviour.cpp); this is the
// concrete subclass that user-authored TypeScript classes shadow at runtime.
// The auto-registrar inside IMPLEMENT_REGISTER_CLASS runs at static-init time
// and registers TypeScriptComponent with TypeManager, so no manual entry in
// RegisterRuntime.cpp is required.
// =============================================================================
IMPLEMENT_REGISTER_CLASS(TypeScriptComponent)
IMPLEMENT_OBJECT_SERIALIZE(TypeScriptComponent)
INSTANTIATE_TEMPLATE_TRANSFER_EXPORTED(TypeScriptComponent)

template<typename TransferFunction>
void TypeScriptComponent::Transfer(TransferFunction& transfer)
{
    Super::Transfer(transfer);
    // Both fields are eastl::string -- per AGENTS.md 2.3, that's a hard
    // requirement for any field that participates in Transfer().
    transfer.Transfer(m_ScriptGuid, "m_script_guid");
    transfer.Transfer(m_ClassName, "m_class_name");
    // Phase-7: per-instance field overrides. Stored as a flat alternating
    // [k0,v0,k1,v1,...] vector of eastl::string because that's the
    // SerializeTraits-supported shape (see Runtime/Core/Serialize/
    // SerializeTraits.h: vector<T> + eastl::string both specialised, no
    // pair<eastl::string,eastl::string> path). The accessor helpers
    // below preserve the alternating invariant so callers never see the
    // flat layout.
    //
    // HideInEditorMask: the Inspector should NOT render the raw flat vector
    // (the generic Array renderer would show "Array editing is not
    // implemented yet." which is misleading). Inspector instead renders
    // these via DrawTypeScriptComponentScriptFields, which knows about
    // the alternating [k,v] layout and can present per-field widgets.
    transfer.Transfer(m_SerializedFields, "m_serialized_fields", TransferMetaFlags::HideInEditorMask);
}

// -----------------------------------------------------------------------------
// Construction / destruction
// -----------------------------------------------------------------------------

TypeScriptComponent::TypeScriptComponent()
{
    // Opt into edit-mode tick. The dispatch in GameObject::Tick consults
    // g_editorTickComponentTypes (a string set populated by EditorApplication
    // at startup), so flipping this flag alone isn't enough -- the editor-side
    // registration is what actually lets OnUpdate fire while not in Play
    // mode. We keep the flag set anyway so anyone reading the field sees the
    // intent.
    m_TickInEditorMode = true;
}

TypeScriptComponent::~TypeScriptComponent()
{
    // Component dtors fire on scene unload / GameObject destruction. Make
    // sure the JS instance is told the host is going away, then release
    // the strong ref so QuickJS can collect it.
    TearDown();
}

// -----------------------------------------------------------------------------
// Lifecycle
// -----------------------------------------------------------------------------

void TypeScriptComponent::PostLoadResource(GameObject* parent_object)
{
    // Always run the base wiring first so GetParentObject() works for any
    // helper invoked below.
    Super::PostLoadResource(parent_object);

    // Empty GUID = component was added but never bound to a script. That's
    // a valid intermediate state (e.g. user just dropped one in the
    // inspector), so don't log spam -- just stay dormant.
    if (m_ScriptGuid.empty())
        return;

    BindAndAwake();
}

void TypeScriptComponent::Tick(float delta_time)
{
    if (m_JsInstance == nullptr)
        return;

    auto* sm = GET_SYSTEM(ScriptingManager).get();
    if (sm == nullptr)
        return;

    auto ref = static_cast<pesapi_value_ref>(m_JsInstance);
    sm->InvokeInstanceMethodNumber(ref, "OnUpdate", static_cast<double>(delta_time));
}

// -----------------------------------------------------------------------------
// Authoring hooks
// -----------------------------------------------------------------------------

void TypeScriptComponent::SetScriptGuid(const eastl::string& guid)
{
    if (m_ScriptGuid == guid)
        return;

    // Re-bind: tear down the old instance first so OnDestroy fires under the
    // old class context, then point at the new script.
    TearDown();
    m_ScriptGuid = guid;

    // Only auto-bind if we're already attached to a GameObject -- otherwise
    // postLoadResource will pick up the new value when the component goes
    // live.
    if (GetParentObject() != nullptr && !m_ScriptGuid.empty())
        BindAndAwake();
}

void TypeScriptComponent::SetClassName(const eastl::string& class_name)
{
    if (m_ClassName == class_name)
        return;

    TearDown();
    m_ClassName = class_name;

    if (GetParentObject() != nullptr && !m_ScriptGuid.empty())
        BindAndAwake();
}

bool TypeScriptComponent::TryBind()
{
    // Already bound -> nothing to do.
    if (m_JsInstance != nullptr)
        return true;

    // Need a parent (BindAndAwake reads parent state) and a non-empty guid.
    // Both can be missing during the brief window between component
    // construction and the popup attaching it -- we just bail quietly,
    // matching SetScriptGuid's policy.
    if (GetParentObject() == nullptr || m_ScriptGuid.empty())
        return false;

    BindAndAwake();  // logs its own failure reasons; we don't need to here
    return m_JsInstance != nullptr;
}

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------

bool TypeScriptComponent::ResolveScript(std::string& resolved_class_name)
{
    auto registry = GET_SYSTEM(ScriptRegistry).get();
    if (registry == nullptr)
    {
        LOG_ERROR(ZScripting,
                  "TypeScriptComponent: ScriptRegistry not available "
                  "(guid='{}')",
                  m_ScriptGuid.c_str());
        return false;
    }

    ScriptAsset* asset = registry->FindByGuid(m_ScriptGuid);
    if (asset == nullptr)
    {
        LOG_ERROR(ZScripting,
                  "TypeScriptComponent: no ScriptAsset for guid '{}' (was the .ts deleted?)",
                  m_ScriptGuid.c_str());
        return false;
    }

    if (asset->m_CompiledRelPath.empty())
    {
        LOG_ERROR(ZScripting,
                  "TypeScriptComponent: script '{}' (guid '{}') has no compiled output yet -- "
                  "make sure tsc has run at least once",
                  asset->m_SourceRelPath.c_str(),
                  m_ScriptGuid.c_str());
        return false;
    }

    // ScriptAsset stores compiled paths as project-relative AND lower-cased
    // on Windows (see ScriptRegistry::normaliseInternal -- the lookup map
    // is case-folded so "Player.ts" and "player.ts" can't collide). The
    // module id used by ScriptingManager / ScriptEnv however is whatever
    // PathToModuleId() produces, which is `filesystem::relative` against
    // the on-disk js root and therefore preserves the actual case of the
    // file. Going through string-prefix-stripping here would either
    // silently fail (case-sensitive compare) or hand back a lower-cased
    // id that LoadModule's cache lookup misses against the cold-start /
    // hot-reload entry already keyed by the real-case id.
    //
    // The robust path is: rebuild the absolute .js path by joining
    // <project_root> with m_CompiledRelPath, then ask ScriptingManager
    // to map that back to a module id. That uses the exact same code path
    // as the file watcher and the startup directory scan, so the resulting
    // string is guaranteed to match whatever LoadModule registered.
    std::filesystem::path abs_js_path;
    {
        auto project_info = GET_SYSTEM(ProjectInfo);
        if (project_info == nullptr || project_info->GetProjectRoot().empty())
        {
            LOG_ERROR(ZScripting,
                      "TypeScriptComponent: ProjectInfo unavailable while binding '{}'",
                      asset->m_SourceRelPath.c_str());
            return false;
        }
        // m_CompiledRelPath is forward-slashed, so operator/ does the
        // right thing across platforms.
        abs_js_path = project_info->GetProjectRoot() / std::filesystem::path(asset->m_CompiledRelPath.c_str());
    }

    auto sm = GET_SYSTEM(ScriptingManager);
    if (sm == nullptr)
    {
        LOG_ERROR(ZScripting,
                  "TypeScriptComponent: ScriptingManager unavailable while binding '{}'",
                  asset->m_SourceRelPath.c_str());
        return false;
    }
    std::string mod = sm->PathToModuleId(abs_js_path);
    if (mod.empty())
    {
        LOG_ERROR(ZScripting,
                  "TypeScriptComponent: failed to derive module id from compiled path '{}' "
                  "(js root='{}'); is the .js still under <project>/Intermediate/Scripts/?",
                  asset->m_CompiledRelPath.c_str(),
                  sm->GetJsRoot().generic_string().c_str());
        return false;
    }
    m_ModuleId = std::move(mod);

    // Class-name selection (see header comment, "Class selection"):
    //   1. explicit override on the component
    //   2. ScriptAsset's parsed default
    //   3. give up
    if (!m_ClassName.empty())
    {
        resolved_class_name.assign(m_ClassName.c_str(), m_ClassName.size());
    }
    else if (!asset->m_DefaultClassName.empty())
    {
        resolved_class_name.assign(asset->m_DefaultClassName.c_str(),
                                   asset->m_DefaultClassName.size());
    }
    else
    {
        LOG_ERROR(ZScripting,
                  "TypeScriptComponent: script '{}' has no exported Behaviour class "
                  "and no override on the component -- nothing to instantiate",
                  asset->m_SourceRelPath.c_str());
        return false;
    }
    return true;
}

bool TypeScriptComponent::BindAndAwake()
{
    // Idempotent: if we already have a live instance, tear it down first.
    // This makes Set{ScriptGuid,ClassName} and the future hot-reload path
    // (P6) trivially safe to call repeatedly.
    if (m_JsInstance != nullptr)
        TearDown();

    auto sm = GET_SYSTEM(ScriptingManager);
    if (sm == nullptr)
    {
        LOG_ERROR(ZScripting,
                  "TypeScriptComponent::BindAndAwake: ScriptingManager not initialised");
        return false;
    }

    // Helper to subscribe (idempotently) to module-reload notifications.
    // Used both on success (so subsequent .ts saves rebuild this instance)
    // AND on certain "transient" failures (so the component auto-retries
    // its bind once tsc has emitted the .js / first-load completes).
    // The callback is the same OnModuleReloaded path either way -- it
    // already handles both "rebuild a live instance" and "wake up an
    // unbound component" cases via BindAndAwake's TearDown-then-build
    // semantics.
    auto subscribe_for_retry = [this, &sm]() {
        if (m_ReloadObserverToken == 0)
        {
            m_ReloadObserverToken = sm->AddModuleReloadObserver(
                [this](const std::string& reloaded_module_id) {
                    this->OnModuleReloaded(reloaded_module_id);
                });
        }
    };

    std::string class_name;
    if (!ResolveScript(class_name))
    {
        // ResolveScript failed because the registry says "no compiled
        // output yet" or similar transient state. Subscribe so we can
        // retry when tsc emits the .js (the editor's hot-reload bridge
        // calls NotifyModuleReloaded after every successful first-load).
        // We can't filter by module_id yet (we don't have one), so the
        // observer accepts any reload and OnModuleReloaded re-runs
        // ResolveScript itself; it will either succeed and bind, or
        // bail again and stay subscribed for the next attempt.
        if (!m_ScriptGuid.empty())
            subscribe_for_retry();
        return false;
    }

    if (!sm->IsModuleLoaded(m_ModuleId) && !sm->LoadModule(m_ModuleId))
    {
        // Module exists in the registry (m_CompiledRelPath is set) but
        // ScriptEnv couldn't load it -- could be a tsc emit-in-progress
        // race, or a JS syntax error. Either way it's worth subscribing
        // so the next successful (Re)LoadModule wakes us up.
        // LoadModule already logged the failure; don't double-log.
        subscribe_for_retry();
        return false;
    }

    pesapi_value_ref instance = sm->CreateInstance(m_ModuleId, class_name);
    if (instance == nullptr)
    {
        // CreateInstance already logged. Subscribe for retry on next
        // reload (typical cause: stale exports cache after a partial
        // edit; the next save will fix it).
        subscribe_for_retry();
        return false;
    }
    m_JsInstance = instance;

    // Phase-7: push Inspector-authored field overrides onto the fresh
    // instance BEFORE OnAwake fires. This is the single point where
    // serialised data crosses C++ -> JS, and putting it here means:
    //   - First-time bind (postLoadResource after scene load) sees the
    //     overrides that were just deserialised from disk.
    //   - Hot-reload rebuild (P6) re-applies the same overrides, so the
    //     user doesn't lose their tweaks every time they save a .ts.
    //   - User edits in the Inspector go through LiveSetField which both
    //     updates m_SerializedFields and pushes to JS, so they survive
    //     a subsequent reload via this exact code path.
    ApplySerializedFields();

    // Lifecycle dispatch -- mirrors Unity's ordering. Both methods are
    // optional on the JS side (InvokeInstanceMethod no-ops if the property
    // isn't a function), so user scripts can implement only what they need.
    sm->InvokeInstanceMethod(instance, "OnAwake");
    m_AwakeCalled = true;
    sm->InvokeInstanceMethod(instance, "OnStart");
    m_StartCalled = true;

    // Phase-6: subscribe to hot-reload events for *any* module so we can
    // filter by module_id when notified. Subscribing AFTER OnAwake/OnStart
    // succeeded means a re-entrant reload during construction can't fire
    // before we're ready. The token is dropped in TearDown.
    subscribe_for_retry();

    LOG_INFO(ZScripting,
             "TypeScriptComponent bound: module='{}' class='{}' (guid={})",
             m_ModuleId,
             class_name,
             m_ScriptGuid.c_str());
    return true;
}

void TypeScriptComponent::TearDown()
{
    auto sm = GET_SYSTEM(ScriptingManager);

    // Drop the observer first - if we got here from the observer itself
    // (TearDown invoked from OnModuleReloaded -> BindAndAwake's prelude),
    // we want the new bind to register a fresh subscription rather than
    // walk an entry that's about to fire its destructor.
    if (sm && m_ReloadObserverToken != 0)
    {
        sm->RemoveModuleReloadObserver(m_ReloadObserverToken);
        m_ReloadObserverToken = 0;
    }

    if (m_JsInstance == nullptr)
        return;

    auto ref = static_cast<pesapi_value_ref>(m_JsInstance);

    // Only fire OnDestroy if we actually got past OnAwake. Mirrors Unity's
    // contract -- OnDestroy is paired with OnAwake, not with construction.
    if (sm && m_AwakeCalled)
        sm->InvokeInstanceMethod(ref, "OnDestroy");

    if (sm)
        sm->DestroyInstance(ref);

    m_JsInstance = nullptr;
    m_AwakeCalled = false;
    m_StartCalled = false;
}

void TypeScriptComponent::OnModuleReloaded(const std::string& reloaded_module_id)
{
    // Two distinct entry conditions land here:
    //
    //   (A) We have a live binding for this exact module. Filter strictly
    //       by m_ModuleId so unrelated reloads are basically free.
    //
    //   (B) We're an unbound component that subscribed from BindAndAwake's
    //       failure path while waiting for tsc to emit the .js. In that
    //       case m_ModuleId is either empty (ResolveScript bailed before
    //       it could be set) or stale (set on a prior attempt that then
    //       got TornDown). We can't filter accurately, so any reload is
    //       a "maybe my time has come" hint -- try ResolveScript via
    //       BindAndAwake and let it decide.
    //
    // Discriminator: HasLiveInstance(). Bound -> path (A). Unbound -> (B).
    if (m_JsInstance != nullptr)
    {
        if (reloaded_module_id != m_ModuleId)
            return;
    }
    else
    {
        if (m_ScriptGuid.empty())
            return;  // not waiting on anything; avoid log spam
    }

    LOG_INFO(ZScripting,
             "TypeScriptComponent hot-reload: module='{}' class='{}' (guid={})",
             reloaded_module_id,
             m_ClassName.empty() ? "<default>" : std::string(m_ClassName.c_str()),
             m_ScriptGuid.c_str());

    // Tear down + rebuild. Per the design doc, reload is intentionally
    // STATELESS: we drop the JS instance, OnDestroy fires, and the new
    // OnAwake/OnStart run on a fresh object. Users who need cross-reload
    // state should externalise it (e.g. into a C++ component field).
    //
    // BindAndAwake() already calls TearDown() internally if m_JsInstance
    // is non-null, so this single call covers both paths.
    BindAndAwake();
}

// =============================================================================
// Phase-7: serialised field overrides
// =============================================================================
//
// Storage invariant: m_SerializedFields has even length; even indices hold
// keys, odd indices hold values. Helpers below preserve this; nothing else
// in the class touches the vector directly.

size_t TypeScriptComponent::FindSerializedFieldIndex(const eastl::string& key) const
{
    // Linear scan: the override map is expected to hold a handful of
    // entries per component (think Unity's Inspector with ~5-10 visible
    // fields), so a hash table would be overkill and would just bloat
    // the serialised footprint.
    for (size_t i = 0; i + 1 < m_SerializedFields.size(); i += 2)
    {
        if (m_SerializedFields[i] == key)
            return i;
    }
    return SIZE_MAX;
}

eastl::string TypeScriptComponent::GetSerializedField(const eastl::string& key) const
{
    const size_t idx = FindSerializedFieldIndex(key);
    if (idx == SIZE_MAX)
        return eastl::string {};
    return m_SerializedFields[idx + 1];
}

void TypeScriptComponent::SetSerializedField(const eastl::string& key, const eastl::string& value)
{
    const size_t idx = FindSerializedFieldIndex(key);
    if (idx != SIZE_MAX)
    {
        m_SerializedFields[idx + 1] = value;
        return;
    }
    m_SerializedFields.push_back(key);
    m_SerializedFields.push_back(value);
}

void TypeScriptComponent::RemoveSerializedField(const eastl::string& key)
{
    const size_t idx = FindSerializedFieldIndex(key);
    if (idx == SIZE_MAX)
        return;
    // erase the (key, value) pair as a contiguous range
    m_SerializedFields.erase(m_SerializedFields.begin() + idx,
                             m_SerializedFields.begin() + idx + 2);
}

size_t TypeScriptComponent::GetSerializedFieldCount() const
{
    // Pair-count: vector size is always even by invariant.
    return m_SerializedFields.size() / 2;
}

void TypeScriptComponent::ApplySerializedFields()
{
    if (m_JsInstance == nullptr)
        return;
    if (m_SerializedFields.empty())
        return;

    auto sm = GET_SYSTEM(ScriptingManager);
    if (sm == nullptr)
        return;

    auto ref = static_cast<pesapi_value_ref>(m_JsInstance);

    // Each field is pushed individually rather than as a batch object so
    // a single bad value (e.g. unparseable number) doesn't poison the
    // rest. ScriptingManager logs at WARN on individual failures.
    for (size_t i = 0; i + 1 < m_SerializedFields.size(); i += 2)
    {
        const eastl::string& key = m_SerializedFields[i];
        const eastl::string& value = m_SerializedFields[i + 1];
        sm->ApplyInstanceField(ref,
                               std::string(key.c_str(), key.size()),
                               std::string(value.c_str(), value.size()));
    }
}

bool TypeScriptComponent::LiveSetField(const eastl::string& key, const eastl::string& value)
{
    // Always update the persisted map so a save right now captures the
    // edit even if the live push fails (e.g. instance got torn down a
    // microsecond ago).
    SetSerializedField(key, value);

    if (m_JsInstance == nullptr)
        return false;

    auto sm = GET_SYSTEM(ScriptingManager);
    if (sm == nullptr)
        return false;

    return sm->ApplyInstanceField(static_cast<pesapi_value_ref>(m_JsInstance),
                                  std::string(key.c_str(), key.size()),
                                  std::string(value.c_str(), value.size()));
}

std::vector<std::tuple<std::string, std::string, std::string>>
TypeScriptComponent::EnumerateLiveFields() const
{
    std::vector<std::tuple<std::string, std::string, std::string>> out;
    if (m_JsInstance == nullptr)
        return out;

    auto sm = GET_SYSTEM(ScriptingManager);
    if (sm == nullptr)
        return out;

    return sm->EnumerateInstanceFields(static_cast<pesapi_value_ref>(m_JsInstance));
}
