# =============================================================================
# TexPreview - Standalone Block-Compressed Texture Preview Tool
# -----------------------------------------------------------------------------
# A standalone executable that previews ASTC and BC7 compressed textures.
# Uses ZRuntime's rendering and Slate for UI.
# Launched from the Inspector or as a standalone tool.
# =============================================================================

set(TEX_PREVIEW_TARGET TexPreview)

# Source files
set(TEX_PREVIEW_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/TexPreviewWindow.cpp
)

set(TEX_PREVIEW_HEADERS
    ${CMAKE_CURRENT_SOURCE_DIR}/TexPreviewWindow.h
)

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${TEX_PREVIEW_SOURCES} ${TEX_PREVIEW_HEADERS})

# Create executable
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

# Set output directories
z_set_config_output_dirs(${TEX_PREVIEW_TARGET} "${BINARY_ROOT_DIR}" RUNTIME)

# Include directories
target_include_directories(${TEX_PREVIEW_TARGET} PRIVATE
    ${ENGINE_ROOT_DIR}/Source
    ${ENGINE_ROOT_DIR}/Source/CommonPCH
)

# Link against ZRuntime
target_link_libraries(${TEX_PREVIEW_TARGET} PRIVATE ZRuntime)

# Use precompiled headers
if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.16" AND TARGET ZEnginePCH)
    target_link_libraries(${TEX_PREVIEW_TARGET} PRIVATE ZEnginePCH)
    add_dependencies(${TEX_PREVIEW_TARGET} ZEnginePCH)
    target_precompile_headers(${TEX_PREVIEW_TARGET} REUSE_FROM ZEnginePCH)
    message(STATUS "TexPreview: precompiled headers reused from ZEnginePCH")
endif()

# Platform-specific libraries
if(WIN32)
    target_link_libraries(${TEX_PREVIEW_TARGET} PRIVATE comdlg32 shell32)
endif()

message(STATUS "TexPreview: target configured (standalone block-compressed texture preview tool)")
