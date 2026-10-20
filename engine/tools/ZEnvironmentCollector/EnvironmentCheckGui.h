#pragma once
// ============================================================================
// Entry point of the ZSlate front-end of the merged shell.
// ============================================================================
// The GUI needs ZSlate; the console does not. In a full-tree configure ZSlate
// already exists as /MD while this toolships /MT (it runs on machines with no
// VC redist), so linking the two is LNK2038 and the GUI is compiled out with
// ENVIRONMENT_CHECK_BUILD_GUI=OFF. The exe still has to exist in that
// configuration -- it is what serves -batchmode -- so the front-end lives behind
// this one function and CMake swaps in EnvironmentCheckGuiFallback.cpp.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// `show` is the SW_* flag the window should be shown with; the shell passes
// SW_SHOW because a console-subsystem binary has no nCmdShow of its own.
int RunEnvironmentCheckGui(int show, int argumentCount, wchar_t** arguments);
