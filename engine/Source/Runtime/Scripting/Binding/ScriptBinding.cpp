#include "Runtime/Scripting/Binding/ScriptBinding.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace
{
    // Constant-initialized, so it is valid before any dynamic static init
    // (generated registrars run during dynamic init, in unspecified order).
    ZClassScriptBinding* g_Head = nullptr;
    ZScriptEnum* g_EnumHead = nullptr;
    ZSyntheticScriptClass* g_SyntheticHead = nullptr;
    bool g_Installed = false;

    // Placeholder `declare class <Super> {}` emitted for supers that have no
    // binding of their own. Kept outside AppendTypeScriptDeclarations so a
    // second call (one .d.ts per project, but the call may be exercised more
    // than once in tests) does not duplicate them.
    std::vector<const char*> g_EmittedPlaceholders;

    /// Whether `name` is emitted as a `declare class` in the generated file.
    ///
    /// A reserved name is not: InstallGlobals refuses to publish it as a global
    /// (it would shadow a JS built-in), and emitting the class anyway would
    /// collide with lib.d.ts. Both halves have to agree, so `extends` must treat
    /// a reserved super as absent too -- `extends Object` with no `declare class
    /// Object` in the file is a compile error in every user project.
    bool IsDeclared(const std::vector<const ZClassScriptBinding*>& classes, const char* name)
    {
        if (ScriptBindingRegistry::IsReservedScriptName(name))
            return false;
        for (const ZClassScriptBinding* cls : classes)
        {
            if (cls->scriptName != nullptr && std::strcmp(cls->scriptName, name) == 0)
                return true;
        }
        return false;
    }

    bool IsPlaceholderEmitted(const char* name)
    {
        for (const char* emitted : g_EmittedPlaceholders)
        {
            if (std::strcmp(emitted, name) == 0)
                return true;
        }
        return false;
    }

    bool IsTypeDeclared(const std::vector<const void*>& ids, const void* id)
    {
        for (const void* known : ids)
        {
            if (known == id)
                return true;
        }
        return false;
    }

    // Identifiers TypeScript's own lib.d.ts already declares at global scope.
    // The engine root class is literally called `Object`, so emitting
    // `declare class Object {}` would collide with the built-in `Object`
    // interface and break every user project, and publishing a global of that
    // name would shadow the JS built-in at runtime. Names in this list are
    // therefore dropped from both the .d.ts placeholder pass and the global
    // install; a class that would inherit from one is emitted without an
    // `extends` clause (it contributes no bound members anyway).
    const char* const kReservedScriptNames[] = {
        "Object", "Function", "String",  "Number",    "Boolean",   "Symbol",  "BigInt",  "Array",
        "Date",   "RegExp",   "Error",   "Promise",   "Map",       "Set",     "WeakMap", "WeakSet",
        "Math",   "JSON",     "Reflect", "Proxy",     "ArrayBuffer", "DataView", "console",
    };

    // --- Behaviour -------------------------------------------------------
    //
    // The Unity-compatible base class every user-authored TS behaviour extends.
    // It has no C++ binding table: the instance is a plain JS object produced by
    // `__zNewInstance`, and TypeScriptComponent injects these three properties
    // onto it once, right after construction and before OnAwake.
    const ZSyntheticProperty kBehaviourProperties[] = {
        {ZBehaviourScript::kGameObject, "GameObject", true},
        {ZBehaviourScript::kTransform, "Transform", true},
        {ZBehaviourScript::kComponent, "TypeScriptComponent", true},
    };

    // Lifecycle hooks. These live on the JS prototype (ScriptEnv's console
    // shim), so they are plain .d.ts lines with no C++ counterpart.
    const char* const kBehaviourMethodLines[] = {
        "    OnAwake(): void;",
        "    OnStart(): void;",
        "    OnUpdate(delta_time: number): void;",
        "    OnDestroy(): void;",
        // Unity 的 Invoke 家族。签名必须与 ScriptEnv 里
        // `g.Behaviour.prototype.*` 的转发一致；实现在 TypeScriptComponent 上
        // （Behaviour 不能作为绑定类发布，否则与合成类重名）。
        "    Invoke(methodName: string, time: number): void;",
        "    InvokeRepeating(methodName: string, time: number, repeatRate: number): void;",
        "    CancelInvoke(methodName: string): void;",
        "    IsInvoking(methodName: string): boolean;",
        // Unity 的碰撞 / 触发回调。由 PhysicsManager::DispatchContactEvents
        // 通过 SendMessageWithOther 调用（不是 ZFUNCTION，所以这里只是
        // 声明）—— 不写的话 tsc 会因为 `this.OnCollisionEnter` 不存在而报
        // 错，用户被迫写 `(this as any)` 绕过，类型检查也就没了。
        //
        // 两边都会收到：触发器自己和被触发的那个（只要有脚本）。
        // `other` 在自身碰撞（A 撞 A）时为 undefined —— Jolt 不会产生这种
        // pair，但声明成可选比让调用方不检查更诚实。
        "    OnCollisionEnter(other?: GameObject): void;",
        "    OnCollisionStay(other?: GameObject): void;",
        "    OnCollisionExit(other?: GameObject): void;",
        "    OnTriggerEnter(other?: GameObject): void;",
        "    OnTriggerStay(other?: GameObject): void;",
        "    OnTriggerExit(other?: GameObject): void;",
    };

    ZSyntheticScriptClass g_BehaviourClass {
        "Behaviour",
        nullptr,
        kBehaviourProperties,
        sizeof(kBehaviourProperties) / sizeof(kBehaviourProperties[0]),
        kBehaviourMethodLines,
        sizeof(kBehaviourMethodLines) / sizeof(kBehaviourMethodLines[0]),
        nullptr,
    };
    const ZSyntheticScriptClassRegistrar g_BehaviourRegistrar(g_BehaviourClass);

    // zengine.d.ts:
    //   * Lives under <project>/Intermediate/Typings/ which is gitignored - the
    //     engine OWNS this file and overwrites it on every project open. Users
    //     should not hand-edit it; if they do, their changes will be wiped on
    //     the next launch (this is the same contract Unity uses for the
    //     auto-generated UnityEngine.dll typings).
    //   * Mirrors the runtime shim installed by ScriptEnv::InstallConsoleAndDebugGlobals
    //     (see Wrapper/ScriptEnv.cpp) plus the __zNewInstance helper used by
    //     ScriptEnv::CreateInstance.
    //   * The engine-side class bindings (`declare class GameObject`, ...) are
    //     NOT hand-written here: ZHeaderTool emits a ZClassScriptBinding per
    //     ZFUNCTION-annotated class and ScriptBindingRegistry appends the
    //     matching `declare class` block (see AppendTypeScriptDeclarations).
    //     They are installed as globals of the same name by ScriptEnv, so the
    //     .d.ts and the runtime agree by construction.
    //   * `Behaviour` is NOT declared here: it is a synthetic script class, and
    //     AppendTypeScriptDeclarations emits its `declare class` from the same
    //     ZSyntheticProperty table TypeScriptComponent injects from (keeping the
    //     hand-written block would be a duplicate declaration). It is a class
    //     rather than an interface because `extends Behaviour` needs a
    //     constructor in TS's eyes.
    constexpr const char* kZEngineDtsTemplate =
        "// =========================================================================\n"
        "// AUTO-GENERATED by ZEngine. Do NOT edit -- regenerated on every project open.\n"
        "// Source of truth: ScriptBindingRegistry::BuildTypeScriptDeclarationFile\n"
        "// =========================================================================\n"
        "\n"
        "/** Base class for any script that wants to be attached to a GameObject. */\n"
        // Value types. These are not engine Objects: ZScript::Converter
        // marshals them as plain JS objects with these exact keys, so they are
        // declared as structural interfaces rather than classes.
        "/** Engine value types, marshalled as plain JS objects. */\n"
        "declare interface Vector3 {\n"
        "    x: number;\n"
        "    y: number;\n"
        "    z: number;\n"
        "}\n"
        "\n"
        // Vector4 是运行时材质实例带进来的：SetBaseColor / GetBaseColor 要走
        // ZScript::Converter<Vector4>，脚本侧得有个对应的结构类型。
        "declare interface Vector4 {\n"
        "    x: number;\n"
        "    y: number;\n"
        "    z: number;\n"
        "    w: number;\n"
        "}\n"
        "\n"
        "declare interface Vector2 {\n"
        "    x: number;\n"
        "    y: number;\n"
        "}\n"
        "\n"
        "declare interface Quaternion {\n"
        "    w: number;\n"
        "    x: number;\n"
        "    y: number;\n"
        "    z: number;\n"
        "}\n"
        "\n"
        // m<row><col>，与 ZScript::Converter<Matrix4x4> 的键名一致，也是 Unity 的
        // 拼写。不用扁平数组，否则下标含义会取决于源端是行主序还是列主序。
        "declare interface Matrix4x4 {\n"
        "    m00: number; m01: number; m02: number; m03: number;\n"
        "    m10: number; m11: number; m12: number; m13: number;\n"
        "    m20: number; m21: number; m22: number; m23: number;\n"
        "    m30: number; m31: number; m32: number; m33: number;\n"
        "}\n"
        "\n"
        // AI 的两个意图结构。键名与 ZScript::Converter<AIMoveIntent> /
        // <AIActionRequest> 一致。moveTarget / faceTarget 复用 Vector3。
        "declare interface AIMoveIntent {\n"
        "    hasMoveTarget: boolean;\n"
        "    moveTarget: Vector3;\n"
        "    speedRatio: number;\n"
        "    hasFaceTarget: boolean;\n"
        "    faceTarget: Vector3;\n"
        "}\n"
        "\n"
        "declare interface AIActionRequest {\n"
        "    actionName: string;\n"
        "    targetId: number;\n"
        "}\n"
        "\n"
        // Unity 的 RaycastHit。`valid` 是 Unity 用 Physics.Raycast 的 bool
        // 返回值表达的那一位——TS 侧没有 out 参数，所以由记录自己携带。
        // 键名与 ZScript::Converter<RaycastHit> 一致。
        "declare interface RaycastHit {\n"
        "    valid: boolean;\n"
        "    point: Vector3;\n"
        "    normal: Vector3;\n"
        "    distance: number;\n"
        "    bodyId: number;\n"
        "    layer: number;\n"
        "    /** 打中的 body 属于哪个对象。纯几何体（没有 RigidBodyComponent 的）为 null。 */\n"
        "    gameObject: GameObject | null;\n"
        "}\n"
        "\n"
        "/** Engine-side logging facade. Routes to the [ZScripting] log category. */\n"
        // 注意：`Debug` 是 namespace 而不是 class —— 它是一个纯 JS 对象
        // （ScriptEnv 的 shim），支持 `Debug.Log("a", 1, obj)` 任意个参数，
        // 而 ZFUNCTION 的签名是固定的。改成绑定类会同时丢掉多参和覆盖掉
        // 那个 JS 对象。见 Runtime/Scripting/ScriptDebug/ScriptDebug.h。
        "declare namespace Debug {\n"
        "    function Log(...args: any[]): void;\n"
        "    function LogWarning(...args: any[]): void;\n"
        "    function LogError(...args: any[]): void;\n"
        "    /** 打异常的 stack（没有就退化为 String(e)）。 */\n"
        "    function LogException(e: any): void;\n"
        "    /** 条件为假时记一条 error。与 C++ assert 不同，不中断进程。 */\n"
        "    function Assert(condition: boolean, message?: string): void;\n"
        "    /** 当前二进制是不是开发构建（判据是 NDEBUG）。 */\n"
        "    function IsDebugBuild(): boolean;\n"
        "    /** 日志留痕并挂住调试器；没有调试器时只记日志，不中断。 */\n"
        "    function Break(message?: string): void;\n"
        "}\n"
        "\n"
        "/** Standard browser-style console, also routed into [ZScripting]. */\n"
        "declare const console: {\n"
        "    log(...args: any[]): void;\n"
        "    info(...args: any[]): void;\n"
        "    debug(...args: any[]): void;\n"
        "    warn(...args: any[]): void;\n"
        "    error(...args: any[]): void;\n"
        "};\n"
        "\n"
        "// Internal helper - keep declared so user code that accidentally references\n"
        "// it doesn't get a fresh `any` (which would mask typos).\n"
        "declare function __zNewInstance<T>(ctor: { new(): T }): T;\n"
        "\n"
        "// ---- P8 HMR ----\n"
        "// 运行时由 ScriptEnv 的 IIFE 包装注入（`module.hot` 只在这层里存在）。\n"
        "// 不声明的话用户写 `if (module.hot) module.hot.accept()` 会直接 TS2304，\n"
        "// HMR 在 .ts 侧等于不可用 —— 而 .ts 才是用户真正写的东西。\n"
        "// `hot` 声明成可选：非模块上下文（比如被 eval 的裸片段）里它可能不在。\n"
        "declare var module: {\n"
        "    exports: any;\n"
        "    hot?: {\n"
        "        /** 声明本模块可以热替换。给了 cb 就在换完 exports 之后以新 exports 调它。 */\n"
        "        accept(callback?: (newExports: any) => void): void;\n"
        "        /** 旧 exports 被丢弃之前调用（清定时器 / 事件监听）。 */\n"
        "        dispose(callback: () => void): void;\n"
        "        /** 永不热替换，一律走 P6 全量重建。 */\n"
        "        decline(): void;\n"
        "    };\n"
        "};\n";

}  // namespace

namespace ScriptBindingRegistry
{
    bool IsReservedScriptName(const char* name)
    {
        if (name == nullptr)
            return false;
        for (const char* reserved : kReservedScriptNames)
        {
            if (std::strcmp(reserved, name) == 0)
                return true;
        }
        return false;
    }

    void Register(ZClassScriptBinding& binding)
    {
        binding.next = g_Head;
        g_Head = &binding;
    }

    const ZClassScriptBinding* Classes() { return g_Head; }

    void RegisterEnum(ZScriptEnum& script_enum)
    {
        script_enum.next = g_EnumHead;
        g_EnumHead = &script_enum;
    }

    const ZScriptEnum* Enums() { return g_EnumHead; }

    void RegisterSynthetic(ZSyntheticScriptClass& cls)
    {
        cls.next = g_SyntheticHead;
        g_SyntheticHead = &cls;
    }

    const ZSyntheticScriptClass* SyntheticClasses() { return g_SyntheticHead; }

    void InstallAll(pesapi_registry_api* apis, pesapi_registry registry)
    {
        if (apis == nullptr || registry == nullptr || g_Installed)
            return;
        g_Installed = true;

        std::vector<const ZClassScriptBinding*> classes;
        for (const ZClassScriptBinding* cls = g_Head; cls != nullptr; cls = cls->next)
            classes.push_back(cls);

        // Pass 1 -- a bound class may derive from a class that has no ZFUNCTION
        // of its own (GameObject -> Object). pesapi resolves the super chain
        // through the registry, so an unknown super type id would leave the
        // derived class with a broken prototype. Declare those supers first as
        // empty classes; the .d.ts emits matching `declare class <Super> {}`.
        std::vector<const void*> declared;
        for (const ZClassScriptBinding* cls : classes)
            declared.push_back(cls->typeId);
        for (const ZClassScriptBinding* cls : classes)
        {
            if (cls->superTypeId == nullptr || cls->superName == nullptr || cls->superName[0] == '\0')
                continue;
            if (IsTypeDeclared(declared, cls->superTypeId))
                continue;
            apis->define_class(registry, cls->superTypeId, nullptr, nullptr, cls->superName, nullptr, nullptr, nullptr, 1);
            apis->set_property_info_size(registry, cls->superTypeId, 0, 0, 0, 0);
            declared.push_back(cls->superTypeId);
        }

        // Pass 2 -- the real classes. Registration order does not matter here:
        // pesapi only needs the type ids to be unique, and pass 1 already made
        // sure every referenced super exists.
        for (const ZClassScriptBinding* cls : classes)
        {
            apis->define_class(registry, cls->typeId, cls->superTypeId, nullptr, cls->scriptName, nullptr, nullptr, nullptr, 1);
            // pesapi 把实例方法与静态方法放在**两张**表里（ScriptClassDefinition::Methods / Functions，各按
            // set_property_info_size 给的个数 malloc，名字为空的那一项当结束符）⇒ 两种要分开计数、各自从 0 编号。
            // 以前静态方法个数传 0、下标用合表里的位置：第一个静态 ZFUNCTION（SpineRenderer::Of）一注册就写穿
            // Functions[] 的堆块（启动即崩），而且 Methods[] 在它的位置留下一个空洞 ⇒ 排在后面的实例方法全部"消失"。
            int instance_count = 0;
            int static_count = 0;
            for (std::size_t i = 0; i < cls->methodCount; ++i)
            {
                if (cls->methods[i].callback != nullptr)
                    ++(cls->methods[i].isStatic ? static_count : instance_count);
            }
            apis->set_property_info_size(registry, cls->typeId, instance_count, static_count,
                                         static_cast<int>(cls->propertyCount), 0);
            int instance_index = 0;
            int static_index = 0;
            for (std::size_t i = 0; i < cls->methodCount; ++i)
            {
                const ZScriptMethodEntry& method = cls->methods[i];
                if (method.callback == nullptr)
                    continue;
                const int index = method.isStatic ? static_index++ : instance_index++;
                apis->set_method_info(registry, cls->typeId, index, method.name, method.isStatic ? 1 : 0, method.callback,
                                      nullptr, 1);
            }
            // Instance (not static) accessor properties. is_static must be 0 or
            // pesapi files them under `Variables` (class-level) instead of
            // `Properties`, and only Properties are installed on the prototype
            // -- so a 1 here would silently produce an unreachable property.
            for (std::size_t i = 0; i < cls->propertyCount; ++i)
            {
                const ZScriptPropertyEntry& prop = cls->properties[i];
                if (prop.name == nullptr || prop.getter == nullptr)
                    continue;
                apis->set_property_info(registry, cls->typeId, static_cast<int>(i), prop.name, 0,
                                        prop.getter, prop.readonly ? nullptr : prop.setter,
                                        nullptr, nullptr, 1);
            }
        }
    }

    void InstallGlobals(pesapi_ffi* apis, pesapi_env env, bool include_editor_domain)
    {
        if (apis == nullptr || env == nullptr)
            return;

        pesapi_value global = apis->global(env);
        if (global == nullptr)
            return;

        for (const ZClassScriptBinding* cls = g_Head; cls != nullptr; cls = cls->next)
        {
            if (cls->domain == ZScriptDomain::Editor && !include_editor_domain)
                continue;
            if (cls->scriptName == nullptr || cls->scriptName[0] == '\0')
                continue;
            // Never shadow a JS built-in (see kReservedScriptNames).
            if (ScriptBindingRegistry::IsReservedScriptName(cls->scriptName))
                continue;
            pesapi_value ctor = apis->create_class(env, cls->typeId);
            if (ctor == nullptr)
                continue;
            apis->set_property(env, global, cls->scriptName, ctor);
        }

        // ZENUM: a plain object of `{ Name: value }` pairs, matching the
        // `declare enum` the .d.ts emits from the same table. Without it the
        // ambient declaration would type-check and then evaluate to undefined.
        for (const ZScriptEnum* script_enum = g_EnumHead; script_enum != nullptr; script_enum = script_enum->next)
        {
            if (script_enum->domain == ZScriptDomain::Editor && !include_editor_domain)
                continue;
            if (script_enum->scriptName == nullptr || script_enum->scriptName[0] == '\0')
                continue;
            if (ScriptBindingRegistry::IsReservedScriptName(script_enum->scriptName))
                continue;
            pesapi_value object = apis->create_object(env);
            if (object == nullptr)
                continue;
            for (std::size_t i = 0; i < script_enum->valueCount; ++i)
            {
                const ZScriptEnumEntry& entry = script_enum->values[i];
                if (entry.name == nullptr || entry.name[0] == '\0')
                    continue;
                apis->set_property(env, object, entry.name, apis->create_int32(env, entry.value));
            }
            apis->set_property(env, global, script_enum->scriptName, object);
        }
    }

    void AppendTypeScriptDeclarations(std::string& out, ZScriptDomain domain)
    {
        // `all` answers "does this name exist anywhere in the program" (an
        // Editor class may extend a Runtime one); `classes` is what this call
        // actually emits.
        std::vector<const ZClassScriptBinding*> all;
        std::vector<const ZClassScriptBinding*> classes;
        for (const ZClassScriptBinding* cls = g_Head; cls != nullptr; cls = cls->next)
        {
            all.push_back(cls);
            if (cls->domain == domain)
                classes.push_back(cls);
        }

        // ZENUM first: class members declare their types with these names, and
        // a reader meeting `mode: CameraMode` before seeing what CameraMode is
        // has to scroll up. The values are printed as literal numbers so the
        // ambient enum and the runtime object cannot drift -- both are fed
        // from the same ZScriptEnumEntry table.
        for (const ZScriptEnum* script_enum = g_EnumHead; script_enum != nullptr; script_enum = script_enum->next)
        {
            if (script_enum->domain != domain)
                continue;
            if (script_enum->scriptName == nullptr || script_enum->scriptName[0] == '\0')
                continue;
            if (ScriptBindingRegistry::IsReservedScriptName(script_enum->scriptName))
                continue;
            out += "declare enum ";
            out += script_enum->scriptName;
            out += " {\n";
            for (std::size_t i = 0; i < script_enum->valueCount; ++i)
            {
                const ZScriptEnumEntry& entry = script_enum->values[i];
                if (entry.name == nullptr || entry.name[0] == '\0')
                    continue;
                out += "    ";
                out += entry.name;
                out += " = ";
                out += std::to_string(entry.value);
                out += ",\n";
            }
            out += "}\n\n";
        }

        if (classes.empty())
            return;

        // Names an `extends` clause may legally refer to. A Runtime file only
        // knows itself; an Editor file is compiled together with the Runtime
        // one, so it may extend a Runtime class too.
        const std::vector<const ZClassScriptBinding*>& known = (domain == ZScriptDomain::Runtime) ? classes : all;

        // A super class with no ZFUNCTION of its own still has to exist in the
        // .d.ts, otherwise `extends <Super>` does not compile. Two derived
        // classes sharing the same unbound super must only emit it once.
        // Editor files never emit placeholders: that is the Runtime file's job.
        for (const ZClassScriptBinding* cls : classes)
        {
            if (domain != ZScriptDomain::Runtime)
                break;
            if (cls->superName == nullptr || cls->superName[0] == '\0')
                continue;
            if (IsDeclared(known, cls->superName))
                continue;
            if (IsPlaceholderEmitted(cls->superName))
                continue;
            // `declare class Object {}` would collide with lib.d.ts.
            if (IsReservedScriptName(cls->superName))
                continue;
            out += "declare class ";
            out += cls->superName;
            out += " {}\n\n";
            g_EmittedPlaceholders.push_back(cls->superName);
        }

        for (const ZClassScriptBinding* cls : classes)
        {
            // A reserved name is skipped by InstallGlobals too, so declaring it
            // here would be a `declare class` with no runtime counterpart (and
            // a collision with lib.d.ts). --check-script-bindings reports it.
            if (ScriptBindingRegistry::IsReservedScriptName(cls->scriptName))
                continue;
            out += "declare class ";
            out += cls->scriptName != nullptr ? cls->scriptName : "?";
            // Only write `extends <Super>` when that name actually exists in
            // the emitted file: either the super has its own binding, or a
            // placeholder was emitted for it above. A reserved name (Object)
            // gets neither, so the class is emitted standalone.
            const bool super_is_bound = cls->superName != nullptr && IsDeclared(known, cls->superName);
            const bool super_has_placeholder = cls->superName != nullptr && IsPlaceholderEmitted(cls->superName);
            if (cls->superName != nullptr && cls->superName[0] != '\0' && (super_is_bound || super_has_placeholder))
            {
                out += " extends ";
                out += cls->superName;
            }
            out += " {\n";
            // Properties first: they are what a Unity-authored script reaches
            // for, and reading the shape of a class before its methods is the
            // order the Inspector shows too.
            for (std::size_t i = 0; i < cls->propertyCount; ++i)
            {
                const ZScriptPropertyEntry& prop = cls->properties[i];
                if (prop.name == nullptr || prop.tsType == nullptr)
                    continue;
                if (prop.docComment != nullptr && prop.docComment[0] != '\0')
                {
                    out += "    /** ";
                    out += prop.docComment;
                    out += " */\n";
                }
                out += "    ";
                out += prop.readonly ? "readonly " : "";
                out += prop.name;
                out += ": ";
                out += prop.tsType;
                out += ";\n";
            }
            for (std::size_t i = 0; i < cls->methodCount; ++i)
            {
                const ZScriptMethodEntry& method = cls->methods[i];
                if (method.tsSignature == nullptr || method.tsSignature[0] == '\0')
                    continue;
                if (method.docComment != nullptr && method.docComment[0] != '\0')
                {
                    out += "    /** ";
                    out += method.docComment;
                    out += " */\n";
                }
                out += method.isStatic ? "    static " : "    ";
                out += method.tsSignature;
                out += ";\n";
            }
            out += "}\n\n";
        }

        // Script-side classes last, so the reader sees the engine types their
        // members refer to first. They belong to the Runtime file only.
        for (const ZSyntheticScriptClass* cls = g_SyntheticHead; domain == ZScriptDomain::Runtime && cls != nullptr; cls = cls->next)
        {
            if (cls->scriptName == nullptr || cls->scriptName[0] == '\0')
                continue;
            out += "declare class ";
            out += cls->scriptName;
            if (cls->superName != nullptr && cls->superName[0] != '\0')
            {
                out += " extends ";
                out += cls->superName;
            }
            out += " {\n";
            for (std::size_t i = 0; i < cls->propertyCount; ++i)
            {
                const ZSyntheticProperty& prop = cls->properties[i];
                if (prop.name == nullptr || prop.typeName == nullptr)
                    continue;
                out += "    ";
                if (prop.readonly)
                    out += "readonly ";
                out += prop.name;
                out += ": ";
                out += prop.typeName;
                out += ";\n";
            }
            for (std::size_t i = 0; i < cls->methodCount; ++i)
            {
                if (cls->methodLines[i] == nullptr)
                    continue;
                out += cls->methodLines[i];
                out += "\n";
            }
            out += "}\n\n";
        }
    }

    std::string BuildTypeScriptDeclarationFile()
    {
        std::string dts = kZEngineDtsTemplate;
        dts += "\n// ---- engine bindings (generated from ZFUNCTION / ZPROPERTY / ZENUM) ----\n\n";
        AppendTypeScriptDeclarations(dts);
        return dts;
    }

    std::string BuildEditorTypeScriptDeclarationFile()
    {
        std::string dts =
            "// =========================================================================\n"
            "// AUTO-GENERATED by ZEngine. Do NOT edit -- regenerated on every editor start.\n"
            "// Source of truth: ScriptBindingRegistry::BuildEditorTypeScriptDeclarationFile\n"
            "// Editor-only API (the UnityEditor.dll equivalent). Compile together with\n"
            "// zengine.d.ts; game scripts must never see this file.\n"
            "// =========================================================================\n"
            "\n";
        AppendTypeScriptDeclarations(dts, ZScriptDomain::Editor);
        return dts;
    }
}  // namespace ScriptBindingRegistry
