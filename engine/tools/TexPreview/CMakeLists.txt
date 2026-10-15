# =============================================================================
# TexPreview — ZSlate UI + D3D11 rendering, ZRuntime for decompressors only
# =============================================================================
set(TEX_PREVIEW_TARGET TexPreview)

set(TEX_PREVIEW_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/TexPreviewWindow.cpp
)

set(TEX_PREVIEW_HEADERS
    ${CMAKE_CURRENT_SOURCE_DIR}/TexPreviewWindow.h
)

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${TEX_PREVIEW_SOURCES} ${TEX_PREVIEW_HEADERS})

if(WIN32)
    add_executable(${TEX_PREVIEW_TARGET} WIN32 ${TEX_PREVIEW_SOURCES} ${TEX_PREVIEW_HEADERS})
else()
    add_executable(${TEX_PREVIEW_TARGET} ${TEX_PREVIEW_SOURCES} ${TEX_PREVIEW_HEADERS})
endif()

set_target_properties(${TEX_PREVIEW_TARGET} PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    OUTPUT_NAME "TexPreview"
    FOLDER "Engine/Tools"
)

z_set_tool_output_dir(${TEX_PREVIEW_TARGET})

# App icon: embeds resource/TexPreview.ico as IDI_ICON1 and defines IDI_ICON1 for
# every TU. No-op off Windows / when RC is unavailable / before the .ico exists.
z_embed_app_icon(${TEX_PREVIEW_TARGET})

target_include_directories(${TEX_PREVIEW_TARGET} PRIVATE
    ${ENGINE_ROOT_DIR}/Source
    ${ENGINE_ROOT_DIR}/Source/CommonPCH
    ${ENGINE_ROOT_DIR}/Libraries/ZSlate/include
)

# ZSlate: UI framework + D3D11 render backend
# ZRuntime: decompressor symbols only (ASTC/BC7/ETC2), no engine boot
target_link_libraries(${TEX_PREVIEW_TARGET} PRIVATE ZSlate ZRuntime)

# No ZEnginePCH — PCH may include windows.h which pollutes DrawText before
# ZSlate headers can #undef it.

if(WIN32)
    target_link_libraries(${TEX_PREVIEW_TARGET} PRIVATE comdlg32 shell32 d3d11)
endif()

message(STATUS "TexPreview: ZSlate (UI+D3D11) + ZRuntime (decompressors only, no engine boot)")
