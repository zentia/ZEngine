cmake_minimum_required(VERSION 3.20)
project(nanite_dedup_probe CXX)

# 独立小工程：只编译 scripts/nanite_dedup_probe.cpp，纯 CPU headless。
# 用引擎同款生成器（Visual Studio 17 2022）让 CMake 自动配好 MSVC + Windows SDK，
# 免去手工 vcvars 环境（直接调 cl.exe 会缺 crtdbg.h 等 SDK 头）。
add_executable(nanite_dedup_probe ../nanite_dedup_probe.cpp)

target_compile_features(nanite_dedup_probe PRIVATE cxx_std_17)

if(MSVC)
    target_compile_options(nanite_dedup_probe PRIVATE /W4 /O2)
endif()
