cmake_minimum_required(VERSION 3.19 FATAL_ERROR)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(ZEnvironmentCollector VERSION 1.0.0 LANGUAGES CXX)
endif()

if(NOT WIN32)
    message(STATUS "ZEnvironmentCollector is Windows-only; target skipped")
    return()
endif()

# One executable ships now: the collector GUI, the GPU inspector GUI and the CLI
# merged into a single shell (EnvironmentCheckShell.cpp + EnvironmentCheckGui.cpp)
# that runs the GUI by default and the CLI under -batchmode. See README.md.
set(ENVIRONMENT_CHECK_TARGET ZEnvironmentCollector)
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

# ZIP container: minizip-ng from the engine's own 3rdparty tree -- the same
# submodule the editor's XlsxImporter links. `../..` is engine/, so (unlike the
# old arrangement, which read miniz out of the root tools/renderdoc submodule
# and patched it with miniz-zip64.patch at configure time) this stays inside
# the engine tree and needs no patching: minizip-ng speaks ZIP64 natively.
#
# In a full-tree configure engine/3rdparty/CMakeLists.txt has already added it
# (it is not Emscripten-gated), hence the target guard -- same pattern as the
# ZSlate guard below. Standalone, the options below strip it to deflate-only
# and let it FetchContent zlib (WEC already fetches zydis, so a network
# configure was already required).
# minizip-ng force-sets BUILD_TESTING OFF in the cache (that is how it silences
# zlib-ng's own tests). It is added before `include(CTest)` below, so without
# saving and restoring the option this project's tests would silently vanish
# from a standalone configure.
if(DEFINED BUILD_TESTING)
    set(_environment_check_build_testing "${BUILD_TESTING}")
else()
    set(_environment_check_build_testing ON)
endif()

if(NOT TARGET minizip-ng)
    set(MZ_COMPAT OFF CACHE BOOL "" FORCE)
    set(MZ_ZLIB ON CACHE BOOL "" FORCE)
    set(MZ_BZIP2 OFF CACHE BOOL "" FORCE)
    set(MZ_LZMA OFF CACHE BOOL "" FORCE)
    set(MZ_PPMD OFF CACHE BOOL "" FORCE)
    set(MZ_ZSTD OFF CACHE BOOL "" FORCE)
    set(MZ_LIBCOMP OFF CACHE BOOL "" FORCE)
    set(MZ_PKCRYPT OFF CACHE BOOL "" FORCE)
    set(MZ_WZAES OFF CACHE BOOL "" FORCE)
    set(MZ_OPENSSL OFF CACHE BOOL "" FORCE)
    set(MZ_LIBBSD OFF CACHE BOOL "" FORCE)
    set(MZ_ICONV OFF CACHE BOOL "" FORCE)
    set(MZ_FETCH_LIBS ON CACHE BOOL "" FORCE)
    # When minizip-ng fetches zlib itself it links the *shared* zlib-ng target
    # (its CMake appends plain `zlib` whenever the fetched tree is zlib-ng),
    # which leaves ZEnvironmentCollector.exe importing zlib-ng2.dll. This tool
    # ships /MT to machines with no VC redist and must be a self-contained
    # directory, so the deflate backend has to be static. zlib-ng honours
    # BUILD_SHARED_LIBS, so turning it off before the fetch is what makes
    # minizip-ng pick up the static archive.
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(MZ_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(MZ_BUILD_UNIT_TESTS OFF CACHE BOOL "" FORCE)
    set(MZ_BUILD_FUZZ_TESTS OFF CACHE BOOL "" FORCE)
    set(SKIP_INSTALL_ALL ON CACHE BOOL "" FORCE)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../3rdparty/minizip-ng"
                     "${CMAKE_CURRENT_BINARY_DIR}/minizip-ng")
    set(BUILD_TESTING "${_environment_check_build_testing}" CACHE BOOL "" FORCE)
endif()

add_library(ZEnvironmentCheckCore STATIC
    AttributionRules.cpp
    Diagnosis.cpp
    DiagnosisReport.cpp
    DiagnosisUtil.cpp
    EventLogReader.cpp
    Collection.cpp
    EnvironmentCheckCommand.cpp
    EnvironmentCheckCommand.h
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
    JsonEscape.h
)
target_include_directories(ZEnvironmentCheckCore PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(ZEnvironmentCheckCore PUBLIC cxx_std_20)
target_compile_definitions(ZEnvironmentCheckCore PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
# minizip-ng exports both include dirs it needs as PUBLIC BUILD_INTERFACE
# entries (submodule root for mz_zip.h / mz_zip_rw.h, its own binary dir for
# the generated mz_config.h mz.h includes), so linking it is enough -- do not
# hand-roll the paths here, they differ between a full-tree and a standalone
# configure.
target_link_libraries(ZEnvironmentCheckCore PUBLIC Zydis minizip-ng dxgi setupapi wevtapi advapi32 shell32 ole32 bcrypt wintrust crypt32 version comdlg32)
if(MSVC)
    target_compile_options(ZEnvironmentCheckCore PRIVATE /W4 /permissive- /EHsc /utf-8)
endif()
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
# ------------------------------------------------------------------------------
# ZSlate front-end (optional)
# ------------------------------------------------------------------------------
# OFF in a full-tree configure, and that default is deliberate -- do not "fix"
# it:
#
# This directory sets CMAKE_MSVC_RUNTIME_LIBRARY to /MT because the tool ships to
# machines with no VC redist. Standalone, ZSlate is added inside that scope and
# inherits /MT. In a full tree ZSlate already exists as /MD (engine default), so
# linking it into a /MT executable fails with LNK2038 (RuntimeLibrary mismatch).
# One ZSlate instance cannot be both. Build the GUI from its own configure:
#     cmake -S engine/tools/WindowsEnvironmentCheck -B <dir>
#
# The executable itself is built either way: without ZSlate it is CLI-only
# (EnvironmentCheckGuiFallback.cpp), which is what a full-tree configure wants.
set(_collector_default OFF)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(_collector_default ON)
endif()
option(ENVIRONMENT_CHECK_BUILD_GUI "Build the ZSlate front-end of ZEnvironmentCollector" ${_collector_default})
set(ENVIRONMENT_CHECK_WITH_ZSLATE OFF)
if(ENVIRONMENT_CHECK_BUILD_GUI)
    if(NOT TARGET ZSlate)
        set(ZSLATE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../Libraries/ZSlate" CACHE PATH "ZSlate source directory")
        add_subdirectory("${ZSLATE_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/zslate")
    endif()
    set(ENVIRONMENT_CHECK_WITH_ZSLATE ON)
endif()

# ------------------------------------------------------------------------------
# The one executable
# ------------------------------------------------------------------------------
if(ENVIRONMENT_CHECK_WITH_ZSLATE)
    # No .manifest beside the sources: DPI awareness now comes from
    # z_embed_app_manifest() below, which shares one manifest across every
    # ZEngine GUI instead of each tool carrying a copy.
    set(_shell_sources
        EnvironmentCheckShell.cpp
        EnvironmentCheckGui.cpp
        EnvironmentCheckGui.h
    )
else()
    set(_shell_sources
        EnvironmentCheckShell.cpp
        EnvironmentCheckGuiFallback.cpp
    )
endif()
# WINDOWS subsystem: the loader creates no console, so a double-clicked GUI never
# flashes a black window. It used to be CONSOLE + FreeConsole(), but that console
# always existed before any of our code ran, so it could only be closed *after*
# it had already flashed. -batchmode is unaffected: it attaches to the caller's
# console (see EnvironmentCheckShell.cpp), so stdout and the exit code survive.
# cmd.exe still waits for it in a .bat (checked, see the shell's header comment),
# so `exe && next` keeps working.
add_executable(${ENVIRONMENT_CHECK_TARGET} WIN32 ${_shell_sources})
target_link_libraries(${ENVIRONMENT_CHECK_TARGET} PRIVATE ZEnvironmentCheckCore)
if(ENVIRONMENT_CHECK_WITH_ZSLATE)
    target_link_libraries(${ENVIRONMENT_CHECK_TARGET} PRIVATE ZSlate d3d11 dxgi d3dcompiler)
endif()
add_dependencies(${ENVIRONMENT_CHECK_TARGET} ${DLL_PROBE_TARGET})

set_target_properties(${ENVIRONMENT_CHECK_TARGET} PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    OUTPUT_NAME "ZEnvironmentCollector"
    FOLDER "Tools"
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

# App icon. The GUI path registers its own WNDCLASSEX (EnvironmentCheckGui.cpp)
# instead of ZSlate::FSlateStandaloneApp, so it loads IDI_ICON1 itself; the .rc
# alone would only give Explorer an icon. After set_target_properties so
# OUTPUT_NAME is what the .ico is named after.
include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/AppIcon.cmake")
z_embed_app_icon(${ENVIRONMENT_CHECK_TARGET})

# Per-monitor DPI v2. This one is not optional: the GUI registers its own
# WNDCLASSEX (EnvironmentCheckGui.cpp) instead of using
# ZSlate::FSlateStandaloneApp, so DeclareDpiAwareness() never runs on its path
# and the manifest is its only declaration -- without it the whole window is
# bitmap-stretched on a scaled display.
include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/AppManifest.cmake")
z_embed_app_manifest(${ENVIRONMENT_CHECK_TARGET})

# ZAppShell: header-only, so the /MT collector can use it even though it cannot
# link the /MD ZSlate. Provides the command-line split and the console attach
# that the CLI path needs.
target_include_directories(${ENVIRONMENT_CHECK_TARGET} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../Libraries/ZAppShell/include")

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES
    DllProbe.cpp
    DllProbe.h
    DllProbeProtocol.h
    EnvironmentCheckShell.cpp
    EnvironmentCheckGui.h
    EnvironmentCheckGui.cpp
    EnvironmentCheckCommand.cpp
    EnvironmentCheckCommand.h
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
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${ENVIRONMENT_CHECK_TARGET}>/profiles"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/profiles/generic.ini" "$<TARGET_FILE_DIR:${ENVIRONMENT_CHECK_TARGET}>/profiles/default.ini"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/profiles/osgame.ini" "$<TARGET_FILE_DIR:${ENVIRONMENT_CHECK_TARGET}>/profiles/osgame.ini"
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

# Output: <repo>/bin/Tools/<Config>/WindowsEnvironmentCheck/ -- its own level
# under the shared tools directory, because this tool is not one .exe: it is
# ZEnvironmentCollector.exe + ZDllLoadProbe.exe + profiles/ + LICENSE files, and
# dropping those straight into bin/Tools/<Config>/ leaves you unable to tell
# which of the four tools a stray .exe, .ini or LICENSE belongs to. The
# subdirectory is also the distribution unit: zip it and it runs.
#
# Standalone, BINARY_ROOT_DIR is undefined, so the helper derives the repo root
# by walking up to engine/CMakeLists.txt. ZDllLoadProbe goes to the SAME
# directory on purpose -- the collector re-launches it as a child process from
# its own executable directory (ToolExecutablePath()), and reads
# profiles/default.ini from there too.
#
# The name matches the source directory (engine/tools/WindowsEnvironmentCheck),
# not the executable, so bin/ and engine/tools/ line up one-to-one.
set(ENVIRONMENT_CHECK_OUTPUT_SUBDIR WindowsEnvironmentCheck)
include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/ToolOutputDir.cmake")
z_set_tool_output_dir(${ENVIRONMENT_CHECK_TARGET} ${DLL_PROBE_TARGET}
    SUBDIR "${ENVIRONMENT_CHECK_OUTPUT_SUBDIR}")

# install() mirrors the build layout: --prefix <dir> gives you the same folder
# you would have zipped out of bin/Tools/<Config>/.
install(TARGETS ${ENVIRONMENT_CHECK_TARGET} ${DLL_PROBE_TARGET} RUNTIME
        DESTINATION "${ENVIRONMENT_CHECK_OUTPUT_SUBDIR}" COMPONENT EnvironmentCollector)
install(FILES "${zydis_SOURCE_DIR}/LICENSE" DESTINATION "${ENVIRONMENT_CHECK_OUTPUT_SUBDIR}"
        RENAME LICENSE-Zydis.txt COMPONENT EnvironmentCollector)
install(FILES "${zycore_SOURCE_DIR}/LICENSE" DESTINATION "${ENVIRONMENT_CHECK_OUTPUT_SUBDIR}"
        RENAME LICENSE-Zycore.txt COMPONENT EnvironmentCollector)
install(FILES profiles/generic.ini DESTINATION "${ENVIRONMENT_CHECK_OUTPUT_SUBDIR}/profiles"
        RENAME default.ini COMPONENT EnvironmentCollector)
install(FILES profiles/osgame.ini DESTINATION "${ENVIRONMENT_CHECK_OUTPUT_SUBDIR}/profiles"
        COMPONENT EnvironmentCollector)
install(FILES LICENSE-stb.txt DESTINATION "${ENVIRONMENT_CHECK_OUTPUT_SUBDIR}" COMPONENT EnvironmentCollector)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../../3rdparty/minizip-ng/LICENSE"
        DESTINATION "${ENVIRONMENT_CHECK_OUTPUT_SUBDIR}" RENAME LICENSE-minizip-ng.txt
        COMPONENT EnvironmentCollector)

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
    if(ENVIRONMENT_CHECK_WITH_ZSLATE)
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
if(ENVIRONMENT_CHECK_WITH_ZSLATE)
    set(_shell_front_end "GUI + -batchmode CLI (ZSlate)")
else()
    set(_shell_front_end "-batchmode CLI only (no ZSlate)")
endif()
message(STATUS "ZEnvironmentCollector configured: ${_shell_front_end}; no ZRuntime, GameCore, or AVX compile flags")
