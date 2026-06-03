#pragma once

#include <type_traits>

#if Z_PLATFORM_WINDOWS
    #include <windef.h>
using WindowsHandle_t = HWND;
#endif  // Z_PLATFORM_WINDOWS
