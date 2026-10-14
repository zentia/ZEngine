# =============================================================================
# LuaMemPreview — standalone viewer for Lua memory snapshot CSVs
#
# Also the MemoryProfiler .zasset -> CSV exporter: `LuaMemPreview -batchmode
# <in.zasset> [out_base]` writes _csharp/_userdata/_lua.csv headlessly. This
# replaces the former Source/Profiler (ZProfilerCsvExporter) target.
#
# ZSlate (UI + D3D11 backend) only; no engine boot, no ZRuntime, and crucially
# no dependency on Source/Editor — every ZSlate *window* there is currently
# excluded from the build pending the UIRenderer->ISlateRenderer migration.
#
# GUI subsystem (WIN32_EXECUTABLE + /ENTRY:mainCRTStartup): the headless modes
# attach to the console they were started from, so their parse timings, row
# counts and load errors still show up without a window of their own.
# =============================================================================
set(LUA_MEM_PREVIEW_TARGET LuaMemPreview)

set(LUA_MEM_PREVIEW_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/LuaMemCsv.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/LuaMemZasset.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/LuaMemCsvExport.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/LuaMemLog.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/LuaMemTypeDoc.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/LuaMemWidgets.cpp
)

# FSlateDpiScaleRenderer / FSlateRecordingRenderer used to live here; they are
# now ZSlate library headers (ZSlate/Renderer, ZSlate/Testing).
set(LUA_MEM_PREVIEW_HEADERS
    ${CMAKE_CURRENT_SOURCE_DIR}/LuaMemCsv.h
    ${CMAKE_CURRENT_SOURCE_DIR}/LuaMemZasset.h
    ${CMAKE_CURRENT_SOURCE_DIR}/LuaMemCsvExport.h
    ${CMAKE_CURRENT_SOURCE_DIR}/LuaMemLog.h
    ${CMAKE_CURRENT_SOURCE_DIR}/LuaMemTypeDoc.h
    ${CMAKE_CURRENT_SOURCE_DIR}/LuaMemWidgets.h
)

# App icon. rc.exe resolves the .ico next to the .rc, so both live in
# resource/. MSVC-only because .rc is a Windows resource script; the tool is
# Win32-only anyway (comdlg32, shell32, d3d11).
if(MSVC)
    list(APPEND LUA_MEM_PREVIEW_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/resource/LuaMemPreview.rc)
endif()

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LUA_MEM_PREVIEW_SOURCES} ${LUA_MEM_PREVIEW_HEADERS})

add_executable(${LUA_MEM_PREVIEW_TARGET} ${LUA_MEM_PREVIEW_SOURCES} ${LUA_MEM_PREVIEW_HEADERS})

set_target_properties(${LUA_MEM_PREVIEW_TARGET} PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    OUTPUT_NAME "LuaMemPreview"
    FOLDER "Engine/Tools"
    # No console window behind the Slate window. WIN32_EXECUTABLE implies
    # /SUBSYSTEM:WINDOWS, so the entry point is WinMain - same as ZEditor /
    # ZInsights / TexPreview. The headless modes read their arguments from the
    # CRT globals (__argv / __argc) and attach to the caller's console at
    # startup (main.cpp: EnsureConsole) so their output is still visible.
    WIN32_EXECUTABLE TRUE
)

z_set_config_output_dirs(${LUA_MEM_PREVIEW_TARGET} "${BINARY_ROOT_DIR}" RUNTIME)

target_include_directories(${LUA_MEM_PREVIEW_TARGET} PRIVATE
    ${ENGINE_ROOT_DIR}/Source
    ${ENGINE_ROOT_DIR}/Libraries/ZSlate/include
)

# ZRuntime: reads the MemoryProfiler `.zasset` through SerializedFile. Core
# systems only (RegisterCore) -- no engine boot, no window, no RHI, same as
# Runtime/Core/Serialize/Test/SchemaEvolutionSmokeTest.cpp. TexPreview already
# proves ZSlate + ZRuntime coexist in one tool.
#
# ZEnginePCH is linked for its PUBLIC include dirs only (EASTL, Source/Runtime,
# bqlog, ...). Its target_precompile_headers is PRIVATE, so linking does NOT
# force the PCH onto our TUs - which is what we want, see the note below.
target_link_libraries(${LUA_MEM_PREVIEW_TARGET} PRIVATE ZSlate ZRuntime ZEnginePCH)

# No target-wide PCH — pch.h pulls in windows.h, which #defines DrawText and
# would clash with ISlateRenderer::DrawTextLabel's neighbours before ZSlate can
# #undef it.
#
# LuaMemZasset.cpp / LuaMemCsvExport.cpp / LuaMemLog.cpp are the exceptions:
# they include no ZSlate header at all, and the engine headers they do pull
# (MemoryProfiler -> EASTL containers, VirtualFileSystemTypes ->
# std::filesystem, LogSystem -> Macro.h's LOG_* ) are written assuming pch.h is
# in effect, which is how every ZRuntime TU compiles. Force-including pch.h for
# those files reproduces exactly that environment instead of guessing by hand
# which headers to pre-include.
set(LUA_MEM_PREVIEW_PCH "${ENGINE_ROOT_DIR}/Source/CommonPCH/pch.h")
set(LUA_MEM_PREVIEW_ENGINE_TUS
    ${CMAKE_CURRENT_SOURCE_DIR}/LuaMemZasset.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/LuaMemCsvExport.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/LuaMemLog.cpp
)
if(MSVC)
    set_source_files_properties(${LUA_MEM_PREVIEW_ENGINE_TUS}
        PROPERTIES COMPILE_OPTIONS "/FI${LUA_MEM_PREVIEW_PCH}")
else()
    set_source_files_properties(${LUA_MEM_PREVIEW_ENGINE_TUS}
        PROPERTIES COMPILE_OPTIONS "-include;${LUA_MEM_PREVIEW_PCH}")
endif()

if(WIN32)
    target_link_libraries(${LUA_MEM_PREVIEW_TARGET} PRIVATE comdlg32 shell32 d3d11)
endif()

message(STATUS "LuaMemPreview: ZSlate (UI+D3D11) + ZRuntime (SerializedFile), Lua memory .zasset/.csv viewer")
