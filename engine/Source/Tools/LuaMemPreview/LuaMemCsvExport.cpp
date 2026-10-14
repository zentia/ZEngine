#include "LuaMemCsvExport.h"

// Compiled with pch.h force-included (see CMakeLists), like LuaMemZasset.cpp.
#include "Runtime/Profiler/Memory/Lua/LuaMemoryProfiler.h"
#include "Runtime/Profiler/Memory/MemoryProfiler.h"

#include <fstream>
#include <string_view>
#include <system_error>

namespace LuaMemPreview
{
namespace
{
std::string EscapeCSVField(std::string_view value)
{
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
        return std::string(value);

    std::string out;
    out.reserve(value.size() + 4);
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

std::string EscapeCSVField(const eastl::string& value)
{
    return EscapeCSVField(std::string_view(value.c_str(), value.size()));
}

void SetError(std::string* errorMessage, const std::string& message)
{
    if (errorMessage != nullptr)
        *errorMessage = message;
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

bool ExportMemoryProfilerToCSV(MemoryProfiler& memoryProfiler,
                               const std::filesystem::path& outputBasePath,
                               std::vector<std::filesystem::path>* writtenFiles,
                               std::string* errorMessage)
{
    std::filesystem::path basePath = outputBasePath;
    if (basePath.has_extension())
        basePath.replace_extension();

    const std::filesystem::path csharpPath = basePath.string() + "_csharp.csv";
    const std::filesystem::path userdataPath = basePath.string() + "_userdata.csv";
    const std::filesystem::path luaPath = basePath.string() + "_lua.csv";

    // All three share one parent directory.
    if (!EnsureParentDirectory(luaPath, errorMessage))
        return false;

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
            csharpFile << info.count << ',' << EscapeCSVField(info.name) << ',' << EscapeCSVField(info.stack) << '\n';
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
            userdataFile << info.count << ',' << EscapeCSVField(info.stack) << '\n';
    }

    {
        std::ofstream luaFile(luaPath);
        if (!luaFile.is_open())
        {
            SetError(errorMessage, "Failed to open CSV for writing: " + luaPath.string());
            return false;
        }
        // Data rows only - no summary row. After a GameCore sync pass the
        // sum of every row already equals Lua's own accounting
        // (collectgarbage("count") * 1024), so any consumer can derive the
        // total itself. Emitting a TOTAL row here would make every such
        // consumer double count it.
        luaFile << "Count,Size,Type,Stack\n";
        for (const auto& info : luaInfos)
            luaFile << info.count << ',' << info.size << ',' << LuaMemoryTypeName(info.type) << ',' << EscapeCSVField(info.stack) << '\n';
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

}  // namespace LuaMemPreview
