#pragma once

// ============================================================================
// LuaMemCsv - data layer for the Lua memory snapshot previewer
// ============================================================================
//
// Parses the CSV produced by ProfilerCsvExport (the "Count,Size,Type,Stack"
// schema written by MemoryProfiler's Lua section) and aggregates it per type.
//
// Memory layout matters here: a real snapshot has ~180K rows whose stacks are
// multi-line and highly repetitive. Storing an std::string per row would mean
// ~180K separate heap blocks. Instead every stack is appended into one blob and
// each row keeps a (offset,length) slice into it, and type names are interned
// into a small table so a row only carries a uint16 id.
//
// This file is deliberately free of any ZSlate/engine dependency so it can be
// unit tested on its own.

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace LuaMemPreview
{
// One CSV data row.
struct FRow
{
    uint64_t Size {0};      // bytes reported for this entry
    uint32_t Count {0};     // object count collapsed into this entry
    uint32_t StackOffset {0};
    uint32_t StackLength {0};
    uint16_t TypeId {0};
};

// Aggregate for one type tag.
struct FTypeBucket
{
    uint16_t TypeId {0};
    uint64_t Size {0};      // summed bytes
    uint64_t Count {0};     // summed object count
    uint32_t Rows {0};      // number of CSV rows folded in
};

// Column identity, shared by the table widget and the sort comparator.
enum class EColumn
{
    Count = 0,
    Size,
    Type,
    Stack,
};

class FSnapshot
{
public:
    // Load `path`. Dispatches on the extension: `.zasset` goes through the
    // MemoryProfiler reader, anything else is parsed as the exporter CSV.
    // Returns false and fills out_error on failure.
    bool LoadFromFile(const std::string& path, std::string& out_error);

    // --- Incremental build --------------------------------------------------
    // Shared by the CSV parser and the .zasset loader so both end up with an
    // identical model (same interning, blob layout, buckets and totals) and
    // every consumer - table, type bar, --stats, --dump-ui - stays source
    // agnostic.
    void BeginBuild();
    void AddEntry(uint32_t count, uint64_t size, const std::string& type_name, std::string stack);
    bool FinishBuild(const std::string& path, std::string& out_error);

    bool IsLoaded() const { return m_Loaded; }
    const std::string& GetFileName() const { return m_FileName; }

    // --- Rows ---------------------------------------------------------------
    const std::vector<FRow>& GetRows() const { return m_Rows; }
    size_t GetRowCount() const { return m_Rows.size(); }

    // Stack text of a row. Returned as a view into the shared blob; the
    // snapshot must outlive the view.
    std::string_view GetStack(const FRow& row) const
    {
        return std::string_view(m_StackBlob).substr(row.StackOffset, row.StackLength);
    }

    // First line of a row's stack - what the table shows in the Stack column.
    std::string_view GetStackFirstLine(const FRow& row) const
    {
        std::string_view s = GetStack(row);
        const size_t nl = s.find('\n');
        return (nl == std::string_view::npos) ? s : s.substr(0, nl);
    }

    // --- Types --------------------------------------------------------------
    const std::string& GetTypeName(uint16_t type_id) const { return m_TypeNames[type_id]; }
    uint16_t GetTypeCount() const { return static_cast<uint16_t>(m_TypeNames.size()); }

    // Buckets sorted by descending size.
    const std::vector<FTypeBucket>& GetTypeBuckets() const { return m_TypeBuckets; }

    // --- Totals -------------------------------------------------------------
    // Sum of every row. After a GameCore profiler sync this equals Lua's own
    // accounting, i.e. collectgarbage("count") * 1024.
    uint64_t GetTotalSize() const { return m_TotalSize; }
    uint64_t GetTotalCount() const { return m_TotalCount; }

    // Snapshots exported before the summary row was removed end with a
    // "0,<total>,TOTAL," line. It is skipped during parsing (counting it would
    // double the reported total - exactly the artefact that motivated its
    // removal) but kept here because it lets us cross-check our own sum.
    bool HasDeclaredTotal() const { return m_HasDeclaredTotal; }
    uint64_t GetDeclaredTotal() const { return m_DeclaredTotal; }

    // Where the declared total came from, for reporting. The CSV and the
    // `.zasset` disagree on this: one is a legacy summary row, the other is the
    // profiler's own running sum.
    const char* GetDeclaredTotalOrigin() const { return m_DeclaredTotalOrigin; }

    void SetDeclaredTotal(uint64_t total, const char* origin)
    {
        m_DeclaredTotal = total;
        m_DeclaredTotalOrigin = origin;
        m_HasDeclaredTotal = true;
    }

    // True when there is no declared total to compare against, or when it
    // matches the sum of the data rows.
    bool IsSelfConsistent() const
    {
        return !m_HasDeclaredTotal || m_DeclaredTotal == m_TotalSize;
    }

private:
    uint16_t InternType(const std::string& name);
    bool LoadFromCsv(const std::string& path, std::string& out_error);

    // Live only between BeginBuild and FinishBuild; released there so a loaded
    // snapshot does not keep a second copy of every type name around.
    std::unordered_map<std::string, uint16_t> m_TypeLookup;

    bool m_Loaded {false};
    std::string m_FileName;

    std::vector<FRow> m_Rows;
    std::string m_StackBlob;            // all unescaped stacks, concatenated
    std::vector<std::string> m_TypeNames;
    std::vector<FTypeBucket> m_TypeBuckets;

    uint64_t m_TotalSize {0};
    uint64_t m_TotalCount {0};
    uint64_t m_DeclaredTotal {0};
    const char* m_DeclaredTotalOrigin {""};
    bool m_HasDeclaredTotal {false};
};

// ----------------------------------------------------------------------------
// View: the ordered + filtered set of row indices the table actually shows.
// Sorting and filtering only ever touch this index vector, never the rows.
// ----------------------------------------------------------------------------
class FRowView
{
public:
    void Rebuild(const FSnapshot& snap);

    // Keep only rows of `type_id`; pass kAllTypes to clear the filter.
    static constexpr uint16_t kAllTypes = 0xFFFF;
    void SetTypeFilter(const FSnapshot& snap, uint16_t type_id);
    uint16_t GetTypeFilter() const { return m_TypeFilter; }

    // Case-insensitive substring match against the stack text.
    void SetSearch(const FSnapshot& snap, const std::string& needle);
    const std::string& GetSearch() const { return m_Search; }

    void SetSort(const FSnapshot& snap, EColumn column, bool descending);
    EColumn GetSortColumn() const { return m_SortColumn; }
    bool IsSortDescending() const { return m_SortDescending; }

    const std::vector<uint32_t>& GetIndices() const { return m_Indices; }
    size_t Size() const { return m_Indices.size(); }

    // Sum of the currently visible rows (differs from the snapshot total while
    // a filter or search is active).
    uint64_t GetVisibleSize() const { return m_VisibleSize; }

private:
    void ApplyFilter(const FSnapshot& snap);
    void ApplySort(const FSnapshot& snap);

    std::vector<uint32_t> m_Indices;
    uint16_t m_TypeFilter {kAllTypes};
    std::string m_Search;
    std::string m_SearchLower;
    EColumn m_SortColumn {EColumn::Size};
    bool m_SortDescending {true};
    uint64_t m_VisibleSize {0};
};

// ----------------------------------------------------------------------------
// Formatting helpers (shared by the widgets).
// ----------------------------------------------------------------------------
std::string FormatBytes(uint64_t bytes);
std::string FormatCount(uint64_t count);

}  // namespace LuaMemPreview
