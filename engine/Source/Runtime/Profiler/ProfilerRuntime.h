#pragma once

#include "Memory/MemoryProfiler.h"
#include "Runtime/Core/Base/EngineSystem.h"
#include "Runtime/RegisterRuntime.h"

#include <filesystem>

class ProfilerRuntime : public IEngineSystem
{
public:
    void AddLuaData(void* ptr, const char* stack, uint32_t size);
    void ResizeLuaData(void* ptr, uint32_t size);
    void AddCSharpData(const int key, const char* name, const char* stack);
    void RemoveCSharpData(const int key);
    void AddUserData(void* ptr, const char* stack);
    void RemoveUserData(void* ptr);
    void RemoveLuaData(void* ptr);
    void SetSavePath(const char* path);
    void SaveData();
    void ResetData();

protected:
    SystemInitPhase GetInitPhase() const override { return SystemInitPhase::PostInit; }
    bool Initialize() override;
    void Shutdown() override;

private:
    std::filesystem::path m_Path;
    MemoryProfiler* m_MemoryProfiler {nullptr};
};
