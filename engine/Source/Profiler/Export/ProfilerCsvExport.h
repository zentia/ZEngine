#pragma once

#include "Runtime/Core/Memory/LlmTracker.h"

#include <filesystem>
#include <string>
#include <vector>

class MemoryProfiler;

namespace ProfilerCsvExport
{
    bool ExportLlmStatsToCSV(const std::vector<LLMTagStats>& stats,
                             const std::filesystem::path& outputPath,
                             std::string* errorMessage = nullptr);

    bool ExportMemoryProfilerToCSV(MemoryProfiler& memoryProfiler,
                                   const std::filesystem::path& outputBasePath,
                                   std::vector<std::filesystem::path>* writtenFiles = nullptr,
                                   std::string* errorMessage = nullptr);
}  // namespace ProfilerCsvExport
