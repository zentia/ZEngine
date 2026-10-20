#pragma once

// =====================================================================
// Cast<T> -- ZHeaderTool phase 4: replace dynamic_cast inside the Object
// hierarchy with the engine's own O(1) type check.
// ---------------------------------------------------------------------
// Why:
//   * `dynamic_cast` walks the RTTI graph at runtime and drags -frtti into
//     every translation unit that touches an Object subclass. ZEngine already
//     has a cheaper and stricter answer: TypeManager numbers every registered
//     type by a DFS from the roots, which makes "is B a base of A" a single
//     range test (`Type::IsBaseOf`, see Type.h).
//   * It also lets the answer depend on the *engine's* idea of the hierarchy
//     (REGISTER_CLASS / ZCLASS) rather than on which translation units happen
//     to emit RTTI.
//
// What it is NOT:
//   * It only covers the Object hierarchy -- `static_assert` rejects anything
//     else, so the render backends (DX12RHI, RenderPipeline, ...) keep using
//     dynamic_cast, which is correct: they are not Objects.
//   * It is a downcast/sidecast check on a SINGLE inheritance chain. ZEngine
//     has no multiply-inherited Objects; if one is ever introduced it must be
//     flattened (a derived-from-Object mixin still works, a second Object
//     base does not).
//
// Correctness contract (enforced by `--check-cast`):
//   `Cast<T>(p) != nullptr`  <=>  `dynamic_cast<T*>(p) != nullptr`
// for every registered type and every target type. The gate cross-checks the
// O(1) range test against a base-chain walk for all (runtime type, type)
// pairs, and spot-checks Cast against dynamic_cast directly.
// =====================================================================

#include "Runtime/BaseClasses/Object.h"
#include "Runtime/BaseClasses/Type.h"

#include <type_traits>

namespace CastDetail
{
    // TypeOf<const T> would reach a *different* TypeContainer<T> instance than
    // TypeOf<T> (and an unregistered one), so every lookup runs on the bare
    // type. The cv-qualifiers are re-applied by the return type.
    template <typename T>
    using Plain = typename std::remove_cv<T>::type;
}  // namespace CastDetail

/// Downcast within the Object hierarchy. Returns nullptr when `object` is null
/// or is not a `T` (exactly like dynamic_cast on a pointer).
///
/// Unrelated T/U are accepted and always yield nullptr, again like dynamic_cast:
/// the pointer conversion is guarded by `if constexpr` because static_cast
/// (unlike dynamic_cast) rejects unrelated types outright.
template <typename T, typename U>
T* Cast(U* object)
{
    using PlainT = CastDetail::Plain<T>;
    using PlainU = CastDetail::Plain<U>;
    static_assert(std::is_base_of<Object, PlainU>::value,
                  "Cast<T>: the source must derive from Object (use dynamic_cast for non-Object hierarchies)");
    static_assert(std::is_base_of<Object, PlainT>::value,
                  "Cast<T>: the target must derive from Object (use dynamic_cast for non-Object hierarchies)");
    if (object == nullptr)
        return nullptr;

    // Deliberately the VIRTUAL lookup, not Object::GetType(): the latter reads
    // m_CachedTypeIndex, which is only meaningful after
    // InitializeRuntimeTypeInfo() has run, and a handful of objects (raw
    // CreateObject results, objects still under construction) have not.
    // dynamic_cast never depended on that, so neither may Cast.
    const Type* runtime_type = object->GetMostDerivedType();
    if (runtime_type == nullptr)
        return nullptr;
    if (!TypeOf<PlainT>()->IsBaseOf(runtime_type->GetRuntimeTypeIndex()))
        return nullptr;

    if constexpr (std::is_base_of<PlainT, PlainU>::value || std::is_base_of<PlainU, PlainT>::value)
        return static_cast<T*>(object);
    else
        return nullptr;
}

/// Const overload, so `const U*` stays const (dynamic_cast does the same).
/// Called as `Cast<T>(const_ptr)` -- T stays unqualified, the const-ness of the
/// argument selects this overload.
template <typename T, typename U>
const T* Cast(const U* object)
{
    using PlainT = CastDetail::Plain<T>;
    using PlainU = CastDetail::Plain<U>;
    static_assert(std::is_base_of<Object, PlainU>::value,
                  "Cast<T>: the source must derive from Object (use dynamic_cast for non-Object hierarchies)");
    static_assert(std::is_base_of<Object, PlainT>::value,
                  "Cast<T>: the target must derive from Object (use dynamic_cast for non-Object hierarchies)");
    if (object == nullptr)
        return nullptr;

    const Type* runtime_type = object->GetMostDerivedType();
    if (runtime_type == nullptr)
        return nullptr;
    if (!TypeOf<PlainT>()->IsBaseOf(runtime_type->GetRuntimeTypeIndex()))
        return nullptr;

    if constexpr (std::is_base_of<PlainT, PlainU>::value || std::is_base_of<PlainU, PlainT>::value)
        return static_cast<const T*>(object);
    else
        return nullptr;
}
