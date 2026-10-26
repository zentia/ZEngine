# =====================================================================
# Linux X11 window-layer smoke test（commit 6a0a578b 真机验证）
# ---------------------------------------------------------------------
# 验证 LinuxApplication / LinuxWindow（X11 + XRandR 多显示器）：无头降级路径
# （XOpenDisplay 失败返回 false 不崩）、真窗口路径（Xvfb 下建窗 + PollEvents +
# 析构）、子窗口生命周期、剪贴板。
#
# 与 EnhancedInput 冒烟测试不同：本测试要真正实例化 LinuxApplication（其实现
# 在 ZRuntime 里的 LinuxApplication.cpp / LinuxWindow.cpp），故必须链接 ZRuntime
# （拿到 X11::X11 X11::Xrandr 的 PUBLIC 传递 + Linux 后端 TU）。
#
# 只在原生 Linux 桌面有意义：LinuxApplication.cpp 整体被
# Z_PLATFORM_LINUX && !__EMSCRIPTEN__ 守卫，非 Linux 上 ZRuntime 里它是空 TU，
# 本测试的 main() 也会走 SKIP 分支（return 0）。因此本 target 在任意主机都能
# 配置/链接通过，只是非 Linux 上运行即跳过。
#
#   cmake -B build -DZENGINE_BUILD_LINUX_X11_WINDOW_SMOKE_TEST=ON
#   cmake --build build --target LinuxX11WindowLayerSmokeTest --config Debug
#   # 无头：unset DISPLAY 后运行；真窗口：在 Xvfb 下运行（见 wsl_verify_linux.sh）
# =====================================================================

option(ZENGINE_BUILD_LINUX_X11_WINDOW_SMOKE_TEST
       "Build the Linux X11 window-layer smoke-test executable"
       OFF)

if(NOT ZENGINE_BUILD_LINUX_X11_WINDOW_SMOKE_TEST)
    return()
endif()

add_executable(LinuxX11WindowLayerSmokeTest LinuxX11WindowLayerSmokeTest.cpp)

set_target_properties(LinuxX11WindowLayerSmokeTest PROPERTIES
    CXX_STANDARD 20
    FOLDER "Engine/Tests"
)

# ZRuntime 提供 LinuxApplication.cpp + LinuxWindow.cpp（X11 后端）+ 传递的
# X11::X11 X11::Xrandr 链接。PRIVATE 链接避免 main() 被 WHOLEARCHIVE 拖进
# 其它消费者（与 RHIFactoryRegistrySmokeTest 同一理由）。
target_link_libraries(LinuxX11WindowLayerSmokeTest PRIVATE ZRuntime)

# ZRuntime PRIVATE 链 ZEnginePCH，其 include 目录（EASTL 等）不会传递，需显式补上。
if(TARGET ZEnginePCH)
    target_link_libraries(LinuxX11WindowLayerSmokeTest PRIVATE ZEnginePCH)
    target_include_directories(LinuxX11WindowLayerSmokeTest PRIVATE ${ENGINE_ROOT_DIR}/Source/CommonPCH)
    add_dependencies(LinuxX11WindowLayerSmokeTest ZEnginePCH)
    target_precompile_headers(LinuxX11WindowLayerSmokeTest REUSE_FROM ZEnginePCH)
endif()

if(MSVC)
    target_compile_options(LinuxX11WindowLayerSmokeTest PRIVATE /W4)
endif()

add_test(NAME LinuxX11WindowLayerSmokeTest COMMAND LinuxX11WindowLayerSmokeTest)

message(STATUS "Configured Linux X11 window-layer smoke-test target: LinuxX11WindowLayerSmokeTest")
