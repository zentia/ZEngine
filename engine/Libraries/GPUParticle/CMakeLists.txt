# =====================================================================
# GPUParticle — 静态库（核心 C++，ZEngine/Unity 公用）
# =====================================================================
# 与 GPUSpine 同款定位：核心只吃标准库，引擎侧（ZEngine 的 ParticlePass、
# Unity 的渲染器）只做"上传 POD + 发实例化绘制"。
#
#   cmake -S . -B build -DGPUPARTICLE_BUILD_TESTS=ON
#   cmake --build build --config Debug
#   ctest --test-dir build -C Debug
# =====================================================================
cmake_minimum_required(VERSION 3.23)
project(GPUParticle VERSION 0.1 LANGUAGES CXX)

# ---- 选项 ----
option(GPUPARTICLE_BUILD_TESTS "构建测试" OFF)

# ---- C++ 标准（与 GPUSpine 一致用 20）----
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---- 核心库（只依赖标准库）----
add_library(GPUParticle STATIC
    Runtime/GPUParticle/ParticleGpuFormat.h
    Runtime/GPUParticle/ParticleGpuCurves.h
    Runtime/GPUParticle/ParticleGpuCurves.cpp
    Runtime/GPUParticle/ParticleGpuEmitter.h
    Runtime/GPUParticle/ParticleGpuEmitter.cpp
    Runtime/GPUParticle/ParticleGpuBatch.h
    Runtime/GPUParticle/ParticleGpuBatch.cpp
)

target_include_directories(GPUParticle PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Runtime>
    $<INSTALL_INTERFACE:include>
)

# 链进 .so/.dll（ZRuntime::Shared、Unity native plugin）需要 PIC
set_target_properties(GPUParticle PROPERTIES POSITION_INDEPENDENT_CODE ON FOLDER "GPUParticle")

# ---- 导出别名，让 ZEngine 可以用 GPUParticle::Core 来链接 ----
add_library(GPUParticle::Core ALIAS GPUParticle)

if(MSVC)
    target_compile_options(GPUParticle PRIVATE /W4 /utf-8)
endif()

# ---- 测试（不依赖任何第三方）----
if(GPUPARTICLE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(Tests~)
endif()
