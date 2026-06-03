#include "engine/source/runtime/core/base/platform.h"
#include <iostream>

int main() {
    std::cout << "=== ZEngine Platform Detection ===" << std::endl;
    
    // Platform detection
    std::cout << "Platform: " << Runtime::Platform::GetPlatformName() << std::endl;
    std::cout << "Architecture: " << Runtime::Platform::GetArchitectureName() << std::endl;
    std::cout << "Compiler: " << Runtime::Platform::GetCompilerName() << std::endl;
    
    // Using macros for platform-specific code
    std::cout << "\n=== Platform-specific code ===" << std::endl;
    
    if (Z_IS_WINDOWS()) {
        std::cout << "Running on Windows!" << std::endl;
        std::cout << "Path separator: " << Z_PATH_SEPARATOR_STR << std::endl;
    }
    else if (Z_IS_ANDROID()) {
        std::cout << "Running on Android!" << std::endl;
    }
    else if (Z_IS_MACOS()) {
        std::cout << "Running on macOS!" << std::endl;
    }
    else if (Z_IS_LINUX()) {
        std::cout << "Running on Linux!" << std::endl;
    }
    else if (Z_IS_IOS()) {
        std::cout << "Running on iOS!" << std::endl;
    }
    
    // Architecture detection
    if (Z_IS_X64()) {
        std::cout << "64-bit x86 architecture" << std::endl;
    }
    else if (Z_IS_X86()) {
        std::cout << "32-bit x86 architecture" << std::endl;
    }
    else if (Z_IS_ARM64()) {
        std::cout << "64-bit ARM architecture" << std::endl;
    }
    else if (Z_IS_ARM32()) {
        std::cout << "32-bit ARM architecture" << std::endl;
    }
    
    // Compiler detection
    if (Z_IS_MSVC()) {
        std::cout << "Using MSVC compiler" << std::endl;
    }
    else if (Z_IS_GCC()) {
        std::cout << "Using GCC compiler" << std::endl;
    }
    else if (Z_IS_CLANG()) {
        std::cout << "Using Clang compiler" << std::endl;
    }
    else if (Z_IS_APPLE_CLANG()) {
        std::cout << "Using Apple Clang compiler" << std::endl;
    }
    
    return 0;
}
