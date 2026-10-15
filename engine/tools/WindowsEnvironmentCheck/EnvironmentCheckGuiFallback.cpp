// ============================================================================
// Front-end stub used when the shell is built without ZSlate.
// ============================================================================
// A full-tree configure already has ZSlate as /MD (the engine default) while
// this tool ships /MT, so the GUI cannot be linked there and
// ENVIRONMENT_CHECK_BUILD_GUI is OFF. The single executable still has to exist
// because it is what serves -batchmode; this is what it does when somebody runs
// it without that switch.
// ============================================================================

#include "EnvironmentCheckGui.h"

#include <iostream>

int RunEnvironmentCheckGui(int, int, wchar_t**)
{
    std::cerr << "This build has no GUI front-end. Use: ZEnvironmentCollector.exe -batchmode --help\n";
    MessageBoxW(nullptr,
        L"本构建不包含图形界面（整树配置下 ZSlate 为 /MD，与本工具的 /MT 冲突）。\n"
        L"请使用命令行：ZEnvironmentCollector.exe -batchmode --collect\n"
        L"完整参数见：ZEnvironmentCollector.exe -batchmode --help",
        L"环境采集", MB_OK | MB_ICONINFORMATION);
    return 30;
}
