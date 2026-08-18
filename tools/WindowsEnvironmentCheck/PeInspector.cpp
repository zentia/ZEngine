#include "PeInspector.h"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>

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
    return result;
}
} // namespace zengine::envcheck
