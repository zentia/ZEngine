# =============================================================================
# iOS toolchain（P0-1 移动端出包链）
#
# 用法：
#   cmake -S engine -B build/ios -G Xcode \
#         -DCMAKE_TOOLCHAIN_FILE=engine/toolchain/ios.cmake \
#         -DZENGINE_IOS_DEVELOPMENT_TEAM=XXXXXXXXXX
#
# 为什么整个文件这么短：CMake 自带 Darwin-Initialize，设了 CMAKE_SYSTEM_NAME=iOS
# 之后它自己会推导出 Xcode 需要的绝大多数变量。手写 toolchain 只补"iOS 特有的
# 那几项"，不要重复造（重复造的那类 toolchain 通常在 Xcode 升级后就失效）。
# =============================================================================

set(CMAKE_SYSTEM_NAME iOS)
set(CMAKE_SYSTEM_PROCESSOR arm64)

# 真机 SDK。要跑模拟器就换成 iphonesimulator，并把 CMAKE_OSX_ARCHITECTURES
# 改成宿主架构（Apple Silicon 是 arm64，Intel 是 x86_64）。
set(CMAKE_OSX_SYSROOT "iphoneos")
set(CMAKE_OSX_ARCHITECTURES "arm64")
set(CMAKE_XCODE_EFFECTIVE_PLATFORMS "-iphoneos")

# iOS 上不要用宿主的工具 / 库：只在 SDK 里找，否则会把 macOS 的 .dylib 链进来。
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Bitcode 在 Xcode 14 起已被移除，显式关掉以免老工程设置把它打开。
set(CMAKE_XCODE_ATTRIBUTE_ENABLE_BITCODE NO)
