#include <iostream>

#include "engine/source/runtime/core/base/platform.h"

int main() {
  std::cout << "=== ZEngine Platform Detection Test ===" << std::endl;

  // Test platform detection
  std::cout << "Platform: " << Runtime::Platform::GetPlatformName() << std::endl;
  std::cout << "Architecture: " << Runtime::Platform::GetArchitectureName()
            << std::endl;
  std::cout << "Compiler: " << Runtime::Platform::GetCompilerName() << std::endl;

  // Test macros
  std::cout << "\n=== Macro Tests ===" << std::endl;

#ifdef Z_PLATFORM_WINDOWS
  std::cout << "Z_PLATFORM_WINDOWS: defined" << std::endl;
#else
  std::cout << "Z_PLATFORM_WINDOWS: not defined" << std::endl;
#endif

#ifdef Z_PLATFORM_ANDROID
  std::cout << "Z_PLATFORM_ANDROID: defined" << std::endl;
#else
  std::cout << "Z_PLATFORM_ANDROID: not defined" << std::endl;
#endif

#ifdef Z_ARCH_X64
  std::cout << "Z_ARCH_X64: defined" << std::endl;
#else
  std::cout << "Z_ARCH_X64: not defined" << std::endl;
#endif

#ifdef Z_COMPILER_MSVC
  std::cout << "Z_COMPILER_MSVC: defined" << std::endl;
#else
  std::cout << "Z_COMPILER_MSVC: not defined" << std::endl;
#endif

  // Test runtime functions
  std::cout << "\n=== Runtime Tests ===" << std::endl;

  if (Z_IS_WINDOWS()) {
    std::cout << "Running on Windows!" << std::endl;
  } else if (Z_IS_ANDROID()) {
    std::cout << "Running on Android!" << std::endl;
  } else if (Z_IS_MACOS()) {
    std::cout << "Running on macOS!" << std::endl;
  } else if (Z_IS_LINUX()) {
    std::cout << "Running on Linux!" << std::endl;
  } else if (Z_IS_IOS()) {
    std::cout << "Running on iOS!" << std::endl;
  }

  if (Z_IS_X64()) {
    std::cout << "64-bit x86 architecture" << std::endl;
  } else if (Z_IS_X86()) {
    std::cout << "32-bit x86 architecture" << std::endl;
  } else if (Z_IS_ARM64()) {
    std::cout << "64-bit ARM architecture" << std::endl;
  } else if (Z_IS_ARM32()) {
    std::cout << "32-bit ARM architecture" << std::endl;
  }

  if (Z_IS_MSVC()) {
    std::cout << "Using MSVC compiler" << std::endl;
  } else if (Z_IS_GCC()) {
    std::cout << "Using GCC compiler" << std::endl;
  } else if (Z_IS_CLANG()) {
    std::cout << "Using Clang compiler" << std::endl;
  } else if (Z_IS_APPLE_CLANG()) {
    std::cout << "Using Apple Clang compiler" << std::endl;
  }

  std::cout << "\n=== Test Complete ===" << std::endl;
  return 0;
}
