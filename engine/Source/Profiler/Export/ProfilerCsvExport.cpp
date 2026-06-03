#include "Profiler/Export/ProfilerCsvExport.h"

#include "CommonPCH/pch.h"
#include "Runtime/Profiler/Memory/MemoryProfiler.h"

#include <fstream>

namespace
{
    std::string EscapeCSVField(const eastl::string& value)
    {
        std::string out;
        out.reserve(value.size() + 4);

        bool needQuotes = false;
        for (char c : value)
        {
            if (c == '"' || c == ',' || c == '\n' || c == '\r')
            {
                needQuotes = true;
                break;
            }
        }

        if (!needQuotes)
        {
            return value.c_str();
        }

        out += '"';
        for (char c : value)
        {
            if (c == '"')
                out += "\"\"";
            else
                out += c;
        }
        out += '"';
        return out;
    }

    std::string EscapeCSVField(const std::string& value)
    {
        std::string out;
        out.reserve(value.size() + 4);

        bool needQuotes = false;
        for (char c : value)
        {
            if (c == '"' || c == ',' || c == '\n' || c == '\r')
            {
                needQuotes = true;
                break;
            }
        }

        if (!needQuotes)
        {
            return value;
        }

        out += '"';
        for (char c : value)
        {
            if (c == '"')
                out += "\"\"";
            else
                out += c;
        }
        out += '"';
        return out;
    }

    float BytesToMB(size_t bytes)
    {
        return static_cast<float>(bytes) / (1024.0f * 1024.0f);
    }

    float BytesToKB(size_t bytes)
    {
        return static_cast<float>(bytes) / 1024.0f;
    }

    void SetError(std::string* errorMessage, const std::string& message)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = message;
        }
    }

    bool EnsureParentDirectory(const std::filesystem::path& path, std::string* errorMessage)
    {
        const auto parent = path.parent_path();
        if (parent.empty())
            return true;

        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
        if (ec)
        {
            SetError(errorMessage, "Failed to create output directory: " + parent.string() + ", " + ec.message());
            return false;
        }
        return true;
    }
}  // namespace

namespace ProfilerCsvExport
{
    bool ExportLlmStatsToCSV(const std::vector<LLMTagStats>& stats,
                             const std::filesystem::path& outputPath,
                             std::string* errorMessage)
    {
        if (!EnsureParentDirectory(outputPath, errorMessage))
            return false;

        std::ofstream file(outputPath);
        if (!file.is_open())
        {
            SetError(errorMessage, "Failed to open CSV for writing: " + outputPath.string());
            return false;
        }

        file << "Tag Name,Current (MB),Peak (MB),Total Allocated (MB),Total Freed (MB),"
             << "Allocations,Deallocations,Avg Size (KB)\n";

        for (const auto& stat : stats)
        {
            const float avgSizeKB = stat.allocation_count > 0 ? BytesToKB(stat.total_allocated) / stat.allocation_count : 0.0f;
            file << EscapeCSVField(stat.tag_name) << ',' << BytesToMB(stat.current_bytes) << ',' << BytesToMB(stat.peak_bytes)
                 << ',' << BytesToMB(stat.total_allocated) << ',' << BytesToMB(stat.total_freed) << ','
                 << stat.allocation_count << ',' << stat.deallocation_count << ',' << avgSizeKB << '\n';
        }

        return true;
    }

    bool ExportMemoryProfilerToCSV(MemoryProfiler& memoryProfiler,
                                   const std::filesystem::path& outputBasePath,
                                   std::vector<std::filesystem::path>* writtenFiles,
                                   std::string* errorMessage)
    {
        std::filesystem::path basePath = outputBasePath;
        if (basePath.has_extension())
        {
            basePath.replace_extension();
        }

        const std::filesystem::path csharpPath = basePath.string() + "_csharp.csv";
        const std::filesystem::path userdataPath = basePath.string() + "_userdata.csv";
        const std::filesystem::path luaPath = basePath.string() + "_lua.csv";

        if (!EnsureParentDirectory(csharpPath, errorMessage) || !EnsureParentDirectory(userdataPath, errorMessage) ||
            !EnsureParentDirectory(luaPath, errorMessage))
        {
            return false;
        }

        auto& csharpProfiler = memoryProfiler.GetCSharpMemoryProfiler();
        auto& luaProfiler = memoryProfiler.GetLuaMemoryProfiler();
        const auto& csharpInfos = csharpProfiler.GetCSharpInfos();
        const auto& userdataInfos = csharpProfiler.GetUserdataInfos();
        const auto& luaInfos = luaProfiler.GetLuaInfos();

        {
            std::ofstream csharpFile(csharpPath);
            if (!csharpFile.is_open())
            {
                SetError(errorMessage, "Failed to open CSV for writing: " + csharpPath.string());
                return false;
            }
            csharpFile << "Count,Name,Stack\n";
            for (const auto& info : csharpInfos)
            {
                csharpFile << info.count << ',' << EscapeCSVField(info.name) << ',' << EscapeCSVField(info.stack) << '\n';
            }
        }

        {
            std::ofstream userdataFile(userdataPath);
            if (!userdataFile.is_open())
            {
                SetError(errorMessage, "Failed to open CSV for writing: " + userdataPath.string());
                return false;
            }
            userdataFile << "Count,Stack\n";
            for (const auto& info : userdataInfos)
            {
                userdataFile << info.count << ',' << EscapeCSVField(info.stack) << '\n';
            }
        }

        {
            std::ofstream luaFile(luaPath);
            if (!luaFile.is_open())
            {
                SetError(errorMessage, "Failed to open CSV for writing: " + luaPath.string());
                return false;
            }
            luaFile << "Count,Size,Stack\n";
            for (const auto& info : luaInfos)
            {
                luaFile << info.count << ',' << info.size << ',' << EscapeCSVField(info.stack) << '\n';
            }
        }

        if (writtenFiles != nullptr)
        {
            writtenFiles->clear();
            writtenFiles->push_back(csharpPath);
            writtenFiles->push_back(userdataPath);
            writtenFiles->push_back(luaPath);
        }

        return true;
    }
}  // namespace ProfilerCsvExport
