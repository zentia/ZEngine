# =====================================================================
# Unity-parity smoke test.
#
#   cmake -B build -DZENGINE_BUILD_UNITY_PARITY_SMOKE_TEST=ON
#   cmake --build build --target UnityParitySmokeTest
#   ctest --test-dir build -C Debug -R UnityParitySmokeTest
#
# Own main(); carved out of the ZRuntime GLOB_RECURSE in Runtime/CMakeLists.txt
# (both SOURCE_FILES and REGULAR_SOURCE_FILES) so /WHOLEARCHIVE cannot drag it
# into every consumer executable (LNK2005).
#
# Headless: no project, no window, no GPU, no audio device. Runs everywhere.
# =====================================================================

option(ZENGINE_BUILD_UNITY_PARITY_SMOKE_TEST
       "Build the Unity API parity smoke-test executable"
       OFF)

if(NOT ZENGINE_BUILD_UNITY_PARITY_SMOKE_TEST)
    return()
endif()

add_executable(UnityParitySmokeTest UnityParitySmokeTest.cpp)

set_target_properties(UnityParitySmokeTest PROPERTIES
    CXX_STANDARD 23
    FOLDER "Engine/Tests"
)

# Needs ZRuntime: TagLayerManager is compiled into it and GameObject lives there.
target_link_libraries(UnityParitySmokeTest PRIVATE ZRuntime)
target_include_directories(UnityParitySmokeTest PRIVATE ${ENGINE_ROOT_DIR}/Source)

if(TARGET ZEnginePCH)
    target_link_libraries(UnityParitySmokeTest PRIVATE ZEnginePCH)
    target_include_directories(UnityParitySmokeTest PRIVATE ${ENGINE_ROOT_DIR}/Source/CommonPCH)
    add_dependencies(UnityParitySmokeTest ZEnginePCH)
    target_precompile_headers(UnityParitySmokeTest REUSE_FROM ZEnginePCH)
endif()

if(MSVC)
    target_compile_options(UnityParitySmokeTest PRIVATE /W4)
    # ⚠️ Debug 默认开 /INCREMENTAL（.ilk）。增量链接有个要命的行为：改了
    # `kZEngineDtsTemplate` 那种**大只读字符串**之后，.obj 和 .lib 里是新内容、
    # exe 里还是旧的 —— 于是 d.ts 的接线门读到的是上一次的字符串，编译零警告、
    # 测试零失败，变异体假活。以前只能靠手动删 .ilk 绕过，代价是"每次都要清
    # 东西"。这里直接对这一个 exe 关掉增量链接：它只链 ZRuntime + 一个 .cpp，
    # 全链比删 .ilk 再整链还快一点。
    target_link_options(UnityParitySmokeTest PRIVATE $<$<CONFIG:Debug>:/INCREMENTAL:NO>)
endif()

if(APPLE)
    target_sources(UnityParitySmokeTest PRIVATE
        ${ENGINE_ROOT_DIR}/Source/Runtime/Core/Serialize/Test/SmokeTestAppleLinkStubs.cpp)
endif()

# The wiring gate reads engine sources off disk; it must look at the tree it was
# built from, not at a hard-coded drive letter.
target_compile_definitions(UnityParitySmokeTest PRIVATE
    "ZENGINE_ENGINE_ROOT_DIR=\"${ENGINE_ROOT_DIR}\"")

add_test(NAME UnityParitySmokeTest COMMAND UnityParitySmokeTest)
message(STATUS "Configured Unity parity smoke-test target: UnityParitySmokeTest")
