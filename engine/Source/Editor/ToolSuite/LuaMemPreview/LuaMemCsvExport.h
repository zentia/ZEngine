#pragma once

// ============================================================================
// LuaMemCsvExport - MemoryProfiler -> CSV writer (batch mode backend)
// ============================================================================
//
// Formerly Source/Profiler/Export/ProfilerCsvExport (the ZProfilerCsvExporter
// tool), merged into LuaMemPreview so a single executable both views and
// exports a MemoryProfiler `.zasset`. Writes three sibling files:
//   <base>_csharp.csv    Count,Name,Stack
//   <base>_userdata.csv  Count,Stack
//   <base>_lua.csv       Count,Size,Type,Stack   (what FSnapshot::LoadFromCsv reads)
//
// Engine-free header (MemoryProfiler is only forward-declared) for the same
// reason as LuaMemZasset.h; only the .cpp is compiled with pch.h.

#include <filesystem>
#include <string>
#include <vector>

class MemoryProfiler;

namespace LuaMemPreview
{
// `outputBasePath` may carry an extension (e.g. "foo.csv"); it is stripped
// before the "_csharp/_userdata/_lua.csv" suffixes are appended.
bool ExportMemoryProfilerToCSV(MemoryProfiler& memoryProfiler,
                               const std::filesystem::path& outputBasePath,
                               std::vector<std::filesystem::path>* writtenFiles = nullptr,
                               std::string* errorMessage = nullptr);

}  // namespace LuaMemPreview
