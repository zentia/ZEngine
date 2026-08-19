cmake_minimum_required(VERSION 3.19 FATAL_ERROR)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(ZWindowsEnvironmentCheck VERSION 1.0.0 LANGUAGES CXX)
endif()

if(NOT WIN32)
    message(STATUS "ZWindowsEnvironmentCheck is Windows-only; target skipped")
    return()
endif()

set(ENVIRONMENT_CHECK_TARGET ZWindowsEnvironmentCheck)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)

set(ZYDIS_BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_MAN OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_DOXYGEN OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZYDIS_FEATURE_DECODER ON CACHE BOOL "" FORCE)
set(ZYDIS_FEATURE_FORMATTER ON CACHE BOOL "" FORCE)
set(ZYDIS_FEATURE_ENCODER OFF CACHE BOOL "" FORCE)
set(ZYDIS_FEATURE_AVX512 ON CACHE BOOL "" FORCE)
set(ZYDIS_FEATURE_KNC OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    zycore
    URL https://github.com/zyantific/zycore-c/archive/0b2432ced0884fd152b471d97ecf0258ff4d859f.tar.gz
    URL_HASH SHA256=698e4623e661db312eb9da327519f53f0a9dd7fbbfacf02159687bca62369467
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR _populate_only
)
FetchContent_MakeAvailable(zycore)
set(ZYAN_ZYCORE_PATH "${zycore_SOURCE_DIR}" CACHE PATH "" FORCE)

FetchContent_Declare(
    zydis
    URL https://github.com/zyantific/zydis/archive/refs/tags/v4.1.1.tar.gz
    URL_HASH SHA256=45c6d4d499a1cc80780f7834747c637509777c01dca1e98c5e7c0bfaccdb1514
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(zydis)

add_executable(${ENVIRONMENT_CHECK_TARGET}
    DllProbe.cpp
    DllProbe.h
    DllProbeProtocol.h
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

set(DLL_PROBE_TARGET ZDllLoadProbe)
add_executable(${DLL_PROBE_TARGET}
    DllProbeMain.cpp
    DllProbeProtocol.h
)
set_target_properties(${DLL_PROBE_TARGET} PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    OUTPUT_NAME "ZDllLoadProbe"
    FOLDER "Tools"
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)
target_compile_definitions(${DLL_PROBE_TARGET} PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
)
target_link_libraries(${DLL_PROBE_TARGET} PRIVATE kernel32 shell32)
if(MSVC)
    target_compile_options(${DLL_PROBE_TARGET} PRIVATE /GS- /GR-)
    target_link_options(${DLL_PROBE_TARGET} PRIVATE
        /NODEFAULTLIB
        /ENTRY:ProbeEntry
        /SUBSYSTEM:CONSOLE
    )
endif()
add_dependencies(${ENVIRONMENT_CHECK_TARGET} ${DLL_PROBE_TARGET})

set_target_properties(${ENVIRONMENT_CHECK_TARGET} PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    OUTPUT_NAME "ZWindowsEnvironmentCheck"
    FOLDER "Tools"
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES
    DllProbe.cpp
    DllProbe.h
    DllProbeProtocol.h
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
    Zydis
    dxgi
    setupapi
)

add_custom_command(TARGET ${ENVIRONMENT_CHECK_TARGET} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${zydis_SOURCE_DIR}/LICENSE"
        "$<TARGET_FILE_DIR:${ENVIRONMENT_CHECK_TARGET}>/LICENSE-Zydis.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${zycore_SOURCE_DIR}/LICENSE"
        "$<TARGET_FILE_DIR:${ENVIRONMENT_CHECK_TARGET}>/LICENSE-Zycore.txt"
)

if(MSVC)
    target_compile_options(${ENVIRONMENT_CHECK_TARGET} PRIVATE
        /W4
        /permissive-
        /EHsc
        /utf-8
    )
    target_compile_options(${DLL_PROBE_TARGET} PRIVATE
        /W4
        /permissive-
        /EHsc
        /utf-8
    )
else()
    target_compile_options(${ENVIRONMENT_CHECK_TARGET} PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(${DLL_PROBE_TARGET} PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(DEFINED BINARY_ROOT_DIR)
    foreach(configuration Debug Release RelWithDebInfo MinSizeRel)
        string(TOUPPER "${configuration}" configuration_upper)
        set_target_properties(${ENVIRONMENT_CHECK_TARGET} ${DLL_PROBE_TARGET} PROPERTIES
            "RUNTIME_OUTPUT_DIRECTORY_${configuration_upper}" "${BINARY_ROOT_DIR}/${configuration}"
        )
    endforeach()
else()
    set_target_properties(${ENVIRONMENT_CHECK_TARGET} ${DLL_PROBE_TARGET} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
endif()

message(STATUS "ZWindowsEnvironmentCheck configured without ZRuntime, GameCore, or AVX compile flags")
