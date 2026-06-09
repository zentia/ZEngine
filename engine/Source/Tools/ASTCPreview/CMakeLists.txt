# =============================================================================
# ASTCPreview - Standalone ASTC Texture Preview Tool
# -----------------------------------------------------------------------------
# A standalone executable that previews ASTC-compressed textures.
# Uses ZRuntime's rendering and Slate for UI.
# Launched from the Inspector or as a standalone tool.
# =============================================================================

set(ASTC_PREVIEW_TARGET ASTCPreview)

# Source files
set(ASTC_PREVIEW_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ASTCPreviewWindow.cpp
)

set(ASTC_PREVIEW_HEADERS
    ${CMAKE_CURRENT_SOURCE_DIR}/ASTCPreviewWindow.h
)

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${ASTC_PREVIEW_SOURCES} ${ASTC_PREVIEW_HEADERS})

# Create executable
if(WIN32)
    add_executable(${ASTC_PREVIEW_TARGET} WIN32 ${ASTC_PREVIEW_SOURCES} ${ASTC_PREVIEW_HEADERS})
else()
    add_executable(${ASTC_PREVIEW_TARGET} ${ASTC_PREVIEW_SOURCES} ${ASTC_PREVIEW_HEADERS})
endif()

set_target_properties(${ASTC_PREVIEW_TARGET} PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    OUTPUT_NAME "ASTCPreview"
    FOLDER "Engine/Tools"
)

# Set output directories
z_set_config_output_dirs(${ASTC_PREVIEW_TARGET} "${BINARY_ROOT_DIR}" RUNTIME)

# Include directories
target_include_directories(${ASTC_PREVIEW_TARGET} PRIVATE
    ${ENGINE_ROOT_DIR}/Source
    ${ENGINE_ROOT_DIR}/Source/CommonPCH
)

# Link against ZRuntime
target_link_libraries(${ASTC_PREVIEW_TARGET} PRIVATE ZRuntime)

# Use precompiled headers
if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.16" AND TARGET ZEnginePCH)
    target_link_libraries(${ASTC_PREVIEW_TARGET} PRIVATE ZEnginePCH)
    add_dependencies(${ASTC_PREVIEW_TARGET} ZEnginePCH)
    target_precompile_headers(${ASTC_PREVIEW_TARGET} REUSE_FROM ZEnginePCH)
    message(STATUS "ASTCPreview: precompiled headers reused from ZEnginePCH")
endif()

# Platform-specific libraries
if(WIN32)
    target_link_libraries(${ASTC_PREVIEW_TARGET} PRIVATE comdlg32 shell32)
endif()

message(STATUS "ASTCPreview: target configured (standalone ASTC texture preview tool)")
