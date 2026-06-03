#include "CommonPCH/pch.h"
#include "Profiler/Export/ProfilerCsvExport.h"
#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Core/Memory/MemoryManager.h"
#include "Runtime/Profiler/Memory/MemoryProfiler.h"
#include "Runtime/RegisterRuntime.h"
#include "Runtime/Resource/Asset/AssetManager.h"

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace
{
    void PrintUsage(const char* exeName)
    {
        std::cout << "Usage: " << exeName << " <input_profiler.zasset> [output_base_or_csv]\n"
                  << "\n"
                  << "Examples:\n"
                  << "  " << exeName << " C:/tmp/profiler.zasset\n"
                  << "  " << exeName << " C:/tmp/profiler.zasset C:/tmp/profiler_export.csv\n"
                  << "\n"
                  << "Output files:\n"
                  << "  <base>_csharp.csv\n"
                  << "  <base>_userdata.csv\n"
                  << "  <base>_lua.csv\n";
    }

    std::filesystem::path MakeDefaultOutputBase(const std::filesystem::path& inputPath)
    {
        auto outputBase = inputPath;
        outputBase.replace_extension();
        return outputBase;
    }
}  // namespace

int main(int argc, char** argv)
{
    if (argc < 2 || argc > 3)
    {
        PrintUsage(argv[0]);
        return 1;
    }

    const std::filesystem::path inputPath = argv[1];
    if (!std::filesystem::exists(inputPath))
    {
        std::cerr << "Input file does not exist: " << inputPath.string() << '\n';
        return 1;
    }

    const std::filesystem::path outputBase = argc >= 3 ? std::filesystem::path(argv[2]) : MakeDefaultOutputBase(inputPath);

    MemoryProfiler* memoryProfiler = nullptr;
    bool systemsStarted = false;
    try
    {
        RegisterCore();
        RegisterPlatform();
        START_SYSTEM_WITHOUT_UI();
        systemsStarted = true;

        std::filesystem::path loadPath = inputPath;
        memoryProfiler = GET_SYSTEM(AssetManager)->ReadObject<MemoryProfiler>(loadPath);
        if (memoryProfiler == nullptr)
        {
            LOG_ERROR(ZEngine, "Failed to read MemoryProfiler object from: {}", inputPath.string());
            SHUTDOWN_SYSTEM();
            return 1;
        }

        std::vector<std::filesystem::path> writtenFiles;
        std::string errorMessage;
        if (!ProfilerCsvExport::ExportMemoryProfilerToCSV(*memoryProfiler, outputBase, &writtenFiles, &errorMessage))
        {
            std::cerr << errorMessage << '\n';
            MemoryManager::DestroyObject<MemoryProfiler>(memoryProfiler);
            SHUTDOWN_SYSTEM();
            return 1;
        }

        std::cout << "Exported CSV files:\n";
        for (const auto& path : writtenFiles)
        {
            std::cout << "  " << path.string() << '\n';
        }

        MemoryManager::DestroyObject<MemoryProfiler>(memoryProfiler);
        SHUTDOWN_SYSTEM();
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "Export failed: " << e.what() << '\n';
        if (memoryProfiler != nullptr)
        {
            MemoryManager::DestroyObject<MemoryProfiler>(memoryProfiler);
        }
        if (systemsStarted)
        {
            SHUTDOWN_SYSTEM();
        }
        return 1;
    }
}
