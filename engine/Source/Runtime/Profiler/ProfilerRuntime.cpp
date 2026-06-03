#include "ProfilerRuntime.h"

#include "Runtime/Core/Serialize/WriteData.h"
#include "Runtime/Resource/Asset/AssetManager.h"

bool ProfilerRuntime::Initialize()
{
    if (m_MemoryProfiler == nullptr)
    {
        m_MemoryProfiler = MemoryManager::CreateObject<MemoryProfiler>();
    }
    return m_MemoryProfiler != nullptr;
}

void ProfilerRuntime::Shutdown()
{
    SaveData();
    if (m_MemoryProfiler != nullptr)
    {
        MemoryManager::DestroyObject<MemoryProfiler>(m_MemoryProfiler);
        m_MemoryProfiler = nullptr;
    }
    m_Path.clear();
}

void ProfilerRuntime::ResetData()
{
    SaveData();
    if (m_MemoryProfiler != nullptr)
    {
        m_MemoryProfiler->Clear();
    }
    m_Path.clear();
}

void ProfilerRuntime::AddCSharpData(const int key, const char* name, const char* stack)
{
    if (m_MemoryProfiler != nullptr)
        m_MemoryProfiler->AddCSharpData(key, name, stack);
}

void ProfilerRuntime::RemoveCSharpData(const int key)
{
    if (m_MemoryProfiler != nullptr)
        m_MemoryProfiler->RemoveCSharpData(key);
}

void ProfilerRuntime::AddUserData(void* ptr, const char* stack)
{
    if (m_MemoryProfiler != nullptr)
        m_MemoryProfiler->AddUserData((uint64_t)ptr, stack);
}

void ProfilerRuntime::RemoveUserData(void* ptr)
{
    if (m_MemoryProfiler != nullptr)
        m_MemoryProfiler->RemoveUserData((uint64_t)ptr);
}

void ProfilerRuntime::AddLuaData(void* ptr, const char* stack, uint32_t size)
{
    if (m_MemoryProfiler != nullptr)
        m_MemoryProfiler->AddLuaData((uint64_t)ptr, stack, size);
}

void ProfilerRuntime::ResizeLuaData(void* ptr, uint32_t size)
{
    if (m_MemoryProfiler != nullptr)
        m_MemoryProfiler->ResizeLuaData((uint64_t)ptr, size);
}

void ProfilerRuntime::RemoveLuaData(void* ptr)
{
    if (m_MemoryProfiler != nullptr)
        m_MemoryProfiler->RemoveLuaData(reinterpret_cast<uint64_t>(ptr));
}

void ProfilerRuntime::SetSavePath(const char* path)
{
    // 注意：先 clear() 再赋值，避免 std::filesystem::path::operator= 内部走
    // libc++ 的 std::string::__move_assign 路径（旧 buffer 释放）。在 host 与
    // dylib 跨模块共享 std::filesystem::path 实例时，move-assign 释放的旧 buffer
    // 可能不属于当前模块的分配器，从而触发 allocator 不匹配崩溃。
    m_Path.clear();
    if (path != nullptr && path[0] != '\0')
    {
        m_Path = std::filesystem::path(path);
    }
}

void ProfilerRuntime::SaveData()
{
    const std::string ext = ".zasset";
    if (m_MemoryProfiler == nullptr || m_Path.empty())
        return;
    const std::filesystem::path baseParent = m_Path.parent_path();
    if (baseParent.empty())
        return;
    if (!std::filesystem::exists(baseParent))
    {
        std::filesystem::create_directory(baseParent);
    }
    const std::string baseStem = m_Path.stem().string();

    std::filesystem::path pathToWrite;
    for (int n = 0;; ++n)
    {
        std::string fileName = baseStem + (n == 0 ? "" : "_" + std::to_string(n)) + ext;
        pathToWrite = baseParent.empty() ? std::filesystem::path(fileName) : baseParent / fileName;
        if (!std::filesystem::exists(pathToWrite))
            break;
    }

    auto&& assetManager = GET_SYSTEM(AssetManager);
    if (assetManager != nullptr)
    {
        assetManager->WriteObjectToDiskThreadSafe(pathToWrite, *m_MemoryProfiler);
    }
}
