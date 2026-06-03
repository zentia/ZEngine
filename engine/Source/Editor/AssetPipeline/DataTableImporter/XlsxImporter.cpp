#include "XlsxImporter.h"

#include "DataTableImporter.h"
#include "Runtime/BaseClasses/ObjectManager.h"
#include "Runtime/BaseClasses/Type.h"
#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Core/Log/LogSystem.h"
#include "Runtime/Core/Memory/MemoryManager.h"
#include "Runtime/Project/ProjectInfo.h"
#include "Runtime/Resource/Asset/AssetManager.h"
#include "Runtime/Resource/Asset/Data/DataTable.h"

// minizip-ng public API. The vendored CMake produces target `minizip` (no
// suffix) with the public include dir set to engine/3rdparty/minizip-ng/, so
// these headers resolve as plain "mz_*.h".
#include "mz.h"
#include "mz_strm.h"
#include "mz_strm_mem.h"
#include "mz_zip.h"
#include "mz_zip_rw.h"

#include <EASTL/algorithm.h>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>

// =============================================================================
// Anonymous helpers: ZIP unpacking, XML SAX, XLSX-specific value reconstruction.
// =============================================================================
namespace
{
    // -------------------------------------------------------------------------
    // FNV-1a 64. Same hash family DataTableImporter uses; we replicate the
    // function instead of exporting it from data_table_importer.cpp to keep
    // the call private and avoid widening that header's API surface.
    // -------------------------------------------------------------------------
    uint64_t fnv1a64(const std::string& s)
    {
        uint64_t h = 0xcbf29ce484222325ULL;
        for (unsigned char c : s)
        {
            h ^= c;
            h *= 0x100000001b3ULL;
        }
        return h;
    }

    // -------------------------------------------------------------------------
    // Read a single named entry (e.g. "xl/sharedStrings.xml") out of a .xlsx.
    // Returns true on success and writes the decompressed payload into `out`.
    // Missing entry -> returns true with `out` cleared (caller decides if
    // that's fatal -- sharedStrings.xml is OPTIONAL in xlsx).
    //
    // Path matching: minizip-ng uses case-sensitive matching by default;
    // xlsx parts written by Excel are always lower-case ("xl/sharedStrings.xml"
    // exactly), but we pass `ignore_case=1` to mz_zip_reader_locate_entry so
    // workbooks produced by quirky tools (some Java libs upper-case the path)
    // still resolve.
    //
    // We use mz_zip_reader_open_buffer with the entire file slurped into
    // memory rather than mz_zip_reader_open_file. .xlsx is config-scale
    // (KB to a few MB at the absolute worst); the streaming complexity
    // isn't worth it, and the in-memory path lets us avoid a second open
    // for sharedStrings + sheet1.
    // -------------------------------------------------------------------------
    bool readZipEntry(void* reader,
                      const char* entry_name,
                      std::string& out,
                      eastl::string& out_error)
    {
        out.clear();
        const int32_t locate_rc = mz_zip_reader_locate_entry(reader, entry_name, /*ignore_case=*/1);
        if (locate_rc != MZ_OK)
        {
            // Not found is signalled differently across minizip-ng versions
            // (MZ_END_OF_LIST / MZ_EXIST_ERROR). We treat any non-OK as
            // "absent" without erroring, since sharedStrings.xml is optional.
            // Caller distinguishes required vs optional by checking out.empty().
            return true;
        }
        if (mz_zip_reader_entry_open(reader) != MZ_OK)
        {
            out_error = "mz_zip_reader_entry_open failed for ";
            out_error.append(entry_name);
            return false;
        }

        mz_zip_file* info = nullptr;
        mz_zip_reader_entry_get_info(reader, &info);
        // info->uncompressed_size may be -1 for entries written without a
        // pre-known size (rare for xlsx but spec-allowed). Fall back to
        // chunked read in that case.
        if (info != nullptr && info->uncompressed_size >= 0 &&
            info->uncompressed_size < (1LL << 30))
        {
            out.resize(static_cast<size_t>(info->uncompressed_size));
            int32_t total = 0;
            while (total < static_cast<int32_t>(out.size()))
            {
                const int32_t got = mz_zip_reader_entry_read(
                    reader,
                    out.data() + total,
                    static_cast<int32_t>(out.size()) - total);
                if (got <= 0)
                {
                    break;
                }
                total += got;
            }
            out.resize(static_cast<size_t>(total));
        }
        else
        {
            // Streaming fallback: read in 64KB chunks until EOF.
            char chunk[64 * 1024];
            for (;;)
            {
                const int32_t got =
                    mz_zip_reader_entry_read(reader, chunk, static_cast<int32_t>(sizeof(chunk)));
                if (got <= 0)
                {
                    break;
                }
                out.append(chunk, static_cast<size_t>(got));
                if (out.size() > (256u * 1024u * 1024u))
                {
                    out_error = "xlsx entry exceeds 256MB sanity cap (likely corrupted)";
                    mz_zip_reader_entry_close(reader);
                    return false;
                }
            }
        }
        mz_zip_reader_entry_close(reader);
        return true;
    }

    // -------------------------------------------------------------------------
    // Load the entire .xlsx into a memory buffer + open it as a ZIP via
    // mz_zip_reader. Caller is responsible for mz_zip_reader_close +
    // mz_zip_reader_delete on success.
    //
    // We read the whole file ourselves (std::ifstream) and hand the buffer
    // to mz_zip_reader_open_buffer with copy=0 -- the buffer outlives the
    // reader (caller scope) so minizip can hold the pointer directly.
    // -------------------------------------------------------------------------
    bool openXlsxAsZip(const std::filesystem::path& xlsx_path,
                       std::string& buffer,
                       void*& out_reader,
                       eastl::string& out_error)
    {
        std::ifstream f(xlsx_path, std::ios::binary);
        if (!f.is_open())
        {
            out_error = "failed to open xlsx file";
            return false;
        }
        std::ostringstream ss;
        ss << f.rdbuf();
        buffer = ss.str();
        if (buffer.size() < 4 ||
            buffer[0] != 'P' || buffer[1] != 'K')
        {
            // .xlsx must start with the local-file-header magic 0x504B0304.
            // Empty / corrupted files fail this check before we even open
            // the ZIP, giving a clearer error than "central directory not found".
            out_error = "file is not a valid ZIP container (missing PK signature)";
            return false;
        }

        out_reader = mz_zip_reader_create();
        if (out_reader == nullptr)
        {
            out_error = "mz_zip_reader_create returned null";
            return false;
        }
        const int32_t open_rc = mz_zip_reader_open_buffer(
            out_reader,
            reinterpret_cast<uint8_t*>(buffer.data()),
            static_cast<int32_t>(buffer.size()),
            /*copy=*/0);
        if (open_rc != MZ_OK)
        {
            mz_zip_reader_delete(&out_reader);
            out_reader = nullptr;
            out_error = "mz_zip_reader_open_buffer failed (corrupted .xlsx?)";
            return false;
        }
        return true;
    }

    // -------------------------------------------------------------------------
    // Hand-rolled XML SAX scanner. We deliberately re-invent here instead of
    // pulling in pugixml/tinyxml2 (no extra 3rdparty dep) and instead of
    // using rapidjson-shaped DOM (we don't need parent->child navigation
    // for xlsx; we just need linear "fire on tag start / end / text").
    //
    // Recognised constructs:
    //   <tag attr="val" attr='val' .../>           - self-closing element
    //   <tag attr=...>...</tag>                    - normal element
    //   <!-- comment -->                           - skipped
    //   <?xml ... ?>                               - skipped (PI)
    //   <!DOCTYPE ...>                             - skipped (best-effort,
    //                                                 .xlsx never has one)
    //   <![CDATA[...]]>                            - emitted as text verbatim
    //   character data between tags                - emitted as text
    //   entities &amp; &lt; &gt; &quot; &apos;     - decoded
    //   numeric entities &#NNN; / &#xHHHH;         - decoded as UTF-8
    //
    // Things we do NOT handle (and deliberately log nothing about):
    //   * namespaces -- the scanner sees `r:id` as a single literal name.
    //     XLSX never namespaces an element we care about; it does namespace
    //     attributes but we ignore those without consequence.
    //   * external DTD subsets -- never seen in xlsx.
    //   * processing instructions other than the XML decl -- never seen.
    //   * mixed content -- our tag handlers ignore text between tags
    //     except inside <t>/<v>/<is>, which is exactly the xlsx contract.
    //
    // The scanner takes a callback object instead of going through
    // virtual dispatch, so the tight inner loop inlines fully.
    // -------------------------------------------------------------------------
    struct SaxAttr
    {
        std::string name;
        std::string value;
    };

    // Decode the five named XML entities + numeric character references.
    // Returns false on a syntactically broken &...; sequence; we currently
    // surface that by leaving the bytes verbatim (lossy but non-fatal --
    // xlsx authored by Excel is well-formed).
    void decodeXmlEntities(std::string& s)
    {
        std::string out;
        out.reserve(s.size());
        for (size_t i = 0; i < s.size();)
        {
            if (s[i] != '&')
            {
                out.push_back(s[i++]);
                continue;
            }
            const size_t semi = s.find(';', i + 1);
            if (semi == std::string::npos || semi - i > 16)
            {
                out.push_back(s[i++]);
                continue;
            }
            const std::string ent = s.substr(i + 1, semi - i - 1);
            if (ent == "amp")
            {
                out.push_back('&');
            }
            else if (ent == "lt")
            {
                out.push_back('<');
            }
            else if (ent == "gt")
            {
                out.push_back('>');
            }
            else if (ent == "quot")
            {
                out.push_back('"');
            }
            else if (ent == "apos")
            {
                out.push_back('\'');
            }
            else if (!ent.empty() && ent[0] == '#')
            {
                // Numeric reference. Decimal &#NNN; or hex &#xHHHH; / &#XHHHH;.
                uint32_t code = 0;
                bool ok = false;
                try
                {
                    if (ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X'))
                    {
                        code = static_cast<uint32_t>(std::stoul(ent.substr(2), nullptr, 16));
                    }
                    else
                    {
                        code = static_cast<uint32_t>(std::stoul(ent.substr(1), nullptr, 10));
                    }
                    ok = true;
                }
                catch (...)
                {
                    ok = false;
                }

                if (!ok)
                {
                    // Fall through to verbatim copy.
                    out.append(s, i, semi - i + 1);
                }
                else
                {
                    // UTF-8 encode the codepoint. xlsx stores the workbook
                    // as UTF-8 bytes; we keep cells as UTF-8 std::string
                    // and only convert at the C++->engine-string boundary.
                    if (code < 0x80)
                    {
                        out.push_back(static_cast<char>(code));
                    }
                    else if (code < 0x800)
                    {
                        out.push_back(static_cast<char>(0xC0 | (code >> 6)));
                        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                    }
                    else if (code < 0x10000)
                    {
                        out.push_back(static_cast<char>(0xE0 | (code >> 12)));
                        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                    }
                    else if (code < 0x110000)
                    {
                        out.push_back(static_cast<char>(0xF0 | (code >> 18)));
                        out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
                        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                    }
                    else
                    {
                        // Out-of-range codepoint; emit replacement.
                        out.append("\xEF\xBF\xBD");
                    }
                }
            }
            else
            {
                // Unknown entity -- pass through verbatim.
                out.append(s, i, semi - i + 1);
            }
            i = semi + 1;
        }
        s.swap(out);
    }

    template<class Handler>
    void runSaxScan(const std::string& xml, Handler& h)
    {
        const size_t n = xml.size();
        size_t i = 0;

        // Buffer for character data between tags.
        std::string text;
        text.reserve(64);

        auto flush_text = [&]() {
            if (!text.empty())
            {
                decodeXmlEntities(text);
                h.onText(text);
                text.clear();
            }
        };

        while (i < n)
        {
            const char c = xml[i];
            if (c != '<')
            {
                text.push_back(c);
                ++i;
                continue;
            }

            // We hit '<' -- flush any preceding character data.
            flush_text();

            // ---- Comment / DOCTYPE / CDATA / PI dispatch ----
            if (i + 3 < n && xml[i + 1] == '!' && xml[i + 2] == '-' && xml[i + 3] == '-')
            {
                // <!-- comment -->
                const size_t end = xml.find("-->", i + 4);
                i = (end == std::string::npos) ? n : (end + 3);
                continue;
            }
            if (i + 8 < n && xml.compare(i, 9, "<![CDATA[") == 0)
            {
                const size_t end = xml.find("]]>", i + 9);
                if (end == std::string::npos)
                {
                    i = n;
                    break;
                }
                // CDATA contents are emitted as text verbatim (no entity
                // decode -- by spec, &amp; inside CDATA stays literal).
                std::string cdata = xml.substr(i + 9, end - (i + 9));
                h.onText(cdata);
                i = end + 3;
                continue;
            }
            if (i + 1 < n && xml[i + 1] == '!')
            {
                // <!DOCTYPE ...> or <!ENTITY...> -- skip to next '>'.
                const size_t end = xml.find('>', i + 2);
                i = (end == std::string::npos) ? n : (end + 1);
                continue;
            }
            if (i + 1 < n && xml[i + 1] == '?')
            {
                // <?xml ...?> -- skip to "?>".
                const size_t end = xml.find("?>", i + 2);
                i = (end == std::string::npos) ? n : (end + 2);
                continue;
            }

            // ---- Element tag ----
            const bool is_close = (i + 1 < n && xml[i + 1] == '/');
            const size_t tag_start = i + (is_close ? 2u : 1u);

            // Find the matching '>' that terminates the tag. Attribute
            // values may contain '>' inside quotes -- track the quote
            // state so we don't bail out early.
            size_t j = tag_start;
            char in_quote = 0;
            bool self_closing = false;
            while (j < n)
            {
                const char cj = xml[j];
                if (in_quote)
                {
                    if (cj == in_quote)
                        in_quote = 0;
                }
                else
                {
                    if (cj == '"' || cj == '\'')
                    {
                        in_quote = cj;
                    }
                    else if (cj == '>')
                    {
                        if (j > tag_start && xml[j - 1] == '/')
                            self_closing = true;
                        break;
                    }
                }
                ++j;
            }
            if (j >= n)
            {
                // Truncated tag; nothing to do but bail.
                break;
            }

            // Slice out the inside of <...>; strip a trailing '/' for
            // self-closing tags.
            const size_t inner_end = self_closing ? j - 1 : j;
            const std::string inner = xml.substr(tag_start, inner_end - tag_start);

            // Element name = leading run of non-whitespace.
            size_t name_end = 0;
            while (name_end < inner.size() &&
                   !std::isspace(static_cast<unsigned char>(inner[name_end])))
            {
                ++name_end;
            }
            std::string name = inner.substr(0, name_end);

            if (is_close)
            {
                h.onEndElement(name);
            }
            else
            {
                // Parse attributes from the remaining slice.
                std::vector<SaxAttr> attrs;
                size_t k = name_end;
                while (k < inner.size())
                {
                    while (k < inner.size() &&
                           std::isspace(static_cast<unsigned char>(inner[k])))
                    {
                        ++k;
                    }
                    if (k >= inner.size())
                        break;

                    const size_t kn_start = k;
                    while (k < inner.size() && inner[k] != '=' &&
                           !std::isspace(static_cast<unsigned char>(inner[k])))
                    {
                        ++k;
                    }
                    SaxAttr a;
                    a.name = inner.substr(kn_start, k - kn_start);

                    while (k < inner.size() &&
                           std::isspace(static_cast<unsigned char>(inner[k])))
                    {
                        ++k;
                    }
                    if (k >= inner.size() || inner[k] != '=')
                    {
                        // Attribute without value (HTML-style). XLSX never
                        // emits this; treat as empty value and continue.
                        attrs.push_back(std::move(a));
                        continue;
                    }
                    ++k;  // skip '='
                    while (k < inner.size() &&
                           std::isspace(static_cast<unsigned char>(inner[k])))
                    {
                        ++k;
                    }
                    if (k >= inner.size())
                        break;
                    if (inner[k] == '"' || inner[k] == '\'')
                    {
                        const char quote = inner[k++];
                        const size_t v_start = k;
                        while (k < inner.size() && inner[k] != quote)
                            ++k;
                        a.value = inner.substr(v_start, k - v_start);
                        if (k < inner.size())
                            ++k;  // skip closing quote
                    }
                    else
                    {
                        // Unquoted value (also non-spec; xlsx never uses).
                        const size_t v_start = k;
                        while (k < inner.size() &&
                               !std::isspace(static_cast<unsigned char>(inner[k])))
                        {
                            ++k;
                        }
                        a.value = inner.substr(v_start, k - v_start);
                    }
                    decodeXmlEntities(a.value);
                    attrs.push_back(std::move(a));
                }

                h.onStartElement(name, attrs, self_closing);
                if (self_closing)
                {
                    h.onEndElement(name);
                }
            }

            i = j + 1;
        }
        flush_text();
    }

    // -------------------------------------------------------------------------
    // sharedStrings.xml handler. The structure is:
    //   <sst ...>
    //     <si><t>plain text</t></si>
    //     <si><r><t>rich</t></r><r><t> text</t></r></si>   -- runs concat
    //     <si><t xml:space="preserve">  spacey  </t></si>
    //   </sst>
    //
    // We produce a vector<string> indexed by the order of <si> elements.
    // Rich-text runs (<r> children) are flattened by concatenating their
    // <t> contents -- xlsx allows formatting within a single string but
    // the resulting plain text is identical, which is all we need.
    // -------------------------------------------------------------------------
    struct SstHandler
    {
        std::vector<std::string>* out;
        std::string current;
        bool in_si = false;
        bool in_t = false;

        void onStartElement(const std::string& name,
                            const std::vector<SaxAttr>& /*attrs*/,
                            bool /*self_closing*/)
        {
            if (name == "si")
            {
                in_si = true;
                current.clear();
            }
            else if (name == "t" && in_si)
            {
                in_t = true;
            }
            // <r> just groups runs; we keep `current` accumulating across
            // <t> elements regardless of <r> boundaries.
        }
        void onEndElement(const std::string& name)
        {
            if (name == "t")
            {
                in_t = false;
            }
            else if (name == "si")
            {
                out->push_back(std::move(current));
                current.clear();
                in_si = false;
            }
        }
        void onText(const std::string& chunk)
        {
            if (in_si && in_t)
                current.append(chunk);
        }
    };

    bool parseSharedStrings(const std::string& xml,
                            std::vector<std::string>& out)
    {
        out.clear();
        SstHandler h;
        h.out = &out;
        runSaxScan(xml, h);
        return true;  // any malformed xlsx still gives us partial data
    }

    // -------------------------------------------------------------------------
    // sheet1.xml handler. The structure (only bits we touch):
    //   <worksheet>
    //     <sheetData>
    //       <row r="1" ...>
    //         <c r="A1" t="..." s="..."><v>raw</v></c>
    //         <c r="B1" t="inlineStr"><is><t>literal</t></is></c>
    //         <c r="C1"><f>...</f><v>cached_result</v></c>
    //         ...
    //       </row>
    //       <row r="2" ...>...</row>
    //     </sheetData>
    //   </worksheet>
    //
    // Cell types we recognise:
    //   t="s"          : <v> is an INDEX into sharedStrings.
    //   t="inlineStr"  : <is><t>...</t></is> contains the literal string.
    //   t="b"          : <v>0|1</v> -> "false"/"true" string.
    //   t="str"        : <v> is the cached string result of a formula.
    //   t="n" or absent: <v> is a numeric literal -- emitted verbatim.
    //   t="e"          : <v> is an error code like "#REF!" -- emitted verbatim.
    //   t="d"          : ISO date (rare, mostly LibreOffice) -- emitted verbatim.
    //
    // Sparse rows: cell `r="C1"` after `r="A1"` skips column B; we pad the
    // output row with an empty string at index 1. Excel never reorders
    // columns within a row, so a left-to-right scan + monotonically-
    // increasing column index assumption is safe.
    //
    // Sparse trailing rows: row `r="5"` after row `r="2"` leaves rows 3
    // and 4 entirely missing. We deliberately DO NOT pad these -- the
    // CSV path doesn't preserve trailing blank rows either, and xlsx
    // designers routinely have section gaps. The first row defines the
    // header; subsequent rows that are entirely blank are skipped at the
    // width-normalisation pass below, matching parseCsv's flush_row.
    // -------------------------------------------------------------------------
    int xlsxColLetterToIndex(const std::string& addr)
    {
        // addr like "A1" / "AB12" / "AAA999". Leading letters are 1-based
        // base-26 (A=1, Z=26, AA=27). Convert to 0-based column index.
        int col = 0;
        for (char c : addr)
        {
            const char up = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            if (up >= 'A' && up <= 'Z')
            {
                col = col * 26 + (up - 'A' + 1);
            }
            else
            {
                break;
            }
        }
        return col - 1;  // 0-based
    }

    struct SheetHandler
    {
        const std::vector<std::string>* sst = nullptr;
        std::vector<std::vector<std::string>>* rows_out = nullptr;
        std::vector<std::string> cur_row;
        std::string cur_text;

        // Cell-level state.
        bool in_row = false;
        bool in_cell = false;
        bool in_v = false;     // <v> -- raw value
        bool in_is_t = false;  // <is><t> -- inline literal string
        std::string cell_type;
        int cell_col = -1;

        void onStartElement(const std::string& name,
                            const std::vector<SaxAttr>& attrs,
                            bool /*self_closing*/)
        {
            if (name == "row")
            {
                in_row = true;
                cur_row.clear();
            }
            else if (name == "c" && in_row)
            {
                in_cell = true;
                cell_type.clear();
                cell_col = -1;
                cur_text.clear();
                for (const auto& a : attrs)
                {
                    if (a.name == "r")
                    {
                        cell_col = xlsxColLetterToIndex(a.value);
                    }
                    else if (a.name == "t")
                    {
                        cell_type = a.value;
                    }
                }
            }
            else if (in_cell && (name == "v" || (name == "t" && cell_type == "inlineStr")))
            {
                cur_text.clear();
                if (name == "v")
                    in_v = true;
                else
                    in_is_t = true;
            }
            else if (in_cell && name == "is")
            {
                // wrapper; child <t> handled above when cell_type=="inlineStr"
            }
        }

        void onEndElement(const std::string& name)
        {
            if (name == "v" && in_cell && in_v)
            {
                in_v = false;
                // Resolve string cells via sharedStrings; everything else
                // emits the raw <v> contents verbatim.
                std::string cell_value;
                if (cell_type == "s" && sst != nullptr)
                {
                    int idx = -1;
                    try
                    {
                        idx = std::stoi(cur_text);
                    }
                    catch (...)
                    {
                        idx = -1;
                    }
                    if (idx >= 0 && idx < static_cast<int>(sst->size()))
                    {
                        cell_value = (*sst)[idx];
                    }
                }
                else if (cell_type == "b")
                {
                    cell_value = (cur_text == "1") ? "true" : "false";
                }
                else
                {
                    cell_value = cur_text;
                }
                placeCell(cell_value);
                cur_text.clear();
            }
            else if (name == "t" && in_cell && in_is_t)
            {
                in_is_t = false;
                placeCell(cur_text);
                cur_text.clear();
            }
            else if (name == "c" && in_cell)
            {
                // Cell ended without a <v> (empty cell, e.g. just <c r="C1"/>).
                // If we never placed anything, push an empty string at the
                // recorded column index. We allow this branch to run even
                // when in_v / in_is_t briefly toggled; the placeCell calls
                // in those branches set cur_text empty and we treat that as
                // "cell explicitly stored an empty string", same observable
                // behaviour.
                if (cur_text.empty() &&
                    static_cast<int>(cur_row.size()) <= cell_col &&
                    cell_col >= 0)
                {
                    placeCell(std::string());
                }
                in_cell = false;
                cur_text.clear();
            }
            else if (name == "row" && in_row)
            {
                rows_out->push_back(std::move(cur_row));
                cur_row.clear();
                in_row = false;
            }
        }

        void onText(const std::string& chunk)
        {
            if (in_v || in_is_t)
                cur_text.append(chunk);
        }

        // Pad with empty strings up to cell_col, then push the value.
        // Negative cell_col (no `r` attribute) appends to the current end.
        void placeCell(const std::string& v)
        {
            if (cell_col < 0)
            {
                cur_row.push_back(v);
                return;
            }
            while (static_cast<int>(cur_row.size()) < cell_col)
            {
                cur_row.emplace_back();
            }
            if (static_cast<int>(cur_row.size()) == cell_col)
            {
                cur_row.push_back(v);
            }
            else
            {
                // Same column written twice (Excel never does this, but a
                // malformed sheet could). Last write wins.
                cur_row[cell_col] = v;
            }
        }
    };

    bool parseFirstSheet(const std::string& xml,
                         const std::vector<std::string>& sst,
                         std::vector<std::vector<std::string>>& rows_out)
    {
        rows_out.clear();
        SheetHandler h;
        h.sst = &sst;
        h.rows_out = &rows_out;
        runSaxScan(xml, h);
        return true;
    }

    // ASCII trim helper (header normalisation), mirrors parseCsv's helper.
    eastl::string trim_ascii(const eastl::string& s)
    {
        size_t a = 0;
        size_t b = s.size();
        while (a < b && std::isspace(static_cast<unsigned char>(s[a])))
            ++a;
        while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])))
            --b;
        return s.substr(a, b - a);
    }

    // -------------------------------------------------------------------------
    // Find the path to the FIRST sheet inside the workbook. xlsx workbooks
    // store sheet ordering in xl/workbook.xml as
    //   <workbook>
    //     <sheets>
    //       <sheet name="Sheet1" sheetId="1" r:id="rId1"/>
    //       ...
    //     </sheets>
    //   </workbook>
    //
    // The actual path is in xl/_rels/workbook.xml.rels, indexed by `r:id`:
    //   <Relationships>
    //     <Relationship Id="rId1" Type="..." Target="worksheets/sheet1.xml"/>
    //     ...
    //   </Relationships>
    //
    // Excel always emits the first <sheet> as rId1 -> worksheets/sheet1.xml,
    // but third-party tools (OpenPyXL with custom IDs) reorder. We resolve
    // the rels properly to be safe.
    //
    // The Target is RELATIVE to the workbook part's directory ("xl/"), so
    // the full archive path is "xl/" + Target.
    // -------------------------------------------------------------------------
    struct WorkbookFirstSheetHandler
    {
        std::string first_rid;
        bool in_sheets = false;
        bool captured = false;

        void onStartElement(const std::string& name,
                            const std::vector<SaxAttr>& attrs,
                            bool /*self_closing*/)
        {
            if (name == "sheets")
            {
                in_sheets = true;
                return;
            }
            if (in_sheets && !captured && (name == "sheet"))
            {
                for (const auto& a : attrs)
                {
                    // r:id, sometimes plain id depending on namespace handling.
                    if (a.name == "r:id" || a.name == "id")
                    {
                        first_rid = a.value;
                        captured = true;
                        break;
                    }
                }
            }
        }
        void onEndElement(const std::string& name)
        {
            if (name == "sheets")
                in_sheets = false;
        }
        void onText(const std::string& /*chunk*/) {}
    };

    struct RelsHandler
    {
        const std::string* target_id = nullptr;
        std::string target;
        bool captured = false;

        void onStartElement(const std::string& name,
                            const std::vector<SaxAttr>& attrs,
                            bool /*self_closing*/)
        {
            if (captured)
                return;
            if (name != "Relationship")
                return;
            std::string id;
            std::string tgt;
            for (const auto& a : attrs)
            {
                if (a.name == "Id")
                    id = a.value;
                else if (a.name == "Target")
                    tgt = a.value;
            }
            if (target_id != nullptr && id == *target_id)
            {
                target = std::move(tgt);
                captured = true;
            }
        }
        void onEndElement(const std::string& /*name*/) {}
        void onText(const std::string& /*chunk*/) {}
    };

    // Resolve "xl/" + target path with minimal "../" handling. xlsx
    // relationships almost always emit a relative path with no "..", but
    // we collapse any leading "../" segments anyway because the spec
    // permits them.
    std::string normaliseRelTarget(const std::string& base_dir, const std::string& target)
    {
        std::string out = base_dir;
        if (!out.empty() && out.back() != '/')
            out.push_back('/');
        out += target;
        // Resolve "/./" and "/foo/../" segments lexically.
        std::vector<std::string> parts;
        size_t i = 0;
        while (i < out.size())
        {
            size_t j = out.find('/', i);
            if (j == std::string::npos)
                j = out.size();
            std::string seg = out.substr(i, j - i);
            if (seg == "..")
            {
                if (!parts.empty())
                    parts.pop_back();
            }
            else if (!seg.empty() && seg != ".")
            {
                parts.push_back(seg);
            }
            i = j + 1;
        }
        std::string joined;
        for (size_t k = 0; k < parts.size(); ++k)
        {
            if (k > 0)
                joined.push_back('/');
            joined += parts[k];
        }
        return joined;
    }

    // Locate the first sheet's archive path. Returns "xl/worksheets/sheet1.xml"
    // as a fallback when workbook.xml / rels parsing fails -- this is what
    // 99% of workbooks ship with.
    std::string resolveFirstSheetPath(void* reader)
    {
        constexpr const char* kFallback = "xl/worksheets/sheet1.xml";

        std::string workbook_xml;
        eastl::string err;
        if (!readZipEntry(reader, "xl/workbook.xml", workbook_xml, err) || workbook_xml.empty())
        {
            return kFallback;
        }

        WorkbookFirstSheetHandler wh;
        runSaxScan(workbook_xml, wh);
        if (!wh.captured)
            return kFallback;

        std::string rels_xml;
        if (!readZipEntry(reader, "xl/_rels/workbook.xml.rels", rels_xml, err) ||
            rels_xml.empty())
        {
            return kFallback;
        }

        RelsHandler rh;
        rh.target_id = &wh.first_rid;
        runSaxScan(rels_xml, rh);
        if (!rh.captured)
            return kFallback;

        // Targets in workbook.xml.rels are relative to "xl/" by spec.
        return normaliseRelTarget("xl", rh.target);
    }
}  // namespace

// =============================================================================
// XlsxImporter -- public API
// =============================================================================

bool XlsxImporter::CanImport(const std::filesystem::path& file_path) const
{
    auto ext = file_path.extension().string();
    std::string ext_lower = ext;
    std::transform(ext_lower.begin(), ext_lower.end(), ext_lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext_lower == ".xlsx";
}

std::vector<std::string> XlsxImporter::GetSupportedExtensions() const
{
    return {".xlsx"};
}

std::string XlsxImporter::DeriveStableGuid(const std::filesystem::path& project_relative_xlsx_path)
{
    // Identical formula to DataTableImporter::DeriveStableGuid except for
    // the namespace prefix ("zdt-x-" vs "zdt-"), so XLSX-derived and
    // CSV-derived GUIDs for the same stem differ even when they collide
    // on disk. The Inspector / AssetRegistry treat them as separate entries.
    std::string norm = project_relative_xlsx_path.generic_string();
    std::transform(norm.begin(), norm.end(), norm.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const uint64_t h_lo = fnv1a64(norm);
    const uint64_t h_hi = fnv1a64("zdt-x-" + norm);

    std::ostringstream oss;
    oss << std::hex << std::setfill('0') << std::nouppercase
        << std::setw(8) << static_cast<uint32_t>(h_lo >> 32) << "-"
        << std::setw(4) << static_cast<uint16_t>(h_lo >> 16) << "-"
        << "4" << std::setw(3) << (static_cast<uint16_t>(h_lo) & 0x0FFFu) << "-"
        << std::setw(1) << ((static_cast<uint16_t>(h_hi >> 60) & 0x3) | 0x8)
        << std::setw(3) << (static_cast<uint16_t>(h_hi >> 48) & 0x0FFFu) << "-"
        << std::setw(12) << (h_hi & 0x0000FFFFFFFFFFFFULL);
    return oss.str();
}

bool XlsxImporter::ParseXlsx(const std::filesystem::path& xlsx_path,
                             eastl::vector<eastl::string>& out_headers,
                             eastl::vector<eastl::vector<eastl::string>>& out_rows,
                             eastl::string& out_error)
{
    out_headers.clear();
    out_rows.clear();
    out_error.clear();

    // ---- 1. Open the .xlsx as a ZIP, slurped into memory ----
    std::string buffer;
    void* reader = nullptr;
    if (!openXlsxAsZip(xlsx_path, buffer, reader, out_error))
    {
        return false;
    }
    struct ReaderGuard
    {
        void* h;
        ~ReaderGuard()
        {
            if (h)
            {
                mz_zip_reader_close(h);
                mz_zip_reader_delete(&h);
            }
        }
    } guard {reader};

    // ---- 2. Read sharedStrings.xml (optional) ----
    std::string sst_xml;
    std::vector<std::string> sst;
    if (readZipEntry(reader, "xl/sharedStrings.xml", sst_xml, out_error))
    {
        if (!sst_xml.empty())
            parseSharedStrings(sst_xml, sst);
    }
    else
    {
        // I/O error reading the entry; bail. (The "absent" case returns
        // true with sst_xml empty.)
        return false;
    }

    // ---- 3. Locate + read the first sheet ----
    const std::string sheet_path = resolveFirstSheetPath(reader);
    std::string sheet_xml;
    if (!readZipEntry(reader, sheet_path.c_str(), sheet_xml, out_error) ||
        sheet_xml.empty())
    {
        out_error = "first worksheet not found at ";
        out_error.append(sheet_path.c_str());
        return false;
    }

    std::vector<std::vector<std::string>> raw_rows;
    parseFirstSheet(sheet_xml, sst, raw_rows);

    // ---- 4. Width-normalise + emit header / body ----
    // Drop fully-empty rows -- xlsx grids often have trailing whitespace
    // rows when designers delete cells.
    raw_rows.erase(
        std::remove_if(raw_rows.begin(), raw_rows.end(), [](const std::vector<std::string>& r) {
            for (const auto& c : r)
                if (!c.empty())
                    return false;
            return true;
        }),
        raw_rows.end());

    if (raw_rows.empty())
    {
        out_error = "xlsx first sheet is empty (no header row)";
        return false;
    }

    // Header row.
    out_headers.reserve(raw_rows[0].size());
    for (auto& h : raw_rows[0])
    {
        eastl::string h_e(h.c_str());
        out_headers.push_back(trim_ascii(h_e));
    }
    if (out_headers.empty() || out_headers[0] != "id")
    {
        out_error = "first column header must be 'id' (case-sensitive); got '";
        out_error += out_headers.empty() ? "" : out_headers[0].c_str();
        out_error += "'";
        return false;
    }

    const size_t expected_w = out_headers.size();
    out_rows.reserve(raw_rows.size() - 1);
    for (size_t r = 1; r < raw_rows.size(); ++r)
    {
        eastl::vector<eastl::string> row;
        row.reserve(expected_w);
        for (size_t c = 0; c < expected_w; ++c)
        {
            row.emplace_back(c < raw_rows[r].size() ? raw_rows[r][c].c_str() : "");
        }
        out_rows.emplace_back(std::move(row));
    }

    return true;
}

bool XlsxImporter::Import(const std::filesystem::path& source_path,
                          const std::filesystem::path& output_path,
                          const AssetImporterSettings& /*import_settings*/,
                          AssetMetadata& out_metadata)
{
    // ---- 1. Resolve schema (same registry as CSV) ----
    eastl::string alias(source_path.stem().string().c_str());
    const CsvSchemaRegistry::Schema* schema = CsvSchemaRegistry::Get().Find(alias);
    if (schema == nullptr)
    {
        // No registered DataTable wrapper claims this xlsx. compileProject
        // batches and dedupes the warning; here we just signal via false.
        return false;
    }

    // ---- 2. Parse xlsx ----
    eastl::vector<eastl::string> headers;
    eastl::vector<eastl::vector<eastl::string>> data_rows;
    eastl::string parse_err;
    if (!ParseXlsx(source_path, headers, data_rows, parse_err))
    {
        LOG_ERROR(ZDataTable,
                  "XLSX parse failed for {}: {}",
                  source_path.generic_string(),
                  std::string(parse_err.c_str()));
        return false;
    }

    // ---- 3. Allocate the wrapper instance ----
    auto object_manager = GET_SYSTEM(ObjectManager);
    if (object_manager == nullptr)
    {
        LOG_ERROR(ZDataTable, "ObjectManager unavailable (xlsx import {})", source_path.generic_string());
        return false;
    }

    Object* produced = object_manager->Produce(schema->wrapper_type, /*instanceID=*/0);
    if (produced == nullptr)
    {
        LOG_ERROR(ZDataTable,
                  "failed to allocate wrapper of type {} for {}",
                  schema->wrapper_type ? schema->wrapper_type->GetName() : "<null>",
                  source_path.generic_string());
        return false;
    }
    auto* table = static_cast<DataTableBase*>(produced);

    // Stash the source-relative path so the Inspector can offer "open
    // source XLSX". Same field as CSV; the inspector branches on
    // extension to decide the editor mode (read-only for .xlsx).
    {
        auto project_info = GET_SYSTEM(ProjectInfo);
        if (project_info)
        {
            std::error_code ec;
            const auto rel = std::filesystem::relative(
                source_path, project_info->GetProjectRoot(), ec);
            table->m_SourceCsvRelpath = ec
                                            ? eastl::string(source_path.generic_string().c_str())
                                            : eastl::string(rel.generic_string().c_str());
        }
    }

    // ---- 4. Populate rows via the schema applier ----
    size_t row_index = 0;
    for (auto& cells : data_rows)
    {
        if (cells.empty() || cells[0].empty())
        {
            LOG_WARNING(ZDataTable,
                        "{}: skipping row {} because primary key 'id' is empty",
                        source_path.generic_string(),
                        row_index + 2);
            ++row_index;
            continue;
        }
        schema->applier(*table, cells, headers);
        ++row_index;
    }

    table->onPostLoad();

    // ---- 5. Serialise to .zasset ----
    auto asset_manager = GET_SYSTEM(AssetManager);
    if (asset_manager == nullptr)
    {
        LOG_ERROR(ZDataTable, "AssetManager unavailable; cannot write {}", output_path.generic_string());
        MemoryManager::DestroyObject(produced);
        return false;
    }

    {
        std::error_code ec;
        std::filesystem::create_directories(output_path.parent_path(), ec);
    }

    const bool ok = asset_manager->WriteObjectToDiskThreadSafe(output_path, *produced);

    out_metadata.guid = DeriveStableGuid(table->m_SourceCsvRelpath.empty()
                                             ? std::string(source_path.generic_string())
                                             : std::string(table->m_SourceCsvRelpath.c_str()));
    out_metadata.source_file_path = source_path.generic_string();
    {
        std::error_code ec;
        out_metadata.source_file_time = std::filesystem::last_write_time(source_path, ec);
    }
    out_metadata.dependencies.clear();
    out_metadata.custom_metadata.clear();

    MemoryManager::DestroyObject(produced);

    if (ok)
    {
        LOG_INFO(ZDataTable,
                 "compiled {} -> {} ({} rows, xlsx)",
                 source_path.generic_string(),
                 output_path.generic_string(),
                 data_rows.size());
    }
    else
    {
        LOG_ERROR(ZDataTable, "failed to write {} (xlsx)", output_path.generic_string());
    }
    return ok;
}

bool XlsxImporter::Reimport(const std::filesystem::path& zasset_path,
                            const AssetImporterSettings& import_settings)
{
    // For xlsx-sourced data tables, "reimport" means: find the source
    // .xlsx by path-derivation from the .zasset under
    // Assets/_Generated/Data/<rel>.zasset -> <Project>/Data/<rel>.xlsx.
    // Symmetric to DataTableImporter::Reimport but with .xlsx extension.
    auto project_info = GET_SYSTEM(ProjectInfo);
    if (project_info == nullptr)
    {
        LOG_ERROR(ZDataTable, "xlsx reimport: ProjectInfo unavailable");
        return false;
    }

    const std::filesystem::path generated_root = project_info->GetGeneratedDataRoot();
    if (generated_root.empty())
    {
        LOG_ERROR(ZDataTable, "xlsx reimport: no project loaded");
        return false;
    }

    std::error_code ec;
    auto rel = std::filesystem::relative(zasset_path, generated_root, ec);
    if (ec || rel.empty())
    {
        LOG_ERROR(ZDataTable,
                  "xlsx reimport: {} is not under {}",
                  zasset_path.generic_string(),
                  generated_root.generic_string());
        return false;
    }

    std::filesystem::path xlsx_rel = rel;
    xlsx_rel.replace_extension(".xlsx");
    const std::filesystem::path xlsx_path = project_info->GetDataRoot() / xlsx_rel;

    if (!std::filesystem::exists(xlsx_path))
    {
        LOG_ERROR(ZDataTable,
                  "xlsx reimport: source xlsx {} no longer exists for zasset {}",
                  xlsx_path.generic_string(),
                  zasset_path.generic_string());
        return false;
    }

    AssetMetadata new_metadata;
    return Import(xlsx_path, zasset_path, import_settings, new_metadata);
}

std::unique_ptr<AssetImporterSettings> XlsxImporter::GetDefaultSettings() const
{
    return std::make_unique<DataTableImporterSettings>();
}

std::filesystem::path
XlsxImporter::GeneratedPathFor(const std::filesystem::path& xlsx_path)
{
    auto project_info = GET_SYSTEM(ProjectInfo);
    if (project_info == nullptr)
        return {};

    const std::filesystem::path data_root = project_info->GetDataRoot();
    const std::filesystem::path out_root = project_info->GetGeneratedDataRoot();
    if (data_root.empty() || out_root.empty())
        return {};

    std::error_code ec;
    const std::filesystem::path abs_xlsx =
        xlsx_path.is_absolute() ? xlsx_path
                                : std::filesystem::weakly_canonical(xlsx_path, ec);

    const std::filesystem::path rel = std::filesystem::relative(abs_xlsx, data_root, ec);
    if (ec || rel.empty() || rel.generic_string().find("..") != std::string::npos)
    {
        return {};
    }

    std::filesystem::path dst = out_root / rel;
    dst.replace_extension(".zasset");
    return dst;
}

bool XlsxImporter::CompileOne(const std::filesystem::path& xlsx_path)
{
    if (!std::filesystem::exists(xlsx_path))
    {
        return false;
    }

    const std::filesystem::path dst = GeneratedPathFor(xlsx_path);
    if (dst.empty())
    {
        LOG_WARNING(ZDataTable,
                    "xlsx compileOne: {} is not under <Project>/Data/, ignoring",
                    xlsx_path.generic_string());
        return false;
    }

    eastl::string alias(xlsx_path.stem().string().c_str());
    const CsvSchemaRegistry::Schema* schema = CsvSchemaRegistry::Get().Find(alias);
    if (schema == nullptr)
    {
        LOG_INFO(ZDataTable,
                 "xlsx compileOne: skipping {} (no REGISTER_DATA_TABLE for stem '{}')",
                 xlsx_path.generic_string(),
                 xlsx_path.stem().string());
        return false;
    }

    XlsxImporter importer;
    DataTableImporterSettings settings;
    AssetMetadata meta;
    return importer.Import(xlsx_path, dst, settings, meta);
}

bool XlsxImporter::DeleteGeneratedFor(const std::filesystem::path& xlsx_path)
{
    const std::filesystem::path dst = GeneratedPathFor(xlsx_path);
    if (dst.empty())
        return false;

    std::error_code ec;
    if (!std::filesystem::exists(dst, ec))
        return true;

    const bool removed = std::filesystem::remove(dst, ec);
    if (ec)
    {
        LOG_WARNING(ZDataTable,
                    "xlsx deleteGeneratedFor: failed to remove {}: {}",
                    dst.generic_string(),
                    ec.message());
        return false;
    }
    if (removed)
    {
        LOG_INFO(ZDataTable, "deleted generated {} (source xlsx removed)", dst.generic_string());
    }
    return removed;
}

size_t XlsxImporter::CompileProject()
{
    auto project_info = GET_SYSTEM(ProjectInfo);
    if (project_info == nullptr)
        return 0;

    const std::filesystem::path data_root = project_info->GetDataRoot();
    const std::filesystem::path out_root = project_info->GetGeneratedDataRoot();
    if (data_root.empty() || out_root.empty())
        return 0;
    if (!std::filesystem::exists(data_root))
        return 0;

    XlsxImporter importer;
    DataTableImporterSettings default_settings;
    size_t compiled = 0;
    size_t skipped_no_schema = 0;
    eastl::vector<eastl::string> unmatched_stems;
    // Detect xlsx<->csv stem collisions. If a CSV produced the same dst
    // path earlier this session, our overwrite needs to surface as a
    // single LOG_WARNING per pair.
    eastl::vector<std::string> overwrites;

    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(data_root, ec);
         it != std::filesystem::recursive_directory_iterator();
         it.increment(ec))
    {
        if (ec)
        {
            ec.clear();
            continue;
        }
        const auto& entry = *it;
        if (!entry.is_regular_file(ec))
            continue;
        const auto& src = entry.path();
        if (!importer.CanImport(src))
            continue;

        eastl::string alias(src.stem().string().c_str());
        const CsvSchemaRegistry::Schema* schema = CsvSchemaRegistry::Get().Find(alias);
        if (schema == nullptr)
        {
            unmatched_stems.emplace_back(src.stem().string().c_str());
            ++skipped_no_schema;
            continue;
        }

        std::error_code rel_ec;
        const std::filesystem::path rel = std::filesystem::relative(src, data_root, rel_ec);
        std::filesystem::path dst = out_root / (rel_ec ? src.filename() : rel);
        dst.replace_extension(".zasset");

        // Stem-collision detection: if a sibling .csv at the same rel
        // path exists, we are about to overwrite the CSV's product.
        std::filesystem::path sibling_csv = src;
        sibling_csv.replace_extension(".csv");
        const bool collision = std::filesystem::exists(sibling_csv);

        AssetMetadata meta;
        if (importer.Import(src, dst, default_settings, meta))
        {
            ++compiled;
            if (collision)
            {
                overwrites.push_back(rel.generic_string());
            }
        }
    }

    if (compiled > 0)
    {
        LOG_INFO(ZDataTable, "xlsx compileProject: compiled {} table(s) under {}", compiled, data_root.generic_string());
    }
    if (skipped_no_schema > 0)
    {
        eastl::sort(unmatched_stems.begin(), unmatched_stems.end());
        unmatched_stems.erase(
            eastl::unique(unmatched_stems.begin(), unmatched_stems.end()),
            unmatched_stems.end());
        std::string joined;
        for (size_t i = 0; i < unmatched_stems.size(); ++i)
        {
            if (i > 0)
                joined += ", ";
            joined += unmatched_stems[i].c_str();
        }
        LOG_WARNING(ZDataTable,
                    "xlsx compileProject: skipped {} XLSX(s) with no REGISTER_DATA_TABLE schema: {}",
                    skipped_no_schema,
                    joined);
    }
    if (!overwrites.empty())
    {
        std::string joined;
        for (size_t i = 0; i < overwrites.size(); ++i)
        {
            if (i > 0)
                joined += ", ";
            joined += overwrites[i];
        }
        LOG_WARNING(ZDataTable,
                    "xlsx compileProject: {} stem collision(s) -- xlsx wins, csv product overwritten: {}",
                    overwrites.size(),
                    joined);
    }
    return compiled;
}
