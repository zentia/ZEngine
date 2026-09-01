#include "ZEngineApi.h"

#include "Runtime/Core/Serialize/TransferUtility.h"
#include "Runtime/Profiler/ProfilerRuntime.h"
#include "Runtime/RegisterRuntime.h"
#include "Runtime/Resource/Asset/AssetManager.h"
#include "pesapi.h"

#include <mutex>

#define ZENGINE_API_EXPORT extern "C" PESAPI_MODULE_EXPORT

namespace
{
    std::mutex g_memory_profiler_data_mutex;
    bool g_memory_profiler_data_initialized = false;
}  // namespace

ZENGINE_API_EXPORT void InitMemoryProfilerData()
{
    std::lock_guard<std::mutex> lock(g_memory_profiler_data_mutex);
    if (g_memory_profiler_data_initialized)
    {
        return;
    }

    RegisterCore();
    RegisterPlatform();
    REGISTER_SYSTEM(ProfilerRuntime);
    g_memory_profiler_data_initialized = START_SYSTEM_WITHOUT_UI();
}

ZENGINE_API_EXPORT void UnInitMemoryProfilerData()
{
    std::lock_guard<std::mutex> lock(g_memory_profiler_data_mutex);
    if (!g_memory_profiler_data_initialized)
    {
        return;
    }

    auto&& profilerRuntime = GET_SYSTEM(ProfilerRuntime);
    if (profilerRuntime != nullptr)
    {
        profilerRuntime->ResetData();
    }
}

ZENGINE_API_EXPORT void AddCSharpData(const int key, const char* name, const char* stack)
{
    auto&& profilerRuntime = GET_SYSTEM(ProfilerRuntime);
    if (profilerRuntime != nullptr)
        profilerRuntime->AddCSharpData(key, name, stack);
}

ZENGINE_API_EXPORT void RemoveCSharpData(const int key)
{
    auto&& profilerRuntime = GET_SYSTEM(ProfilerRuntime);
    if (profilerRuntime != nullptr)
        profilerRuntime->RemoveCSharpData(key);
}

ZENGINE_API_EXPORT void AddUserData(void* t, const char* stack)
{
    auto&& profilerRuntime = GET_SYSTEM(ProfilerRuntime);
    if (profilerRuntime != nullptr)
        profilerRuntime->AddUserData(t, stack);
}

ZENGINE_API_EXPORT void RemoveUserData(void* t)
{
    auto&& profilerRuntime = GET_SYSTEM(ProfilerRuntime);
    if (profilerRuntime != nullptr)
        profilerRuntime->RemoveUserData(t);
}

ZENGINE_API_EXPORT void AddLuaData(void* t, const char* stack, uint32_t size, uint32_t type)
{
    auto&& profilerRuntime = GET_SYSTEM(ProfilerRuntime);
    if (profilerRuntime != nullptr)
        profilerRuntime->AddLuaData(t, stack, size, type);
}

ZENGINE_API_EXPORT void ResizeLuaData(void* t, uint32_t size)
{
    auto&& profilerRuntime = GET_SYSTEM(ProfilerRuntime);
    if (profilerRuntime != nullptr)
        profilerRuntime->ResizeLuaData(t, size);
}

ZENGINE_API_EXPORT void RemoveLuaData(void* t)
{
    auto&& profilerRuntime = GET_SYSTEM(ProfilerRuntime);
    if (profilerRuntime != nullptr)
        profilerRuntime->RemoveLuaData(t);
}

ZENGINE_API_EXPORT void SaveData(const char* path)
{
    auto&& profilerRuntime = GET_SYSTEM(ProfilerRuntime);
    if (profilerRuntime != nullptr)
    {
        profilerRuntime->SetSavePath(path);
        profilerRuntime->SaveData();
    }
}

ZEngine::Api ZEngineApi::api {
    &InitMemoryProfilerData,
    &UnInitMemoryProfilerData,
    &AddCSharpData,
    &RemoveCSharpData,
    &AddUserData,
    &RemoveUserData,
    &AddLuaData,
    &ResizeLuaData,
    &RemoveLuaData,
    &SaveData,
};

ZENGINE_API_EXPORT void* ZEngineApi()
{
    return static_cast<void*>(&ZEngineApi::api);
}