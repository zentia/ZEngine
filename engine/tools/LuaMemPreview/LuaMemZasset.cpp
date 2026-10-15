#include "LuaMemZasset.h"

#include "LuaMemCsv.h"
#include "LuaMemCsvExport.h"

// Standard headers FIRST: the engine headers below are written against
// CommonPCH/pch.h, which we deliberately do not use here (it drags in
// windows.h, whose DrawText macro breaks ZSlate in the other TUs). Several of
// them - e.g. VirtualFileSystemTypes.h with its std::filesystem::path member -
// therefore rely on these already being visible.
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <algorithm>
#include <system_error>
#include <vector>

// Engine side. Confined to this TU on purpose - see LuaMemZasset.h.
#include "RegisterRuntime.h"
#include "Runtime/BaseClasses/Object.h"
#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Core/Memory/MemoryManager.h"
#include "Runtime/Core/Serialize/SerializedFile.h"
#include "Runtime/Profiler/Memory/Lua/LuaMemoryProfiler.h"
#include "Runtime/Profiler/Memory/MemoryProfiler.h"

namespace LuaMemPreview
{
namespace
{
// Matches AssetManager's kCacheSizeForWriting and the serialize smoke test.
constexpr size_t kCacheSize = 1024 * 1024;

// The Core systems are process-wide and re-entrant loads are normal in the GUI
// (Ctrl+O, drag-and-drop), so bring them up exactly once. RegisterCore() pulls
// in TypeManager / TypeTreeCache / MemoryManager / ObjectManager / FileSystem -
// no window, no RHI, no AssetManager.
bool EnsureCoreSystems(std::string& out_error)
{
    static int s_state = 0;  // 0 = untried, 1 = ready, -1 = failed
    if (s_state == 1)
        return true;
    if (s_state == -1)
    {
        out_error = "engine Core systems failed to initialise earlier in this process";
        return false;
    }

    RegisterCore();
    if (!SystemRegistry::GetInstance().InitializeAll(false))
    {
        s_state = -1;
        out_error = "SystemRegistry::InitializeAll(false) failed";
        return false;
    }
    s_state = 1;
    return true;
}

// Reads `path` into a heap MemoryProfiler. Caller owns the result and must
// release it with MemoryManager::DestroyObject. Returns nullptr on failure.
MemoryProfiler* ReadMemoryProfiler(const std::string& path, std::string& out_error)
{
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec)
    {
        out_error = "cannot open file: " + path;
        return nullptr;
    }

    if (!EnsureCoreSystems(out_error))
        return nullptr;

    // MUST be heap-allocated through MemoryManager: that is the only path that
    // runs Object::InitializeRuntimeTypeInfo() and seeds m_CachedTypeIndex. A
    // stack instance access-violates on the first GetType().
    MemoryProfiler* profiler = MemoryManager::CreateObject<MemoryProfiler>();
    if (profiler == nullptr)
    {
        out_error = "MemoryProfiler type is not registered (linker dropped its auto-registrar?)";
        return nullptr;
    }

    SerializedFile* file = MemoryManager::CreateObject<SerializedFile>();
    if (file == nullptr)
    {
        MemoryManager::DestroyObject(profiler);
        out_error = "failed to allocate SerializedFile";
        return nullptr;
    }

    if (file->InitializeRead(std::filesystem::path(path), kCacheSize) != kSerializedFileLoadError_None)
    {
        MemoryManager::DestroyObject(file);
        MemoryManager::DestroyObject(profiler);
        out_error = "not a readable .zasset (SerializedFile::InitializeRead failed): " + path;
        return nullptr;
    }

    // fileID 1 is the single top-level Object written by
    // AssetManager::WriteObjectToDiskThreadSafe.
    file->ReadObject(/*fileID=*/1, *profiler);
    MemoryManager::DestroyObject(file);
    return profiler;
}

std::string ToLowerAscii(std::string s)
{
    for (char& c : s)
    {
        if (c >= 'A' && c <= 'Z')
            c += 'a' - 'A';
    }
    return s;
}

bool HasZassetExtension(const std::filesystem::path& path)
{
    return ToLowerAscii(path.extension().string()) == ".zasset";
}

// One entry per snapshot file to export; sorted so the run is reproducible.
std::vector<std::filesystem::path> CollectSnapshots(const std::filesystem::path& folder, bool recursive)
{
    std::vector<std::filesystem::path> out;
    std::error_code ec;
    if (recursive)
    {
        for (std::filesystem::recursive_directory_iterator it(folder, ec), end; it != end; it.increment(ec))
        {
            if (ec)
                break;
            if (it->is_regular_file(ec) && !ec && HasZassetExtension(it->path()))
                out.push_back(it->path());
        }
    }
    else
    {
        for (std::filesystem::directory_iterator it(folder, ec), end; it != end; it.increment(ec))
        {
            if (ec)
                break;
            if (it->is_regular_file(ec) && !ec && HasZassetExtension(it->path()))
                out.push_back(it->path());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}
}  // namespace

bool ExportZassetToCsv(const std::string& path,
                       const std::string& output_base,
                       std::vector<std::string>& out_written,
                       std::string& out_error)
{
    out_written.clear();

    MemoryProfiler* profiler = ReadMemoryProfiler(path, out_error);
    if (profiler == nullptr)
        return false;

    std::filesystem::path base = output_base.empty() ? std::filesystem::path(path) : std::filesystem::path(output_base);
    if (output_base.empty())
        base.replace_extension();

    std::vector<std::filesystem::path> written;
    const bool ok = ExportMemoryProfilerToCSV(*profiler, base, &written, &out_error);
    MemoryManager::DestroyObject(profiler);
    if (!ok)
        return false;

    for (const auto& p : written)
        out_written.push_back(p.string());
    return true;
}

bool IsDirectory(const std::string& path)
{
    std::error_code ec;
    return std::filesystem::is_directory(path, ec) && !ec;
}

bool IsSnapshotPath(const std::string& path)
{
    if (path.size() < 7)
        return false;
    return ToLowerAscii(path.substr(path.size() - 7)) == ".zasset";
}

bool ExportFolderToCsv(const std::string& folder,
                       const std::string& output_dir,
                       bool recursive,
                       std::vector<std::string>& out_written,
                       std::vector<std::string>& out_errors)
{
    out_written.clear();
    out_errors.clear();

    std::error_code ec;
    if (!std::filesystem::is_directory(folder, ec) || ec)
    {
        out_errors.push_back("not a readable directory: " + folder);
        return false;
    }

    const std::filesystem::path out_dir = output_dir.empty() ? std::filesystem::path(folder)
                                                             : std::filesystem::path(output_dir);
    const std::vector<std::filesystem::path> snapshots = CollectSnapshots(folder, recursive);
    if (snapshots.empty())
    {
        out_errors.push_back("no *.zasset found in " + folder + (recursive ? " (recursive)" : ""));
        return true;
    }

    for (const std::filesystem::path& snapshot : snapshots)
    {
        std::string err;
        std::vector<std::string> written;
        // Base is <out_dir>/<stem>: the input's own path is irrelevant here,
        // since everything lands side by side in one directory.
        if (ExportZassetToCsv(snapshot.string(), (out_dir / snapshot.stem()).string(), written, err))
        {
            out_written.insert(out_written.end(), written.begin(), written.end());
        }
        else
        {
            out_errors.push_back(snapshot.filename().string() + ": " + err);
        }
    }

    return true;
}

bool LoadSnapshotFromZasset(const std::string& path, FSnapshot& out, std::string& out_error)
{
    out.BeginBuild();

    MemoryProfiler* profiler = ReadMemoryProfiler(path, out_error);
    if (profiler == nullptr)
        return false;

    // GetLuaInfos() folds the raw per-allocation map (ptr -> {size,type,stack})
    // into one row per (type, stack) and sorts by descending size. This is the
    // exact call ExportMemoryProfilerToCSV (batch mode) uses to produce
    // `_lua.csv`, so the asset and the CSV must agree row for row.
    const auto& lua_infos = profiler->GetLuaMemoryProfiler().GetLuaInfos();

    // A valid asset with an empty Lua section is a normal outcome, not a parse
    // failure: ProfilerRuntime::SaveData() writes one asset per snapshot and
    // appends "_N" on name collisions, so a run can leave behind a snapshot
    // taken while Lua tracking was off (C# / userdata only). Say so, instead of
    // letting FinishBuild report the generic "no data rows".
    if (lua_infos.empty())
    {
        const size_t csharp_rows = profiler->GetCSharpMemoryProfiler().GetCSharpInfos().size();
        const size_t userdata_rows = profiler->GetCSharpMemoryProfiler().GetUserdataInfos().size();
        MemoryManager::DestroyObject(profiler);
        out_error = "read OK but this MemoryProfiler snapshot has no Lua data (C# rows: " +
                    std::to_string(csharp_rows) + ", userdata rows: " + std::to_string(userdata_rows) +
                    "). It is a non-Lua snapshot: " + path;
        return false;
    }

    for (const auto& info : lua_infos)
    {
        out.AddEntry(info.count,
                     static_cast<uint64_t>(info.size),
                     LuaMemoryTypeName(info.type),
                     std::string(info.stack.c_str(), info.stack.size()));
    }

    // The profiler's own running total, independent of our per-row sum - the
    // same cross-check the CSV's legacy TOTAL row used to provide.
    out.SetDeclaredTotal(static_cast<uint64_t>(profiler->GetLuaMemoryProfiler().size),
                         "MemoryProfiler running total");

    MemoryManager::DestroyObject(profiler);

    return out.FinishBuild(path, out_error);
}

}  // namespace LuaMemPreview
