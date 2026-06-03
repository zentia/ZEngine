# ZInsights -- the standalone ZEngine Insights viewer (Windows-only).
#
# A small Win32 + Direct2D host that loads a .ztrace capture and renders it via
# the shared SInsightsTimeline flame-chart widget (the same widget the in-editor
# Insights panel paints). Links the STATIC ZRuntime for InsightsTrace + the Slate
# base types; no RHI / GLFW / scripting code is referenced, so the linker pulls
# only the timing-trace objects out of the archive.
#
# Direct2D is Windows-only by design; on other desktop platforms this target is
# simply skipped (the in-editor panel still works everywhere).

if(NOT WIN32)
    return()
endif()

set(INSIGHTS_TARGET ZInsights)

file(GLOB_RECURSE INSIGHTS_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
file(GLOB_RECURSE INSIGHTS_HEADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.h")

add_executable(${INSIGHTS_TARGET} WIN32 ${INSIGHTS_SOURCES} ${INSIGHTS_HEADERS})

set_target_properties(${INSIGHTS_TARGET} PROPERTIES CXX_STANDARD 23 OUTPUT_NAME "ZInsights")
set_target_properties(${INSIGHTS_TARGET} PROPERTIES FOLDER "Engine")
z_set_config_output_dirs(${INSIGHTS_TARGET} "${BINARY_ROOT_DIR}" RUNTIME)

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${INSIGHTS_SOURCES} ${INSIGHTS_HEADERS})

target_include_directories(${INSIGHTS_TARGET} PRIVATE
    ${ENGINE_ROOT_DIR}/Source
    ${ENGINE_ROOT_DIR}/Source/CommonPCH
)

target_link_libraries(${INSIGHTS_TARGET} PUBLIC ZRuntime)
target_link_libraries(${INSIGHTS_TARGET} PRIVATE ZEnginePCH)
add_dependencies(${INSIGHTS_TARGET} ZEnginePCH)

# Direct2D / DirectWrite + the common-dialog and shell helpers used by the host.
target_link_libraries(${INSIGHTS_TARGET} PRIVATE d2d1 dwrite comdlg32 shell32)

message(STATUS "ZInsights standalone viewer target configured (Win32 + Direct2D)")
