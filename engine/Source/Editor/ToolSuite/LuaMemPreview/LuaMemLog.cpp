#include "LuaMemLog.h"

// Compiled with CommonPCH/pch.h force-included (see CMakeLists), like
// LuaMemZasset.cpp / LuaMemCsvExport.cpp: the engine log macros are written
// against that environment, and pch.h is what brings in Macro.h + LogSystem.h.
#include "Runtime/Core/Log/LogSystem.h"

namespace LuaMemPreview
{
void LogInfo(const std::string& message)
{
    LOG_INFO(ZEngine, "{}", message.c_str());
}

void LogWarning(const std::string& message)
{
    LOG_WARNING(ZEngine, "{}", message.c_str());
}

void LogError(const std::string& message)
{
    LOG_ERROR(ZEngine, "{}", message.c_str());
}

void LogFlush()
{
    LOG_FLUSH();
}
}  // namespace LuaMemPreview
