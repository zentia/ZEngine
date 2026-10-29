# =====================================================================
# GPUParticle 测试
# ---------------------------------------------------------------------
# 全部不依赖任何引擎（ZEngine / Unity）——核心只吃标准库，也不依赖 rapidjson
# （GPUSpine 的测试要 json 夹具，粒子这边暂时没有外部夹具格式）。
#
#   cmake -S .. -B build -DGPUPARTICLE_BUILD_TESTS=ON
#   cmake --build build --config Debug
#   ctest --test-dir build -C Debug
# =====================================================================

function(gpuparticle_add_test name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE GPUParticle::Core)
    set_target_properties(${name} PROPERTIES CXX_STANDARD 20 FOLDER "GPUParticle/Tests")
    if(MSVC)
        target_compile_options(${name} PRIVATE /W4 /utf-8)
    endif()
endfunction()

# ---- 核心自测：曲线 / 渐变 / 随机 / 发射调度 / 闭式积分镜像 ----
gpuparticle_add_test(ParticleGpuCoreTest ParticleGpuCoreTest.cpp)
add_test(NAME ParticleGpuCoreTest COMMAND ParticleGpuCoreTest)
