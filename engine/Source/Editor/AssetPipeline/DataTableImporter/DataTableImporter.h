#pragma once

// =============================================================================
// DataTableImporter -- compiles a CSV (V1) source file under <Project>/Data/
// into a DataTableBase-derived .zasset under <Project>/Assets/_Generated/Data/.
//
// Pipeline overview
// -----------------
// 1) ProjectInfo::EnsureScriptsScaffold() (called once at editor startup) has
//    already created <Project>/Data/ and <Project>/Assets/_Generated/Data/.
// 2) EditorAssetManager::Initialize() instantiates a DataTableImporter and
//    registers it with AssetImportManager. It then calls CompileProject()
//    which performs a full scan of <Project>/Data/**/*.csv. This mirrors
//    TypeScriptCompiler::Initialize() doing a one-shot tsc pass on startup.
// 3) For each CSV file found, the importer asks the CsvSchemaRegistry which
//    DataTableBase subclass owns it (lookup by filename stem against the set
//    of REGISTER_DATA_TABLE'd table aliases). If no schema is registered the
//    file is skipped with a one-shot warning.
// 4) The importer parses the CSV (RFC 4180 minimal -- comma separator,
//    double-quote escape, first row = header, first column = "id" primary
//    key), invokes the schema-registered row applier per-row to populate
//    a freshly-Produced wrapper instance, then writes it to disk via
//    AssetManager::WriteObjectsToDiskThreadSafe (the same path prefabs use).
// 5) The .zasset products land under Assets/_Generated/Data/<rel>.zasset
//    where <rel> is the CSV's path relative to <Project>/Data/, with the
//    extension swapped. AssetRegistry then indexes them automatically the
//    next time the FileSystemWatcher pumps (no special wiring needed --
//    Assets/ is already the watched root, see EditorAssetManager).
//
// Schema registration: REGISTER_DATA_TABLE(WrapperClass, RowType, alias, applier)
// -------------------------------------------------------------------------------
// User code says (in WeaponDataTable.cpp):
//
//   REGISTER_DATA_TABLE(WeaponDataTable, WeaponRow, "Weapon",
//       [](WeaponRow& row, const eastl::vector<eastl::string>& cells,
//          const eastl::vector<eastl::string>& headers)
//       {
//           // headers[i] -> cells[i]; user maps to fields by name.
//           // The "id" column is auto-handled before the lambda is invoked.
//           row.damage = atoi(cells[/*"damage"*/...].c_str());
//           ...
//       });
//
// We deliberately ask the user to write a small applier lambda instead of
// trying to introspect TRow::Transfer() via reflection. ZEngine's Type*
// metadata covers class layout and inheritance but exposes no "for each
// field, name+type+offset" iterator suitable for a CSV string -> field
// converter. UE's UDataTable::CreateTableFromCSV uses UScriptStruct's
// FProperty walk; we would need an equivalent layer first. Punting that
// to V2 keeps the per-game cost at ~5 lines while leaving the door open
// to auto-generated appliers later.
//
// Determinism / GUID stability
// ----------------------------
// Generated .zassets carry a GUID derived from the relative source CSV
// path (FNV-1a 64 of "Data/<rel>.csv" lowercased -> formatted as a
// canonical GUID string). That way, deleting Assets/_Generated/Data/
// and re-running CompileProject() produces .zassets with identical GUIDs,
// and any references in scenes / inspector dropdowns survive a clean.
// Same trick the script_registry uses for source-file GUIDs (see
// AGENTS.md 2.2).
// =============================================================================

#include "Editor/AssetPipeline/AssetImporter.h"
#include "Editor/AssetPipeline/DataTableImporter/DataTableImporterSettings.h"

#include <EASTL/string.h>
#include <EASTL/vector.h>
#include <filesystem>
#include <functional>
#include <mutex>
#include <unordered_map>

class DataTableBase;
class Type;

// -----------------------------------------------------------------------------
// CsvSchemaRegistry -- maps a CSV "table alias" (= filename stem by default,
// or an explicit override passed to REGISTER_DATA_TABLE) to:
//   - a Type* the importer hands to ObjectManager::Produce() to allocate
//     a fresh DataTable wrapper instance
//   - an applier callback that converts a parsed CSV row (cells + headers)
//     into one row appended to the wrapper's m_Rows
//
// Registration happens at static-init time via REGISTER_DATA_TABLE in user
// translation units. The registry is process-global (singleton); the
// SystemRegistry doesn't fit because we need to register entries before any
// engine system is up (translation-unit static init runs first).
// -----------------------------------------------------------------------------
class CsvSchemaRegistry
{
public:
    /// Type-erased applier signature. Receives the wrapper instance (always
    /// downcast-safe to the registered DataTable<TRow> by callers; the macro
    /// generates a thin trampoline that performs the cast). cells.size() is
    /// guaranteed to equal headers.size() by the parser. The applier appends
    /// exactly one row to the wrapper's m_Rows; the importer takes care of
    /// the auto-id-column step before invoking it.
    using RowApplier = std::function<void(DataTableBase& table,
                                          const eastl::vector<eastl::string>& cells,
                                          const eastl::vector<eastl::string>& headers)>;

    struct Schema
    {
        const Type* wrapper_type = nullptr;  ///< TypeOf<WeaponDataTable>()
        eastl::string alias;                 ///< "Weapon"  (lookup key, from filename stem)
        eastl::string row_struct_name;       ///< "WeaponRow" (logging only)
        RowApplier applier;
    };

    static CsvSchemaRegistry& Get();

    /// Register a schema. Called from the static initialiser the macro emits.
    /// Duplicate aliases overwrite (later REGISTER wins); the warning is
    /// emitted lazily on first compileProject pass so static-init can't
    /// touch the log subsystem before it's up.
    void Register(Schema schema);

    /// Find by alias (filename stem). Returns nullptr if unregistered.
    const Schema* Find(const eastl::string& alias) const;

    /// Snapshot all registered aliases. Used by the importer's diagnostic
    /// "no schema for X.csv" log line so we can suggest close matches.
    eastl::vector<eastl::string> AllAliases() const;

private:
    mutable std::mutex m_Mutex;
    std::unordered_map<std::string, Schema> m_ByAlias;  // std::string key for hashing convenience
};

// -----------------------------------------------------------------------------
// DataTableImporter -- the actual AssetImporter implementation. Stateless;
// all per-table knowledge lives in the CsvSchemaRegistry.
// -----------------------------------------------------------------------------
class DataTableImporter : public AssetImporter
{
public:
    bool CanImport(const std::filesystem::path& file_path) const override;

    std::vector<std::string> GetSupportedExtensions() const override;

    /// Compile one CSV. `output_path` is the target .zasset location; caller
    /// is responsible for placing it under Assets/_Generated/Data/<rel>.zasset.
    /// On schema-miss we return false WITHOUT logging an error -- the caller
    /// (compileProject) deduplicates "missing schema" warnings so a project
    /// with 200 unregistered CSVs doesn't spam 200 log lines.
    bool Import(const std::filesystem::path& source_path,
                const std::filesystem::path& output_path,
                const AssetImporterSettings& import_settings,
                AssetMetadata& out_metadata) override;

    bool Reimport(const std::filesystem::path& zasset_path,
                  const AssetImporterSettings& import_settings) override;

    std::unique_ptr<AssetImporterSettings> GetDefaultSettings() const override;

    // -------------------------------------------------------------------------
    // One-shot whole-project compile. Walks <Project>/Data/**/*.csv and
    // emits .zassets under <Project>/Assets/_Generated/Data/. Returns the
    // number of tables successfully compiled (best-effort -- failures are
    // logged but don't abort the rest of the scan, same as TypeScriptCompiler
    // when one .ts has a syntax error).
    //
    // Safe to call multiple times; produces deterministic GUIDs (see file
    // header), so re-running just overwrites in place. The first call is
    // wired from EditorAssetManager::Initialize after the importer is
    // registered.
    // -------------------------------------------------------------------------
    static size_t CompileProject();

    // -------------------------------------------------------------------------
    // Incremental rebuild API (P5).
    //
    // EditorAssetManager owns a second FileSystemWatcher rooted at
    // <Project>/Data/ that filters .csv. On create/change events it calls
    // CompileOne(); on delete events it calls DeleteGeneratedFor(). Both
    // are static so the watcher closure doesn't need to retain an importer
    // instance -- the importer itself is stateless beyond CsvSchemaRegistry,
    // which is process-global.
    //
    // - csv_path may be absolute or relative-to-cwd; we normalise inside.
    // - On unknown schema (no REGISTER_DATA_TABLE matches the stem) we
    //   log a single LOG_INFO line per call so a designer who renames a
    //   CSV to an unregistered name notices, but we don't escalate to
    //   warning because that's a frequent transient state during file
    //   moves.
    // - Returns true on a successful compile / delete. Watcher callers
    //   ignore the return value; the boolean is for future automation tests.
    // -------------------------------------------------------------------------
    static bool CompileOne(const std::filesystem::path& csv_path);

    /// Delete the generated .zasset that mirrors `csv_path` under
    /// Assets/_Generated/Data/. Path-derivation only -- we deliberately do
    /// NOT walk the AssetRegistry to find by-source-file, because the
    /// registry doesn't currently track inverse `generated -> source` links
    /// for DataTables (zero need until a second producer of _Generated
    /// .zassets exists). Idempotent: missing target is a no-op.
    static bool DeleteGeneratedFor(const std::filesystem::path& csv_path);

    /// Map a source CSV path under <Project>/Data/ to its generated
    /// .zasset path under <Project>/Assets/_Generated/Data/. Returns an
    /// empty path if the input is not under Data/ or no project is loaded.
    /// Exposed for the Inspector "Reimport" button which needs the inverse
    /// translation (source CSV -> destination zasset).
    static std::filesystem::path GeneratedPathFor(const std::filesystem::path& csv_path);

    // -------------------------------------------------------------------------
    // CSV write-back (PR #7).
    //
    // The Inspector's edit mode lets a designer mutate cells in-place. We
    // do NOT round-trip those edits through the loaded DataTableBase --
    // that would let the in-memory zasset drift from the source CSV.
    // Instead, edits are gathered as a (headers, rows[][]) pair and pushed
    // through WriteCsv() to overwrite the source CSV on disk; the file
    // watcher (PR #5) then fires CompileOne() on the resulting change
    // event, producing a fresh .zasset that the Inspector picks up on its
    // next refresh.
    //
    // Format guarantees:
    //   * UTF-8 with BOM (Excel on Windows expects it; we're symmetric
    //     with parseCsv which strips a leading BOM).
    //   * LF line endings (designers on Windows can still open in Excel /
    //     Notepad). git's autocrlf will normalise on commit; our parser
    //     accepts CRLF too.
    //   * RFC 4180 quoting: cells containing comma / double-quote / CR /
    //     LF are wrapped in "..." with internal " doubled. Cells made of
    //     only printable ASCII (and no comma) pass through verbatim --
    //     keeps diffs clean.
    //   * Width invariant: every row.size() must equal headers.size();
    //     a mismatch returns false and writes nothing (no partial file).
    //
    // The parent directory of csv_path must exist (it always does for
    // existing CSVs being saved-back; we deliberately don't auto-create
    // because the Inspector's "Save" path implies the user is editing a
    // CSV that loaded successfully a moment ago).
    //
    // Return: true on full successful write; false on validation error
    // (caller logs). On false, no file system mutation has occurred.
    // -------------------------------------------------------------------------
    static bool WriteCsv(const std::filesystem::path& csv_path,
                         const eastl::vector<eastl::string>& headers,
                         const eastl::vector<eastl::vector<eastl::string>>& rows,
                         eastl::string& out_error);

    /// Parse one CSV file into rows of cells. Encoding is UTF-8 (BOM
    /// stripped). Empty lines and lines that consist of only whitespace are
    /// skipped. Quoted fields with embedded commas / newlines / "" escape
    /// are supported; everything else (multi-char quote, alternative
    /// separators, trailing whitespace trim) is V2.
    ///
    /// `out_headers` receives row 0; `out_rows` receives rows 1..N. Row
    /// width consistency (every row must have headers.size() cells) is
    /// enforced -- short rows pad with empty strings, long rows are truncated
    /// with a warning. We err on the lenient side so a designer half-finishing
    /// a row in Excel doesn't crash the editor.
    ///
    /// Public so the Inspector's edit mode can re-load the CSV string
    /// matrix on selection change (it edits the matrix in place rather
    /// than the loaded DataTable's row memory; see inspector_window.cpp's
    /// "PR #7 write-back model" comment).
    static bool ParseCsv(const std::filesystem::path& csv_path,
                         eastl::vector<eastl::string>& out_headers,
                         eastl::vector<eastl::vector<eastl::string>>& out_rows,
                         eastl::string& out_error);

private:
    /// Pick the schema for a given source file. Default policy: filename
    /// stem (without extension) matches a registered alias verbatim. Future
    /// extensions (e.g. an explicit "@Table=Weapon" pragma in the CSV head)
    /// would only need to widen this method.
    static const CsvSchemaRegistry::Schema* ResolveSchema(const std::filesystem::path& csv_path);

    /// Stable GUID from "Data/<rel>.csv" lowercased. See header doc.
    static std::string DeriveStableGuid(const std::filesystem::path& project_relative_csv_path);
};

// =============================================================================
// REGISTER_DATA_TABLE -- the user-facing macro.
//
// Invoked exactly once per (WrapperClass, RowType) pair, from a translation
// unit that is guaranteed to be linked into ZEditor. Best home is the same
// .cpp that already has IMPLEMENT_DATA_TABLE -- both run at static-init
// time and share the IMPLEMENT_REGISTER_CLASS forced-link guarantees.
//
// `Alias` is a string literal: the CSV filename stem the importer looks up.
// E.g. REGISTER_DATA_TABLE(WeaponDataTable, WeaponRow, "Weapon", ...) means
// <Project>/Data/Weapon.csv compiles into a WeaponDataTable; subdirectories
// (e.g. Data/Combat/Weapon.csv) also resolve to the same alias because the
// importer keys on filename stem only.
//
// `Applier` must match the signature
//     void(WeaponRow&, const eastl::vector<eastl::string>& cells,
//                      const eastl::vector<eastl::string>& headers)
// The macro emits a trampoline that downcasts the DataTableBase& we hand it
// to the right DataTable<RowType>, appends a fresh default-constructed TRow
// to m_Rows, and forwards the applier so the user lambda only ever sees TRow.
// =============================================================================
#define REGISTER_DATA_TABLE(WrapperClass_, RowType_, Alias_, /*applier lambda*/...)             \
    namespace                                                                                   \
    {                                                                                           \
        struct WrapperClass_##_DataTableSchemaRegistrar                                         \
        {                                                                                       \
            WrapperClass_##_DataTableSchemaRegistrar()                                          \
            {                                                                                   \
                /* Capture the user's lambda by value once; reference it inside the trampoline. \
                   The lambda must be copy-constructible -- captureless lambdas trivially are.  \
                */                                                                              \
                auto applier_fn = (__VA_ARGS__);                                                \
                                                                                                \
                CsvSchemaRegistry::Schema schema;                                               \
                schema.wrapper_type = TypeOf<WrapperClass_>();                                  \
                schema.alias = (Alias_);                                                        \
                schema.row_struct_name = #RowType_;                                             \
                schema.applier = [applier_fn](DataTableBase& table,                             \
                                              const eastl::vector<eastl::string>& cells,        \
                                              const eastl::vector<eastl::string>& headers) {    \
                    /* Cast to the typed wrapper. CsvSchemaRegistry guarantees                  \
                       the table was Produced from schema.wrapper_type, so this                 \
                       static_cast is sound (DataTable<TRow> is a non-virtual base). */         \
                    auto& typed = static_cast<DataTable<RowType_>&>(table);                     \
                    typed.m_Rows.emplace_back();                                                \
                    applier_fn(typed.m_Rows.back(), cells, headers);                            \
                };                                                                              \
                CsvSchemaRegistry::Get().Register(std::move(schema));                           \
            }                                                                                   \
        };                                                                                      \
        static WrapperClass_##_DataTableSchemaRegistrar                                         \
            g_##WrapperClass_##_DataTableSchemaRegistrar_instance;                              \
    }
