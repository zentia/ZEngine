# ZInsights -- the standalone ZEngine Insights viewer (Windows-only).
#
# Hosted entirely by ZSlate::FSlateStandaloneApp (window, DPI, pump, input
# routing and the D3D11 render backend). This exe contributes only the widget
# tree -- a status bar over the shared SInsightsTimeline flame chart, the same
# widget the in-editor Insights panel paints -- plus the trace loading. Links
# the STATIC ZRuntime for InsightsTrace + the Slate base types; no RHI / GLFW /
# scripting code is referenced, so the linker pulls only the timing-trace
# objects out of the archive.
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

# App icon: embeds resource/ZInsights.ico as IDI_ICON1 (drawn by
# engine/resource/gen_icon.ps1 -Motif flame). After the OUTPUT_NAME line, since
# the icon base name defaults to it. This alone only puts the icon in the
# executable -- the title bar / Alt-Tab / taskbar come from the window class,
# which FSlateStandaloneApp fills from desc.LargeIconResId / SmallIconResId.
z_embed_app_icon(${INSIGHTS_TARGET})

# Per-monitor DPI v2, declared in the manifest so it is in force before main()
# runs. The shell also declares it in code; the manifest just cannot lose to a
# window created too early. Without a declaration the window is DPI-virtualised
# (rendered at 96 dpi and bitmap-stretched) and DirectWrite text comes soft.
include("${ENGINE_ROOT_DIR}/cmake/AppManifest.cmake")
z_embed_app_manifest(${INSIGHTS_TARGET})

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${INSIGHTS_SOURCES} ${INSIGHTS_HEADERS})

target_include_directories(${INSIGHTS_TARGET} PRIVATE
    ${ENGINE_ROOT_DIR}/Source
    ${ENGINE_ROOT_DIR}/Source/CommonPCH
    ${ENGINE_ROOT_DIR}/Libraries/ZAppShell/include
    ${ENGINE_ROOT_DIR}/Libraries/ZSlate/include
)

# SInsightsTimeline derives from ZSlate::SLeafWidget and paints through
# ZSlate::ISlateRenderer (the shell's D3D11-backed batched renderer). ZRuntime links
# ZSlate PRIVATE, so neither its include dir nor its symbols propagate -- link
# it explicitly.
target_link_libraries(${INSIGHTS_TARGET} PUBLIC ZRuntime)
# Hard dependency, not a nicety: ZInsightsMain.cpp is built on
# ZSlate::FSlateStandaloneApp. ZSlate is defined by Source/Runtime, which the
# engine adds before this directory, so the target always exists in a full
# configure.
if(NOT TARGET ZSlate)
    message(FATAL_ERROR "ZInsights needs the ZSlate target (Win32 DPI helpers). "
                        "Configure through engine/CMakeLists.txt, not standalone.")
endif()
target_link_libraries(${INSIGHTS_TARGET} PRIVATE ZSlate)
target_link_libraries(${INSIGHTS_TARGET} PRIVATE ZEnginePCH)
add_dependencies(${INSIGHTS_TARGET} ZEnginePCH)

# d3d11: the shell's ZSlateD3D11Renderer (its own #pragma comment pulls in
# d3dcompiler). comdlg32 / shell32 for the open dialog and the drop target.
target_link_libraries(${INSIGHTS_TARGET} PRIVATE comdlg32 shell32 d3d11)

message(STATUS "ZInsights standalone viewer target configured (ZSlate + D3D11)")
