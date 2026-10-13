#include "LuaMemZasset.h"

#include "LuaMemCsv.h"

// Standard headers FIRST: the engine headers below are written against
// CommonPCH/pch.h, which we deliberately do not use here (it drags in
// windows.h, whose DrawText macro breaks ZSlate in the other TUs). Several of
// them - e.g. VirtualFileSystemTypes.h with its std::filesystem::path member -
// therefore rely on these already being visible.
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>

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
}  // namespace

bool LoadSnapshotFromZasset(const std::string& path, FSnapshot& out, std::string& out_error)
{
    out.BeginBuild();

    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec)
    {
        out_error = "cannot open file: " + path;
        return false;
    }

    if (!EnsureCoreSystems(out_error))
        return false;

    // MUST be heap-allocated through MemoryManager: that is the only path that
    // runs Object::InitializeRuntimeTypeInfo() and seeds m_CachedTypeIndex. A
    // stack instance access-violates on the first GetType().
    MemoryProfiler* profiler = MemoryManager::CreateObject<MemoryProfiler>();
    if (profiler == nullptr)
    {
        out_error = "MemoryProfiler type is not registered (linker dropped its auto-registrar?)";
        return false;
    }

    SerializedFile* file = MemoryManager::CreateObject<SerializedFile>();
    if (file == nullptr)
    {
        MemoryManager::DestroyObject(profiler);
        out_error = "failed to allocate SerializedFile";
        return false;
    }

    if (file->InitializeRead(std::filesystem::path(path), kCacheSize) != kSerializedFileLoadError_None)
    {
        MemoryManager::DestroyObject(file);
        MemoryManager::DestroyObject(profiler);
        out_error = "not a readable .zasset (SerializedFile::InitializeRead failed): " + path;
        return false;
    }

    // fileID 1 is the single top-level Object written by
    // AssetManager::WriteObjectToDiskThreadSafe.
    file->ReadObject(/*fileID=*/1, *profiler);
    MemoryManager::DestroyObject(file);

    // GetLuaInfos() folds the raw per-allocation map (ptr -> {size,type,stack})
    // into one row per (type, stack) and sorts by descending size. This is the
    // exact call ProfilerCsvExport uses to produce `_lua.csv`, so the asset and
    // the CSV must agree row for row.
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
