#include "PeInspector.h"

#include <Windows.h>
#include <Zydis/Zydis.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <sstream>
#include <string_view>

namespace zengine::envcheck
{
namespace
{
std::string PathToUtf8(const std::filesystem::path& path)
{
    const std::wstring value = path.wstring();
    if (value.empty())
    {
        return {};
    }

    const int byteCount = WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (byteCount <= 0)
    {
        return path.string();
    }

    std::string result(static_cast<std::size_t>(byteCount), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), byteCount, nullptr, nullptr);
    return result;
}

std::string ToLower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character)
    {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

bool IsDebugRuntime(std::string libraryName)
{
    libraryName = ToLower(std::move(libraryName));
    if (libraryName == "ucrtbased.dll")
    {
        return true;
    }

    constexpr const char* debugPrefixes[] = {
        "msvcp", "vcruntime", "concrt", "mfc", "mfcm", "atl"
    };
    if (!libraryName.ends_with("d.dll"))
    {
        return false;
    }
    return std::any_of(std::begin(debugPrefixes), std::end(debugPrefixes), [&libraryName](const char* prefix)
    {
        return libraryName.starts_with(prefix);
    });
}

bool IsRangeValid(std::size_t offset, std::size_t length, std::size_t totalSize)
{
    return offset <= totalSize && length <= totalSize - offset;
}

std::size_t RvaToFileOffset(
    DWORD rva,
    const IMAGE_SECTION_HEADER* sections,
    WORD sectionCount,
    std::size_t fileSize)
{
    for (WORD index = 0; index < sectionCount; ++index)
    {
        const auto& section = sections[index];
        const DWORD sectionSize = std::max(section.Misc.VirtualSize, section.SizeOfRawData);
        if (rva >= section.VirtualAddress && rva - section.VirtualAddress < sectionSize)
        {
            const std::uint64_t offset = static_cast<std::uint64_t>(section.PointerToRawData) +
                (rva - section.VirtualAddress);
            return offset < fileSize ? static_cast<std::size_t>(offset) : std::numeric_limits<std::size_t>::max();
        }
    }
    return rva < fileSize ? static_cast<std::size_t>(rva) : std::numeric_limits<std::size_t>::max();
}

std::string ReadNullTerminatedAscii(const std::vector<unsigned char>& bytes, std::size_t offset)
{
    if (offset >= bytes.size())
    {
        return {};
    }
    const auto begin = bytes.begin() + static_cast<std::ptrdiff_t>(offset);
    const auto end = std::find(begin, bytes.end(), static_cast<unsigned char>(0));
    return std::string(begin, end);
}

std::string InstructionBytes(const unsigned char* bytes, std::size_t length)
{
    std::ostringstream stream;
    stream << std::hex << std::uppercase << std::setfill('0');
    for (std::size_t index = 0; index < length; ++index)
    {
        if (index != 0)
        {
            stream << ' ';
        }
        stream << std::setw(2) << static_cast<unsigned int>(bytes[index]);
    }
    return stream.str();
}

enum class InstructionCategory
{
    None,
    Avx,
    Avx2,
    Fma,
    F16c,
    Avx512
};

InstructionCategory ClassifyInstructionSet(const char* isaSetName)
{
    if (isaSetName == nullptr)
    {
        return InstructionCategory::None;
    }

    const std::string_view name(isaSetName);
    if (name.starts_with("AVX512"))
    {
        return InstructionCategory::Avx512;
    }
    if (name == "AVX2" || name == "AVX2GATHER")
    {
        return InstructionCategory::Avx2;
    }
    if (name == "FMA" || name == "FMA4")
    {
        return InstructionCategory::Fma;
    }
    if (name == "F16C")
    {
        return InstructionCategory::F16c;
    }
    if (name == "AVX" || name.starts_with("AVX_") || name == "AVXAES")
    {
        return InstructionCategory::Avx;
    }
    return InstructionCategory::None;
}

const char* CategoryName(InstructionCategory category)
{
    switch (category)
    {
    case InstructionCategory::Avx: return "AVX";
    case InstructionCategory::Avx2: return "AVX2";
    case InstructionCategory::Fma: return "FMA";
    case InstructionCategory::F16c: return "F16C";
    case InstructionCategory::Avx512: return "AVX-512";
    case InstructionCategory::None: break;
    }
    return "NONE";
}

void RecordInstruction(
    InstructionSetSummary& summary,
    InstructionCategory category,
    const char* isaSetName,
    const ZydisFormatter& formatter,
    const ZydisDecodedInstruction& instruction,
    const ZydisDecodedOperand* operands,
    const unsigned char* bytes,
    std::uint32_t rva,
    std::uint64_t fileOffset)
{
    std::uint64_t* count = nullptr;
    switch (category)
    {
    case InstructionCategory::Avx:
        count = &summary.avxInstructionCount;
        summary.containsAvx = true;
        break;
    case InstructionCategory::Avx2:
        count = &summary.avx2InstructionCount;
        summary.containsAvx = true;
        summary.containsAvx2 = true;
        break;
    case InstructionCategory::Fma:
        count = &summary.fmaInstructionCount;
        summary.containsAvx = true;
        summary.containsFma = true;
        break;
    case InstructionCategory::F16c:
        count = &summary.f16cInstructionCount;
        summary.containsAvx = true;
        summary.containsF16c = true;
        break;
    case InstructionCategory::Avx512:
        count = &summary.avx512InstructionCount;
        summary.containsAvx = true;
        summary.containsAvx512 = true;
        break;
    case InstructionCategory::None:
        return;
    }
    ++*count;

    constexpr std::size_t maximumSamplesPerCategory = 8;
    const char* categoryName = CategoryName(category);
    const auto existingSamples = std::count_if(
        summary.samples.begin(), summary.samples.end(), [categoryName](const InstructionSample& sample)
        {
            return sample.category == categoryName;
        });
    if (existingSamples >= maximumSamplesPerCategory)
    {
        return;
    }

    std::array<char, 256> text{};
    if (ZYAN_FAILED(ZydisFormatterFormatInstruction(
            &formatter,
            &instruction,
            operands,
            instruction.operand_count_visible,
            text.data(),
            text.size(),
            rva,
            ZYAN_NULL)))
    {
        const char* mnemonic = ZydisMnemonicGetString(instruction.mnemonic);
        if (mnemonic != nullptr)
        {
            strcpy_s(text.data(), text.size(), mnemonic);
        }
    }

    InstructionSample sample;
    sample.category = categoryName;
    sample.isaSet = isaSetName == nullptr ? "UNKNOWN" : isaSetName;
    sample.text = text.data();
    sample.bytes = InstructionBytes(bytes, instruction.length);
    sample.rva = rva;
    sample.fileOffset = fileOffset;
    summary.samples.push_back(std::move(sample));
}

struct CodeRange
{
    std::uint32_t rva = 0;
    std::size_t fileOffset = 0;
    std::size_t size = 0;
};

std::vector<CodeRange> CollectRuntimeFunctionRanges(
    const std::vector<unsigned char>& bytes,
    const IMAGE_OPTIONAL_HEADER64& optionalHeader,
    const std::vector<IMAGE_SECTION_HEADER>& sections,
    InstructionSetSummary& summary)
{
    std::vector<CodeRange> ranges;
    if (optionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXCEPTION)
    {
        summary.error = "PE image has no exception directory; instruction scan was skipped.";
        return ranges;
    }

    const auto& directory = optionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (directory.VirtualAddress == 0 || directory.Size < sizeof(RUNTIME_FUNCTION))
    {
        summary.error = "PE image has no x64 runtime function ranges; instruction scan was skipped.";
        return ranges;
    }

    const std::size_t directoryOffset =
        RvaToFileOffset(directory.VirtualAddress, sections.data(),
            static_cast<WORD>(sections.size()), bytes.size());
    if (directoryOffset == std::numeric_limits<std::size_t>::max() ||
        !IsRangeValid(directoryOffset, directory.Size, bytes.size()))
    {
        summary.error = "PE exception directory is outside the file; instruction scan was skipped.";
        return ranges;
    }

    const std::size_t entryCount = directory.Size / sizeof(RUNTIME_FUNCTION);
    ranges.reserve(entryCount);
    for (std::size_t index = 0; index < entryCount; ++index)
    {
        RUNTIME_FUNCTION function{};
        std::memcpy(
            &function,
            bytes.data() + directoryOffset + index * sizeof(RUNTIME_FUNCTION),
            sizeof(function));
        if (function.BeginAddress >= function.EndAddress)
        {
            continue;
        }

        const std::size_t fileOffset = RvaToFileOffset(
            function.BeginAddress, sections.data(), static_cast<WORD>(sections.size()), bytes.size());
        if (fileOffset == std::numeric_limits<std::size_t>::max())
        {
            continue;
        }

        const std::uint64_t length =
            static_cast<std::uint64_t>(function.EndAddress) - function.BeginAddress;
        if (length == 0 || length > bytes.size() - fileOffset)
        {
            continue;
        }
        ranges.push_back({function.BeginAddress, fileOffset, static_cast<std::size_t>(length)});
    }

    std::sort(ranges.begin(), ranges.end(), [](const CodeRange& left, const CodeRange& right)
    {
        if (left.rva != right.rva)
        {
            return left.rva < right.rva;
        }
        return left.size < right.size;
    });
    ranges.erase(std::unique(ranges.begin(), ranges.end(), [](const CodeRange& left, const CodeRange& right)
    {
        return left.rva == right.rva && left.size == right.size;
    }), ranges.end());
    return ranges;
}

void ScanRuntimeFunctions(
    const std::vector<unsigned char>& bytes,
    const std::vector<IMAGE_SECTION_HEADER>& sections,
    const std::vector<CodeRange>& ranges,
    PeImageInfo& result)
{
    auto& summary = result.instructionSets;
    summary.scanAttempted = true;
    summary.scanStrategy = "PE_EXCEPTION_DIRECTORY_RUNTIME_FUNCTIONS";

    for (const auto& section : sections)
    {
        if ((section.Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0 || section.SizeOfRawData == 0)
        {
            continue;
        }
        ++summary.executableSectionCount;
        const std::size_t fileOffset = section.PointerToRawData;
        if (fileOffset < bytes.size())
        {
            summary.executableBytes += std::min<std::size_t>(
                section.SizeOfRawData, bytes.size() - fileOffset);
        }
    }

    summary.runtimeFunctionCount = ranges.size();
    if (ranges.empty())
    {
        summary.scanCompleted = true;
        return;
    }

    ZydisDecoder decoder{};
    ZydisFormatter formatter{};
    if (ZYAN_FAILED(ZydisDecoderInit(
            &decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64)) ||
        ZYAN_FAILED(ZydisFormatterInit(&formatter, ZYDIS_FORMATTER_STYLE_INTEL)))
    {
        summary.error = "Failed to initialize the Zydis decoder or formatter.";
        return;
    }

    for (const auto& range : ranges)
    {
        summary.scannedCodeBytes += range.size;
        std::size_t offset = 0;
        while (offset < range.size)
        {
            ZydisDecodedInstruction instruction{};
            ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT]{};
            const ZyanStatus status = ZydisDecoderDecodeFull(
                &decoder,
                bytes.data() + range.fileOffset + offset,
                range.size - offset,
                &instruction,
                operands);
            if (ZYAN_FAILED(status) || instruction.length == 0)
            {
                ++summary.undecodableByteCount;
                ++offset;
                continue;
            }

            ++summary.decodedInstructionCount;
            const char* isaSetName = ZydisISASetGetString(instruction.meta.isa_set);
            const InstructionCategory category = ClassifyInstructionSet(isaSetName);
            const std::uint64_t instructionRva = static_cast<std::uint64_t>(range.rva) + offset;
            if (instructionRva <= std::numeric_limits<std::uint32_t>::max())
            {
                RecordInstruction(
                    summary,
                    category,
                    isaSetName,
                    formatter,
                    instruction,
                    operands,
                    bytes.data() + range.fileOffset + offset,
                    static_cast<std::uint32_t>(instructionRva),
                    range.fileOffset + offset);
            }
            offset += instruction.length;
        }
    }

    summary.scanCompleted = true;
}

template <typename OptionalHeader>
bool ParseOptionalHeader(
    const std::vector<unsigned char>& bytes,
    const OptionalHeader& optionalHeader,
    const IMAGE_SECTION_HEADER* sections,
    WORD sectionCount,
    PeImageInfo& result)
{
    if (optionalHeader.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_DEBUG)
    {
        const auto& debugDirectory = optionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
        result.hasDebugDirectory = debugDirectory.VirtualAddress != 0 && debugDirectory.Size != 0;
    }

    if (optionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_IMPORT)
    {
        return true;
    }

    const auto& importDirectory = optionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (importDirectory.VirtualAddress == 0 || importDirectory.Size == 0)
    {
        return true;
    }

    const std::size_t importOffset =
        RvaToFileOffset(importDirectory.VirtualAddress, sections, sectionCount, bytes.size());
    if (importOffset == std::numeric_limits<std::size_t>::max())
    {
        result.error = "Import directory RVA is outside the file.";
        return false;
    }

    const std::size_t maximumDescriptors = importDirectory.Size / sizeof(IMAGE_IMPORT_DESCRIPTOR) + 1;
    for (std::size_t index = 0; index < maximumDescriptors; ++index)
    {
        const std::size_t descriptorOffset = importOffset + index * sizeof(IMAGE_IMPORT_DESCRIPTOR);
        if (!IsRangeValid(descriptorOffset, sizeof(IMAGE_IMPORT_DESCRIPTOR), bytes.size()))
        {
            result.error = "Import descriptor is truncated.";
            return false;
        }

        IMAGE_IMPORT_DESCRIPTOR descriptor{};
        std::memcpy(&descriptor, bytes.data() + descriptorOffset, sizeof(descriptor));
        if (descriptor.Name == 0 && descriptor.FirstThunk == 0 && descriptor.OriginalFirstThunk == 0)
        {
            break;
        }

        const std::size_t nameOffset = RvaToFileOffset(descriptor.Name, sections, sectionCount, bytes.size());
        if (nameOffset == std::numeric_limits<std::size_t>::max())
        {
            result.error = "Imported library name RVA is outside the file.";
            return false;
        }

        const std::string library = ReadNullTerminatedAscii(bytes, nameOffset);
        if (library.empty())
        {
            result.error = "Imported library name is empty or truncated.";
            return false;
        }
        result.importedLibraries.push_back(library);
        if (IsDebugRuntime(library))
        {
            result.debugRuntimeLibraries.push_back(library);
        }
    }

    result.usesDebugRuntime = !result.debugRuntimeLibraries.empty();
    return true;
}
} // namespace

PeImageInfo InspectPeImage(const std::filesystem::path& path)
{
    PeImageInfo result;
    result.path = PathToUtf8(path);
    result.fileExists = std::filesystem::is_regular_file(path);
    if (!result.fileExists)
    {
        result.error = "File does not exist.";
        return result;
    }

    std::ifstream input(path, std::ios::binary);
    std::vector<unsigned char> bytes(
        (std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (input.bad() || bytes.size() < sizeof(IMAGE_DOS_HEADER))
    {
        result.error = "Failed to read the file or file is too small.";
        return result;
    }

    IMAGE_DOS_HEADER dosHeader{};
    std::memcpy(&dosHeader, bytes.data(), sizeof(dosHeader));
    if (dosHeader.e_magic != IMAGE_DOS_SIGNATURE || dosHeader.e_lfanew < 0)
    {
        result.error = "Invalid DOS header.";
        return result;
    }

    const std::size_t ntOffset = static_cast<std::size_t>(dosHeader.e_lfanew);
    const std::size_t fixedHeaderSize = sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER);
    if (!IsRangeValid(ntOffset, fixedHeaderSize, bytes.size()))
    {
        result.error = "PE header is truncated.";
        return result;
    }

    DWORD signature = 0;
    std::memcpy(&signature, bytes.data() + ntOffset, sizeof(signature));
    if (signature != IMAGE_NT_SIGNATURE)
    {
        result.error = "Invalid PE signature.";
        return result;
    }

    IMAGE_FILE_HEADER fileHeader{};
    std::memcpy(&fileHeader, bytes.data() + ntOffset + sizeof(DWORD), sizeof(fileHeader));
    result.timeDateStamp = fileHeader.TimeDateStamp;
    result.is64Bit = fileHeader.Machine == IMAGE_FILE_MACHINE_AMD64 ||
        fileHeader.Machine == IMAGE_FILE_MACHINE_ARM64;

    const std::size_t optionalOffset = ntOffset + fixedHeaderSize;
    if (!IsRangeValid(optionalOffset, fileHeader.SizeOfOptionalHeader, bytes.size()) ||
        fileHeader.SizeOfOptionalHeader < sizeof(WORD))
    {
        result.error = "Optional header is truncated.";
        return result;
    }

    WORD magic = 0;
    std::memcpy(&magic, bytes.data() + optionalOffset, sizeof(magic));
    const std::size_t sectionsOffset = optionalOffset + fileHeader.SizeOfOptionalHeader;
    const std::size_t sectionsSize =
        static_cast<std::size_t>(fileHeader.NumberOfSections) * sizeof(IMAGE_SECTION_HEADER);
    if (!IsRangeValid(sectionsOffset, sectionsSize, bytes.size()))
    {
        result.error = "Section table is truncated.";
        return result;
    }
    std::vector<IMAGE_SECTION_HEADER> sections(fileHeader.NumberOfSections);
    if (!sections.empty())
    {
        std::memcpy(sections.data(), bytes.data() + sectionsOffset, sectionsSize);
    }

    bool parsed = false;
    if (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC &&
        fileHeader.SizeOfOptionalHeader >= sizeof(IMAGE_OPTIONAL_HEADER64))
    {
        IMAGE_OPTIONAL_HEADER64 optionalHeader{};
        std::memcpy(&optionalHeader, bytes.data() + optionalOffset, sizeof(optionalHeader));
        parsed = ParseOptionalHeader(bytes, optionalHeader, sections.data(), fileHeader.NumberOfSections, result);
        if (parsed && fileHeader.Machine == IMAGE_FILE_MACHINE_AMD64)
        {
            const auto ranges = CollectRuntimeFunctionRanges(
                bytes, optionalHeader, sections, result.instructionSets);
            ScanRuntimeFunctions(bytes, sections, ranges, result);
        }
    }
    else if (magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC &&
        fileHeader.SizeOfOptionalHeader >= sizeof(IMAGE_OPTIONAL_HEADER32))
    {
        IMAGE_OPTIONAL_HEADER32 optionalHeader{};
        std::memcpy(&optionalHeader, bytes.data() + optionalOffset, sizeof(optionalHeader));
        parsed = ParseOptionalHeader(bytes, optionalHeader, sections.data(), fileHeader.NumberOfSections, result);
    }
    else
    {
        result.error = "Unsupported or truncated PE optional header.";
        return result;
    }

    result.validPeImage = parsed;
    if (parsed && fileHeader.Machine != IMAGE_FILE_MACHINE_AMD64)
    {
        result.instructionSets.error = "Instruction scanning currently supports x64 PE images only.";
    }
    return result;
}
} // namespace zengine::envcheck
