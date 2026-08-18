#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace zengine::envcheck
{
struct PeImageInfo
{
    std::string path;
    bool fileExists = false;
    bool validPeImage = false;
    bool is64Bit = false;
    bool hasDebugDirectory = false;
    bool usesDebugRuntime = false;
    std::uint32_t timeDateStamp = 0;
    std::vector<std::string> importedLibraries;
    std::vector<std::string> debugRuntimeLibraries;
    std::string error;
};

PeImageInfo InspectPeImage(const std::filesystem::path& path);
} // namespace zengine::envcheck
