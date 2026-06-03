#pragma once

// =============================================================================
// XlsxImporter (PR #8) -- compiles a `.xlsx` (Office Open XML SpreadsheetML)
// source file under <Project>/Data/ into a DataTableBase-derived `.zasset`
// under <Project>/Assets/_Generated/Data/, in symmetrical fashion to
// DataTableImporter (CSV, V1).
//
// Single-source-of-truth direction (decided in the PR #8 design Q&A):
// ------------------------------------------------------------------
//  * `.csv` and `.xlsx` are INDEPENDENT source files. Each carries its
//    own table; the importer never converts xlsx -> csv or vice versa.
//  * If both `<Project>/Data/<rel>/Foo.csv` AND `<Project>/Data/<rel>/Foo.xlsx`
//    exist, XLSX wins: compileProject runs CSV first, then XLSX, so the
//    XLSX product overwrites the CSV product at the same `<rel>/Foo.zasset`
//    path. We log a single LOG_WARNING per collision so the designer can
//    delete one of the duplicates.
//  * .xlsx pipeline is READ-ONLY -- there is no Inspector "save back to
//    XLSX" path equivalent to PR #7's writeCsv. The Inspector detects an
//    .xlsx source via `m_SourceCsvRelpath` extension and grays out the
//    edit / save UI, falling back to PR #6's read-only memory view.
//
// Schema reuse:
// -------------
// The importer reuses CsvSchemaRegistry verbatim. Resolution policy is the
// same as for CSV: filename stem (without extension) maps to a registered
// alias, e.g. <Project>/Data/Combat/Weapon.xlsx -> alias "Weapon" ->
// WeaponDataTable. The applier signature is
// `void(DataTableBase&, vector<string> cells, vector<string> headers)` --
// already format-agnostic, so the same REGISTER_DATA_TABLE call covers
// both extensions for free.
//
// Multi-sheet handling:
// ---------------------
// V1 reads the FIRST worksheet only, mirroring the V1 "one CSV, one
// table" contract. The xlsx may contain extra sheets (Excel users
// frequently keep notes / pivot tables on sheet 2+); we ignore them
// without warning. A future V2 could opt into a multi-sheet mode via a
// "<stem>__<sheetname>" alias scheme, but the design Q&A explicitly
// chose A (first sheet only) to keep the surface minimal.
//
// XLSX format internals (only the bits we touch):
// -----------------------------------------------
// .xlsx is a ZIP container of XML parts. We need:
//   * `xl/sharedStrings.xml` -- string lookup table; cells of type "s"
//     reference an index into this list. Optional (workbooks with zero
//     literal strings omit it).
//   * `xl/worksheets/sheet1.xml` -- the first sheet's grid. Each row is
//     `<row r="N">` containing `<c r="A1" t="s|inlineStr|n|b" s="..."><v>X</v></c>`
//     (or `<c><is><t>...</t></is></c>` for inline strings).
//   * We do NOT consult `xl/styles.xml` (number-format codes for date/time
//     cells). V1 emits the raw stored value as a string; designers who
//     want a date should format the column as text in Excel before saving.
//
// Cell coordinate semantics:
// --------------------------
// Excel addresses cells as A1, B1, ... AA1, AB1. A trailing row index is
// optional (`<c r="A1">` -> col 0 row 0). Skipped columns (sparse rows)
// must be padded with empty strings so the resulting `cells` vector has
// `cells.size() == headers.size()`. Same width contract as ParseCsv().
//
// XML parsing:
// ------------
// We hand-roll a tiny SAX-style scanner instead of pulling a third-party
// XML library: ~250 lines of state machine, recognises just `<tag>` /
// `</tag>` / `<tag attr="val".../>` / text content / `<![CDATA[...]]>` /
// XML entity references for the five core entities (&amp; &lt; &gt;
// &quot; &apos;) and `&#NNN;` numeric. No DTD, no namespace processing,
// no PI/comment handling beyond skipping. This is lossy as a general
// XML parser but PRECISELY adequate for the well-formed XLSX subset
// Excel/LibreOffice/Google Sheets emit.
//
// Determinism / GUID stability:
// -----------------------------
// Same hashing strategy as DataTableImporter: FNV-1a 64 of "Data/<rel>.xlsx"
// lowercased. So a clean rebuild produces .zassets with identical GUIDs,
// and references in scenes / inspector dropdowns survive a full clean.
// XLSX-derived GUIDs and CSV-derived GUIDs differ (the input string ends in
// ".xlsx" vs ".csv"), so even when XLSX overwrites a CSV's product on disk,
// the GUID changes -- the AssetRegistry will treat it as a new entry. This
// is the price of the "stem collision = XLSX wins" rule; we surface it via
// the LOG_WARNING in compileProject so the designer is aware that
// scene/PPtr references made against the CSV product will rebind to the
// XLSX product on next compile.
// =============================================================================

#include "Editor/AssetPipeline/AssetImporter.h"
#include "Editor/AssetPipeline/DataTableImporter/DataTableImporterSettings.h"

#include <EASTL/string.h>
#include <EASTL/vector.h>
#include <filesystem>

class XlsxImporter : public AssetImporter
{
public:
    bool CanImport(const std::filesystem::path& file_path) const override;

    std::vector<std::string> GetSupportedExtensions() const override;

    /// Compile one .xlsx. `output_path` is the target .zasset location;
    /// caller is responsible for placing it under Assets/_Generated/Data/
    /// (compileProject does this, mirroring DataTableImporter).
    bool Import(const std::filesystem::path& source_path,
                const std::filesystem::path& output_path,
                const AssetImporterSettings& import_settings,
                AssetMetadata& out_metadata) override;

    bool Reimport(const std::filesystem::path& zasset_path,
                  const AssetImporterSettings& import_settings) override;

    std::unique_ptr<AssetImporterSettings> GetDefaultSettings() const override;

    /// One-shot whole-project compile of every .xlsx under <Project>/Data/.
    /// Run AFTER DataTableImporter::CompileProject() so XLSX overwrites
    /// CSV products on stem collisions. Best-effort; failures are logged
    /// but do not abort the rest of the scan.
    static size_t CompileProject();

    /// Incremental rebuild API (mirrors DataTableImporter). Routed from
    /// EditorAssetManager's m_DataWatcher when an .xlsx file is created
    /// or modified.
    static bool CompileOne(const std::filesystem::path& xlsx_path);

    /// Idempotent: deletes the .zasset under Assets/_Generated/Data/ that
    /// mirrors xlsx_path. No-op if the target is missing.
    static bool DeleteGeneratedFor(const std::filesystem::path& xlsx_path);

    /// Map a source XLSX path under <Project>/Data/ to its generated
    /// .zasset path under <Project>/Assets/_Generated/Data/.
    /// Returns empty path if input is not under Data/.
    static std::filesystem::path GeneratedPathFor(const std::filesystem::path& xlsx_path);

    /// Parse one .xlsx file's first worksheet into rows of cells.
    /// Same output contract as DataTableImporter::parseCsv: row 0 ->
    /// out_headers, rows 1..N -> out_rows; widths are normalised so
    /// every row.size() == headers.size().
    ///
    /// Public so unit tests / future inspector "open as read-only"
    /// flow can preview an .xlsx without going through full compile.
    /// V2 would gain an "auto-prepend BOM" knob if anyone ever needs to
    /// round-trip XLSX -> CSV; today we punt that to "designer exports
    /// CSV from Excel by hand".
    static bool ParseXlsx(const std::filesystem::path& xlsx_path,
                          eastl::vector<eastl::string>& out_headers,
                          eastl::vector<eastl::vector<eastl::string>>& out_rows,
                          eastl::string& out_error);

private:
    /// FNV-1a 64 -derived canonical GUID string. Mirrors
    /// DataTableImporter::DeriveStableGuid; only the input path differs
    /// (".xlsx" vs ".csv" suffix) so the two extensions never collide.
    static std::string DeriveStableGuid(const std::filesystem::path& project_relative_xlsx_path);
};
