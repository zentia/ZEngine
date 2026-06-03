# The standalone ImGui-based ZProfiler GUI was removed when ImGui was dropped
# from the engine. Only the headless CSV exporter survives here: it reads a
# captured profiler .zasset and writes CSV(s), linking ZRuntime with no UI /
# GLFW / ImGui dependency.

set(PROFILER_CSV_EXPORT_TARGET ZProfilerCsvExporter)

file(GLOB_RECURSE PROFILER_COMMANDLINE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/CommandLine/*.cpp")
file(GLOB_RECURSE PROFILER_EXPORT_HEADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Export/*.h")
file(GLOB_RECURSE PROFILER_EXPORT_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Export/*.cpp")

add_executable(${PROFILER_CSV_EXPORT_TARGET}
    ${PROFILER_COMMANDLINE_SOURCES}
    ${PROFILER_EXPORT_HEADERS}
    ${PROFILER_EXPORT_SOURCES}
)
set_target_properties(${PROFILER_CSV_EXPORT_TARGET} PROPERTIES CXX_STANDARD 23 OUTPUT_NAME "ZProfilerCsvExporter")
set_target_properties(${PROFILER_CSV_EXPORT_TARGET} PROPERTIES FOLDER "Engine")
z_set_config_output_dirs(${PROFILER_CSV_EXPORT_TARGET} "${BINARY_ROOT_DIR}" RUNTIME)

target_link_libraries(${PROFILER_CSV_EXPORT_TARGET} PUBLIC ZRuntime)
target_link_libraries(${PROFILER_CSV_EXPORT_TARGET} PRIVATE ZEnginePCH)
target_include_directories(${PROFILER_CSV_EXPORT_TARGET} PRIVATE ${ENGINE_ROOT_DIR}/Source ${ENGINE_ROOT_DIR}/Source/CommonPCH)
add_dependencies(${PROFILER_CSV_EXPORT_TARGET} ZEnginePCH)

if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC" OR (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"))
    target_compile_options(${PROFILER_CSV_EXPORT_TARGET} PRIVATE
        $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/W4>
        $<$<AND:$<COMPILE_LANG_AND_ID:CXX,MSVC>,$<BOOL:${ZENGINE_ENABLE_WERROR}>>:/WX>
        $<$<AND:$<COMPILE_LANG_AND_ID:CXX,MSVC>,$<BOOL:${ZENGINE_ENABLE_WERROR}>>:/we4716>
    )
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${PROFILER_CSV_EXPORT_TARGET} PRIVATE -Wreturn-type -Werror=return-type)
endif()
