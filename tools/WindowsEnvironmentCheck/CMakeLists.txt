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

set(ENVIRONMENT_CHECK_MINIZ_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../renderdoc/renderdoc/3rdparty/miniz" CACHE PATH "Unmodified miniz sources from the pinned RenderDoc submodule")
find_package(Git REQUIRED)
set(_miniz_patched_dir "${CMAKE_CURRENT_BINARY_DIR}/miniz-patched")
set(_miniz_patch "${CMAKE_CURRENT_SOURCE_DIR}/miniz-zip64.patch")
file(MAKE_DIRECTORY "${_miniz_patched_dir}")
configure_file("${ENVIRONMENT_CHECK_MINIZ_DIR}/miniz.c" "${_miniz_patched_dir}/miniz.c" COPYONLY)
configure_file("${ENVIRONMENT_CHECK_MINIZ_DIR}/miniz.h" "${_miniz_patched_dir}/miniz.h" COPYONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_miniz_patch}")
set(_miniz_git_command "${CMAKE_COMMAND}" -E env --unset=GIT_DIR --unset=GIT_WORK_TREE
    "GIT_CEILING_DIRECTORIES=${CMAKE_CURRENT_BINARY_DIR}" "${GIT_EXECUTABLE}")
execute_process(
    COMMAND ${_miniz_git_command} apply --check -- "${_miniz_patch}"
    WORKING_DIRECTORY "${_miniz_patched_dir}"
    RESULT_VARIABLE _miniz_check_result ERROR_VARIABLE _miniz_check_error)
if(NOT _miniz_check_result EQUAL 0)
    message(FATAL_ERROR "miniz ZIP64 patch check failed; use unmodified sources from the pinned RenderDoc submodule: ${_miniz_check_error}")
endif()
execute_process(
    COMMAND ${_miniz_git_command} apply -- "${_miniz_patch}"
    WORKING_DIRECTORY "${_miniz_patched_dir}"
    RESULT_VARIABLE _miniz_apply_result ERROR_VARIABLE _miniz_apply_error)
if(NOT _miniz_apply_result EQUAL 0)
    message(FATAL_ERROR "miniz ZIP64 patch application failed: ${_miniz_apply_error}")
endif()
execute_process(
    COMMAND ${_miniz_git_command} apply --reverse --check -- "${_miniz_patch}"
    WORKING_DIRECTORY "${_miniz_patched_dir}"
    RESULT_VARIABLE _miniz_verify_result ERROR_VARIABLE _miniz_verify_error)
if(NOT _miniz_verify_result EQUAL 0)
    message(FATAL_ERROR "miniz ZIP64 patch verification failed: ${_miniz_verify_error}")
endif()
add_library(ZEnvironmentMiniz STATIC "${_miniz_patched_dir}/miniz.c")
target_include_directories(ZEnvironmentMiniz PUBLIC "${_miniz_patched_dir}")
target_compile_definitions(ZEnvironmentMiniz PRIVATE _CRT_SECURE_NO_WARNINGS)

add_library(ZEnvironmentCheckCore STATIC
    AttributionRules.cpp
    Diagnosis.cpp
    DiagnosisReport.cpp
    DiagnosisUtil.cpp
    EventLogReader.cpp
    Collection.cpp
    FirmwareProbe.cpp
    ReportArchive.cpp
    EvidenceFiles.cpp
    CrashCapture.cpp
    CapturePackage.cpp
    WerRegistry.cpp
    WerCapture.cpp
    DllProbe.cpp
    DllProbe.h
    DllProbeProtocol.h
    EnvironmentProbe.cpp
    EnvironmentProbe.h
    GpuSelectionProbe.cpp
    GpuSelectionProbe.h
    HardwareProbe.cpp
    HardwareProbe.h
    PeInspector.cpp
    PeInspector.h
    ReportWriter.cpp
    ReportWriter.h
)
target_include_directories(ZEnvironmentCheckCore PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(ZEnvironmentCheckCore PUBLIC cxx_std_20)
target_compile_definitions(ZEnvironmentCheckCore PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_link_libraries(ZEnvironmentCheckCore PUBLIC Zydis ZEnvironmentMiniz dxgi setupapi wevtapi advapi32 shell32 ole32 bcrypt wintrust crypt32 version comdlg32)
if(MSVC)
    target_compile_options(ZEnvironmentCheckCore PRIVATE /W4 /permissive- /EHsc /utf-8)
endif()
add_executable(${ENVIRONMENT_CHECK_TARGET} main.cpp)
target_link_libraries(${ENVIRONMENT_CHECK_TARGET} PRIVATE ZEnvironmentCheckCore)

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
    GpuSelectionProbe.cpp
    GpuSelectionProbe.h
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

set(_collector_default OFF)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(_collector_default ON)
endif()
option(ENVIRONMENT_CHECK_BUILD_GUI "Build standalone ZSlate collector" ${_collector_default})
if(ENVIRONMENT_CHECK_BUILD_GUI)
    if(NOT TARGET ZSlate)
        set(ZSLATE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../engine/Libraries/ZSlate" CACHE PATH "ZSlate source directory")
        add_subdirectory("${ZSLATE_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/zslate")
    endif()
    add_executable(ZEnvironmentCollector WIN32 CollectorGui.cpp CollectorGui.manifest)
    target_link_libraries(ZEnvironmentCollector PRIVATE ZEnvironmentCheckCore ZSlate d3d11 dxgi d3dcompiler)
    target_compile_definitions(ZEnvironmentCollector PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
    target_compile_features(ZEnvironmentCollector PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(ZEnvironmentCollector PRIVATE /W4 /permissive- /EHsc /utf-8)
    endif()
    add_dependencies(ZEnvironmentCollector ${ENVIRONMENT_CHECK_TARGET})
    if(DEFINED BINARY_ROOT_DIR)
        foreach(configuration Debug Release RelWithDebInfo MinSizeRel)
            string(TOUPPER "${configuration}" configuration_upper)
            set_target_properties(ZEnvironmentCollector PROPERTIES
                "RUNTIME_OUTPUT_DIRECTORY_${configuration_upper}" "${BINARY_ROOT_DIR}/${configuration}")
        endforeach()
    else()
        set_target_properties(ZEnvironmentCollector PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    endif()
    add_custom_command(TARGET ZEnvironmentCollector POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:ZEnvironmentCollector>/profiles"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/profiles/generic.ini" "$<TARGET_FILE_DIR:ZEnvironmentCollector>/profiles/default.ini"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/profiles/osgame.ini" "$<TARGET_FILE_DIR:ZEnvironmentCollector>/profiles/osgame.ini")
    install(TARGETS ZEnvironmentCollector RUNTIME DESTINATION . COMPONENT EnvironmentCollector)
endif()

# The GPU auto-select inspector is a second, much smaller ZSlate GUI over the
# same probe module. It used to be added separately from the root CMakeLists
# (which had to explain why it belonged after add_subdirectory(engine)); being
# here next to the collector keeps that ordering constraint in one place.
# Self-skips when ZSlate is not available.
if(TARGET ZSlate)
    add_subdirectory(GpuSelectionInspectorGui)
endif()

install(TARGETS ${ENVIRONMENT_CHECK_TARGET} ${DLL_PROBE_TARGET} RUNTIME DESTINATION . COMPONENT EnvironmentCollector)
install(FILES "${zydis_SOURCE_DIR}/LICENSE" DESTINATION . RENAME LICENSE-Zydis.txt COMPONENT EnvironmentCollector)
install(FILES "${zycore_SOURCE_DIR}/LICENSE" DESTINATION . RENAME LICENSE-Zycore.txt COMPONENT EnvironmentCollector)
install(FILES profiles/generic.ini DESTINATION profiles RENAME default.ini COMPONENT EnvironmentCollector)
install(FILES profiles/osgame.ini DESTINATION profiles COMPONENT EnvironmentCollector)
install(FILES LICENSE-stb.txt LICENSE-miniz.txt DESTINATION . COMPONENT EnvironmentCollector)

include(CTest)
if(BUILD_TESTING)
    add_executable(EnvironmentCollectorTests CollectionTests.cpp)
    target_link_libraries(EnvironmentCollectorTests PRIVATE ZEnvironmentCheckCore)
    if(MSVC)
        target_compile_options(EnvironmentCollectorTests PRIVATE /utf-8)
    endif()
    add_test(NAME environment_collector_core COMMAND EnvironmentCollectorTests)
    add_executable(EnvironmentCaptureTests CaptureTests.cpp)
    target_link_libraries(EnvironmentCaptureTests PRIVATE ZEnvironmentCheckCore)
    add_test(NAME environment_capture_safety COMMAND EnvironmentCaptureTests)
    add_executable(EnvironmentCaptureTestTarget CaptureTestTarget.cpp)
    add_executable(EnvironmentCaptureIntegrationTests CaptureIntegrationTests.cpp)
    target_link_libraries(EnvironmentCaptureIntegrationTests PRIVATE ZEnvironmentCheckCore)
    add_dependencies(EnvironmentCaptureIntegrationTests EnvironmentCaptureTestTarget ${ENVIRONMENT_CHECK_TARGET})
    add_executable(EnvironmentWerTests WerTests.cpp)
    target_link_libraries(EnvironmentWerTests PRIVATE ZEnvironmentCheckCore dbghelp)
    add_dependencies(EnvironmentWerTests EnvironmentCaptureTestTarget ${ENVIRONMENT_CHECK_TARGET})
    add_test(NAME environment_wer_safety COMMAND EnvironmentWerTests)
    foreach(testTarget EnvironmentCaptureTests EnvironmentCaptureTestTarget EnvironmentCaptureIntegrationTests EnvironmentWerTests)
        target_compile_features(${testTarget} PRIVATE cxx_std_20)
        target_compile_definitions(${testTarget} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
        if(MSVC)
            target_compile_options(${testTarget} PRIVATE /utf-8)
        endif()
        set_target_properties(${testTarget} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    endforeach()
    if(ENVIRONMENT_CHECK_BUILD_GUI)
        add_executable(EnvironmentCollectorUiTests CollectorUiTests.cpp)
        target_link_libraries(EnvironmentCollectorUiTests PRIVATE ZSlate)
        target_compile_features(EnvironmentCollectorUiTests PRIVATE cxx_std_20)
        target_compile_definitions(EnvironmentCollectorUiTests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
        if(MSVC)
            target_compile_options(EnvironmentCollectorUiTests PRIVATE /utf-8)
        endif()
        add_test(NAME environment_collector_ui COMMAND EnvironmentCollectorUiTests)
    endif()
endif()
message(STATUS "ZWindowsEnvironmentCheck configured without ZRuntime, GameCore, or AVX compile flags")
