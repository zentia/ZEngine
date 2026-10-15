#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace zengine::envcheck
{
struct InstructionSample
{
    std::string category;
    std::string isaSet;
    std::string text;
    std::string bytes;
    std::uint32_t rva = 0;
    std::uint64_t fileOffset = 0;
};

struct InstructionSetSummary
{
    bool scanAttempted = false;
    bool scanCompleted = false;
    std::uint64_t executableSectionCount = 0;
    std::uint64_t executableBytes = 0;
    std::uint64_t runtimeFunctionCount = 0;
    std::uint64_t scannedCodeBytes = 0;
    std::uint64_t decodedInstructionCount = 0;
    std::uint64_t undecodableByteCount = 0;
    std::uint64_t avxInstructionCount = 0;
    std::uint64_t avx2InstructionCount = 0;
    std::uint64_t fmaInstructionCount = 0;
    std::uint64_t f16cInstructionCount = 0;
    std::uint64_t avx512InstructionCount = 0;
    bool containsAvx = false;
    bool containsAvx2 = false;
    bool containsFma = false;
    bool containsF16c = false;
    bool containsAvx512 = false;
    std::vector<InstructionSample> samples;
    std::string scanStrategy;
    std::string error;
};

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
    InstructionSetSummary instructionSets;
    std::string error;
};

PeImageInfo InspectPeImage(const std::filesystem::path& path);
} // namespace zengine::envcheck
