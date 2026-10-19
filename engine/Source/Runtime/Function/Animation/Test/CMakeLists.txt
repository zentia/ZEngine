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

    message(STATUS "Configured animator smoke-test target: AnimatorSmokeTest")
endif()
