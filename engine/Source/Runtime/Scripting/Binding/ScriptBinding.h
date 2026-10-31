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
// `RaycastHit::gameObject` 的 Converter 要用 `Converter<GameObject*>`，而那个
// 特化要求 `std::is_base_of<Object, T>` —— 只有前向声明的话它会静默退回
// "no ZScript::Converter for this type"。Object 已经包含在上面，这里补
// GameObject 的完整定义。
//
// 循环依赖检查：GameObject.h -> Component.h -> Object.h，都不反过来包含本文件
// （只有 GameObject.cpp 包含），所以没有环。本文件也只被 10 个翻译单元引入，
// 不是全量 .gen.cpp。
#include "Runtime/BaseClasses/GameObject.h"
// Color is a plain POD (no Object base), which is exactly why it can be a
// script *value* type: ZScript::Converter<Color> marshals it as a plain
// {r,g,b,a} object. Core/Color/Color.h is header-only and pulls in nothing but
// Vector3, so including it here costs every .gen.cpp nothing.
#include "Runtime/Core/Color/Color.h"
#include "Runtime/Core/Math/Matrix4.h"
#include "Runtime/Core/Math/Quaternion.h"
#include "Runtime/Core/Math/Vector2.h"
#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Core/Math/Vector4.h"
// AIMoveIntent / AIActionRequest: deliberately the lightweight header that
// MotorComponent also includes -- it pulls in math + object ids and nothing
// else, so binding the two AI value structs does not drag behaviour trees or
// perception into every generated .gen.cpp.
#include "Runtime/Function/AI/AIMoveIntent.h"
// PhysicsQuery.h is the Jolt-free half of the physics module: RaycastHit has to
// be marshallable here, and PhysicsScene.h would pull Jolt/Jolt.h into every
// generated .gen.cpp (and into the Web build, which does not link Jolt).
#include "Runtime/Function/Physics/PhysicsQuery.h"
#include "pesapi.h"

#include <eastl/string.h>

#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <string>
#include <type_traits>
#include <vector>
#include <utility>

// --- registry types -------------------------------------------------------

struct ZScriptMethodEntry
{
    const char* name = nullptr;         // name exposed to TypeScript
    const char* tsSignature = nullptr;  // d.ts member line, e.g. "GetName(): string"
    // JSDoc 注释行（ZHeaderTool 从 C++ 侧注释提取，null = 没有）。在生成的
    // zengine.d.ts 里作为 `/** ... */` 打在成员前面 —— 编辑器里的 TS 悬停提示
    // 全靠它。⚠️ 这个字段是 ZHeaderTool 的 codegen.py **已经在产出**的，删掉
    // 会让所有 .gen.cpp 编译失败（C7559 designator 不匹配）。
    const char* docComment = nullptr;
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
    const char* docComment = nullptr;  // JSDoc 注释行（见 ZScriptMethodEntry 的同名字段）
    bool readonly = false;
    pesapi_callback getter = nullptr;
    pesapi_callback setter = nullptr;  // null -> read-only
    // true -> the getter is a static ZFUNCTION, so this is a *class-level*
    // property (`Color.red`) rather than an instance one (`transform.position`).
    //
    // pesapi keeps the two apart on purpose: `pesapi_set_property_info(...,
    // is_static=1)` files the entry under the class's `Variables` instead of its
    // `Properties`, and the backend installs `Variables` on the *constructor*
    // while `Properties` go on the prototype. Getting this wrong is silent --
    // the property simply never appears anywhere -- so the flag is part of the
    // generated table rather than being re-derived at install time.
    bool isStatic = false;
};

// Forward-declared: ScriptBindingRegistry (below) hands out pointers to it.
struct ZSyntheticScriptClass;

/// Which script VM a binding is visible from. Mirrors Unity's split between
/// UnityEngine.dll (runtime, shipped in the player) and UnityEditor.dll
/// (editor only): a class registered in the Editor domain is published only
/// into the editor-scripting VM and is declared in zengine-editor.d.ts, never
/// in the zengine.d.ts that game scripts compile against.
///
/// ZHeaderTool stamps the domain per module (ZRuntime -> Runtime, ZEditor ->
/// Editor), so annotating an editor class needs no extra specifier.
enum class ZScriptDomain : unsigned char
{
    Runtime = 0,
    Editor = 1,
};

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
    ZScriptDomain domain = ZScriptDomain::Runtime;  // must stay last: generated code aggregate-inits up to here
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
    ZScriptDomain domain = ZScriptDomain::Runtime;
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
    ///
    /// `include_editor_domain` also publishes the Editor-domain classes and
    /// enums. Only the editor-scripting VM passes true; the game VM never sees
    /// editor bindings.
    void InstallGlobals(pesapi_ffi* apis, pesapi_env env, bool include_editor_domain = false);

    /// Append `declare enum <Name> { ... }` for every registered enum, then
    /// `declare class <Name> [extends <Super>] { ... }` for every registered
    /// class. Super classes that have no binding of their own are declared
    /// empty so the emitted TypeScript still compiles.
    ///
    /// Only bindings of `domain` are emitted. Runtime also emits the synthetic
    /// (script-side) classes and the placeholder supers; Editor assumes the
    /// Runtime file is part of the same TypeScript program.
    void AppendTypeScriptDeclarations(std::string& out, ZScriptDomain domain = ZScriptDomain::Runtime);

    /// 完整的 `zengine.d.ts` 内容 = 手写前缀 + `AppendTypeScriptDeclarations`。
    ///
    /// 编辑器写文件（`ProjectInfo::EnsureScriptsScaffold`）和任何想在编辑器之外
    /// 复现同一份声明的地方（工具链校验、冒烟测试）都走这里 —— 分成两处写的后果
    /// 是用户编译所依赖的 .d.ts 和运行时实际装的 global 悄悄漂移，而两边各自的
    /// 检查都还是绿的。
    std::string BuildTypeScriptDeclarationFile();

    /// `zengine-editor.d.ts`: the Editor-domain bindings only. It is meant to be
    /// compiled together with `zengine.d.ts` (editor scripts see both), the way
    /// UnityEditor.dll references UnityEngine.dll.
    std::string BuildEditorTypeScriptDeclarationFile();
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

    // A path crosses the boundary as a plain string, but the *direction* matters:
    // ToJs goes through generic_string() so scripts always see forward slashes,
    // while FromJs accepts whatever the script hands over. Without the
    // generic_string() the same asset would surface as "a/b.prefab" on Linux and
    // "a\\b.prefab" on Windows, and any script that compares or concatenates
    // paths would silently break on exactly one platform.
    template <>
    struct Converter<std::filesystem::path>
    {
        static pesapi_value ToJs(pesapi_ffi* apis, pesapi_env env, const std::filesystem::path& v)
        {
            const std::string s = v.generic_string();
            return apis->create_string_utf8(env, s.c_str(), s.size());
        }
        static std::filesystem::path FromJs(pesapi_ffi* apis, pesapi_env env, pesapi_value v)
        {
            std::size_t len = 0;
            apis->get_value_string_utf8(env, v, nullptr, &len);
            if (len == 0)
                return {};
            std::string out(len + 1, '\0');
            std::size_t capacity = len + 1;
            apis->get_value_string_utf8(env, v, &out[0], &capacity);
            out.resize(len);
            return std::filesystem::path(out);
        }
    };

    // --- containers (P4) -------------------------------------------------
    //
    // std::vector<T> <-> JS array, for any T that already has a Converter. The
    // element conversion is recursive, so vector<vector<int>> works too, and a
    // vector of an unsupported element type still fails at compile time with
    // the primary template's static_assert rather than misbinding.
    //
    // Why this is BY VALUE in both directions
    // ---------------------------------------
    // A JS array and a std::vector have no shared representation, so there is
    // no way to hand a script a live view of engine memory here -- every
    // crossing copies. That is the honest cost, and it is why a getter like
    // `GetSelectedObjects()` is a snapshot: mutating the returned array does
    // not touch the selection, and the array does not update when the
    // selection changes. Counted accessors (GetObjectCount + GetObjectAt)
    // remain the right shape for a hot loop over a large collection; this is
    // for the ergonomic case where a script wants the whole thing at once.
    //
    // A non-array / null JS value converts to an EMPTY vector rather than
    // throwing: these cross at a C ABI callback boundary where there is no
    // exception to propagate, and every other Converter here is similarly
    // permissive about bad input.
    template <typename T>
    struct Converter<std::vector<T>>
    {
        static pesapi_value ToJs(pesapi_ffi* apis, pesapi_env env, const std::vector<T>& v)
        {
            pesapi_value array = apis->create_array(env);
            for (std::size_t i = 0; i < v.size(); ++i)
            {
                apis->set_property_uint32(env, array, static_cast<uint32_t>(i),
                                          Converter<Plain<T>>::ToJs(apis, env, v[i]));
            }
            return array;
        }

        static std::vector<T> FromJs(pesapi_ffi* apis, pesapi_env env, pesapi_value v)
        {
            std::vector<T> out;
            if (v == nullptr || apis->is_array(env, v) == 0)
                return out;
            const uint32_t length = apis->get_array_length(env, v);
            out.reserve(length);
            for (uint32_t i = 0; i < length; ++i)
            {
                out.push_back(Converter<Plain<T>>::FromJs(apis, env, apis->get_property_uint32(env, v, i)));
            }
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

    // Vector4 crosses as x / y / z / w. Needed by the runtime material instance:
    // the base-colour slot carries alpha in w, and a vec3 cannot hold it.
    template <>
    struct Converter<Vector4>
    {
        static pesapi_value ToJs(pesapi_ffi* apis, pesapi_env env, const Vector4& v)
        {
            pesapi_value o = apis->create_object(env);
            apis->set_property(env, o, "x", apis->create_double(env, v.x));
            apis->set_property(env, o, "y", apis->create_double(env, v.y));
            apis->set_property(env, o, "z", apis->create_double(env, v.z));
            apis->set_property(env, o, "w", apis->create_double(env, v.w));
            return o;
        }
        static Vector4 FromJs(pesapi_ffi* apis, pesapi_env env, pesapi_value v)
        {
            Vector4 out;
            out.x = static_cast<float>(apis->get_value_double(env, apis->get_property(env, v, "x")));
            out.y = static_cast<float>(apis->get_value_double(env, apis->get_property(env, v, "y")));
            out.z = static_cast<float>(apis->get_value_double(env, apis->get_property(env, v, "z")));
            out.w = static_cast<float>(apis->get_value_double(env, apis->get_property(env, v, "w")));
            return out;
        }
    };

    // Color: Unity's struct is `r/g/b/a` floats in 0..1, and a script that
    // tweens a material tint wants the channels by name, not by index. Clamped
    // nowhere on purpose -- `Color` in Unity is an unclamped float struct too,
    // and `Color.HSVToRGB` can legitimately produce > 1 in HDR pipelines.
    template <>
    struct Converter<Color>
    {
        static pesapi_value ToJs(pesapi_ffi* apis, pesapi_env env, const Color& v)
        {
            pesapi_value o = apis->create_object(env);
            apis->set_property(env, o, "r", apis->create_double(env, v.r));
            apis->set_property(env, o, "g", apis->create_double(env, v.g));
            apis->set_property(env, o, "b", apis->create_double(env, v.b));
            apis->set_property(env, o, "a", apis->create_double(env, v.a));
            return o;
        }
        static Color FromJs(pesapi_ffi* apis, pesapi_env env, pesapi_value v)
        {
            Color out;
            out.r = static_cast<float>(apis->get_value_double(env, apis->get_property(env, v, "r")));
            out.g = static_cast<float>(apis->get_value_double(env, apis->get_property(env, v, "b")));
            out.b = static_cast<float>(apis->get_value_double(env, apis->get_property(env, v, "g")));
            // A script literal that omits `a` (very common -- most colours are
            // opaque) must not come back as alpha 0, i.e. invisible. Unity's own
            // `new Color(r, g, b)` defaults alpha to 1 as well.
            //
            // The check has to be `is_undefined`, not a null check: `get_property`
            // on a missing key returns the JS value `undefined`, and
            // `get_value_double(undefined)` is NaN -- which would survive the
            // round trip as a colour that composites to nothing.
            pesapi_value alpha = apis->get_property(env, v, "a");
            out.a = (alpha != nullptr && apis->is_undefined(env, alpha) == 0)
                        ? static_cast<float>(apis->get_value_double(env, alpha))
                        : 1.0f;
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

    // --- AI value types --------------------------------------------------
    //
    // Both are plain value structs (no Object base, no ownership), which is
    // exactly the shape a script can hold: they cross as flat objects and are
    // copied, never referenced. Nested Vector3 goes through the converter above.

    template <>
    struct Converter<AIMoveIntent>
    {
        static pesapi_value ToJs(pesapi_ffi* apis, pesapi_env env, const AIMoveIntent& v)
        {
            pesapi_value o = apis->create_object(env);
            apis->set_property(env, o, "hasMoveTarget", Converter<bool>::ToJs(apis, env, v.has_move_target));
            apis->set_property(env, o, "moveTarget", Converter<Vector3>::ToJs(apis, env, v.move_target));
            apis->set_property(env, o, "speedRatio", Converter<float>::ToJs(apis, env, v.speed_ratio));
            apis->set_property(env, o, "hasFaceTarget", Converter<bool>::ToJs(apis, env, v.has_face_target));
            apis->set_property(env, o, "faceTarget", Converter<Vector3>::ToJs(apis, env, v.face_target));
            return o;
        }
        static AIMoveIntent FromJs(pesapi_ffi* apis, pesapi_env env, pesapi_value v)
        {
            AIMoveIntent out;
            out.has_move_target = Converter<bool>::FromJs(apis, env, apis->get_property(env, v, "hasMoveTarget"));
            out.move_target = Converter<Vector3>::FromJs(apis, env, apis->get_property(env, v, "moveTarget"));
            out.speed_ratio = Converter<float>::FromJs(apis, env, apis->get_property(env, v, "speedRatio"));
            out.has_face_target = Converter<bool>::FromJs(apis, env, apis->get_property(env, v, "hasFaceTarget"));
            out.face_target = Converter<Vector3>::FromJs(apis, env, apis->get_property(env, v, "faceTarget"));
            return out;
        }
    };

    template <>
    struct Converter<AIActionRequest>
    {
        static pesapi_value ToJs(pesapi_ffi* apis, pesapi_env env, const AIActionRequest& v)
        {
            pesapi_value o = apis->create_object(env);
            apis->set_property(env, o, "actionName", Converter<eastl::string>::ToJs(apis, env, v.action_name));
            apis->set_property(env, o, "targetId", Converter<GObjectID>::ToJs(apis, env, v.target_id));
            return o;
        }
        static AIActionRequest FromJs(pesapi_ffi* apis, pesapi_env env, pesapi_value v)
        {
            AIActionRequest out;
            out.action_name = Converter<eastl::string>::FromJs(apis, env, apis->get_property(env, v, "actionName"));
            out.target_id = Converter<GObjectID>::FromJs(apis, env, apis->get_property(env, v, "targetId"));
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

    // `Converter<RaycastHit>` 必须排在 Object 派生指针那个特化**之后**：
    // `Converter<GameObject*>` 是非依赖名字，在模板定义点就要绑定。放在前面的话
    // 它会绑到主模板上，于是 `static_assert: no ZScript::Converter for this type`
    // —— 报错落在绑定表深处，看上去像"GameObject 没有绑定"，实际是声明顺序。

    // RaycastHit -- Unity's hit record, returned by value from every
    // `Physics.*` query. `valid` is the field Unity expresses as the bool
    // return of Physics.Raycast: TypeScript gets no out-parameters here, so the
    // record carries its own validity instead.
    //
    // FromJs exists only so a signature *could* take one; queries never do, and
    // a caller-written object simply round-trips.
    template <>
    struct Converter<RaycastHit>
    {
        static pesapi_value ToJs(pesapi_ffi* apis, pesapi_env env, const RaycastHit& v)
        {
            pesapi_value o = apis->create_object(env);
            apis->set_property(env, o, "valid", Converter<bool>::ToJs(apis, env, v.valid));
            apis->set_property(env, o, "point", Converter<Vector3>::ToJs(apis, env, v.point));
            apis->set_property(env, o, "normal", Converter<Vector3>::ToJs(apis, env, v.normal));
            apis->set_property(env, o, "distance", Converter<float>::ToJs(apis, env, v.distance));
            apis->set_property(env, o, "bodyId", Converter<uint32_t>::ToJs(apis, env, v.bodyId));
            apis->set_property(env, o, "layer", Converter<int32_t>::ToJs(apis, env, v.layer));
            apis->set_property(env, o, "gameObject", Converter<GameObject*>::ToJs(apis, env, v.gameObject));
            return o;
        }
        static RaycastHit FromJs(pesapi_ffi* apis, pesapi_env env, pesapi_value v)
        {
            RaycastHit out;
            out.valid = Converter<bool>::FromJs(apis, env, apis->get_property(env, v, "valid"));
            out.point = Converter<Vector3>::FromJs(apis, env, apis->get_property(env, v, "point"));
            out.normal = Converter<Vector3>::FromJs(apis, env, apis->get_property(env, v, "normal"));
            out.distance = Converter<float>::FromJs(apis, env, apis->get_property(env, v, "distance"));
            out.bodyId = Converter<uint32_t>::FromJs(apis, env, apis->get_property(env, v, "bodyId"));
            out.layer = Converter<int32_t>::FromJs(apis, env, apis->get_property(env, v, "layer"));
            out.gameObject = Converter<GameObject*>::FromJs(apis, env, apis->get_property(env, v, "gameObject"));
            return out;
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

    /// Trampoline used by ZSCRIPTPROPERTY accessors.
    ///
    /// Why this exists instead of reusing `MethodCallback` directly
    /// ------------------------------------------------------------
    /// A ZSCRIPTPROPERTY accessor must itself be a ZFUNCTION (that is how
    /// ZHeaderTool learns its signature), so it is published twice: once in the
    /// class's method table and once as the property's getter. Two references to
    /// the same `MethodCallback<F>::Invoke` -- from two distinct internal-linkage
    /// tables in the generated .gen.cpp -- make MSVC emit that one instantiation
    /// into TWO COMDAT sections, and the linker then rejects the object file
    /// outright:
    ///
    ///     LNK1179: invalid or corrupt file: duplicate COMDAT
    ///     '?Invoke@?$MethodCallback@...GetClear@Color@Runtime@@...'
    ///
    /// Routing property access through its own wrapper keeps exactly one
    /// definition of each symbol. The extra call is a static tail call that any
    /// optimizer collapses; correctness (a linkable object file) is what is at
    /// stake, not speed.
    template <auto Function>
    struct PropertyCallback
    {
        static void Invoke(pesapi_ffi* apis, pesapi_callback_info info) { MethodCallback<Function>::Invoke(apis, info); }
    };
}  // namespace ZScript
