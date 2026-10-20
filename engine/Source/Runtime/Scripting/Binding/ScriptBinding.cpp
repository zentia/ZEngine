#include "Runtime/Scripting/Binding/ScriptBinding.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace
{
    // Constant-initialized, so it is valid before any dynamic static init
    // (generated registrars run during dynamic init, in unspecified order).
    ZClassScriptBinding* g_Head = nullptr;
    ZSyntheticScriptClass* g_SyntheticHead = nullptr;
    bool g_Installed = false;

    // Placeholder `declare class <Super> {}` emitted for supers that have no
    // binding of their own. Kept outside AppendTypeScriptDeclarations so a
    // second call (one .d.ts per project, but the call may be exercised more
    // than once in tests) does not duplicate them.
    std::vector<const char*> g_EmittedPlaceholders;

    bool IsDeclared(const std::vector<const ZClassScriptBinding*>& classes, const char* name)
    {
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
}  // namespace

namespace ScriptBindingRegistry
{
    void Register(ZClassScriptBinding& binding)
    {
        binding.next = g_Head;
        g_Head = &binding;
    }

    const ZClassScriptBinding* Classes() { return g_Head; }

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
            apis->set_property_info_size(registry, cls->typeId, static_cast<int>(cls->methodCount), 0,
                                         static_cast<int>(cls->propertyCount), 0);
            for (std::size_t i = 0; i < cls->methodCount; ++i)
            {
                const ZScriptMethodEntry& method = cls->methods[i];
                if (method.callback == nullptr)
                    continue;
                apis->set_method_info(registry, cls->typeId, static_cast<int>(i), method.name,
                                      method.isStatic ? 1 : 0, method.callback, nullptr, 1);
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

    void InstallGlobals(pesapi_ffi* apis, pesapi_env env)
    {
        if (apis == nullptr || env == nullptr)
            return;

        pesapi_value global = apis->global(env);
        if (global == nullptr)
            return;

        for (const ZClassScriptBinding* cls = g_Head; cls != nullptr; cls = cls->next)
        {
            if (cls->scriptName == nullptr || cls->scriptName[0] == '\0')
                continue;
            // Never shadow a JS built-in (see kReservedScriptNames).
            if (IsReservedScriptName(cls->scriptName))
                continue;
            pesapi_value ctor = apis->create_class(env, cls->typeId);
            if (ctor == nullptr)
                continue;
            apis->set_property(env, global, cls->scriptName, ctor);
        }
    }

    void AppendTypeScriptDeclarations(std::string& out)
    {
        std::vector<const ZClassScriptBinding*> classes;
        for (const ZClassScriptBinding* cls = g_Head; cls != nullptr; cls = cls->next)
            classes.push_back(cls);
        if (classes.empty())
            return;

        // A super class with no ZFUNCTION of its own still has to exist in the
        // .d.ts, otherwise `extends <Super>` does not compile. Two derived
        // classes sharing the same unbound super must only emit it once.
        for (const ZClassScriptBinding* cls : classes)
        {
            if (cls->superName == nullptr || cls->superName[0] == '\0')
                continue;
            if (IsDeclared(classes, cls->superName))
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
            out += "declare class ";
            out += cls->scriptName != nullptr ? cls->scriptName : "?";
            // Only write `extends <Super>` when that name actually exists in
            // the emitted file: either the super has its own binding, or a
            // placeholder was emitted for it above. A reserved name (Object)
            // gets neither, so the class is emitted standalone.
            const bool super_is_bound = cls->superName != nullptr && IsDeclared(classes, cls->superName);
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
                out += "    ";
                out += method.tsSignature;
                out += ";\n";
            }
            out += "}\n\n";
        }

        // Script-side classes last, so the reader sees the engine types their
        // members refer to first.
        for (const ZSyntheticScriptClass* cls = g_SyntheticHead; cls != nullptr; cls = cls->next)
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
}  // namespace ScriptBindingRegistry
