#pragma once

// =============================================================================
// ZEngineToolApi -- the C ABI ZEngine's standalone tools link against.
// =============================================================================
// ZSlate / ZRuntime are C++ and Windows-tooling-hostile to link directly: an
// /MT tool cannot import a /MD class without dragging the whole engine header
// graph (EASTL, Serialize, Type*) into its own translation units. This header
// is the narrow seam instead. Everything crosses it as `const char*` / `int`,
// so no allocator, exception, std::string or Object* ever crosses a module
// boundary -- which is what lets an /MT executable consume a ZRuntimeShared.dll
// built with a different CRT choice.
//
// Who uses it
// -----------
// ZEnvironmentCollector (engine/tools/WindowsEnvironmentCheck). It needs two
// things the engine already owns:
//   * logging (bqlog through LogSystem -- the tool is a WINDOWS-subsystem
//     binary, so stderr goes nowhere when it is double-clicked);
//   * localised UI strings, as a real DataTable rather than string literals
//     compiled into a .cpp (see Runtime/Resource/Asset/Data/LocalizationTable.h).
//
// String ownership
// ----------------
// Every `const char*` returned here points at storage owned by the DLL and
// valid until the matching Shutdown/Unload call. Callers copy; they never free.
//
// Threading
// ---------
// Init / table loads are guarded by a mutex and idempotent. The lookup
// (ZRuntime_Localize) is deliberately lock-free: it only reads a table that is
// fully built before it is published, and it runs on the UI thread.
// =============================================================================

#include "Runtime/ExportRuntime.h"

// -----------------------------------------------------------------------------
// Lifetime
// -----------------------------------------------------------------------------

/// Start the smallest slice of the engine a tool needs:
///   LogSystem (bqlog) + TypeManager/reflection + FileSystem + ObjectManager +
///   AssetManager.
/// Deliberately does NOT register Application, WindowSystem, any RHI,
/// ScriptingManager or ProjectInfo -- a tool has no window, no project and no
/// business creating a GPU device on a machine it was sent to diagnose.
///
/// Returns 1 on success (or when already initialised), 0 on failure. Safe to
/// call repeatedly.
EXPORT_RUNTIME int ZRuntime_ToolInit();

/// Tear the slice down: drains bqlog, releases loaded tables, shuts the systems
/// down. Safe to call without a successful Init.
EXPORT_RUNTIME void ZRuntime_ToolShutdown();

// -----------------------------------------------------------------------------
// Logging -- all records go to the engine's per-executable bqlog file
// (%USERPROFILE%\AppData\LocalLow\ZEngine\Logs\<ExeName>_*.log) under the
// ZEnvironmentCheck category, i.e. the same file support already asks for.
// -----------------------------------------------------------------------------

EXPORT_RUNTIME void ZRuntime_LogInfo(const char* message);
EXPORT_RUNTIME void ZRuntime_LogWarning(const char* message);
EXPORT_RUNTIME void ZRuntime_LogError(const char* message);

/// bqlog is asynchronous: a short-lived -batchmode run has to drain explicitly
/// or the tail is lost.
EXPORT_RUNTIME void ZRuntime_LogFlush();

/// Directory the log file is written to. Empty until ZRuntime_ToolInit().
/// Meant for a "logs are here" line in the tool's report.
EXPORT_RUNTIME const char* ZRuntime_LogFilePath();

// -----------------------------------------------------------------------------
// Localization tables (DataTable-backed)
// -----------------------------------------------------------------------------

/// Load one language's table from a cooked `.zasset` (an absolute UTF-8 path, or
/// one relative to the engine asset root -- the tool always passes absolute).
/// Replaces any previously loaded table. Returns 1 on success, 0 on failure
/// (missing file, wrong asset type, engine not initialised).
EXPORT_RUNTIME int ZRuntime_LoadLocalizationTable(const char* zasset_path_utf8);

/// Resolve `id` in the loaded table. Returns nullptr when no table is loaded or
/// the id is absent -- the caller then falls back to its own English literal,
/// which is exactly ZSlate's localization contract.
EXPORT_RUNTIME const char* ZRuntime_Localize(const char* id);

/// Number of rows in the loaded table (0 when none).
///
/// Together with ZRuntime_LocalizationRowAt() this is how a host whose UI
/// framework wants a whole table up front (ZSlate's RegisterStrings() takes an
/// array, not a per-string callback) walks the cooked .zasset without knowing
/// the ids in advance -- the id list is data, not something the host keeps a
/// second copy of.
EXPORT_RUNTIME unsigned ZRuntime_LocalizationRowCount();

/// Read row `index` of the loaded table. Writes both out-params and returns 1,
/// or returns 0 when `index` is out of range / no table is loaded. Both
/// returned pointers are owned by the DLL and stay valid until the table is
/// unloaded.
EXPORT_RUNTIME int ZRuntime_LocalizationRowAt(unsigned index, const char** out_id, const char** out_text);

/// Drop the loaded table. After this ZRuntime_Localize() returns nullptr until
/// the next load.
EXPORT_RUNTIME void ZRuntime_UnloadLocalizationTable();

// -----------------------------------------------------------------------------
// Cook step -- build-time only.
//
// `ZRuntime_CompileLocalizationTable` parses `csv_path_utf8`
// (id,text -- RFC 4180 subset: comma separator, double-quote escape, first row
// is the header, UTF-8 with or without BOM) and bakes it into
// `zasset_path_utf8` through AssetManager::WriteObjectToDiskThreadSafe, the
// same .zasset writer prefabs use.
//
// This is what DataTableCook (engine/tools/DataTableCook) drives at build time,
// so a shipped tool never parses CSV at run time. It lives in Runtime rather
// than in the Editor's CsvSchemaRegistry because ZRuntime must not depend on
// ZEditor and the cook has to run in a build tree with no project open.
//
// Returns 1 on success, 0 on failure (unreadable CSV, no rows, unwritable
// output directory).
// -----------------------------------------------------------------------------
EXPORT_RUNTIME int ZRuntime_CompileLocalizationTable(const char* csv_path_utf8,
                                                     const char* zasset_path_utf8);
