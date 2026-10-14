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

}  // namespace LuaMemPreview
