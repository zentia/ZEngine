#pragma once

// =============================================================================
// ZEngine Platform Detection Macros
// =============================================================================
// This file provides platform detection macros that are automatically
// defined by CMake during compilation. Use these macros for platform-specific
// code instead of checking compiler-specific macros directly.

// =============================================================================
// Platform Detection
// =============================================================================

// Define platform detection macros based on CMake definitions
#ifdef Z_PLATFORM_WINDOWS
    #ifndef Z_PLATFORM_WIN
        #define Z_PLATFORM_WIN
    #endif
    #ifndef Z_PLATFORM_WINDOWS
        #define Z_PLATFORM_WINDOWS
    #endif
    #define Z_PLATFORM_IS_WINDOWS 1
    #define Z_PLATFORM_IS_ANDROID 0
    #define Z_PLATFORM_IS_OHOS    0
    #define Z_PLATFORM_IS_MACOS   0
    #define Z_PLATFORM_IS_LINUX   0
    #define Z_PLATFORM_IS_IOS     0
#elif defined(Z_PLATFORM_ANDROID)
    #ifndef Z_PLATFORM_ANDROID
        #define Z_PLATFORM_ANDROID
    #endif
    #define Z_PLATFORM_IS_WINDOWS 0
    #define Z_PLATFORM_IS_ANDROID 1
    #define Z_PLATFORM_IS_OHOS    0
    #define Z_PLATFORM_IS_MACOS   0
    #define Z_PLATFORM_IS_LINUX   0
    #define Z_PLATFORM_IS_IOS     0
#elif defined(Z_PLATFORM_OHOS)
    #ifndef Z_PLATFORM_OHOS
        #define Z_PLATFORM_OHOS
    #endif
    #ifndef Z_PLATFORM_HARMONYOS
        #define Z_PLATFORM_HARMONYOS
    #endif
    #define Z_PLATFORM_IS_WINDOWS 0
    #define Z_PLATFORM_IS_ANDROID 0
    #define Z_PLATFORM_IS_OHOS    1
    #define Z_PLATFORM_IS_MACOS   0
    #define Z_PLATFORM_IS_LINUX   0
    #define Z_PLATFORM_IS_IOS     0
#elif defined(Z_PLATFORM_MACOS)
    #ifndef Z_PLATFORM_MAC
        #define Z_PLATFORM_MAC
    #endif
    #ifndef Z_PLATFORM_MACOS
        #define Z_PLATFORM_MACOS
    #endif
    #define Z_PLATFORM_IS_WINDOWS 0
    #define Z_PLATFORM_IS_ANDROID 0
    #define Z_PLATFORM_IS_OHOS    0
    #define Z_PLATFORM_IS_MACOS   1
    #define Z_PLATFORM_IS_LINUX   0
    #define Z_PLATFORM_IS_IOS     0
#elif defined(Z_PLATFORM_LINUX)
    #ifndef Z_PLATFORM_LINUX
        #define Z_PLATFORM_LINUX
    #endif
    #define Z_PLATFORM_IS_WINDOWS 0
    #define Z_PLATFORM_IS_ANDROID 0
    #define Z_PLATFORM_IS_OHOS    0
    #define Z_PLATFORM_IS_MACOS   0
    #define Z_PLATFORM_IS_LINUX   1
    #define Z_PLATFORM_IS_IOS     0
#elif defined(Z_PLATFORM_IOS)
    #ifndef Z_PLATFORM_IOS
        #define Z_PLATFORM_IOS
    #endif
    #define Z_PLATFORM_IS_WINDOWS 0
    #define Z_PLATFORM_IS_ANDROID 0
    #define Z_PLATFORM_IS_OHOS    0
    #define Z_PLATFORM_IS_MACOS   0
    #define Z_PLATFORM_IS_LINUX   0
    #define Z_PLATFORM_IS_IOS     1
#else
    #error "Unsupported platform detected!"
#endif

// =============================================================================
// Architecture Detection
// =============================================================================

#ifdef Z_ARCH_X64
    #define Z_ARCH_IS_X64   1
    #define Z_ARCH_IS_X86   0
    #define Z_ARCH_IS_ARM64 0
    #define Z_ARCH_IS_ARM32 0
#elif defined(Z_ARCH_X86)
    #define Z_ARCH_IS_X64   0
    #define Z_ARCH_IS_X86   1
    #define Z_ARCH_IS_ARM64 0
    #define Z_ARCH_IS_ARM32 0
#elif defined(Z_ARCH_ARM64)
    #define Z_ARCH_IS_X64   0
    #define Z_ARCH_IS_X86   0
    #define Z_ARCH_IS_ARM64 1
    #define Z_ARCH_IS_ARM32 0
#elif defined(Z_ARCH_ARM32)
    #define Z_ARCH_IS_X64   0
    #define Z_ARCH_IS_X86   0
    #define Z_ARCH_IS_ARM64 0
    #define Z_ARCH_IS_ARM32 1
#else
    #define Z_ARCH_IS_X64   0
    #define Z_ARCH_IS_X86   0
    #define Z_ARCH_IS_ARM64 0
    #define Z_ARCH_IS_ARM32 0
#endif

// =============================================================================
// Compiler Detection
// =============================================================================

#ifdef Z_COMPILER_MSVC
    #define Z_COMPILER_IS_MSVC        1
    #define Z_COMPILER_IS_GCC         0
    #define Z_COMPILER_IS_CLANG       0
    #define Z_COMPILER_IS_APPLE_CLANG 0
#elif defined(Z_COMPILER_GCC)
    #define Z_COMPILER_IS_MSVC        0
    #define Z_COMPILER_IS_GCC         1
    #define Z_COMPILER_IS_CLANG       0
    #define Z_COMPILER_IS_APPLE_CLANG 0
#elif defined(Z_COMPILER_CLANG)
    #define Z_COMPILER_IS_MSVC        0
    #define Z_COMPILER_IS_GCC         0
    #define Z_COMPILER_IS_CLANG       1
    #define Z_COMPILER_IS_APPLE_CLANG 0
#elif defined(Z_COMPILER_APPLE_CLANG)
    #define Z_COMPILER_IS_MSVC        0
    #define Z_COMPILER_IS_GCC         0
    #define Z_COMPILER_IS_CLANG       0
    #define Z_COMPILER_IS_APPLE_CLANG 1
#else
    #define Z_COMPILER_IS_MSVC        0
    #define Z_COMPILER_IS_GCC         0
    #define Z_COMPILER_IS_CLANG       0
    #define Z_COMPILER_IS_APPLE_CLANG 0
#endif

// =============================================================================
// Convenience Macros
// =============================================================================

// Platform checks
#define Z_IS_WINDOWS() (Z_PLATFORM_IS_WINDOWS == 1)
#define Z_IS_ANDROID() (Z_PLATFORM_IS_ANDROID == 1)
#define Z_IS_OHOS()    (Z_PLATFORM_IS_OHOS == 1)
#define Z_IS_MACOS()   (Z_PLATFORM_IS_MACOS == 1)
#define Z_IS_LINUX()   (Z_PLATFORM_IS_LINUX == 1)
#define Z_IS_IOS()     (Z_PLATFORM_IS_IOS == 1)

// Architecture checks
#define Z_IS_X64()   (Z_ARCH_IS_X64 == 1)
#define Z_IS_X86()   (Z_ARCH_IS_X86 == 1)
#define Z_IS_ARM64() (Z_ARCH_IS_ARM64 == 1)
#define Z_IS_ARM32() (Z_ARCH_IS_ARM32 == 1)

// Compiler checks
#define Z_IS_MSVC()        (Z_COMPILER_IS_MSVC == 1)
#define Z_IS_GCC()         (Z_COMPILER_IS_GCC == 1)
#define Z_IS_CLANG()       (Z_COMPILER_IS_CLANG == 1)
#define Z_IS_APPLE_CLANG() (Z_COMPILER_IS_APPLE_CLANG == 1)

// =============================================================================
// Platform-specific includes and definitions
// =============================================================================

#ifdef Z_PLATFORM_WINDOWS
    #include <tchar.h>
    #include <windows.h>
    #define Z_PATH_SEPARATOR     '\\'
    #define Z_PATH_SEPARATOR_STR "\\"
#else
    #include <unistd.h>
    #define Z_PATH_SEPARATOR     '/'
    #define Z_PATH_SEPARATOR_STR "/"
#endif

// =============================================================================
// Debug helpers
// =============================================================================

namespace Runtime
{
    namespace Platform
    {
        constexpr const char* GetPlatformName()
        {
#ifdef Z_PLATFORM_WINDOWS
            return "Windows";
#elif defined(Z_PLATFORM_ANDROID)
            return "Android";
#elif defined(Z_PLATFORM_OHOS)
            return "OHOS";
#elif defined(Z_PLATFORM_MACOS)
            return "macOS";
#elif defined(Z_PLATFORM_LINUX)
            return "Linux";
#elif defined(Z_PLATFORM_IOS)
            return "iOS";
#else
            return "Unknown";
#endif
        }

        constexpr const char* GetArchitectureName()
        {
#ifdef Z_ARCH_X64
            return "x64";
#elif defined(Z_ARCH_X86)
            return "x86";
#elif defined(Z_ARCH_ARM64)
            return "ARM64";
#elif defined(Z_ARCH_ARM32)
            return "ARM32";
#else
            return "Unknown";
#endif
        }

        constexpr const char* GetCompilerName()
        {
#ifdef Z_COMPILER_MSVC
            return "MSVC";
#elif defined(Z_COMPILER_GCC)
            return "GCC";
#elif defined(Z_COMPILER_CLANG)
            return "Clang";
#elif defined(Z_COMPILER_APPLE_CLANG)
            return "AppleClang";
#else
            return "Unknown";
#endif
        }
    }  // namespace Platform
}  // namespace Runtime
