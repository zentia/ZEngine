# =============================================================================
# iOS toolchain（P0-1 移动端出包链）
#
# 用法（2026-10-30 用 Ninja 从零实跑通过；-G Xcode 同样可以，zbuild_ios.py 走的是 Xcode）：
#   cmake -S . -B build/ios -G Ninja \
#         -DCMAKE_BUILD_TYPE=Release \
#         -DCMAKE_TOOLCHAIN_FILE=$PWD/engine/toolchain/ios.cmake
#   ninja -C build/ios ZGameIOS
#
# ⚠️ -S 必须是**仓库根**，不是 engine/：只有工程根的 CMakeLists.txt 登记了
#    engine/Libraries/GPUSpine 与 GPUParticle 两个子模块。传 -S engine 会在
#    configure 时报 "GPUSpine submodule missing"，而它提示你跑的那条 git
#    submodule update --init 对**未登记**的路径必然失败 —— 看起来像本机环境
#    坏了，实际只是根目录传错。CI 里同样只有这一个正确写法。

# 为什么整个文件这么短：CMake 自带 Darwin-Initialize，设了 CMAKE_SYSTEM_NAME=iOS
# 之后它自己会推导出 Xcode 需要的绝大多数变量。手写 toolchain 只补"iOS 特有的
# 那几项"，不要重复造（重复造的那类 toolchain 通常在 Xcode 升级后就失效）。
# =============================================================================

set(CMAKE_SYSTEM_NAME iOS)
set(CMAKE_SYSTEM_PROCESSOR arm64)

# `IOS` 是工程侧判断"这是 iOS 构建"的唯一开关：
#   engine/CMakeLists.txt            elseif(IOS) -> add_subdirectory(Source/Player/IOS)
#   Source/Player/IOS/CMakeLists.txt if(NOT IOS) -> FATAL_ERROR
#
# CMake 自己的 Darwin 平台模块在 project() 时也会设 IOS，但那属于内部行为，
# 而且时机在 toolchain 之后 —— 一旦它不成立，iOS 那个 target 会被**静默跳过**
# （elseif 走空），症状是"配出来没有 ZGameIOS，也不知道为什么"。这里显式设一
# 次，把"依赖 CMake 内部"变成"自己保证"，两者不冲突（值相同）。
set(IOS TRUE)

# 与 ZGameIOS target 上的 XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET 对齐。
# 不设的话 CMake 的编译器检测走 SDK 默认版本，可能与 Xcode 属性不一致。
set(CMAKE_OSX_DEPLOYMENT_TARGET "16.0" CACHE STRING "Minimum iOS deployment target")

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
