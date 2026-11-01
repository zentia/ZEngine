#pragma once

#include "Runtime/Function/Framework/Component/Behaviour.h"

#include <EASTL/string.h>
#include <cstdint>
#include <string>
#include <tuple>
#include <vector>
#include "TypeScriptComponent.generated.h"

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
ZCLASS()
class TypeScriptComponent : public Behaviour
{
    ZGENERATED_BODY(TypeScriptComponent)

public:
    TypeScriptComponent();
    ~TypeScriptComponent() override;

    // Component lifecycle entry points ---------------------------------
    void PostLoadResource(GameObject* parent_object) override;
    void Tick(float delta_time) override;

    /// 转发到 JS 侧的 `OnFixedUpdate(fixedDeltaTime)`。
    ///
    /// 与 OnUpdate 的分工同 Component::FixedTick：物理、移动、任何需要跨机器
    /// 复现的推进都该写在 OnFixedUpdate 里。
    ///
    /// ⚠️ 一帧内可能调用 0 次或多次。写"收到一次输入就做一次事"的逻辑会
    /// 在掉帧时重复触发、高帧率时漏触发 —— 那种逻辑属于 OnUpdate。
    void FixedTick(float fixed_delta_time) override;

    /// 转发到 JS 侧的 `OnLateUpdate(deltaTime)`。
    ///
    /// Unity 的 `LateUpdate`：每帧一次，在所有 Update 之后。典型用途是相机
    /// 跟随 —— 写在 Update 里的话跟随的永远是目标上一帧的位置。
    ///
    /// ⚠️ 与 OnUpdate 的判据必须一致（见 GameObject::LateTick）：这里不需要
    /// 额外判 enabled / 层级激活，因为 GameObject 那一层已经判过了。
    void LateTick(float delta_time) override;

    /// Unity 的 `OnEnable` / `OnDisable`。
    ///
    /// 触发条件是**合取**：`enabled && activeInHierarchy`（Unity 里
    /// `Behaviour.isActiveAndEnabled` 就是这个合取）。单独 `enabled = true`
    /// 但对象在层级里被关着时不该 OnEnable —— 那正是 Unity 里"设了 enabled
    /// 没反应"最常见的误判来源。
    ///
    /// 两条路都会走到这里：组件自己的 `SetEnabled`（Unity 的
    /// `Behaviour.enabled`）和宿主对象层级激活变化（Unity 的
    /// `SetActive` / 父级停用）。两边都只做"重算合取值、变了才派发"，所以
    /// 顺序无关、重复调用无害。
    void SetEnabled(bool enabled) override;

    /// 宿主 `activeInHierarchy` 变化（GameObject::SetActive 推下来的）。
    void OnOwnerActiveChanged(bool active) override;

    // ---- Unity 的应用层消息 ---------------------------------------------
    //
    // 与上面那些**方向相反**：这些由整个应用驱动，派发侧不判 enabled /
    // activeInHierarchy（理由见 Component 里那四个虚函数的注释）。所以这里
    // 也不能补判 —— 补了就等于"被停用的对象收不到退出通知"，而这恰恰是
    // Unity 明确不做的（它的 OnApplicationPause 文档写的是 "sent to all
    // GameObjects"）。
    void OnApplicationFocus(bool has_focus) override;
    void OnApplicationPause(bool paused) override;
    void OnApplicationQuit() override;
    void OnTransformParentChanged() override;
    void OnTransformChildrenChanged() override;

    // Unity 的 gizmo 消息。派发侧（`GameObject::BroadcastDrawGizmos`）只判
    // `activeInHierarchy`、**不判 enabled** —— 所以这里的转发同样不能补判，
    // 否则停用的脚本就画不出辅助线（与 Unity 相反，见 Component 里那两条注释）。
    void OnDrawGizmos() override;
    void OnDrawGizmosSelected() override;

    // Unity 的七条鼠标消息。派发侧同样只判 `activeInHierarchy`、不判 enabled
    // （与 gizmo 那一组同一条规则）。
    void OnMouseEnter() override;
    void OnMouseOver() override;
    void OnMouseExit() override;
    void OnMouseDown() override;
    void OnMouseDrag() override;
    void OnMouseUp() override;
    void OnMouseUpAsButton() override;

    /// Unity 的 `OnValidate()`：编辑器里序列化字段被改之后。只在 `g_isEditor`
    /// 下发（Player 里这条路径唯一的调用方是反序列化，那时脚本还没绑上）。
    void OnSerializedFieldsUpdated() override;

    // Authoring hooks --------------------------------------------------
    /// 32-char lowercase hex GUID of the source ScriptAsset. Empty until
    /// the component is bound (e.g. by Inspector drag-drop in P5+).
    void SetScriptGuid(const eastl::string& guid);
    ZFUNCTION()
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
    // ZFUNCTION: annotated so ZHeaderTool registers TypeScriptComponent in the
    // pesapi registry at all. `native_object_to_value` can only wrap a pointer
    // whose type id has been `define_class`ed, so without at least one bound
    // method `this.component` could not be published to the script.
    ZFUNCTION()
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
    // Both fields are eastl::string -- per AGENTS.md 2.3, that's a hard
    // requirement for any field that participates in Transfer().
    // ReadOnly: also blocks the generated script setter. The guid is the key
    // TypeScriptComponent is indexed by at awake time, so letting a script
    // rewrite it would strand the live instance.
    ZPROPERTY(Name="m_script_guid", ReadOnly)
    eastl::string m_ScriptGuid;

    // Class name to instantiate from the resolved module's exports.
    // Empty -> fall back to ScriptAsset.m_DefaultClassName.
    ZPROPERTY(Name="m_class_name")
    eastl::string m_ClassName;

    // Phase-7 field overrides: flat alternating [k0,v0,k1,v1,...]. See
    // the GetSerializedField/Set/RemoveSerializedField docs above and
    // the design notes in doc/TYPESCRIPT_SCRIPTING_DESIGN.md (Phase 7).
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
    ZPROPERTY(Name="m_serialized_fields", HideInEditor)
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

    /// 上一次派发过的 `enabled && activeInHierarchy`。初值 false，于是首次
    /// BindAndAwake 会（在对象确实开着时）补一次 OnEnable —— 与 Unity 里
    /// "OnEnable 紧跟 OnAwake、在 OnStart 之前"的顺序一致。
    bool m_LastEnabledAndActive {false};

    // Live JS generators backing the coroutines this behaviour started.
    //
    // Indexed by `script_id - 1`. Slots are nulled on release rather than
    // erased, so an id handed out earlier can never silently start pointing at
    // a different generator -- which would make `Stop` kill the wrong
    // coroutine, a bug with no symptom until two routines run at once.
    std::vector<void*> m_CoroutineGenerators;

    /// `WaitUntil` / `WaitWhile` 的谓词，与 `m_CoroutineGenerators` **平行同
    /// 下标**（所以 `predicate_id == script_id`）。
    ///
    /// 分开一张表而不是塞进生成器那张：谓词是可空的（大部分 yield 不带谓词），
    /// 而生成器槽位一旦空了就代表"协程结束了"。混在一张表里，"这个协程还活着
    /// 但没有谓词"和"协程已经结束"就没法区分。
    ///
    /// 释放时机与生成器完全一致（`EndScriptCoroutine` / `TearDown`），且每次
    /// `StepScriptCoroutine` 都会先放掉上一个 —— 循环里
    /// `yield new WaitUntil(() => ...)` 每帧造一个新闭包，不放就是每帧一个
    /// GC 看不见的泄漏。
    std::vector<void*> m_CoroutinePredicates;

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

    /// 把谓词的强引用放进（或拿出）`m_CoroutinePredicates[script_id - 1]`。
    /// 传 nullptr 就是"释放并清空"。若槽位已经不存在，新谓词会被直接放掉 ——
    /// 不能让它挂到下一次复用的槽位上。
    void SetCoroutinePredicate(uint64_t script_id, void* predicate_ref);

    /// OnEnable / OnDisable 的唯一派发点：重算 `enabled && activeInHierarchy`，
    /// 与 m_LastEnabledAndActive 不同才发，并记住新值。
    ///
    /// `SetEnabled` 与 `OnOwnerActiveChanged` 都只调它 —— 两条路各自去判
    /// "是不是自己那条变了"就会漏掉"两个条件同时满足不同方向变化"的组合。
    void RefreshEnabledState();

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

    /// Invokes a method by name on the live JS instance -- the receiving half of
    /// `GameObject.SendMessage()`.
    ///
    /// Returns false when there is no live instance (not bound, or the module
    /// has not compiled yet). A method that simply does not exist on the JS
    /// object is NOT reported as a failure: that matches Unity's SendMessage,
    /// which only complains under SendMessageOptions.RequireReceiver.
    bool InvokeMethod(const char* method_name);

    /// 带一个 GameObject 参数的版本（碰撞 / 触发回调的 `other`）。与
    /// `InvokeMethod` 同样的失败语义：没有实例 / 没有 ScriptingManager 返回
    /// false，方法不存在**不算**失败（Unity SendMessage 的
    /// SendMessageOptions.DontRequireReceiver 语义）。
    bool InvokeMethodWithOther(const char* method_name, class GameObject* other);

    /// 带一个 bool 参数的版本（Unity 的 `OnApplicationFocus(bool)` /
    /// `OnApplicationPause(bool)`）。失败语义同 `InvokeMethod`。
    bool InvokeMethodBool(const char* method_name, bool bool_arg);

    // --- Unity's Invoke family ---------------------------------------------
    //
    // Forwarding to `Behaviour`, not declarations of their own: the timer, the
    // cancellation bookkeeping and the delivery hook all live on the base so
    // that AIControllerComponent / RenderCamera get them too.
    //
    // Why they are bound HERE and not on Behaviour: `Behaviour` is the name of
    // the synthetic script class TS behaviours extend, so publishing the C++
    // class would emit a duplicate `declare class Behaviour`. ScriptEnv's
    // `Behaviour.prototype` shim turns these into `this.Invoke(...)`.

    /// Call `methodName` once, `time` seconds from now. @see Behaviour::Invoke
    ZFUNCTION()
    void Invoke(const eastl::string& methodName, float time);

    /// Call `methodName` after `time`, then every `repeatRate` seconds.
    ZFUNCTION()
    void InvokeRepeating(const eastl::string& methodName, float time, float repeatRate);

    /// Cancel pending invocations of `methodName`; an empty name cancels all.
    ZFUNCTION()
    void CancelInvoke(const eastl::string& methodName);

    /// Cancel everything this behaviour has scheduled.
    ZFUNCTION()
    void CancelAllInvokes();

    /// Is there a pending invocation of `methodName`?
    ZFUNCTION()
    bool IsInvoking(const eastl::string& methodName);

    // --- Unity's coroutine family ------------------------------------------
    // Same shape as the Invoke forwarders above: `Behaviour` owns the
    // bookkeeping, the ZFUNCTIONs can only live on a publishable class, and
    // ScriptEnv's `Behaviour.prototype` shim turns them back into
    // `this.StartCoroutine(...)`.
    //
    // `StartCoroutine` returns the scheduler id (0 on failure); the JS shim
    // wraps it into a `Coroutine` handle so `StopCoroutine(handle)` works with
    // one C++ entry point.

    /// Start `methodName` (which must return a generator) as a coroutine.
    ZFUNCTION()
    uint64_t StartCoroutine(const eastl::string& methodName);

    /// Stop every coroutine this behaviour started from `methodName`.
    ZFUNCTION()
    void StopCoroutine(const eastl::string& methodName);

    /// Stop every coroutine owned by this behaviour.
    ZFUNCTION()
    void StopAllCoroutines();

    /// Is a coroutine started from `methodName` still running?
    ZFUNCTION()
    bool IsCoroutineRunning(const eastl::string& methodName);

public:
    // -- Script-coroutine bridge -------------------------------------------
    // These own the JS generator objects. `Behaviour` refers to them through
    // the opaque `script_id`, which here is an index into
    // m_CoroutineGenerators + 1 (0 is reserved for "none", matching
    // Behaviour's use of 0 as "no script side").

    bool StartScriptCoroutine(const char* method_name, uint64_t& out_script_id) override;
    bool StepScriptCoroutine(uint64_t script_id, ScriptYield& out) override;
    void EndScriptCoroutine(uint64_t script_id) override;
    bool EvalScriptPredicate(uint64_t predicate_id, bool& out_value) override;

protected:
    /// `Behaviour.Invoke` delivery: a delayed call reaches THIS instance rather
    /// than being broadcast to every script on the GameObject, which is what
    /// Unity's `Invoke` does (it is a MonoBehaviour method, not SendMessage).
    bool InvokeBehaviourMethod(const char* method_name) override { return InvokeMethod(method_name); }
};
