# =====================================================================
# AppleSceneBlit smoke test.
#
# Own main(); carved out of the ZRuntime glob in Runtime/CMakeLists.txt so its
# main() cannot be dragged into every consumer executable (LNK2005).
#
#   cmake -B build -DZENGINE_BUILD_APPLE_SCENE_BLIT_SMOKE_TEST=ON
#   cmake --build build --target AppleSceneBlitSmokeTest
#
# This is the only guard on the MSL that carries the Metal scene onto the
# swapchain: AppleSceneBlit keeps its shaders as string literals in the .cpp, so
# they first meet a compiler at runtime (newLibraryWithSource:). Without this
# test a typo in that string is a fully green build and a black window.
#
# Off Apple it degrades to the source-text contract checks (descriptor slots,
# entry-point names, vertex count, cull mode) -- those are platform-independent
# and still worth running.
# =====================================================================

option(ZENGINE_BUILD_APPLE_SCENE_BLIT_SMOKE_TEST
       "Build the AppleSceneBlit (Metal scene -> swapchain) smoke-test executable"
       OFF)

if(ZENGINE_BUILD_APPLE_SCENE_BLIT_SMOKE_TEST)
    add_executable(AppleSceneBlitSmokeTest AppleSceneBlitSmokeTest.cpp)

    set_target_properties(AppleSceneBlitSmokeTest PROPERTIES
        CXX_STANDARD 23
        FOLDER "Engine/Tests"
    )

    # On Apple this TU includes MetalRHI.h (RHI's Initialize/Shutdown are
    # protected, so the test has to drive the concrete backend), and MetalRHI.h
    # pulls in <Metal/Metal.h> + <QuartzCore/CAMetalLayer.h> -- Objective-C
    # headers. Same treatment RegisterRuntime.cpp gets in Runtime/CMakeLists.txt:
    # compile it as ObjC++ and opt out of the C++ PCH, which was built for C++.
    if(APPLE)
        set_source_files_properties(
            "${CMAKE_CURRENT_SOURCE_DIR}/AppleSceneBlitSmokeTest.cpp"
            PROPERTIES LANGUAGE OBJCXX SKIP_PRECOMPILE_HEADERS ON)
    endif()

    target_link_libraries(AppleSceneBlitSmokeTest PRIVATE ZRuntime)

    if(TARGET ZEnginePCH)
        target_link_libraries(AppleSceneBlitSmokeTest PRIVATE ZEnginePCH)
        target_include_directories(AppleSceneBlitSmokeTest PRIVATE ${ENGINE_ROOT_DIR}/Source/CommonPCH)
        add_dependencies(AppleSceneBlitSmokeTest ZEnginePCH)
        target_precompile_headers(AppleSceneBlitSmokeTest REUSE_FROM ZEnginePCH)
    endif()

    # The source-text contract checks read AppleSceneBlit.cpp as text. CMake hands
    # the test the absolute engine root so it does not depend on the working
    # directory.
    if(DEFINED ENGINE_ROOT_DIR)
        target_compile_definitions(AppleSceneBlitSmokeTest PRIVATE
            "ZENGINE_ENGINE_ROOT_DIR=\"${ENGINE_ROOT_DIR}\"")
    endif()

    if(MSVC)
        target_compile_options(AppleSceneBlitSmokeTest PRIVATE /W4)
    endif()

    if(APPLE)
        target_sources(AppleSceneBlitSmokeTest PRIVATE
            ${ENGINE_ROOT_DIR}/Source/Runtime/Core/Serialize/Test/SmokeTestAppleLinkStubs.cpp)
    endif()

    add_test(NAME AppleSceneBlitSmokeTest COMMAND AppleSceneBlitSmokeTest)

    message(STATUS "Configured AppleSceneBlit smoke test: AppleSceneBlitSmokeTest")
endif()
