#pragma once

#include <cstddef>

class Type;

// =====================================================================
// Editor-facing property metadata emitted by ZHeaderTool from
// ZPROPERTY(meta=(...)). Keyed by (Type, serialized field name) -- the same
// name the TypeTree carries -- so the Inspector can look a field up straight
// from the TypeTree node it is drawing.
//
// Lives in Runtime (not Editor) because the tables are generated into each
// module's .gen.cpp; they are a few static aggregates per annotated class and
// cost nothing when unused. Nothing here affects serialization.
// =====================================================================

/// One annotated property. Member order is the designated-initializer order
/// ZHeaderTool emits -- do not reorder without bumping the tool.
struct ZPropertyMeta
{
    const char* serializedName = nullptr;
    const char* displayName = nullptr;  // Inspector label override
    const char* tooltip = nullptr;
    const char* category = nullptr;  // Inspector grouping
    // Serialized name of a bool field on the same object; '!' prefix negates.
    // The property is read-only in the Inspector while the condition is false.
    const char* editCondition = nullptr;

    bool hasClampMin = false;
    double clampMin = 0.0;
    bool hasClampMax = false;
    double clampMax = 0.0;
    // Soft (slider) range. Not enforced on commit; reserved for range widgets.
    bool hasUIMin = false;
    double uiMin = 0.0;
    bool hasUIMax = false;
    double uiMax = 0.0;

    /// Clamp `value` into [ClampMin, ClampMax] (whichever bounds are set).
    double Clamp(double value) const
    {
        if (hasClampMin && value < clampMin)
            value = clampMin;
        if (hasClampMax && value > clampMax)
            value = clampMax;
        return value;
    }
};

/// All annotated properties of exactly one class (bases register their own).
struct ZClassPropertyMeta
{
    const Type* type = nullptr;
    const ZPropertyMeta* properties = nullptr;
    size_t count = 0;
    ZClassPropertyMeta* next = nullptr;  // intrusive registry list
};

namespace PropertyMetaRegistry
{
    /// Called from generated static registrars (static-init time). `TypeOf<T>()`
    /// is only an address there, which is all the key needs.
    void Register(ZClassPropertyMeta& meta);

    /// Metadata declared on exactly `type` (not its bases), or nullptr.
    const ZClassPropertyMeta* FindClass(const Type* type);

    /// Head of the intrusive list of every registered class, for diagnostics
    /// (smoke tests) that must walk all of them. Thread-safety: registration
    /// only happens during static init, so this is safe after main() starts.
    const ZClassPropertyMeta* Classes();

    /// Metadata of `serialized_name` on `type` or the nearest base that declares
    /// it, or nullptr when the field has no meta=(...).
    const ZPropertyMeta* Find(const Type* type, const char* serialized_name);
}  // namespace PropertyMetaRegistry

struct ZPropertyMetaRegistrar
{
    explicit ZPropertyMetaRegistrar(ZClassPropertyMeta& meta) { PropertyMetaRegistry::Register(meta); }
};
