#pragma once

// =====================================================================
// ScriptBinding -- ZFUNCTION -> pesapi script bindings (ZHeaderTool phase 3)
// ---------------------------------------------------------------------
// ZHeaderTool turns every ZFUNCTION-annotated member function into a
// ZScriptMethodEntry: a name, the TypeScript signature used for the generated
// .d.ts, and a `pesapi_callback` trampoline. The tables are emitted into each
// module's .gen.cpp and register themselves at static-init time, exactly like
// the phase-2 ZPropertyMeta tables.
//
// The registry is then consumed twice:
//   * ScriptBindingRegistry::InstallAll()  -> pesapi_define_class /
//     pesapi_set_method_info for every method, so JS can call into C++.
//   * ScriptBindingRegistry::AppendTypeScriptDeclarations() -> the
//     `declare class ...` block of the generated zengine.d.ts.
//
// Phase 8 adds the third kind of table: ZENUM. An enum crosses the boundary as
// a plain int32 (see ZScript::Converter's std::is_enum specialisation), which
// alone is not a usable API, so ZHeaderTool also emits the (name, value) list.
// InstallGlobals publishes it as a global object and the .d.ts mirrors it with
// `declare enum`, so `CameraMode.Free` both type-checks and evaluates.
//
// Nothing here affects serialization.
// =====================================================================

#include "Runtime/BaseClasses/Object.h"
#include "Runtime/Core/Math/Matrix4.h"
#include "Runtime/Core/Math/Quaternion.h"
#include "Runtime/Core/Math/Vector2.h"
#include "Runtime/Core/Math/Vector3.h"
#include "pesapi.h"

#include <eastl/string.h>

#include <cstddef>
#include <cstdio>
#include <string>
#include <type_traits>
#include <utility>

// --- registry types -------------------------------------------------------

struct ZScriptMethodEntry
{
    const char* name = nullptr;         // name exposed to TypeScript
    const char* tsSignature = nullptr;  // d.ts member line, e.g. "GetName(): string"
    bool isStatic = false;
    pesapi_callback callback = nullptr;
};

// A pesapi accessor property (phase 7): `obj.name` reads through `getter` and,
// when `setter` is non-null, writes through it. Two sources feed this table:
//   * a ZPROPERTY field whose type has a ZScript::Converter -- ZHeaderTool
//     generates a public ZhtScriptGet_<member> / ZhtScriptSet_<member> pair,
//     so the property stays typed even though the field itself is private.
//   * ZSCRIPTPROPERTY(Name=..., Getter=..., Setter=...), which publishes an
//     existing ZFUNCTION pair as a property (transform.position -> GetPosition
//     / SetPosition). No field required.
// A null setter makes the property read-only: the V8 backend installs it with
// v8::ReadOnly, and the .d.ts declares it `readonly`.
struct ZScriptPropertyEntry
{
    const char* name = nullptr;  // name exposed to TypeScript
    const char* tsType = nullptr;  // d.ts type, e.g. "Vector3" / "number"
    bool readonly = false;
    pesapi_callback getter = nullptr;
    pesapi_callback setter = nullptr;  // null -> read-only
};

// Forward-declared: ScriptBindingRegistry (below) hands out pointers to it.
struct ZSyntheticScriptClass;

struct ZClassScriptBinding
{
    const void* typeId = nullptr;
    const void* superTypeId = nullptr;  // null for the root of a chain
    const char* scriptName = nullptr;
    const char* superName = nullptr;  // for the d.ts `extends` clause
    const ZScriptMethodEntry* methods = nullptr;
    std::size_t methodCount = 0;
    const ZScriptPropertyEntry* properties = nullptr;
    std::size_t propertyCount = 0;
    ZClassScriptBinding* next = nullptr;  // intrusive registry list
};

/// One enumerator of a ZENUM-annotated enum (phase 8).
struct ZScriptEnumEntry
{
    const char* name = nullptr;  // script-facing spelling (PascalCase)
    int32_t value = 0;
};

/// A ZENUM-annotated enum: a script name plus its (name, value) table.
struct ZScriptEnum
{
    const char* scriptName = nullptr;
    const ZScriptEnumEntry* values = nullptr;
    std::size_t valueCount = 0;
    ZScriptEnum* next = nullptr;  // intrusive registry list
};

namespace ScriptBindingRegistry
{
    /// Called from generated static registrars. Type ids are only addresses at
    /// that point, which is all the key needs.
    void Register(ZClassScriptBinding& binding);

    /// Called from generated static registrars for every ZENUM.
    void RegisterEnum(ZScriptEnum& script_enum);

    /// Register a script-side class that has no C++ binding table (Behaviour).
    /// It only contributes a `declare class` block to the .d.ts.
    void RegisterSynthetic(ZSyntheticScriptClass& cls);

    /// Head of the intrusive list, for InstallAll / d.ts generation.
    const ZClassScriptBinding* Classes();

    /// Head of the enum list, for global install / d.ts generation.
    const ZScriptEnum* Enums();

    /// Head of the synthetic list, for d.ts generation and gates.
    const ZSyntheticScriptClass* SyntheticClasses();

    /// Whether `name` is a JS/TS built-in global (Object, String, Math,
    /// console, ...) that must be neither published as a global nor declared in
    /// the .d.ts. Exposed so the drift gates can consult the same list instead
    /// of keeping a copy that would silently drift from it.
    bool IsReservedScriptName(const char* name);

    /// Define every registered class (and its methods) on `registry`. Safe to
    /// call more than once; the second call is a no-op.
    void InstallAll(pesapi_registry_api* apis, pesapi_registry registry);

    /// Publish the constructor of every registered class as a global of the
    /// same name, so `declare class Foo` in the generated .d.ts resolves to
    /// something real at runtime. Must run inside an open scope and AFTER the
    /// env's registry has been set (pesapi_set_registry), because the class
    /// objects are looked up through it. Safe to call more than once.
    ///
    /// Also publishes every ZENUM as a global object of `declare enum` shape,
    /// so `CameraMode.Free` evaluates to the same number the C++ side sees.
    void InstallGlobals(pesapi_ffi* apis, pesapi_env env);

    /// Append `declare enum <Name> { ... }` for every registered enum, then
    /// `declare class <Name> [extends <Super>] { ... }` for every registered
    /// class. Super classes that have no binding of their own are declared
    /// empty so the emitted TypeScript still compiles.
    void AppendTypeScriptDeclarations(std::string& out);
}  // namespace ScriptBindingRegistry

struct ZClassScriptBindingRegistrar
{
    explicit ZClassScriptBindingRegistrar(ZClassScriptBinding& binding)
    {
        ScriptBindingRegistry::Register(binding);
    }
};

struct ZScriptEnumRegistrar
{
    explicit ZScriptEnumRegistrar(ZScriptEnum& script_enum)
    {
        ScriptBindingRegistry::RegisterEnum(script_enum);
    }
};

// --- synthetic (script-side) classes --------------------------------------
//
// `Behaviour` is the odd one out: it has no C++ ZFUNCTION table behind it,
// because the JS `class Foo extends Behaviour` instance is a plain JS object
// created by `__zNewInstance` -- the engine cannot make it a pesapi native
// object without taking over `super()` in every user class. It is instead a
// JS class installed by ScriptEnv's console shim, and the engine injects the
// host GameObject / Transform / Component onto each instance right after
// construction.
//
// That still has to show up in zengine.d.ts, and the names the C++ side
// injects must be the names TypeScript sees. Declaring it here (rather than
// hand-writing the block in ProjectInfo's template) makes both come from the
// same ZSyntheticProperty table; `--check-behaviour-binding` fails the build
// if they ever drift apart.

/// One engine-injected property of a synthetic class.
struct ZSyntheticProperty
{
    const char* name = nullptr;      // JS property name (injected verbatim)
    const char* typeName = nullptr;  // declared class name in the .d.ts
    bool readonly = true;
};

struct ZSyntheticScriptClass
{
    const char* scriptName = nullptr;
    const char* superName = nullptr;  // optional
    const ZSyntheticProperty* properties = nullptr;
    std::size_t propertyCount = 0;
    // Plain .d.ts body lines for members that live on the JS side (the
    // Unity-style lifecycle hooks). Not checked against C++.
    const char* const* methodLines = nullptr;
    std::size_t methodCount = 0;
    ZSyntheticScriptClass* next = nullptr;
};

struct ZSyntheticScriptClassRegistrar
{
    explicit ZSyntheticScriptClassRegistrar(ZSyntheticScriptClass& cls)
    {
        ScriptBindingRegistry::RegisterSynthetic(cls);
    }
};

/// Property names TypeScriptComponent injects onto every Behaviour instance.
/// Keep in lock-step with ZSyntheticProperty above -- the gate enforces it.
namespace ZBehaviourScript
{
    inline constexpr const char* kGameObject = "gameObject";
    inline constexpr const char* kTransform = "transform";
    inline constexpr const char* kComponent = "component";
}  // namespace ZBehaviourScript

// Stable per-type id. Mirrors puerts' own StaticTypeId<T>::get(): the address
// of a function-local static pointer is unique per T and identical across
// translation units, which is all a registry key needs.
template <typename T>
struct ZScriptTypeId
{
    static const void* Get()
    {
        static T* dummy = nullptr;
        return &dummy;
    }
};

namespace ZScript
{
    template <typename>
    struct AlwaysFalse : std::false_type
    {
    };

    /// C++ <-> JS value conversion. Specialize for any type a ZFUNCTION wants
    /// to take or return; the primary template fails the build on purpose so an
    /// unusable signature is a compile error instead of a silent misbind.
    template <typename T, typename Enable = void>
    struct Converter
    {
        static_assert(AlwaysFalse<T>::value,
                      "no ZScript::Converter for this type -- add a specialization, or drop ZFUNCTION from the method");
    };

    template <typename T>
    using Plain = typename std::remove_cv<typename std::remove_reference<T>::type>::type;

    // --- scalars ---------------------------------------------------------

    template <>
    struct Converter<bool>
    {
        static pesapi_value ToJs(pesapi_ffi* apis, pesapi_env env, bool v) { return apis->create_boolean(env, v ? 1 : 0); }
        static bool FromJs(pesapi_ffi* apis, pesapi_env env, pesapi_value v) { return apis->get_value_bool(env, v) != 0; }
    };

    template <>
    struct Converter<float>
    {
        static pesapi_value ToJs(pesapi_ffi* apis, pesapi_env env, float v) { return apis->create_double(env, static_cast<double>(v)); }
        static float FromJs(pesapi_ffi* apis, pesapi_env env, pesapi_value v) { return static_cast<float>(apis->get_value_double(env, v)); }
    };

    template <>
    struct Converter<double>
    {
        static pesapi_value ToJs(pesapi_ffi* apis, pesapi_env env, double v) { return apis->create_double(env, v); }
        static double FromJs(pesapi_ffi* apis, pesapi_env env, pesapi_value v) { return apis->get_value_double(env, v); }
    };

    template <>
    struct Converter<int32_t>
    {
        static pesapi_value ToJs(pesapi_ffi* apis, pesapi_env env, int32_t v) { return apis->create_int32(env, v); }
        static int32_t FromJs(pesapi_ffi* apis, pesapi_env env, pesapi_value v) { return apis->get_value_int32(env, v); }
    };

    template <>
    struct Converter<uint32_t>
    {
        static pesapi_value ToJs(pesapi_ffi* apis, pesapi_env env, uint32_t v) { return apis->create_uint32(env, v); }
        static uint32_t FromJs(pesapi_ffi* apis, pesapi_env env, pesapi_value v) { return apis->get_value_uint32(env, v); }
    };

    template <>
    struct Converter<int64_t>
    {
        static pesapi_value ToJs(pesapi_ffi* apis, pesapi_env env, int64_t v) { return apis->create_double(env, static_cast<double>(v)); }
        static int64_t FromJs(pesapi_ffi* apis, pesapi_env env, pesapi_value v) { return static_cast<int64_t>(apis->get_value_double(env, v)); }
    };

    template <>
    struct Converter<uint64_t>
    {
        static pesapi_value ToJs(pesapi_ffi* apis, pesapi_env env, uint64_t v) { return apis->create_double(env, static_cast<double>(v)); }
        static uint64_t FromJs(pesapi_ffi* apis, pesapi_env env, pesapi_value v) { return static_cast<uint64_t>(apis->get_value_double(env, v)); }
    };

    // size_t / int / unsigned / long: TS `number`, routed through double so a
    // GObjectID (size_t) survives as an integer-valued number.
    template <typename T>
    struct Converter<T, typename std::enable_if<std::is_integral<T>::value && !std::is_same<T, bool>::value &&
                                                !std::is_same<T, int32_t>::value && !std::is_same<T, uint32_t>::value &&
                                                !std::is_same<T, int64_t>::value && !std::is_same<T, uint64_t>::value>::type>
    {
        static pesapi_value ToJs(pesapi_ffi* apis, pesapi_env env, T v) { return apis->create_double(env, static_cast<double>(v)); }
        static T FromJs(pesapi_ffi* apis, pesapi_env env, pesapi_value v) { return static_cast<T>(apis->get_value_double(env, v)); }
    };

    // --- strings ---------------------------------------------------------

    // Two-pass convention, matching puerts' own V8 implementation: calling with
    // a null buffer only fills `bufsize` (and returns null), the second call
    // writes into the buffer. `len` is the UTF-8 byte count *without* the NUL,
    // so the buffer is one byte larger and the terminator is dropped again.
    template <>
    struct Converter<std::string>
    {
        static pesapi_value ToJs(pesapi_ffi* apis, pesapi_env env, const std::string& v)
        {
            return apis->create_string_utf8(env, v.c_str(), v.size());
        }
        static std::string FromJs(pesapi_ffi* apis, pesapi_env env, pesapi_value v)
        {
            std::size_t len = 0;
            apis->get_value_string_utf8(env, v, nullptr, &len);
            if (len == 0)
                return {};
            std::string out(len + 1, '\0');
            std::size_t capacity = len + 1;
            apis->get_value_string_utf8(env, v, &out[0], &capacity);
            out.resize(len);
            return out;
        }
    };

    template <>
    struct Converter<eastl::string>
    {
        static pesapi_value ToJs(pesapi_ffi* apis, pesapi_env env, const eastl::string& v)
        {
            return apis->create_string_utf8(env, v.c_str(), v.length());
        }
        static eastl::string FromJs(pesapi_ffi* apis, pesapi_env env, pesapi_value v)
        {
            std::size_t len = 0;
            apis->get_value_string_utf8(env, v, nullptr, &len);
            if (len == 0)
                return {};
            eastl::string out;
            out.resize(len + 1);
            std::size_t capacity = len + 1;
            apis->get_value_string_utf8(env, v, &out[0], &capacity);
            out.resize(len);
            return out;
        }
    };

    // --- math ------------------------------------------------------------

    template <>
    struct Converter<Vector3>
    {
        static pesapi_value ToJs(pesapi_ffi* apis, pesapi_env env, const Vector3& v)
        {
            pesapi_value o = apis->create_object(env);
            apis->set_property(env, o, "x", apis->create_double(env, v.x));
            apis->set_property(env, o, "y", apis->create_double(env, v.y));
            apis->set_property(env, o, "z", apis->create_double(env, v.z));
            return o;
        }
        static Vector3 FromJs(pesapi_ffi* apis, pesapi_env env, pesapi_value v)
        {
            Vector3 out;
            out.x = static_cast<float>(apis->get_value_double(env, apis->get_property(env, v, "x")));
            out.y = static_cast<float>(apis->get_value_double(env, apis->get_property(env, v, "y")));
            out.z = static_cast<float>(apis->get_value_double(env, apis->get_property(env, v, "z")));
            return out;
        }
    };

    template <>
    struct Converter<Quaternion>
    {
        static pesapi_value ToJs(pesapi_ffi* apis, pesapi_env env, const Quaternion& q)
        {
            pesapi_value o = apis->create_object(env);
            apis->set_property(env, o, "w", apis->create_double(env, q.w));
            apis->set_property(env, o, "x", apis->create_double(env, q.x));
            apis->set_property(env, o, "y", apis->create_double(env, q.y));
            apis->set_property(env, o, "z", apis->create_double(env, q.z));
            return o;
        }
        static Quaternion FromJs(pesapi_ffi* apis, pesapi_env env, pesapi_value v)
        {
            Quaternion out;
            out.w = static_cast<float>(apis->get_value_double(env, apis->get_property(env, v, "w")));
            out.x = static_cast<float>(apis->get_value_double(env, apis->get_property(env, v, "x")));
            out.y = static_cast<float>(apis->get_value_double(env, apis->get_property(env, v, "y")));
            out.z = static_cast<float>(apis->get_value_double(env, apis->get_property(env, v, "z")));
            return out;
        }
    };

    template <>
    struct Converter<Vector2>
    {
        static pesapi_value ToJs(pesapi_ffi* apis, pesapi_env env, const Vector2& v)
        {
            pesapi_value o = apis->create_object(env);
            apis->set_property(env, o, "x", apis->create_double(env, v.x));
            apis->set_property(env, o, "y", apis->create_double(env, v.y));
            return o;
        }
        static Vector2 FromJs(pesapi_ffi* apis, pesapi_env env, pesapi_value v)
        {
            Vector2 out;
            out.x = static_cast<float>(apis->get_value_double(env, apis->get_property(env, v, "x")));
            out.y = static_cast<float>(apis->get_value_double(env, apis->get_property(env, v, "y")));
            return out;
        }
    };

    // Matrix4x4 crosses as 16 named numbers, Unity's `m<row><col>` spelling.
    // m_Mat is indexed [row][col] (see Matrix4.h), so m12 is row 1 column 2 --
    // the same convention Unity uses, unlike a flat array where the meaning of
    // index 6 would depend on whether the source was row- or column-major.
    template <>
    struct Converter<Matrix4x4>
    {
        static pesapi_value ToJs(pesapi_ffi* apis, pesapi_env env, const Matrix4x4& m)
        {
            pesapi_value o = apis->create_object(env);
            for (int row = 0; row < 4; ++row)
            {
                for (int col = 0; col < 4; ++col)
                {
                    char name[8];
                    std::snprintf(name, sizeof(name), "m%d%d", row, col);
                    apis->set_property(env, o, name, apis->create_double(env, m.m_Mat[row][col]));
                }
            }
            return o;
        }
        static Matrix4x4 FromJs(pesapi_ffi* apis, pesapi_env env, pesapi_value v)
        {
            Matrix4x4 out;
            for (int row = 0; row < 4; ++row)
            {
                for (int col = 0; col < 4; ++col)
                {
                    char name[8];
                    std::snprintf(name, sizeof(name), "m%d%d", row, col);
                    out.m_Mat[row][col] =
                        static_cast<float>(apis->get_value_double(env, apis->get_property(env, v, name)));
                }
            }
            return out;
        }
    };

    // --- enums ----------------------------------------------------------
    //
    // ZENUM-annotated enums cross the boundary as int32. `std::is_enum` is
    // enough to select this generically, but a *number* alone is not a usable
    // script API -- scripts need the names. ZHeaderTool therefore also emits a
    // (name, value) table per enum, which the runtime publishes as a global
    // object and the .d.ts mirrors with `declare enum`, so `CameraMode.Free`
    // both type-checks and evaluates. See ZScriptEnum below.
    //
    // Note this must stay disjoint from the integral specialisation above: an
    // enum is not std::is_integral, and both are checked in the same enable_if
    // slot, so an overlap would be an ambiguous partial specialisation.
    template <typename T>
    struct Converter<T, typename std::enable_if<std::is_enum<T>::value>::type>
    {
        static pesapi_value ToJs(pesapi_ffi* apis, pesapi_env env, T v)
        {
            return apis->create_int32(env, static_cast<int32_t>(v));
        }
        static T FromJs(pesapi_ffi* apis, pesapi_env env, pesapi_value v)
        {
            return static_cast<T>(apis->get_value_int32(env, v));
        }
    };

    // --- Object-derived pointers ----------------------------------------

    template <typename T>
    struct Converter<T*, typename std::enable_if<std::is_base_of<Object, T>::value>::type>
    {
        static pesapi_value ToJs(pesapi_ffi* apis, pesapi_env env, T* value)
        {
            if (value == nullptr)
                return apis->create_null(env);
            // call_finalize = 0: JS does not own engine objects.
            return apis->native_object_to_value(env, ZScriptTypeId<T>::Get(), value, 0);
        }
        static T* FromJs(pesapi_ffi* apis, pesapi_env env, pesapi_value value)
        {
            if (apis->is_null(env, value) || apis->is_undefined(env, value))
                return nullptr;
            return static_cast<T*>(apis->get_native_object_ptr(env, value));
        }
    };

    // --- trampolines ------------------------------------------------------

    template <typename R>
    struct Returner
    {
        template <typename Fn>
        static void Apply(pesapi_ffi* apis, pesapi_callback_info info, Fn&& fn)
        {
            pesapi_env env = apis->get_env(info);
            R value = fn();
            apis->add_return(info, Converter<Plain<R>>::ToJs(apis, env, value));
        }
    };

    template <>
    struct Returner<void>
    {
        template <typename Fn>
        static void Apply(pesapi_ffi*, pesapi_callback_info, Fn&& fn)
        {
            fn();
        }
    };

    template <auto Method>
    struct MethodCallback;

    template <typename C, typename R, typename... Args, R (C::*Method)(Args...)>
    struct MethodCallback<Method>
    {
        template <std::size_t... I>
        static R Call(pesapi_ffi* apis, pesapi_env env, C* self, pesapi_callback_info info, std::index_sequence<I...>)
        {
            return (self->*Method)(Converter<Plain<Args>>::FromJs(apis, env, apis->get_arg(info, static_cast<int>(I)))...);
        }

        static void Invoke(pesapi_ffi* apis, pesapi_callback_info info)
        {
            pesapi_env env = apis->get_env(info);
            C* self = static_cast<C*>(apis->get_native_holder_ptr(info));
            if (self == nullptr)
            {
                apis->throw_by_string(info, "ZScript: method called on a null native object");
                return;
            }
            Returner<R>::Apply(apis, info, [&]() {
                return Call(apis, env, self, info, std::index_sequence_for<Args...> {});
            });
        }
    };

    template <typename C, typename R, typename... Args, R (C::*Method)(Args...) const>
    struct MethodCallback<Method>
    {
        template <std::size_t... I>
        static R Call(pesapi_ffi* apis, pesapi_env env, const C* self, pesapi_callback_info info, std::index_sequence<I...>)
        {
            return (self->*Method)(Converter<Plain<Args>>::FromJs(apis, env, apis->get_arg(info, static_cast<int>(I)))...);
        }

        static void Invoke(pesapi_ffi* apis, pesapi_callback_info info)
        {
            pesapi_env env = apis->get_env(info);
            const C* self = static_cast<const C*>(apis->get_native_holder_ptr(info));
            if (self == nullptr)
            {
                apis->throw_by_string(info, "ZScript: method called on a null native object");
                return;
            }
            Returner<R>::Apply(apis, info, [&]() {
                return Call(apis, env, self, info, std::index_sequence_for<Args...> {});
            });
        }
    };

    template <typename R, typename... Args, R (*Function)(Args...)>
    struct MethodCallback<Function>
    {
        template <std::size_t... I>
        static R Call(pesapi_ffi* apis, pesapi_env env, pesapi_callback_info info, std::index_sequence<I...>)
        {
            return (*Function)(Converter<Plain<Args>>::FromJs(apis, env, apis->get_arg(info, static_cast<int>(I)))...);
        }

        static void Invoke(pesapi_ffi* apis, pesapi_callback_info info)
        {
            pesapi_env env = apis->get_env(info);
            Returner<R>::Apply(apis, info, [&]() { return Call(apis, env, info, std::index_sequence_for<Args...> {}); });
        }
    };
}  // namespace ZScript
