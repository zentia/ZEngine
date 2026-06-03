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

#include <EASTL/algorithm.h>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>

// =============================================================================
// CsvSchemaRegistry
// =============================================================================

CsvSchemaRegistry& CsvSchemaRegistry::Get()
{
    // Function-local static -- the C++ standard guarantees thread-safe init,
    // and crucially defers construction until first use, so REGISTER_DATA_TABLE
    // calls running before main() (translation-unit static init) work without
    // ordering hazards.
    static CsvSchemaRegistry s_Instance;
    return s_Instance;
}

void CsvSchemaRegistry::Register(Schema schema)
{
    if (schema.alias.empty())
    {
        // We can't LOG_WARNING here -- the log subsystem may not be up yet
        // during static init. Silently ignore; the importer's compileProject
        // pass will complain later if a CSV looks for a missing alias.
        return;
    }
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_ByAlias[std::string(schema.alias.c_str())] = std::move(schema);
}

const CsvSchemaRegistry::Schema* CsvSchemaRegistry::Find(const eastl::string& alias) const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    auto it = m_ByAlias.find(std::string(alias.c_str()));
    return it == m_ByAlias.end() ? nullptr : &it->second;
}

eastl::vector<eastl::string> CsvSchemaRegistry::AllAliases() const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    eastl::vector<eastl::string> out;
    out.reserve(m_ByAlias.size());
    for (const auto& kv : m_ByAlias)
    {
        out.emplace_back(kv.first.c_str());
    }
    return out;
}

// =============================================================================
// CSV parser (RFC 4180 minimal). Stateless; everything is local to parseCsv.
// =============================================================================

namespace
{
    // Trim ASCII whitespace from both ends. Used for header normalisation
    // only -- field cells are emitted verbatim so designers can intentionally
    // leave trailing spaces in display strings if they really want to.
    eastl::string trim_ascii(const eastl::string& s)
    {
        size_t a = 0;
        size_t b = s.size();
        while (a < b && std::isspace(static_cast<unsigned char>(s[a])))
        {
            ++a;
        }
        while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])))
        {
            --b;
        }
        return s.substr(a, b - a);
    }

    // FNV-1a 64. Same hash the rest of the engine uses for path-based GUIDs;
    // see ProjectInfo.cpp's getOrAssignScriptGuid for the matching script
    // registry derivation.
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
}  // namespace

bool DataTableImporter::ParseCsv(const std::filesystem::path& csv_path,
                                 eastl::vector<eastl::string>& out_headers,
                                 eastl::vector<eastl::vector<eastl::string>>& out_rows,
                                 eastl::string& out_error)
{
    out_headers.clear();
    out_rows.clear();
    out_error.clear();

    std::ifstream file(csv_path, std::ios::binary);
    if (!file.is_open())
    {
        out_error = "failed to open CSV file";
        return false;
    }

    // Slurp the whole file. Data tables are config-Scale (KBs to low MBs);
    // the streaming complexity isn't worth it.
    std::ostringstream ss;
    ss << file.rdbuf();
    std::string content = ss.str();

    // Strip UTF-8 BOM if present. Excel-on-Windows loves to emit one when
    // saving a CSV; keeping it would corrupt header[0] with a 3-byte prefix.
    if (content.size() >= 3 &&
        static_cast<unsigned char>(content[0]) == 0xEF &&
        static_cast<unsigned char>(content[1]) == 0xBB &&
        static_cast<unsigned char>(content[2]) == 0xBF)
    {
        content.erase(0, 3);
    }

    // RFC 4180 minimal state machine. We hand-roll instead of pulling a
    // CSV library dep because the rules are tiny and deterministic.
    eastl::vector<eastl::vector<eastl::string>> all_rows;
    eastl::vector<eastl::string> current_row;
    eastl::string current_cell;
    bool in_quotes = false;

    auto flush_cell = [&]() {
        current_row.push_back(std::move(current_cell));
        current_cell.clear();
    };
    auto flush_row = [&]() {
        // Skip rows that are completely empty (all cells empty) -- this
        // happens when a designer leaves a blank line between sections in
        // a hand-edited CSV. Matches Excel's "ignore blank rows on import"
        // behaviour.
        bool all_empty = true;
        for (const auto& c : current_row)
        {
            if (!c.empty())
            {
                all_empty = false;
                break;
            }
        }
        if (!all_empty || current_row.size() > 1)
        {
            all_rows.push_back(std::move(current_row));
        }
        current_row.clear();
    };

    for (size_t i = 0; i < content.size(); ++i)
    {
        const char ch = content[i];

        if (in_quotes)
        {
            if (ch == '"')
            {
                // RFC 4180 escape: "" inside a quoted field is one literal ".
                if (i + 1 < content.size() && content[i + 1] == '"')
                {
                    current_cell.push_back('"');
                    ++i;
                }
                else
                {
                    in_quotes = false;
                }
            }
            else
            {
                current_cell.push_back(ch);
            }
        }
        else
        {
            if (ch == '"' && current_cell.empty())
            {
                // Opening quote (only valid at field start).
                in_quotes = true;
            }
            else if (ch == ',')
            {
                flush_cell();
            }
            else if (ch == '\r')
            {
                // Skip; handled by the following '\n' or end-of-buffer.
            }
            else if (ch == '\n')
            {
                flush_cell();
                flush_row();
            }
            else
            {
                current_cell.push_back(ch);
            }
        }
    }
    // Final cell / row (file may not end with a newline).
    if (!current_cell.empty() || !current_row.empty())
    {
        flush_cell();
        flush_row();
    }

    if (all_rows.empty())
    {
        out_error = "CSV is empty (no header row)";
        return false;
    }

    // Row 0 is the header. Trim each header so "  id " in the source still
    // matches the lookup key "id".
    out_headers = std::move(all_rows[0]);
    for (auto& h : out_headers)
    {
        h = trim_ascii(h);
    }

    if (out_headers.empty() || out_headers[0] != "id")
    {
        // Build the error in eastl::string so we don't accidentally form a
        // std::string temporary on the RHS (eastl::string does not accept an
        // implicit assignment from std::string -- they're separate types
        // even though both wrap a char buffer; see AGENTS.md 2.3 string
        // policy and engine/3rdparty/EASTL/include/EASTL/string.h).
        out_error = "first column header must be 'id' (case-sensitive); got '";
        if (!out_headers.empty())
        {
            out_error += out_headers[0];
        }
        out_error += "'";
        return false;
    }

    // Width-normalise data rows. Pad short rows with empty strings, truncate
    // long rows. We log truncations at the importer level (not here), because
    // we don't have a logger handle in this static helper.
    out_rows.reserve(all_rows.size() > 0 ? all_rows.size() - 1 : 0);
    const size_t expected_w = out_headers.size();
    for (size_t r = 1; r < all_rows.size(); ++r)
    {
        auto& row = all_rows[r];
        if (row.size() < expected_w)
        {
            row.resize(expected_w);
        }
        else if (row.size() > expected_w)
        {
            row.resize(expected_w);
        }
        out_rows.emplace_back(std::move(row));
    }

    return true;
}

// =============================================================================
// DataTableImporter
// =============================================================================

bool DataTableImporter::CanImport(const std::filesystem::path& file_path) const
{
    auto ext = file_path.extension().string();
    std::string ext_lower = ext;
    std::transform(ext_lower.begin(), ext_lower.end(), ext_lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return ext_lower == ".csv";
}

std::vector<std::string> DataTableImporter::GetSupportedExtensions() const
{
    return {".csv"};
}

const CsvSchemaRegistry::Schema* DataTableImporter::ResolveSchema(const std::filesystem::path& csv_path)
{
    // Default policy: filename stem == registered alias. So
    // "<Project>/Data/Combat/Weapon.csv" resolves to alias "Weapon".
    eastl::string alias(csv_path.stem().string().c_str());
    return CsvSchemaRegistry::Get().Find(alias);
}

std::string DataTableImporter::DeriveStableGuid(const std::filesystem::path& project_relative_csv_path)
{
    // Hash the lower-cased forward-slash form so case differences and
    // separator differences across OSes don't change the GUID. Mirrors the
    // strategy in ScriptRegistry::deriveGuidFromPath (see AGENTS.md 2.2).
    std::string norm = project_relative_csv_path.generic_string();
    std::transform(norm.begin(), norm.end(), norm.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    const uint64_t h_lo = fnv1a64(norm);
    const uint64_t h_hi = fnv1a64("zdt-" + norm);  // second independent hash for the upper 64 bits

    // Format as canonical 8-4-4-4-12 GUID. Variant/version bits set to make
    // the result a syntactically valid v4 string (we don't claim it IS a v4
    // GUID; the bit is purely cosmetic so tools that validate the format
    // accept it).
    std::ostringstream oss;
    oss << std::hex << std::setfill('0') << std::nouppercase
        << std::setw(8) << static_cast<uint32_t>(h_lo >> 32) << "-"
        << std::setw(4) << static_cast<uint16_t>(h_lo >> 16) << "-"
        << "4" << std::setw(3) << (static_cast<uint16_t>(h_lo) & 0x0FFFu) << "-"
        << std::setw(1) << ((static_cast<uint16_t>(h_hi >> 60) & 0x3) | 0x8) /* variant */
        << std::setw(3) << (static_cast<uint16_t>(h_hi >> 48) & 0x0FFFu) << "-"
        << std::setw(12) << (h_hi & 0x0000FFFFFFFFFFFFULL);
    return oss.str();
}

bool DataTableImporter::Import(const std::filesystem::path& source_path,
                               const std::filesystem::path& output_path,
                               const AssetImporterSettings& /*import_settings*/,
                               AssetMetadata& out_metadata)
{
    // ---------- 1. Resolve schema --------------------------------------------
    const auto* schema = ResolveSchema(source_path);
    if (schema == nullptr)
    {
        // No registered DataTable wrapper claims this CSV. Caller (compileProject)
        // batches and dedupes these warnings; here we just signal via false.
        return false;
    }

    // ---------- 2. Parse CSV -------------------------------------------------
    eastl::vector<eastl::string> headers;
    eastl::vector<eastl::vector<eastl::string>> data_rows;
    eastl::string parse_err;
    if (!ParseCsv(source_path, headers, data_rows, parse_err))
    {
        LOG_ERROR(ZDataTable,
                  "CSV parse failed for {}: {}",
                  source_path.generic_string(),
                  std::string(parse_err.c_str()));
        return false;
    }

    // ---------- 3. Allocate the wrapper instance ------------------------------
    // Produce(type, 0) skips ID registration -- we want a clean transient
    // object that AssetManager will renumber when it writes. Same pattern as
    // PrefabUtility::SaveAsPrefabAsset.
    auto object_manager = GET_SYSTEM(ObjectManager);
    if (object_manager == nullptr)
    {
        LOG_ERROR(ZDataTable, "ObjectManager unavailable");
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

    // Best-effort: stash the relpath so the Inspector can offer "open source CSV".
    // Stored relative to project root if we can resolve it; otherwise keep the
    // raw absolute path so editor still has *something* useful.
    {
        auto project_info = GET_SYSTEM(ProjectInfo);
        if (project_info)
        {
            std::error_code ec;
            const auto rel =
                std::filesystem::relative(source_path, project_info->GetProjectRoot(), ec);
            table->m_SourceCsvRelpath = ec
                                            ? eastl::string(source_path.generic_string().c_str())
                                            : eastl::string(rel.generic_string().c_str());
        }
    }

    // ---------- 4. Populate rows ---------------------------------------------
    size_t row_index = 0;
    for (auto& cells : data_rows)
    {
        // First column == primary key "id". The schema applier still gets it
        // via cells[0], but we DON'T pre-fill row.id for the user -- the
        // user's lambda is responsible for copying cells[0] into row.id, the
        // same way they copy the rest of the cells. Keeping this convention
        // means the lambda is symmetric across columns and we never have to
        // make assumptions about the row struct's layout.
        //
        // We do however validate that cells[0] is non-empty -- a row without
        // a primary key is unusable. Skipping it preserves indices for
        // subsequent rows in the warning message.
        if (cells.empty() || cells[0].empty())
        {
            LOG_WARNING(ZDataTable,
                        "{}: skipping row {} because primary key 'id' is empty",
                        source_path.generic_string(),
                        row_index + 2);  // +2 = +1 for header, +1 for human 1-based
            ++row_index;
            continue;
        }

        schema->applier(*table, cells, headers);
        ++row_index;
    }

    // After all rows are populated, build the runtime hash index. The
    // wrapper's onPostLoad() does the same thing on load; we call it here so
    // anything inspecting the table immediately after Compile (e.g. an
    // editor "preview rows" panel later) gets a fully-functional table
    // without re-loading from disk.
    table->onPostLoad();

    // ---------- 5. Serialise to .zasset --------------------------------------
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
        // Non-fatal: WriteObject... will surface the actual error if the
        // directory is still missing.
    }

    const bool ok = asset_manager->WriteObjectToDiskThreadSafe(output_path, *produced);

    // Fill out metadata regardless of success so the AssetImportManager
    // caller still has something to read on partial failure.
    out_metadata.guid = DeriveStableGuid(table->m_SourceCsvRelpath.empty()
                                             ? std::string(source_path.generic_string())
                                             : std::string(table->m_SourceCsvRelpath.c_str()));
    out_metadata.source_file_path = source_path.generic_string();
    {
        std::error_code ec;
        out_metadata.source_file_time = std::filesystem::last_write_time(source_path, ec);
        // If the timestamp lookup fails we just leave whatever default the
        // file_time_type init gives us; importers above us don't check it.
    }
    out_metadata.dependencies.clear();
    out_metadata.custom_metadata.clear();

    // The wrapper is owned by ObjectManager once Produce() returned; we
    // destroy it now that its bytes are on disk, mirroring PrefabUtility.
    MemoryManager::DestroyObject(produced);

    if (ok)
    {
        LOG_INFO(ZDataTable,
                 "compiled {} -> {} ({} rows)",
                 source_path.generic_string(),
                 output_path.generic_string(),
                 data_rows.size());
    }
    else
    {
        LOG_ERROR(ZDataTable, "failed to write {}", output_path.generic_string());
    }
    return ok;
}

bool DataTableImporter::Reimport(const std::filesystem::path& zasset_path,
                                 const AssetImporterSettings& import_settings)
{
    // For DataTables, "reimport" means "find the source CSV and recompile".
    // The zasset_path is under <Project>/Assets/_Generated/Data/<rel>.zasset;
    // the corresponding source is <Project>/Data/<rel>.csv. We DON'T read
    // metadata from the .zasset header here because the `.zasset` metadata
    // section is currently unused (`AssetFileHeader::metadata_offset` /
    // `metadata_size` are stamped to 0 by `SerializedFile::WriteHeaderAndMetadata`
    // -- see the P2 #6 markers in `Runtime/asset/asset_file.h` and the
    // legacy `AssetFile::writeMetadata` was a no-op return-true before the
    // class was removed in P2 #9). The path-derivation approach below is
    // robust until a real metadata-persistence layer lands.
    auto project_info = GET_SYSTEM(ProjectInfo);
    if (project_info == nullptr)
    {
        LOG_ERROR(ZDataTable, "reimport: ProjectInfo unavailable");
        return false;
    }

    const std::filesystem::path generated_root = project_info->GetGeneratedDataRoot();
    if (generated_root.empty())
    {
        LOG_ERROR(ZDataTable, "reimport: no project loaded");
        return false;
    }

    std::error_code ec;
    auto rel = std::filesystem::relative(zasset_path, generated_root, ec);
    if (ec || rel.empty())
    {
        LOG_ERROR(ZDataTable,
                  "reimport: {} is not under {}",
                  zasset_path.generic_string(),
                  generated_root.generic_string());
        return false;
    }

    // Swap .zasset -> .csv to find the source.
    std::filesystem::path csv_rel = rel;
    csv_rel.replace_extension(".csv");
    const std::filesystem::path csv_path = project_info->GetDataRoot() / csv_rel;

    if (!std::filesystem::exists(csv_path))
    {
        LOG_ERROR(ZDataTable,
                  "reimport: source CSV {} no longer exists for zasset {}",
                  csv_path.generic_string(),
                  zasset_path.generic_string());
        return false;
    }

    AssetMetadata new_metadata;
    return Import(csv_path, zasset_path, import_settings, new_metadata);
}

std::unique_ptr<AssetImporterSettings> DataTableImporter::GetDefaultSettings() const
{
    return std::make_unique<DataTableImporterSettings>();
}

size_t DataTableImporter::CompileProject()
{
    auto project_info = GET_SYSTEM(ProjectInfo);
    if (project_info == nullptr)
    {
        return 0;
    }
    const std::filesystem::path data_root = project_info->GetDataRoot();
    const std::filesystem::path out_root = project_info->GetGeneratedDataRoot();
    if (data_root.empty() || out_root.empty())
    {
        return 0;
    }
    if (!std::filesystem::exists(data_root))
    {
        // No Data/ dir yet (fresh project, nothing for designers to author).
        // Not an error.
        return 0;
    }

    DataTableImporter importer;
    DataTableImporterSettings default_settings;
    size_t compiled = 0;
    size_t skipped_no_schema = 0;
    eastl::vector<eastl::string> unmatched_stems;  // for the dedup'd warning

    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(data_root, ec);
         it != std::filesystem::recursive_directory_iterator();
         it.increment(ec))
    {
        if (ec)
        {
            LOG_WARNING(ZDataTable, "scan error under {}: {}", data_root.generic_string(), ec.message());
            ec.clear();
            continue;
        }
        const auto& entry = *it;
        if (!entry.is_regular_file(ec))
        {
            continue;
        }
        const auto& src = entry.path();
        if (!importer.CanImport(src))
        {
            continue;
        }

        // Skip if no schema is registered for this stem. Collect the stem so
        // we can emit ONE warning at the end with all unmatched names instead
        // of N separate ones (a project with many CSVs and a forgotten
        // REGISTER_DATA_TABLE would otherwise drown the log).
        if (ResolveSchema(src) == nullptr)
        {
            unmatched_stems.emplace_back(src.stem().string().c_str());
            ++skipped_no_schema;
            continue;
        }

        // Compute output path: mirror the relative layout under
        // Assets/_Generated/Data/, just swapping .csv for .zasset.
        std::error_code rel_ec;
        const std::filesystem::path rel = std::filesystem::relative(src, data_root, rel_ec);
        std::filesystem::path dst = out_root / (rel_ec ? src.filename() : rel);
        dst.replace_extension(".zasset");

        AssetMetadata meta;
        if (importer.Import(src, dst, default_settings, meta))
        {
            ++compiled;
        }
    }

    if (compiled > 0)
    {
        LOG_INFO(ZDataTable, "compileProject: compiled {} table(s) under {}", compiled, data_root.generic_string());
    }
    if (skipped_no_schema > 0)
    {
        // Dedup the stem list so re-runs with the same set of unmatched files
        // don't spam the log with the same names many times in one batch (a
        // CSV named foo.csv obviously can't appear twice in one scan, but
        // future glob expansion across multiple roots could).
        eastl::sort(unmatched_stems.begin(), unmatched_stems.end());
        unmatched_stems.erase(eastl::unique(unmatched_stems.begin(), unmatched_stems.end()),
                              unmatched_stems.end());

        std::string joined;
        for (size_t i = 0; i < unmatched_stems.size(); ++i)
        {
            if (i > 0)
            {
                joined += ", ";
            }
            joined += unmatched_stems[i].c_str();
        }
        LOG_WARNING(ZDataTable,
                    "compileProject: skipped {} CSV(s) with no REGISTER_DATA_TABLE schema: {}",
                    skipped_no_schema,
                    joined);
    }
    return compiled;
}

// =============================================================================
// Incremental rebuild API (P5)
//
// generatedPathFor / compileOne / deleteGeneratedFor share a single
// source -> destination derivation, factored into the static helper below.
// FileSystemWatcher events arrive with absolute paths; compileProject's
// recursive walk also emits absolute paths. Both feed in here.
// =============================================================================

std::filesystem::path
DataTableImporter::GeneratedPathFor(const std::filesystem::path& csv_path)
{
    auto project_info = GET_SYSTEM(ProjectInfo);
    if (project_info == nullptr)
    {
        return {};
    }
    const std::filesystem::path data_root = project_info->GetDataRoot();
    const std::filesystem::path out_root = project_info->GetGeneratedDataRoot();
    if (data_root.empty() || out_root.empty())
    {
        return {};
    }

    // Normalise the input path: incremental callers (FileSystemWatcher)
    // pass absolute paths, but Inspector's "Reimport" handler may pass a
    // relative-to-Data path. We accept both: lexically_normal() collapses
    // "..", and weakly_canonical() resolves symlinks where possible. We
    // intentionally don't require the file to exist (delete events fire
    // *after* the file is gone, and we still need to compute the dst).
    std::error_code ec;
    const std::filesystem::path abs_csv =
        csv_path.is_absolute() ? csv_path
                               : std::filesystem::weakly_canonical(csv_path, ec);

    const std::filesystem::path rel = std::filesystem::relative(abs_csv, data_root, ec);
    if (ec || rel.empty() || rel.generic_string().find("..") != std::string::npos)
    {
        // Path is not under Data/. We deliberately reject ".." escapes
        // instead of silently mirroring the parent path -- the watcher
        // is rooted at Data/ so this branch only fires for malformed
        // direct calls (e.g. unit tests passing arbitrary paths).
        return {};
    }

    std::filesystem::path dst = out_root / rel;
    dst.replace_extension(".zasset");
    return dst;
}

bool DataTableImporter::CompileOne(const std::filesystem::path& csv_path)
{
    if (!std::filesystem::exists(csv_path))
    {
        // Race with delete events: watcher coalesces a rapid create+delete
        // back to a single change event. Silently bail; deleteGeneratedFor
        // will fire next.
        return false;
    }

    const std::filesystem::path dst = GeneratedPathFor(csv_path);
    if (dst.empty())
    {
        LOG_WARNING(ZDataTable,
                    "compileOne: {} is not under <Project>/Data/, ignoring",
                    csv_path.generic_string());
        return false;
    }

    if (ResolveSchema(csv_path) == nullptr)
    {
        // No registered DataTable claims this stem. Single info-level line
        // per event so renames to unregistered names are visible without
        // being noisy. compileProject's batched warning still fires on the
        // next full scan.
        LOG_INFO(ZDataTable,
                 "compileOne: skipping {} (no REGISTER_DATA_TABLE for stem '{}')",
                 csv_path.generic_string(),
                 csv_path.stem().string());
        return false;
    }

    DataTableImporter importer;
    DataTableImporterSettings settings;
    AssetMetadata meta;
    return importer.Import(csv_path, dst, settings, meta);
}

bool DataTableImporter::DeleteGeneratedFor(const std::filesystem::path& csv_path)
{
    const std::filesystem::path dst = GeneratedPathFor(csv_path);
    if (dst.empty())
    {
        return false;
    }

    std::error_code ec;
    if (!std::filesystem::exists(dst, ec))
    {
        // Designer deleted a CSV that never compiled (e.g. one with an
        // unregistered schema). Nothing to clean up.
        return true;
    }

    const bool removed = std::filesystem::remove(dst, ec);
    if (ec)
    {
        LOG_WARNING(ZDataTable,
                    "deleteGeneratedFor: failed to remove {}: {}",
                    dst.generic_string(),
                    ec.message());
        return false;
    }
    if (removed)
    {
        LOG_INFO(ZDataTable,
                 "deleted generated {} (source CSV removed)",
                 dst.generic_string());
    }
    return removed;
}

// =============================================================================
// CSV write-back (PR #7)
// =============================================================================

namespace
{
    // RFC 4180 minimal quoting. Cells that need quoting are those containing
    // comma, double-quote, CR, or LF; everything else passes through verbatim
    // to keep diffs clean. Inside a quoted cell, any " is doubled to "".
    bool csv_cell_needs_quoting(const eastl::string& cell)
    {
        for (size_t i = 0; i < cell.size(); ++i)
        {
            const char c = cell[i];
            if (c == ',' || c == '"' || c == '\r' || c == '\n')
            {
                return true;
            }
        }
        return false;
    }

    void csv_emit_cell(std::string& out, const eastl::string& cell)
    {
        if (!csv_cell_needs_quoting(cell))
        {
            out.append(cell.c_str(), cell.size());
            return;
        }
        out.push_back('"');
        for (size_t i = 0; i < cell.size(); ++i)
        {
            const char c = cell[i];
            if (c == '"')
            {
                out.push_back('"');
                out.push_back('"');
            }
            else
            {
                out.push_back(c);
            }
        }
        out.push_back('"');
    }
}  // namespace

bool DataTableImporter::WriteCsv(const std::filesystem::path& csv_path,
                                 const eastl::vector<eastl::string>& headers,
                                 const eastl::vector<eastl::vector<eastl::string>>& rows,
                                 eastl::string& out_error)
{
    out_error.clear();

    if (csv_path.empty())
    {
        out_error = "csv_path is empty";
        return false;
    }
    if (headers.empty())
    {
        out_error = "headers is empty (CSV must have at least one column)";
        return false;
    }
    if (headers[0] != "id")
    {
        // Mirror parseCsv's primary-key contract. If a caller managed to
        // mutate the first header out of "id" we refuse the write rather
        // than emit a CSV that the parser would reject on round-trip.
        out_error = "first header must be 'id'";
        return false;
    }

    // Width invariant: every row.size() == headers.size(). We check first
    // and fail without writing -- partial CSVs are worse than no CSV
    // because they shadow the previous good copy on the next watcher tick.
    const size_t expected_w = headers.size();
    for (size_t r = 0; r < rows.size(); ++r)
    {
        if (rows[r].size() != expected_w)
        {
            char buf[160];
            std::snprintf(buf, sizeof(buf), "row %zu has %zu cells but expected %zu (headers width)", r, rows[r].size(), expected_w);
            out_error = buf;
            return false;
        }
        // Reject rows whose primary key is empty -- ParseCsv() would skip
        // them on read, producing a mysterious row-loss after save.
        if (rows[r][0].empty())
        {
            char buf[80];
            std::snprintf(buf, sizeof(buf), "row %zu has empty primary key 'id'", r);
            out_error = buf;
            return false;
        }
    }

    // Build the entire file in memory first, then write atomically. CSV
    // tables are small (config-scale) so the buffer cost is irrelevant
    // and an all-at-once write avoids a half-written file if the editor
    // crashes mid-stream.
    std::string out;
    out.reserve(rows.size() * expected_w * 16 + headers.size() * 16);

    // UTF-8 BOM. Excel on Windows opens BOM-less CSVs as Latin-1 if any
    // cell contains non-ASCII, so we keep the BOM symmetric with what
    // ParseCsv() strips on read.
    out.push_back(static_cast<char>(0xEF));
    out.push_back(static_cast<char>(0xBB));
    out.push_back(static_cast<char>(0xBF));

    // Header row.
    for (size_t c = 0; c < headers.size(); ++c)
    {
        if (c > 0)
            out.push_back(',');
        csv_emit_cell(out, headers[c]);
    }
    out.push_back('\n');

    // Data rows.
    for (const auto& row : rows)
    {
        for (size_t c = 0; c < row.size(); ++c)
        {
            if (c > 0)
                out.push_back(',');
            csv_emit_cell(out, row[c]);
        }
        out.push_back('\n');
    }

    // Atomic write: emit to <csv>.tmp first, then rename onto the target.
    // std::filesystem::rename on Windows is atomic for a same-volume swap
    // (kernel-level MoveFileEx), which is what we always have here because
    // the temp lives next to the target.
    const std::filesystem::path tmp_path = csv_path.string() + ".tmp";

    {
        std::ofstream f(tmp_path, std::ios::binary | std::ios::trunc);
        if (!f.is_open())
        {
            out_error = "failed to open temp file for writing";
            return false;
        }
        f.write(out.data(), static_cast<std::streamsize>(out.size()));
        if (!f.good())
        {
            f.close();
            std::error_code ec;
            std::filesystem::remove(tmp_path, ec);
            out_error = "stream error while writing temp file";
            return false;
        }
    }

    std::error_code ec;
    std::filesystem::rename(tmp_path, csv_path, ec);
    if (ec)
    {
        // Best-effort fallback: remove + copy + remove-source. We do this
        // because Windows rename can fail across handles held by external
        // tools (Excel keeping the file open). Same fallback strategy
        // ScriptRegistry uses for its registry JSON.
        std::error_code ec2;
        std::filesystem::copy_file(tmp_path, csv_path, std::filesystem::copy_options::overwrite_existing, ec2);
        std::filesystem::remove(tmp_path, ec2);
        if (ec2)
        {
            // Same eastl::string vs std::string concat caveat as the
            // header-validation branch above: keep the LHS eastl-typed
            // and append the std::error_code message via .c_str().
            out_error = "rename failed and fallback copy failed: ";
            out_error += ec.message().c_str();
            return false;
        }
    }

    LOG_INFO(ZDataTable,
             "wrote CSV {} ({} rows)",
             csv_path.generic_string(),
             rows.size());
    return true;
}
