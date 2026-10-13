# =============================================================================
# LuaMemPreview — standalone viewer for Lua memory snapshot CSVs
#
# ZSlate (UI + D3D11 backend) only; no engine boot, no ZRuntime, and crucially
# no dependency on Source/Editor — every ZSlate *window* there is currently
# excluded from the build pending the UIRenderer->ISlateRenderer migration.
#
# Console subsystem on purpose (no WIN32 flag): the tool prints parse timings,
# row counts and load errors, which is exactly what you want when a snapshot
# fails to open.
# =============================================================================
set(LUA_MEM_PREVIEW_TARGET LuaMemPreview)

set(LUA_MEM_PREVIEW_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/LuaMemCsv.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/LuaMemZasset.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/LuaMemWidgets.cpp
)

set(LUA_MEM_PREVIEW_HEADERS
    ${CMAKE_CURRENT_SOURCE_DIR}/DpiScaleRenderer.h
    ${CMAKE_CURRENT_SOURCE_DIR}/LuaMemCsv.h
    ${CMAKE_CURRENT_SOURCE_DIR}/LuaMemZasset.h
    ${CMAKE_CURRENT_SOURCE_DIR}/LuaMemWidgets.h
    ${CMAKE_CURRENT_SOURCE_DIR}/UiRecorder.h
)

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LUA_MEM_PREVIEW_SOURCES} ${LUA_MEM_PREVIEW_HEADERS})

add_executable(${LUA_MEM_PREVIEW_TARGET} ${LUA_MEM_PREVIEW_SOURCES} ${LUA_MEM_PREVIEW_HEADERS})

set_target_properties(${LUA_MEM_PREVIEW_TARGET} PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    OUTPUT_NAME "LuaMemPreview"
    FOLDER "Engine/Tools"
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
# LuaMemZasset.cpp is the one exception: it includes no ZSlate header at all,
# and the engine headers it does pull (MemoryProfiler -> EASTL containers,
# VirtualFileSystemTypes -> std::filesystem) are written assuming pch.h is in
# effect, which is how every ZRuntime TU compiles. Force-including pch.h for
# that single file reproduces exactly that environment instead of guessing by
# hand which headers to pre-include.
set(LUA_MEM_PREVIEW_PCH "${ENGINE_ROOT_DIR}/Source/CommonPCH/pch.h")
if(MSVC)
    set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/LuaMemZasset.cpp
        PROPERTIES COMPILE_OPTIONS "/FI${LUA_MEM_PREVIEW_PCH}")
else()
    set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/LuaMemZasset.cpp
        PROPERTIES COMPILE_OPTIONS "-include;${LUA_MEM_PREVIEW_PCH}")
endif()

if(WIN32)
    target_link_libraries(${LUA_MEM_PREVIEW_TARGET} PRIVATE comdlg32 shell32 d3d11)
endif()

message(STATUS "LuaMemPreview: ZSlate (UI+D3D11) + ZRuntime (SerializedFile), Lua memory .zasset/.csv viewer")
