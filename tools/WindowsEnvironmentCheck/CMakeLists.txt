cmake_minimum_required(VERSION 3.19 FATAL_ERROR)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(ZWindowsEnvironmentCheck VERSION 1.0.0 LANGUAGES CXX)
endif()

if(NOT WIN32)
    message(STATUS "ZWindowsEnvironmentCheck is Windows-only; target skipped")
    return()
endif()

set(ENVIRONMENT_CHECK_TARGET ZWindowsEnvironmentCheck)

add_executable(${ENVIRONMENT_CHECK_TARGET}
    EnvironmentProbe.cpp
    EnvironmentProbe.h
    HardwareProbe.cpp
    HardwareProbe.h
    PeInspector.cpp
    PeInspector.h
    ReportWriter.cpp
    ReportWriter.h
    main.cpp
)

set_target_properties(${ENVIRONMENT_CHECK_TARGET} PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    OUTPUT_NAME "ZWindowsEnvironmentCheck"
    FOLDER "Tools"
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES
    EnvironmentProbe.cpp
    EnvironmentProbe.h
    HardwareProbe.cpp
    HardwareProbe.h
    PeInspector.cpp
    PeInspector.h
    ReportWriter.cpp
    ReportWriter.h
    main.cpp
)

target_compile_definitions(${ENVIRONMENT_CHECK_TARGET} PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
)

target_link_libraries(${ENVIRONMENT_CHECK_TARGET} PRIVATE
    dxgi
    setupapi
)

if(MSVC)
    target_compile_options(${ENVIRONMENT_CHECK_TARGET} PRIVATE
        /W4
        /permissive-
        /EHsc
        /utf-8
    )
else()
    target_compile_options(${ENVIRONMENT_CHECK_TARGET} PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(DEFINED BINARY_ROOT_DIR)
    foreach(configuration Debug Release RelWithDebInfo MinSizeRel)
        string(TOUPPER "${configuration}" configuration_upper)
        set_target_properties(${ENVIRONMENT_CHECK_TARGET} PROPERTIES
            "RUNTIME_OUTPUT_DIRECTORY_${configuration_upper}" "${BINARY_ROOT_DIR}/${configuration}"
        )
    endforeach()
else()
    set_target_properties(${ENVIRONMENT_CHECK_TARGET} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
endif()

message(STATUS "ZWindowsEnvironmentCheck configured without ZRuntime, GameCore, or AVX compile flags")
