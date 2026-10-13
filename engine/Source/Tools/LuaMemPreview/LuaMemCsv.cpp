#include "LuaMemCsv.h"

#include "LuaMemZasset.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <unordered_map>

namespace LuaMemPreview
{
namespace
{
// ----------------------------------------------------------------------------
// RFC4180 field scanner.
//
// Returns the index just past the field. `out` receives the unescaped value.
// `out_quoted` tells the caller whether the field was quoted, which is the only
// way to know that an embedded '\n' was data rather than a row terminator.
//
// ProfilerCsvExport::EscapeCSVField quotes a field whenever it contains a
// comma, a quote or a newline, and doubles inner quotes - so a Lua stack like
//   "[a.lua:1] f()\n[b.lua:2] g()"
// arrives as a single quoted field spanning multiple physical lines.
// ----------------------------------------------------------------------------
size_t ScanField(const std::string& buf, size_t pos, std::string& out, bool& out_quoted)
{
    out.clear();
    out_quoted = false;
    const size_t n = buf.size();
    if (pos >= n)
        return pos;

    if (buf[pos] != '"')
    {
        // Unquoted: run to the next comma or end of line.
        const size_t start = pos;
        while (pos < n && buf[pos] != ',' && buf[pos] != '\n' && buf[pos] != '\r')
            ++pos;
        out.assign(buf, start, pos - start);
        return pos;
    }

    out_quoted = true;
    ++pos;  // consume opening quote
    while (pos < n)
    {
        const char c = buf[pos];
        if (c == '"')
        {
            if (pos + 1 < n && buf[pos + 1] == '"')
            {
                out.push_back('"');  // "" -> literal quote
                pos += 2;
                continue;
            }
            ++pos;  // closing quote
            break;
        }
        out.push_back(c);
        ++pos;
    }
    return pos;
}

// Fast non-throwing unsigned parse. std::stoull would both throw on the
// occasional malformed cell and dominate the profile across ~180K rows x2.
uint64_t ParseU64(const std::string& s)
{
    uint64_t v = 0;
    for (const char c : s)
    {
        if (c < '0' || c > '9')
            continue;
        v = v * 10u + static_cast<uint64_t>(c - '0');
    }
    return v;
}

void ToLowerInPlace(std::string& s)
{
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

// Case-insensitive substring search over a view (needle already lowercased).
bool ContainsNoCase(std::string_view hay, const std::string& needle_lower)
{
    if (needle_lower.empty())
        return true;
    if (hay.size() < needle_lower.size())
        return false;
    const size_t last = hay.size() - needle_lower.size();
    for (size_t i = 0; i <= last; ++i)
    {
        size_t j = 0;
        while (j < needle_lower.size() &&
               std::tolower(static_cast<unsigned char>(hay[i + j])) == static_cast<unsigned char>(needle_lower[j]))
            ++j;
        if (j == needle_lower.size())
            return true;
    }
    return false;
}
}  // namespace

// ============================================================================
// FSnapshot
// ============================================================================

uint16_t FSnapshot::InternType(const std::string& name)
{
    for (size_t i = 0; i < m_TypeNames.size(); ++i)
    {
        if (m_TypeNames[i] == name)
            return static_cast<uint16_t>(i);
    }
    m_TypeNames.push_back(name);
    return static_cast<uint16_t>(m_TypeNames.size() - 1);
}

void FSnapshot::BeginBuild()
{
    m_Loaded = false;
    m_Rows.clear();
    m_StackBlob.clear();
    m_TypeNames.clear();
    m_TypeBuckets.clear();
    m_TypeLookup.clear();
    m_TotalSize = 0;
    m_TotalCount = 0;
    m_DeclaredTotal = 0;
    m_DeclaredTotalOrigin = "";
    m_HasDeclaredTotal = false;
}

void FSnapshot::AddEntry(uint32_t count, uint64_t size, const std::string& type_name, std::string stack)
{
    uint16_t type_id;
    auto it = m_TypeLookup.find(type_name);
    if (it != m_TypeLookup.end())
    {
        type_id = it->second;
    }
    else
    {
        type_id = InternType(type_name);
        m_TypeLookup.emplace(type_name, type_id);
    }

    // Normalise CRLF inside stacks so line splitting downstream is simple.
    if (stack.find('\r') != std::string::npos)
        stack.erase(std::remove(stack.begin(), stack.end(), '\r'), stack.end());
    // Stacks usually end with a newline; it would render as a trailing blank
    // line in the detail pane.
    while (!stack.empty() && stack.back() == '\n')
        stack.pop_back();

    FRow row;
    row.Count = count;
    row.Size = size;
    row.TypeId = type_id;
    row.StackOffset = static_cast<uint32_t>(m_StackBlob.size());
    row.StackLength = static_cast<uint32_t>(stack.size());
    m_StackBlob.append(stack);
    m_Rows.push_back(row);

    m_TotalSize += row.Size;
    m_TotalCount += row.Count;
}

bool FSnapshot::FinishBuild(const std::string& path, std::string& out_error)
{
    // The lookup is build-time scaffolding only.
    m_TypeLookup.clear();

    if (m_Rows.empty())
    {
        out_error = "no data rows found in " + path;
        return false;
    }

    // --- Per-type aggregation ----------------------------------------------
    m_TypeBuckets.assign(m_TypeNames.size(), FTypeBucket {});
    for (size_t i = 0; i < m_TypeBuckets.size(); ++i)
        m_TypeBuckets[i].TypeId = static_cast<uint16_t>(i);
    for (const FRow& r : m_Rows)
    {
        FTypeBucket& b = m_TypeBuckets[r.TypeId];
        b.Size += r.Size;
        b.Count += r.Count;
        b.Rows += 1;
    }
    std::sort(m_TypeBuckets.begin(), m_TypeBuckets.end(),
              [](const FTypeBucket& a, const FTypeBucket& b) { return a.Size > b.Size; });

    // File name for the title bar.
    const size_t slash = path.find_last_of("/\\");
    m_FileName = (slash == std::string::npos) ? path : path.substr(slash + 1);

    m_Loaded = true;
    out_error.clear();
    return true;
}

bool FSnapshot::LoadFromFile(const std::string& path, std::string& out_error)
{
    // `.zasset` is the MemoryProfiler snapshot the CSVs are exported FROM, so
    // it is the richer source; everything else is treated as the exporter CSV.
    std::string ext;
    const size_t dot = path.find_last_of('.');
    if (dot != std::string::npos)
    {
        ext = path.substr(dot);
        ToLowerInPlace(ext);
    }
    if (ext == ".zasset")
        return LoadSnapshotFromZasset(path, *this, out_error);

    return LoadFromCsv(path, out_error);
}

bool FSnapshot::LoadFromCsv(const std::string& path, std::string& out_error)
{
    BeginBuild();

    FILE* fp = nullptr;
#ifdef _WIN32
    if (fopen_s(&fp, path.c_str(), "rb") != 0)
        fp = nullptr;
#else
    fp = std::fopen(path.c_str(), "rb");
#endif
    if (fp == nullptr)
    {
        out_error = "cannot open file: " + path;
        return false;
    }

    std::fseek(fp, 0, SEEK_END);
    const long size = std::ftell(fp);
    std::fseek(fp, 0, SEEK_SET);
    if (size <= 0)
    {
        std::fclose(fp);
        out_error = "file is empty: " + path;
        return false;
    }

    std::string buf;
    buf.resize(static_cast<size_t>(size));
    const size_t read = std::fread(buf.data(), 1, buf.size(), fp);
    std::fclose(fp);
    buf.resize(read);

    // Strip UTF-8 BOM if present.
    size_t pos = 0;
    if (buf.size() >= 3 && static_cast<unsigned char>(buf[0]) == 0xEF &&
        static_cast<unsigned char>(buf[1]) == 0xBB && static_cast<unsigned char>(buf[2]) == 0xBF)
        pos = 3;

    // --- Header ------------------------------------------------------------
    // Only the Lua schema is accepted; anything else is a different section of
    // the profiler dump and would silently produce nonsense rows.
    {
        size_t eol = buf.find('\n', pos);
        if (eol == std::string::npos)
            eol = buf.size();
        std::string header = buf.substr(pos, eol - pos);
        while (!header.empty() && (header.back() == '\r' || header.back() == ' '))
            header.pop_back();
        if (header != "Count,Size,Type,Stack")
        {
            out_error = "unexpected CSV header, expected the Lua schema "
                        "\"Count,Size,Type,Stack\" but found \"" + header + "\"";
            return false;
        }
        pos = (eol < buf.size()) ? eol + 1 : buf.size();
    }

    // --- Rows --------------------------------------------------------------
    m_Rows.reserve(buf.size() / 48);  // rough average row length
    m_StackBlob.reserve(buf.size() / 2);

    std::string f_count, f_size, f_type, f_stack;
    bool quoted = false;

    while (pos < buf.size())
    {
        // Skip stray blank lines / CR.
        if (buf[pos] == '\r' || buf[pos] == '\n')
        {
            ++pos;
            continue;
        }

        pos = ScanField(buf, pos, f_count, quoted);
        if (pos < buf.size() && buf[pos] == ',') ++pos;
        pos = ScanField(buf, pos, f_size, quoted);
        if (pos < buf.size() && buf[pos] == ',') ++pos;
        pos = ScanField(buf, pos, f_type, quoted);
        if (pos < buf.size() && buf[pos] == ',') ++pos;
        pos = ScanField(buf, pos, f_stack, quoted);

        // Advance past the row terminator. A quoted stack has already consumed
        // its embedded newlines, so whatever remains here really ends the row.
        while (pos < buf.size() && buf[pos] != '\n')
            ++pos;
        if (pos < buf.size())
            ++pos;

        if (f_type.empty() && f_size.empty() && f_count.empty())
            continue;

        // Legacy summary row: capture it for cross-checking, never as data.
        if (f_type == "TOTAL")
        {
            SetDeclaredTotal(ParseU64(f_size), "legacy TOTAL row, excluded from data");
            continue;
        }

        AddEntry(static_cast<uint32_t>(ParseU64(f_count)), ParseU64(f_size), f_type, f_stack);
    }

    return FinishBuild(path, out_error);
}

// ============================================================================
// FRowView
// ============================================================================

void FRowView::Rebuild(const FSnapshot& snap)
{
    ApplyFilter(snap);
    ApplySort(snap);
}

void FRowView::SetTypeFilter(const FSnapshot& snap, uint16_t type_id)
{
    m_TypeFilter = type_id;
    Rebuild(snap);
}

void FRowView::SetSearch(const FSnapshot& snap, const std::string& needle)
{
    m_Search = needle;
    m_SearchLower = needle;
    ToLowerInPlace(m_SearchLower);
    Rebuild(snap);
}

void FRowView::SetSort(const FSnapshot& snap, EColumn column, bool descending)
{
    m_SortColumn = column;
    m_SortDescending = descending;
    ApplySort(snap);
}

void FRowView::ApplyFilter(const FSnapshot& snap)
{
    const std::vector<FRow>& rows = snap.GetRows();
    m_Indices.clear();
    m_Indices.reserve(rows.size());
    m_VisibleSize = 0;

    for (size_t i = 0; i < rows.size(); ++i)
    {
        const FRow& r = rows[i];
        if (m_TypeFilter != kAllTypes && r.TypeId != m_TypeFilter)
            continue;
        if (!m_SearchLower.empty() && !ContainsNoCase(snap.GetStack(r), m_SearchLower))
            continue;
        m_Indices.push_back(static_cast<uint32_t>(i));
        m_VisibleSize += r.Size;
    }
}

void FRowView::ApplySort(const FSnapshot& snap)
{
    const std::vector<FRow>& rows = snap.GetRows();
    const bool desc = m_SortDescending;
    const EColumn col = m_SortColumn;

    std::stable_sort(m_Indices.begin(), m_Indices.end(),
        [&rows, &snap, col, desc](uint32_t a, uint32_t b)
        {
            const FRow& ra = rows[a];
            const FRow& rb = rows[b];
            switch (col)
            {
            case EColumn::Count:
                if (ra.Count != rb.Count)
                    return desc ? (ra.Count > rb.Count) : (ra.Count < rb.Count);
                break;
            case EColumn::Size:
                if (ra.Size != rb.Size)
                    return desc ? (ra.Size > rb.Size) : (ra.Size < rb.Size);
                break;
            case EColumn::Type:
            {
                const std::string& ta = snap.GetTypeName(ra.TypeId);
                const std::string& tb = snap.GetTypeName(rb.TypeId);
                if (ta != tb)
                    return desc ? (ta > tb) : (ta < tb);
                break;
            }
            case EColumn::Stack:
            {
                const std::string_view sa = snap.GetStackFirstLine(ra);
                const std::string_view sb = snap.GetStackFirstLine(rb);
                if (sa != sb)
                    return desc ? (sa > sb) : (sa < sb);
                break;
            }
            }
            // Deterministic tie-break so rows never shuffle between sorts.
            return a < b;
        });
}

// ============================================================================
// Formatting
// ============================================================================

std::string FormatBytes(uint64_t bytes)
{
    char tmp[64];
    const double b = static_cast<double>(bytes);
    if (bytes >= 1024ull * 1024ull * 1024ull)
        std::snprintf(tmp, sizeof(tmp), "%.2f GB", b / (1024.0 * 1024.0 * 1024.0));
    else if (bytes >= 1024ull * 1024ull)
        std::snprintf(tmp, sizeof(tmp), "%.2f MB", b / (1024.0 * 1024.0));
    else if (bytes >= 1024ull)
        std::snprintf(tmp, sizeof(tmp), "%.2f KB", b / 1024.0);
    else
        std::snprintf(tmp, sizeof(tmp), "%llu B", static_cast<unsigned long long>(bytes));
    return tmp;
}

std::string FormatCount(uint64_t count)
{
    char tmp[64];
    if (count >= 1000000ull)
        std::snprintf(tmp, sizeof(tmp), "%.1fM", static_cast<double>(count) / 1000000.0);
    else if (count >= 1000ull)
        std::snprintf(tmp, sizeof(tmp), "%.1fK", static_cast<double>(count) / 1000.0);
    else
        std::snprintf(tmp, sizeof(tmp), "%llu", static_cast<unsigned long long>(count));
    return tmp;
}

}  // namespace LuaMemPreview
