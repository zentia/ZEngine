#pragma once

// ============================================================================
// LuaMemZasset - loads a MemoryProfiler `.zasset` into an FSnapshot
// ============================================================================
//
// The `.zasset` is the ORIGINAL snapshot; the three sibling CSVs
// (`_lua.csv` / `_csharp.csv` / `_userdata.csv`) are exports derived from it by
// LuaMemPreview::ExportMemoryProfilerToCSV (LuaMemCsvExport, `-batchmode`). Reading the asset directly
// therefore removes the export step instead of re-deriving it.
//
// This header is deliberately engine-free: only the implementation pulls in
// ZRuntime. Keeping the engine includes out of the header matters because the
// rest of the tool compiles against ZSlate, and the engine headers drag in
// windows.h whose `DrawText` macro collides with ISlateRenderer's neighbours.
//
// Linking requirement (see CMakeLists): ZRuntime. No engine boot, no window and
// no RHI are involved - just the Core systems, exactly like
// Runtime/Core/Serialize/Test/SchemaEvolutionSmokeTest.cpp.

#include <string>
#include <vector>

namespace LuaMemPreview
{
class FSnapshot;

// True when `path` is an existing directory. Exposed here so callers can pick
// the folder branch without pulling <filesystem> into a ZSlate TU.
bool IsDirectory(const std::string& path);

// True when `path` names a snapshot by extension (case-insensitive), i.e. it
// was meant as the single-file form even if the file does not exist (yet).
bool IsSnapshotPath(const std::string& path);

// Reads `path` as a MemoryProfiler asset and fills `out` with its Lua section.
// Returns false and sets out_error on failure.
bool LoadSnapshotFromZasset(const std::string& path, FSnapshot& out, std::string& out_error);

// Batch mode: reads `path` as a MemoryProfiler asset and writes
// <base>_csharp.csv / <base>_userdata.csv / <base>_lua.csv. `output_base`
// empty -> `path` without its extension. Unlike LoadSnapshotFromZasset, a
// snapshot with no Lua data is NOT an error here (C# / userdata still export).
bool ExportZassetToCsv(const std::string& path,
                       const std::string& output_base,
                       std::vector<std::string>& out_written,
                       std::string& out_error);

// Batch export of a whole folder: every `*.zasset` (extension match is
// case-insensitive) under `folder` becomes <output_dir>/<stem>_lua.csv etc.
// Always flat, even with recursive - snapshot stems are already unique, and
// keeping one directory means "open them all" is a single selection.
//
// One unreadable snapshot does not abort the run: its error goes to
// out_errors and the rest are still exported, which is the point of pointing
// this at a capture directory where some snapshot may be truncated.
// Returns false only when the folder cannot be read at all; partial success
// returns true with a non-empty out_errors, so the caller decides the severity.
bool ExportFolderToCsv(const std::string& folder,
                       const std::string& output_dir,
                       bool recursive,
                       std::vector<std::string>& out_written,
                       std::vector<std::string>& out_errors);

}  // namespace LuaMemPreview
