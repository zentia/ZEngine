# =====================================================================
# Animation smoke tests.
#
# Standalone executables so they run headless in CI with no window and no
# GPU. Both use the "own main()" / side-target pattern from
# Core/Serialize/Test: the sources are carved out of the ZRuntime glob in
# Runtime/CMakeLists.txt so /WHOLEARCHIVE cannot drag their main() into
# every consumer executable (LNK2005).
#
#   cmake -B build -DZENGINE_BUILD_ANIMATION_SMOKE_TEST=ON   (PR-SK1)
#   cmake -B build -DZENGINE_BUILD_ANIMATOR_SMOKE_TEST=ON    (PR-SK3)
#   cmake -B build -DZENGINE_BUILD_ANIMATION_PIPELINE_SMOKE_TEST=ON  (P0.2)
#   cmake --build build --target <name>
#   ./build/.../<name>
#
# NOTE: each option guards ITS OWN target rather than `return()`-ing early,
# so enabling one must not silently disable the other.
# =====================================================================

option(ZENGINE_BUILD_ANIMATION_SMOKE_TEST
       "Build the skeletal skinning smoke-test executable (PR-SK1)"
       OFF)
option(ZENGINE_BUILD_ANIMATOR_SMOKE_TEST
       "Build the animator state-machine smoke-test executable (PR-SK3)"
       OFF)
option(ZENGINE_BUILD_ANIMATION_PIPELINE_SMOKE_TEST
       "Build the animation sampling-pipeline smoke-test executable (P0.2)"
       OFF)

if(ZENGINE_BUILD_ANIMATION_SMOKE_TEST)
    add_executable(AnimationSkinningSmokeTest AnimationSkinningSmokeTest.cpp)

    set_target_properties(AnimationSkinningSmokeTest PROPERTIES
        CXX_STANDARD 23
        FOLDER "Engine/Tests"
    )

    target_link_libraries(AnimationSkinningSmokeTest PRIVATE ZRuntime)

    if(TARGET ZEnginePCH)
        target_link_libraries(AnimationSkinningSmokeTest PRIVATE ZEnginePCH)
        target_include_directories(AnimationSkinningSmokeTest PRIVATE ${ENGINE_ROOT_DIR}/Source/CommonPCH)
        add_dependencies(AnimationSkinningSmokeTest ZEnginePCH)
        target_precompile_headers(AnimationSkinningSmokeTest REUSE_FROM ZEnginePCH)
    endif()

    if(MSVC)
        target_compile_options(AnimationSkinningSmokeTest PRIVATE /W4)
    endif()

    # Registered with CTest; the test returns EXIT_FAILURE on the first failed check.
    add_test(NAME AnimationSkinningSmokeTest COMMAND AnimationSkinningSmokeTest)

    message(STATUS "Configured skinning smoke-test target: AnimationSkinningSmokeTest")
endif()

if(ZENGINE_BUILD_ANIMATOR_SMOKE_TEST)
    add_executable(AnimatorSmokeTest AnimatorSmokeTest.cpp)

    set_target_properties(AnimatorSmokeTest PROPERTIES
        CXX_STANDARD 23
        FOLDER "Engine/Tests"
    )

    target_link_libraries(AnimatorSmokeTest PRIVATE ZRuntime)

    if(TARGET ZEnginePCH)
        target_link_libraries(AnimatorSmokeTest PRIVATE ZEnginePCH)
        target_include_directories(AnimatorSmokeTest PRIVATE ${ENGINE_ROOT_DIR}/Source/CommonPCH)
        add_dependencies(AnimatorSmokeTest ZEnginePCH)
        target_precompile_headers(AnimatorSmokeTest REUSE_FROM ZEnginePCH)
    endif()

    if(MSVC)
        target_compile_options(AnimatorSmokeTest PRIVATE /W4)
    endif()

    # Registered with CTest; the test returns EXIT_FAILURE on the first failed check.
    add_test(NAME AnimatorSmokeTest COMMAND AnimatorSmokeTest)

    message(STATUS "Configured animator smoke-test target: AnimatorSmokeTest")
endif()

if(ZENGINE_BUILD_ANIMATION_PIPELINE_SMOKE_TEST)
    add_executable(AnimationPipelineSmokeTest AnimationPipelineSmokeTest.cpp)

    set_target_properties(AnimationPipelineSmokeTest PROPERTIES
        CXX_STANDARD 23
        FOLDER "Engine/Tests"
    )

    target_link_libraries(AnimationPipelineSmokeTest PRIVATE ZRuntime)

    if(TARGET ZEnginePCH)
        target_link_libraries(AnimationPipelineSmokeTest PRIVATE ZEnginePCH)
        target_include_directories(AnimationPipelineSmokeTest PRIVATE ${ENGINE_ROOT_DIR}/Source/CommonPCH)
        add_dependencies(AnimationPipelineSmokeTest ZEnginePCH)
        target_precompile_headers(AnimationPipelineSmokeTest REUSE_FROM ZEnginePCH)
    endif()

    if(MSVC)
        target_compile_options(AnimationPipelineSmokeTest PRIVATE /W4)
    endif()

    # P8 要读仓库里一份"真实的历史资产"来验证 SafeBinaryRead 容忍缺失字段。
    # 那条路径原先硬编码成 `E:/Engine/ZEngine/engine/asset/...`，外面还包着
    # `if (exists)` —— 于是除了作者机器，P8 全程静默跳过（空测试）。改成从
    # 工程根推导，缺文件就报错。
    target_compile_definitions(AnimationPipelineSmokeTest PRIVATE
        "ZENGINE_ENGINE_ROOT_DIR=\"${ENGINE_ROOT_DIR}\"")

    add_test(NAME AnimationPipelineSmokeTest COMMAND AnimationPipelineSmokeTest)

    message(STATUS "Configured animation pipeline smoke-test target: AnimationPipelineSmokeTest")
endif()
