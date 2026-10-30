# =============================================================================
# iOS 模拟器 toolchain（在 ios.cmake 之上只换 sysroot）
#
# 用法：
#   cmake -S . -B build/iossim -G Ninja \
#         -DCMAKE_BUILD_TYPE=Debug \
#         -DCMAKE_TOOLCHAIN_FILE=$PWD/engine/toolchain/ios-simulator.cmake
#   ninja -C build/iossim ZGameIOS
#   xcrun simctl boot <UDID>；simctl install booted <.app>；simctl launch booted com.zengine.player
#
# 为什么单独一个文件而不是在命令行上覆盖：ios.cmake 里是 `set(CMAKE_OSX_SYSROOT
# "iphoneos")`（普通变量），会**盖掉** -D 传进来的缓存变量，所以 -DCMAKE_OSX_SYSROOT=
# iphonesimulator 不生效。先 include 再覆盖才稳。
#
# 这个配置的价值：**不需要签名**。真机部署卡在证书/Provisioning（公开 runner 和
# 没登录 Apple ID 的机器都过不去），而模拟器构建压根不签名 —— 于是"iOS 上能不能真的
# 跑起来"这件事可以在没有设备、没有证书的情况下验证。
#
# ⚠️ 模拟器**不能**替代真机的部分：帧率与热节流（跑的是 Mac 的 GPU/散热）、冷启动耗时、
# 真多指手势。能验的是 boot 全流程、Metal swapchain、帧循环、bundle 资源完整性、
# 触摸回调路径 —— 也就是把"iOS 特化代码有没有真的跑通"从编译期推进到运行期。
# =============================================================================

include(${CMAKE_CURRENT_LIST_DIR}/ios.cmake)

set(CMAKE_OSX_SYSROOT "iphonesimulator")
set(CMAKE_XCODE_EFFECTIVE_PLATFORMS "-iphonesimulator")

# Apple Silicon 的模拟器跑的就是 arm64，与真机一致；Intel 宿主上要改成 x86_64。
set(CMAKE_OSX_ARCHITECTURES "arm64")
