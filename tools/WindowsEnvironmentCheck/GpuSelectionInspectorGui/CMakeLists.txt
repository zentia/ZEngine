# ==============================================================================
# ZGpuSelectionInspector (optional GUI)
# ==============================================================================
# Standalone ZSlate-based visual front-end over the GpuSelectionProbe module
# (defined one directory up, in tools/WindowsEnvironmentCheck), which verifies
# the dual-GPU adapter-selection heuristic added to
# Runtime/GfxDevice/GfxDeviceSetup.cpp without linking against the engine or
# game build. This heuristic fixes a bug where a laptop's integrated GPU could
# over-report DedicatedVideoMemory higher than the real discrete GPU, causing
# the game to select the wrong adapter and crash on launch (white screen).
#
# This target is only added by the parent CMakeLists.txt (see
# tools/WindowsEnvironmentCheck/CMakeLists.txt) AFTER add_subdirectory(engine)
# has already run at the top level, because ZSlate is a target defined deep
# inside engine/Source/Runtime/CMakeLists.txt. Guarded with if(TARGET ZSlate)
# so a standalone WindowsEnvironmentCheck-only configure (ZSlate not built)
# simply skips this target instead of failing.
# ==============================================================================

if(NOT WIN32)
    message(STATUS "ZGpuSelectionInspector is Windows-only; target skipped")
    return()
endif()

if(NOT TARGET ZSlate)
    message(STATUS "ZGpuSelectionInspector skipped: ZSlate target not available")
    return()
endif()

set(GPU_INSPECTOR_TARGET ZGpuSelectionInspector)

add_executable(${GPU_INSPECTOR_TARGET}
    ZGpuSelectionInspector.cpp
    "${CMAKE_CURRENT_SOURCE_DIR}/../GpuSelectionProbe.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/../GpuSelectionProbe.cpp"
)

target_include_directories(${GPU_INSPECTOR_TARGET} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/.."
)

target_link_libraries(${GPU_INSPECTOR_TARGET} PRIVATE
    ZSlate
    dxgi
    d3d11
)

set_target_properties(${GPU_INSPECTOR_TARGET} PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    FOLDER "Tools"
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
)

target_compile_definitions(${GPU_INSPECTOR_TARGET} PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)
# Deliberately NOT defining UNICODE/_UNICODE: this source file uses the ANSI
# Win32 API explicitly (WNDCLASSA, RegisterClassA, CreateWindowA, PeekMessageA,
# lpszClassName as char*), matching examples/standalone_app_example.cpp. The
# bare (unsuffixed) DefWindowProc call in the window procedure is a macro that
# resolves to DefWindowProcA/DefWindowProcW depending on whether UNICODE is
# defined; defining UNICODE here while every other call site uses the ANSI
# API previously caused an ANSI/Unicode window-identity mismatch (window
# registered as ANSI via RegisterClassA, but its default message handling
# went through the Unicode DefWindowProcW), which corrupted the window title
# and produced a black, non-repainting client area.

if(MSVC)
    target_compile_options(${GPU_INSPECTOR_TARGET} PRIVATE
        /W4
        /permissive-
        /EHsc
        /utf-8
    )
endif()

if(DEFINED BINARY_ROOT_DIR)
    foreach(configuration Debug Release RelWithDebInfo MinSizeRel)
        string(TOUPPER "${configuration}" configuration_upper)
        set_target_properties(${GPU_INSPECTOR_TARGET} PROPERTIES
            "RUNTIME_OUTPUT_DIRECTORY_${configuration_upper}" "${BINARY_ROOT_DIR}/${configuration}"
        )
    endforeach()
else()
    set_target_properties(${GPU_INSPECTOR_TARGET} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
endif()

message(STATUS "ZGpuSelectionInspector configured (ZSlate GUI for GPU selection verification)")
