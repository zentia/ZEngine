#pragma once

// =============================================================================
// DataTable -- ZEngine's data-asset for tabular configuration data.
//
// Design intent
// -------------
// A DataTable is the binary product of a CSV (or, in V2, an XLSX) source file.
// Designers author rows in <Project>/Data/<rel>.csv; the editor compiles each
// CSV at startup (and on file change) into <Project>/Assets/_Generated/Data/
// <rel>.zasset. Runtime code calls AssetManager::loadAsset<MyDataTable>() to
// access strongly-typed rows. The .csv source files are checked into VCS; the
// .zasset products are gitignored. See AGENTS.md "Data pipeline" for the
// full pipeline rationale.
//
// Layered model
// -------------
//   Object
//     └── DataTableBase    (non-template, abstract; participates in reflection)
//          └── DataTable<TRow>   (template; type-erased helpers + std::vector<TRow> rows)
//                └── WeaponDataTable / EnemyDataTable / ...   (user, REGISTER_CLASS'd)
//
// Why three layers?
//   * REGISTER_CLASS / IMPLEMENT_REGISTER_CLASS in ObjectDefines.h cannot
//     swallow `<` `>` in a typename, so `DataTable<WeaponRow>` itself cannot
//     be registered. Each user-defined RowType must therefore have a
//     concrete non-template wrapper class (the same pattern UE follows with
//     UDataTable + a per-game `URow` struct under reflection).
//   * DataTableBase exists so editor code (Project window, Inspector,
//     AssetRegistry) can hold a DataTable* and ask for row count / row key
//     without knowing TRow at compile time.
//   * DataTable<TRow> exists so user wrappers stay one-line: they only need
//     to declare/implement two macros and Transfer() of their row struct.
//
// Row contract
// ------------
// A user RowType:
//   - is a plain `struct`, NOT an Object subclass (rows are POD-ish data, not
//     identity-bearing engine objects).
//   - declares `DECLARE_SERIALIZE(MyRow);` (header-side) and implements
//     `template<class TF> void Transfer(TF&);` (cpp-side).
//   - has a string field named `id` as the FIRST entry in Transfer(). This
//     `id` is the row's primary key; CSV's first column maps to it 1:1 and
//     DataTable<TRow>::find(key) hashes against it.
//   - uses `eastl::string` (NOT std::string) for any string field
//     (project-wide invariant; SerializeTraits is only specialised for
//     eastl::string -- see AGENTS.md 2.3).
//
// Hash index
// ----------
// `m_KeyIndex` is a runtime-only hash from row.id to row index. It is NOT
// transferred (cheap to rebuild on load). User wrappers must call
// rebuildKeyIndex() inside their Transfer() once `rows` has been read in
// the read direction. The base class offers a default implementation that
// any wrapper can call as `this->buildKeyIndex();`.
//
// =============================================================================

#include "Runtime/BaseClasses/Object.h"
#include "Runtime/Core/Serialize/SerializeUtility.h"
#include "Runtime/Core/Serialize/TransferFunctions/GenerateTypeTreeTransfer.h"
#include "Runtime/Core/Serialize/TypeTree.h"

#include <EASTL/hash_map.h>
#include <EASTL/string.h>
#include <cstddef>
#include <cstdint>
#include <vector>

// -----------------------------------------------------------------------------
// DataTableBase -- abstract, type-erased base. Holds metadata that does not
// depend on TRow, plus the virtual interface the editor uses for table-level
// operations (count rows, get a row's key for display, trigger re-import).
// -----------------------------------------------------------------------------
class DataTableBase : public Object
{
    REGISTER_CLASS_TRAITS(kTypeIsAbstract);
    REGISTER_CLASS(DataTableBase);
    DECLARE_OBJECT_SERIALIZE();

public:
    DataTableBase() = default;
    ~DataTableBase() override = default;

    /// Path of the CSV the table was compiled from, RELATIVE to the project
    /// root (e.g. "Data/Weapons.csv"). Filled by DataTableImporter at compile
    /// time; persisted so the Inspector can show "(generated, edit source
    /// CSV)" guidance and so re-import knows where to look without a
    /// project-wide rescan.
    eastl::string m_SourceCsvRelpath;

    /// Number of rows in this table. Implemented by DataTable<TRow>.
    virtual size_t rowCount() const = 0;

    /// String key (= the `id` field) of the i-th row. Used by the Inspector
    /// table view; bounds-checked by the implementation.
    virtual eastl::string rowKeyAt(size_t row_index) const = 0;

    // ------------------------------------------------------------------
    // Row introspection (PR #6, Inspector V2 columns).
    //
    // These three virtuals let editor code render a per-row, per-field
    // grid without knowing TRow at compile time. The contract is
    // intentionally tiny:
    //
    //   * fillRowTypeTree(out, flags) populates `out` with the field
    //     layout of TRow exactly the way the serializer sees it -- same
    //     names, same type strings, same byte offsets. The Inspector
    //     walks the tree's first level to discover columns.
    //
    //   * rowDataAt(i) returns a byte pointer to row i's storage. The
    //     Inspector adds the TypeTree's per-field byte offset and casts
    //     to the appropriate primitive / eastl::string to format the
    //     cell. Pointer is invalidated by the next mutation of m_Rows;
    //     callers must not cache it across frames.
    //
    //   * rowSize() returns sizeof(TRow). Used as a sanity bound when
    //     a cell offset would otherwise read past end-of-row.
    //
    // V1 (Inspector) is read-only -- no virtual setter -- so we expose
    // a const data pointer only. A future write path would need a
    // matching `void* rowDataMutableAt(size_t)` plus a "dirty"
    // notification back into the importer.
    // ------------------------------------------------------------------

    /// Generate the row's type tree (field name / type / byte offset).
    /// Implemented generically by DataTable<TRow>; user wrappers do
    /// nothing extra. Called per-Inspector-frame today; cache yourself
    /// if profiling shows it as a hotspot (currently ~10us for a
    /// 5-column row, well below ImGui's per-frame budget).
    virtual void fillRowTypeTree(TypeTree& out, TransferInstructionFlags flags = kNoTransferInstructionFlags) const = 0;

    /// Read-only pointer to row[i]'s storage, or nullptr if i is out of
    /// range. Returned as `const void*` so the Inspector can layer
    /// arbitrary primitive readers on top via byte-offset arithmetic
    /// without forcing a virtual call per cell.
    virtual const void* rowDataAt(size_t row_index) const = 0;

    /// Size of one row in bytes (= sizeof(TRow)). Stable across all
    /// instances of the same wrapper class.
    virtual size_t rowSize() const = 0;

    /// Optional re-import hook: rebuild the hash index after rows have been
    /// mutated externally (e.g. CSV recompile in-place). Default no-op so
    /// thin user wrappers don't have to override it.
    virtual void onPostLoad() {}
};

// -----------------------------------------------------------------------------
// DataTable<TRow> -- concrete row container. Templated; not registered with
// the reflection system (REGISTER_CLASS chokes on `<` `>`). User code derives
// a final non-template class via DECLARE_DATA_TABLE and that wrapper carries
// the REGISTER_CLASS ceremony.
// -----------------------------------------------------------------------------
template<class TRow>
class DataTable : public DataTableBase
{
public:
    // REGISTER_CLASS(WrapperClass) in user-derived final classes expands to
    //   typedef ThisType Super; typedef WrapperClass ThisType;
    // which requires the parent class to have a `ThisType` typedef in scope.
    // Object provides one for DataTableBase; we manually relay it here so
    // user wrappers compile without writing extra typedefs.
    using ThisType = DataTable<TRow>;
    using row_type = TRow;

    /// Row storage. Must be std::vector (not eastl::vector) because
    /// SerializeTraits is specialised for std::vector only --
    /// see Runtime/Core/Serialize/SerializeTraits.h.
    std::vector<TRow> m_Rows;

    /// Runtime-only key->row-index map. Rebuilt on load via onPostLoad();
    /// not serialised. eastl::hash_map<eastl::string, ...> matches the
    /// project's string convention (AGENTS.md 2.3).
    eastl::hash_map<eastl::string, uint32_t> m_KeyIndex;

    // ------------------------------------------------------------------
    // Public lookup API.
    // ------------------------------------------------------------------

    /// O(1) lookup by row key (= row.id). Returns nullptr if not found.
    /// Caller MUST NOT cache the pointer past the next mutation of m_Rows.
    const TRow* find(const eastl::string& key) const
    {
        const auto it = m_KeyIndex.find(key);
        if (it == m_KeyIndex.end())
        {
            return nullptr;
        }
        const uint32_t idx = it->second;
        return idx < m_Rows.size() ? &m_Rows[idx] : nullptr;
    }

    /// Mutable variant. Same caveats.
    TRow* findMutable(const eastl::string& key)
    {
        const auto it = m_KeyIndex.find(key);
        if (it == m_KeyIndex.end())
        {
            return nullptr;
        }
        const uint32_t idx = it->second;
        return idx < m_Rows.size() ? &m_Rows[idx] : nullptr;
    }

    /// Rebuild m_KeyIndex from m_Rows. Called automatically from onPostLoad
    /// (via the user wrapper) and from DataTableImporter after CSV parse.
    /// Conflict policy: later rows OVERWRITE earlier rows' index entries
    /// silently -- the importer is expected to have already warned about
    /// duplicate keys at parse time, which is the right place to surface
    /// the source-file location of the offender.
    void buildKeyIndex()
    {
        m_KeyIndex.clear();
        m_KeyIndex.reserve(m_Rows.size());
        for (uint32_t i = 0; i < m_Rows.size(); ++i)
        {
            m_KeyIndex[m_Rows[i].id] = i;
        }
    }

    // ------------------------------------------------------------------
    // DataTableBase interface
    // ------------------------------------------------------------------

    size_t rowCount() const override { return m_Rows.size(); }

    eastl::string rowKeyAt(size_t row_index) const override
    {
        if (row_index >= m_Rows.size())
        {
            return {};
        }
        return m_Rows[row_index].id;
    }

    // -------------------- Row introspection (PR #6) --------------------
    //
    // We materialise the TypeTree from a *temporary* TRow on the stack
    // so the byte offsets the Transfer machinery records are relative
    // to that temporary's address -- which means the resulting offsets
    // also apply byte-for-byte to any other TRow instance, since field
    // layout is uniform across instances of the same struct. The temp
    // is then thrown away; the TypeTree alone is what the Inspector
    // consumes.
    //
    // We do NOT generate the tree from `m_Rows[0]` because the table
    // may be empty; using a fresh stack instance keeps the call valid
    // for zero-row tables (the Inspector still wants to render the
    // header row in that case).

    void fillRowTypeTree(TypeTree& out, TransferInstructionFlags flags = kNoTransferInstructionFlags) const override
    {
        TRow probe {};
        GenerateTypeTreeTransfer transfer(out, flags, &probe, sizeof(TRow));
        transfer.Transfer(probe, "Row");
    }

    const void* rowDataAt(size_t row_index) const override
    {
        if (row_index >= m_Rows.size())
        {
            return nullptr;
        }
        return static_cast<const void*>(&m_Rows[row_index]);
    }

    size_t rowSize() const override { return sizeof(TRow); }

    void onPostLoad() override { buildKeyIndex(); }
};

// -----------------------------------------------------------------------------
// Boilerplate macros for user RowType wrappers.
//
// Usage (header):
//   DECLARE_DATA_TABLE(WeaponDataTable, WeaponRow);
//
// Usage (translation unit):
//   IMPLEMENT_DATA_TABLE(WeaponDataTable, WeaponRow);
//
// The pair expands to a full Object-derived final class with REGISTER_CLASS,
// DECLARE_OBJECT_SERIALIZE, the virtual Transfer plumbing, and a Transfer()
// body that forwards to DataTableBase::Transfer (for m_SourceCsvRelpath)
// plus a single transfer.Transfer(m_Rows, "m_Rows"). The user only writes their RowType's
// Transfer(); the table wrapper is zero-cost per game.
//
// `friend struct ProduceHelper` is required because Object's factory uses
// the global ProduceHelper<T>::Produce() template, which calls `new T()` on
// the registered type; without the friend, classes that hide their default
// ctor still have to expose it -- but DataTable's default ctor is already
// public, so the friend is technically optional. We add it anyway for
// symmetry with how Unity's UnityScriptingClass macros declare it.
// -----------------------------------------------------------------------------

#define DECLARE_DATA_TABLE(WrapperClass_, RowType_)        \
    class WrapperClass_ final : public DataTable<RowType_> \
    {                                                      \
        REGISTER_CLASS(WrapperClass_);                     \
        DECLARE_OBJECT_SERIALIZE();                        \
                                                           \
    public:                                                \
        WrapperClass_() = default;                         \
        ~WrapperClass_() override = default;               \
    }

#define IMPLEMENT_DATA_TABLE(WrapperClass_, RowType_)                          \
    IMPLEMENT_REGISTER_CLASS(WrapperClass_)                                    \
    IMPLEMENT_OBJECT_SERAILIZE(WrapperClass_)                                  \
    INSTANTIATE_TEMPLATE_TRANSFER_EXPORTED(WrapperClass_)                      \
                                                                               \
    template<typename TransferFunction>                                        \
    void WrapperClass_::Transfer(TransferFunction& transfer)                   \
    {                                                                          \
        /* Super forwards to DataTableBase, which emits m_SourceCsvRelpath. */ \
        Super::Transfer(transfer);                                             \
        transfer.Transfer(m_Rows, "m_rows");                                   \
    }                                                                          \
    /* Compile-time guard: complete-type check for the row struct. */          \
    static_assert(sizeof(RowType_) > 0, "RowType_ must be a complete type")
