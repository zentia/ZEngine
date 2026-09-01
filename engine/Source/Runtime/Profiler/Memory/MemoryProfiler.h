#pragma once

#include "CSharp/CSharpMemoryProfiler.h"
#include "Lua/LuaMemoryProfiler.h"
#include "Runtime/BaseClasses/Object.h"

class MemoryProfiler : public Object
{
    REGISTER_CLASS(MemoryProfiler);
    DECLARE_OBJECT_SERIALIZE(MemoryProfiler);

public:
    virtual ~MemoryProfiler() = default;
    void Clear();
    void AddLuaData(uint64_t ptr, const char* stack, uint32_t size, uint32_t type);
    void RemoveLuaData(uint64_t key);
    void ResizeLuaData(uint64_t ptr, uint32_t size);

    void AddCSharpData(const int key, const char* name, const char* stack);
    void RemoveCSharpData(const int key);

    void AddUserData(uint64_t ptr, const char* stack);
    void RemoveUserData(uint64_t ptr);

    CSharpMemoryProfiler& GetCSharpMemoryProfiler() { return m_CSharpMemoryProfiler; }
    LuaMemoryProfiler& GetLuaMemoryProfiler() { return m_LuaMemoryProfiler; }

private:
    CSharpMemoryProfiler m_CSharpMemoryProfiler;
    LuaMemoryProfiler m_LuaMemoryProfiler;
};