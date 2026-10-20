#include "MemoryProfiler.h"

#include "CSharp/CSharpMemoryProfiler.h"
#include "Lua/LuaMemoryProfiler.h"


void MemoryProfiler::Clear()
{
    m_CSharpMemoryProfiler.data.clear();
    m_CSharpMemoryProfiler.userdata.clear();
    m_CSharpMemoryProfiler.csharpInfos.clear();
    m_CSharpMemoryProfiler.userdataInfos.clear();
    m_CSharpMemoryProfiler.csharpInited = false;
    m_CSharpMemoryProfiler.userdataInited = false;

    m_LuaMemoryProfiler.data.clear();
    m_LuaMemoryProfiler.luaInfos.clear();
    m_LuaMemoryProfiler.size = 0;
}

void MemoryProfiler::AddCSharpData(const int key, const char* name, const char* stack)
{
    auto&& element = m_CSharpMemoryProfiler.data[key];
    if (name != nullptr)
    {
        element.name = name;
    }
    if (stack != nullptr)
    {
        element.stack = stack;
    }
}

void MemoryProfiler::RemoveCSharpData(const int key)
{
    m_CSharpMemoryProfiler.data.erase(key);
}

void MemoryProfiler::AddLuaData(uint64_t ptr, const char* stack, uint32_t size, uint32_t type)
{
    auto&& element = m_LuaMemoryProfiler.data[ptr];
    if (stack != nullptr)
    {
        element.stack = stack;
    }
    element.size = size;
    element.type = type;
}

void MemoryProfiler::ResizeLuaData(uint64_t ptr, uint32_t size)
{
    // Only update known objects: using operator[] here would materialise an
    // entry with type 0 ("table") and an empty stack for any object whose
    // creation happened before the profiler was registered, inflating the
    // table bucket with phantom records.
    auto it = m_LuaMemoryProfiler.data.find(ptr);
    if (it != m_LuaMemoryProfiler.data.end())
        it->second.size = size;
}

void MemoryProfiler::RemoveLuaData(uint64_t ptr)
{
    m_LuaMemoryProfiler.data.erase(ptr);
}

void MemoryProfiler::AddUserData(uint64_t ptr, const char* stack)
{
    m_CSharpMemoryProfiler.userdata[ptr] = stack == nullptr ? "" : stack;
}

void MemoryProfiler::RemoveUserData(uint64_t ptr)
{
    m_CSharpMemoryProfiler.userdata.erase(ptr);
}

