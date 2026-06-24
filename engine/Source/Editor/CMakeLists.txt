set(TARGET_NAME ZEditor)

# Determine editor platform-specific directory.
# Keep platform/interface in the common glob, but add exactly one platform implementation folder.
set(PLATFORM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Platform")
if(WIN32)
    set(PLATFORM_SUBDIR "${PLATFORM_DIR}/Windows")
    message(STATUS "Editor: Using Windows platform directory: ${PLATFORM_SUBDIR}")
elseif(APPLE)
    set(PLATFORM_SUBDIR "${PLATFORM_DIR}/MacOS")
    message(STATUS "Editor: Using macOS platform directory: ${PLATFORM_SUBDIR}")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(PLATFORM_SUBDIR "${PLATFORM_DIR}/Linux")
    message(STATUS "Editor: Using Linux platform directory: ${PLATFORM_SUBDIR}")
else()
    message(WARNING "Editor: Unsupported editor platform, platform implementation directory is disabled")
    set(PLATFORM_SUBDIR "")
endif()

file(GLOB_RECURSE EDITOR_HEADERS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.h)
file(GLOB_RECURSE EDITOR_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
if(APPLE)
    file(GLOB_RECURSE EDITOR_OBJC_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.m ${CMAKE_CURRENT_SOURCE_DIR}/*.mm)
    list(APPEND EDITOR_SOURCES ${EDITOR_OBJC_SOURCES})
endif()
list(FILTER EDITOR_HEADERS EXCLUDE REGEX ".*/[Pp]latform/([Ww]indows|[Mm]ac[Oo][Ss]|[Ll]inux)/.*")
list(FILTER EDITOR_SOURCES EXCLUDE REGEX ".*/[Pp]latform/([Ww]indows|[Mm]ac[Oo][Ss]|[Ll]inux)/.*")
# Standalone smoke-test executables (their own main()) live in .../Test/ dirs
# and must NOT be glommed into the ZEditor binary. They build via their own
# guarded targets (see add_subdirectory below).
list(FILTER EDITOR_SOURCES EXCLUDE REGEX ".*/AssetPipeline/.*/[Tt]est/.*")
file(GLOB EDITOR_RESOURCE CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/resource/ZEditor.rc)

# Collect platform-specific source files if platform directory exists
if(PLATFORM_SUBDIR AND EXISTS "${PLATFORM_SUBDIR}")
    file(GLOB_RECURSE PLATFORM_HEADERS CONFIGURE_DEPENDS "${PLATFORM_SUBDIR}/*.h")
    file(GLOB_RECURSE PLATFORM_SOURCES CONFIGURE_DEPENDS "${PLATFORM_SUBDIR}/*.cpp")
    
    # Also collect Objective-C/Objective-C++ files for Apple platforms if needed
    if(APPLE OR IOS OR ANDROID)
        file(GLOB_RECURSE PLATFORM_OBJC_SOURCES CONFIGURE_DEPENDS "${PLATFORM_SUBDIR}/*.m" "${PLATFORM_SUBDIR}/*.mm")
        list(APPEND PLATFORM_SOURCES ${PLATFORM_OBJC_SOURCES})
    endif()
    
    list(APPEND EDITOR_HEADERS ${PLATFORM_HEADERS})
    list(APPEND EDITOR_SOURCES ${PLATFORM_SOURCES})
    
    list(LENGTH PLATFORM_HEADERS PLATFORM_HEADER_COUNT)
    list(LENGTH PLATFORM_SOURCES PLATFORM_SOURCE_COUNT)
    message(STATUS "Editor: Found ${PLATFORM_HEADER_COUNT} platform header files")
    message(STATUS "Editor: Found ${PLATFORM_SOURCE_COUNT} platform source files")
elseif(PLATFORM_SUBDIR)
    message(WARNING "Editor: Platform directory does not exist: ${PLATFORM_SUBDIR}")
endif()

# Separate regular files from platform files for source_group
# Regular files are in CMAKE_CURRENT_SOURCE_DIR, platform files are in PLATFORM_SUBDIR
set(REGULAR_EDITOR_HEADERS ${EDITOR_HEADERS})
set(REGULAR_EDITOR_SOURCES ${EDITOR_SOURCES})

if(PLATFORM_SUBDIR AND EXISTS "${PLATFORM_SUBDIR}" AND PLATFORM_HEADERS AND PLATFORM_SOURCES)
    # Remove platform files from the regular files lists
    list(REMOVE_ITEM REGULAR_EDITOR_HEADERS ${PLATFORM_HEADERS})
    list(REMOVE_ITEM REGULAR_EDITOR_SOURCES ${PLATFORM_SOURCES})
endif()

# Organize regular files under editor directory (only if there are regular files)
if(REGULAR_EDITOR_HEADERS OR REGULAR_EDITOR_SOURCES OR EDITOR_RESOURCE)
    source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${REGULAR_EDITOR_HEADERS} ${REGULAR_EDITOR_SOURCES} ${EDITOR_RESOURCE})
endif()
# Organize platform files under platform directory
# Use PLATFORM_DIR as root so Visual Studio shows "Platform/Win" directory structure
if(PLATFORM_SUBDIR AND EXISTS "${PLATFORM_SUBDIR}" AND (PLATFORM_HEADERS OR PLATFORM_SOURCES))
    # CMake 4.x: use file(REAL_PATH) on both root and files so casing matches
    # the canonical on-disk form (avoids "Source" vs "source" prefix mismatch).
    file(REAL_PATH "${PLATFORM_DIR}" PLATFORM_DIR_ABS)
    set(_PLATFORM_FILES_NORMALIZED)
    foreach(_pf IN LISTS PLATFORM_HEADERS PLATFORM_SOURCES)
        file(REAL_PATH "${_pf}" _pf_real)
        list(APPEND _PLATFORM_FILES_NORMALIZED "${_pf_real}")
    endforeach()
    source_group(TREE "${PLATFORM_DIR_ABS}" FILES ${_PLATFORM_FILES_NORMALIZED})
    unset(_PLATFORM_FILES_NORMALIZED)
    unset(_pf)
    unset(_pf_real)
endif()
if (WIN32)
    add_executable(${TARGET_NAME} WIN32 ${EDITOR_HEADERS} ${EDITOR_SOURCES} ${EDITOR_RESOURCE})
else()
    add_executable(${TARGET_NAME} ${EDITOR_HEADERS} ${EDITOR_SOURCES} ${EDITOR_RESOURCE})
endif()

if(APPLE)
    if(EDITOR_OBJC_SOURCES)
        set_source_files_properties(${EDITOR_OBJC_SOURCES} PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    endif()
    target_link_libraries(${TARGET_NAME} PUBLIC "-framework Metal" "-framework QuartzCore" "-framework Cocoa")
endif()

if(CMAKE_GENERATOR MATCHES "Visual Studio" OR CMAKE_GENERATOR MATCHES "Visual Studio.*")
    file(GLOB_RECURSE RUNTIME_NATVIS_FILES CONFIGURE_DEPENDS "${ENGINE_ROOT_DIR}/Source/Runtime/*.natvis")
    if(RUNTIME_NATVIS_FILES)
        foreach(_natvis_file IN LISTS RUNTIME_NATVIS_FILES)
            set_source_files_properties(${_natvis_file} PROPERTIES VS_ITEM_TYPE "Natvis")
        endforeach()
        target_sources(${TARGET_NAME} PRIVATE ${RUNTIME_NATVIS_FILES})
        file(REAL_PATH "${ENGINE_ROOT_DIR}/Source/Runtime" _runtime_root)
        set(_RUNTIME_NATVIS_FILES_NORMALIZED)
        foreach(_nv IN LISTS RUNTIME_NATVIS_FILES)
            file(REAL_PATH "${_nv}" _nv_real)
            list(APPEND _RUNTIME_NATVIS_FILES_NORMALIZED "${_nv_real}")
        endforeach()
        source_group(TREE "${_runtime_root}" FILES ${_RUNTIME_NATVIS_FILES_NORMALIZED})
        unset(_RUNTIME_NATVIS_FILES_NORMALIZED)
        unset(_nv)
        unset(_nv_real)
    endif()
endif()

add_compile_definitions("Z_ROOT_DIR=${BINARY_ROOT_DIR}")

target_include_directories(
  ${TARGET_NAME} 
  PUBLIC $<BUILD_INTERFACE:${THIRD_PARTY_DIR}/stb>
)

target_include_directories(
  ${TARGET_NAME} 
  PUBLIC $<BUILD_INTERFACE:${THIRD_PARTY_DIR}/bqlog/include>
)

# ZSlate: editor UI framework (header-only, include path points at .../include)
target_include_directories(
  ${TARGET_NAME}
  PRIVATE $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/engine/Libraries/ZSlate/include>
)

# Add platform-specific include directory (if needed for Editor-specific platform files)
# Note: Runtime already adds the platform include directory, so this is only needed
# if Editor has its own platform-specific files separate from Runtime
if(PLATFORM_SUBDIR AND EXISTS "${PLATFORM_SUBDIR}")
    target_include_directories(
        ${TARGET_NAME}
        PUBLIC $<BUILD_INTERFACE:${PLATFORM_SUBDIR}>
    )
    message(STATUS "Editor: Added platform include directory: ${PLATFORM_SUBDIR}")
endif()

set_target_properties(${TARGET_NAME} PROPERTIES CXX_STANDARD 23 OUTPUT_NAME "ZEditor")
set_target_properties(${TARGET_NAME} PROPERTIES FOLDER "Engine")
z_set_config_output_dirs(ZEditor "${BINARY_ROOT_DIR}" RUNTIME)

target_compile_options(${TARGET_NAME} PUBLIC "$<$<COMPILE_LANG_AND_ID:CXX,MSVC>:$<IF:$<BOOL:${ZENGINE_ENABLE_WERROR}>,/WX,/WX->>")

# Configure precompiled headers (reuse shared ZEnginePCH)
if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.16")
    list(REMOVE_ITEM EDITOR_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/pch.cpp")
    target_link_libraries(${TARGET_NAME} PRIVATE ZEnginePCH)
    target_include_directories(${TARGET_NAME} PRIVATE ${ENGINE_ROOT_DIR}/Source/CommonPCH)
    add_dependencies(${TARGET_NAME} ZEnginePCH)
    target_precompile_headers(${TARGET_NAME} REUSE_FROM ZEnginePCH)
    message(STATUS "Precompiled headers enabled for ${TARGET_NAME} (REUSE_FROM ZEnginePCH)")
else()
    message(WARNING "Precompiled headers require CMake 3.16 or higher. Current version: ${CMAKE_VERSION}")
endif()

# Link to stb for stbi functions
# Note: STB_IMAGE_IMPLEMENTATION is already defined in ZRuntime's render_resource_base.cpp
# so we don't need to define it here to avoid duplicate symbol errors
target_link_libraries(${TARGET_NAME} PUBLIC stb)

target_link_libraries(${TARGET_NAME} PUBLIC ZRuntime)
target_link_libraries(${TARGET_NAME} PUBLIC tinyobjloader)

# ufbx: single-file FBX reader (editor-only mesh import).
set(ZENGINE_UFBX_DIR "${ENGINE_ROOT_DIR}/3rdparty/ufbx")
if(EXISTS "${ZENGINE_UFBX_DIR}/ufbx.c")
    add_library(ufbx STATIC "${ZENGINE_UFBX_DIR}/ufbx.c")
    target_include_directories(ufbx PUBLIC "${ZENGINE_UFBX_DIR}")
    set_target_properties(ufbx PROPERTIES FOLDER "ThirdParty/ufbx")
    target_compile_definitions(${TARGET_NAME} PRIVATE ZENGINE_HAS_UFBX=1)
    target_link_libraries(${TARGET_NAME} PRIVATE ufbx)
    message(STATUS "Editor: ufbx FBX mesh import enabled")
else()
    message(WARNING "Editor: ufbx not found at ${ZENGINE_UFBX_DIR}; FBX import disabled (OBJ still works)")
endif()

# ---------------------------------------------------------------------------
# ShaderLab side-target.
#
# Editor sources (inspector_window, content_browser, shader_preview_renderer)
# unconditionally call ShaderLabParser::ShaderLabParser / ShaderLabParser::Parse.
#
# - On Vulkan-capable hosts, ShaderLab/*.cpp is compiled into ZRuntime via
#   GLOB_RECURSE, so the symbols come from ZRuntime above. Nothing extra
#   to do.
# - On Web (Emscripten) / Apple / Windows-without-Vulkan, ZRuntime EXCLUDE's
#   ShaderLab/*.cpp because shader_lab_compiler.{h,cpp} pulls in the
#   Vulkan shader_compiler header. engine/source/runtime/CMakeLists.txt
#   compensates by add_subdirectory(Function/ShaderLab) which produces a
#   parser-only ShaderLab STATIC. Link it here under the same condition.
# ---------------------------------------------------------------------------
if(TARGET ShaderLab)
    target_link_libraries(${TARGET_NAME} PUBLIC ShaderLab)
    message(STATUS "Editor: linking ShaderLab side-target (parser-only Vulkan-free build)")
endif()

if(TARGET render_doc)
    target_link_libraries(${TARGET_NAME} PRIVATE render_doc)
endif()

# Link to OpenGL
if(WIN32)
    # Windows: Need to link opengl32.lib for OpenGL functions
    target_link_libraries(${TARGET_NAME} PUBLIC opengl32)
elseif(APPLE)
    find_library(OPENGL_LIBRARY OpenGL REQUIRED)
    target_link_libraries(${TARGET_NAME} PUBLIC ${OPENGL_LIBRARY})
elseif(UNIX)
    find_package(OpenGL REQUIRED)
    target_link_libraries(${TARGET_NAME} PUBLIC OpenGL::GL)
endif()

# ZEditor is ImGui-free: all editor chrome renders through native ZSlate. The
# former `target_link_libraries(... imgui)` + imgui/backends include dir were
# removed once the last dead ImGui includes were stripped from the editor TUs.

# Link to minizip-ng (PR #8: XlsxImporter cracks open .xlsx ZIP archives).
# Editor-only; ZRuntime does NOT link it so player builds stay free of the
# ZIP / zlib payload. The 3rdparty CMake already strips minizip down to
# zlib-only so we don't drag in BZIP2 / LZMA / OpenSSL.
# 
# NOTE: We MUST link the library (not just add include dirs), otherwise
# XlsxImporter.obj will have unresolved external symbols (LNK2019).
# 
# Force link minizip-ng (always, not conditional on TARGET existence).
# If minizip-ng was not built, this will cause a CMake error at link time,
# which is better than a silent LNK2019.
target_link_libraries(${TARGET_NAME} PRIVATE minizip-ng)
message(STATUS "Editor: FORCE-linked minizip-ng (ZIP archive support for XlsxImporter)")

# Always add minizip-ng include directory so XlsxImporter.cpp can find mz.h.
# The minizip-ng CMake installs headers to <build>/engine/3rdparty/minizip-ng/,
# but the source tree also has mz.h in the root of the submodule.
# We add BOTH the source directory and the build directory to be safe.
target_include_directories(${TARGET_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/engine/3rdparty/minizip-ng
    ${CMAKE_BINARY_DIR}/engine/3rdparty/minizip-ng
)

# Texture-cook block encoders (editor-only): bc7enc (BC1/3/7) + astc-encoder
# (ASTC LDR). Consumed by TextureCompressor.cpp. PRIVATE -- cooking is an
# editor-side step; ZRuntime never links these.
if(TARGET bc7enc)
    target_link_libraries(${TARGET_NAME} PRIVATE bc7enc)
endif()
if(TARGET astcenc)
    target_link_libraries(${TARGET_NAME} PRIVATE astcenc)
endif()

if(WIN32 AND ZENGINE_USE_VULKAN AND (CMAKE_CXX_COMPILER_ID STREQUAL "MSVC" OR (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")))
    # Windows editor uses DX12 by default. Keep Vulkan support available for optional
    # code paths, but do not require vulkan-1.dll to exist before the process starts.
    target_link_libraries(${TARGET_NAME} PRIVATE delayimp)
    target_link_options(${TARGET_NAME} PRIVATE /DELAYLOAD:vulkan-1.dll)
endif()

message(STATUS "Z_ROOT_DIR:${Z_ROOT_DIR}")

# Opt-in editor-side smoke tests (own main(); excluded from the ZEditor glob above).
add_subdirectory(AssetPipeline/TextureImporter/Test)

set(_EDITOR_CONFIG_BIN "${BINARY_ROOT_DIR}/$<CONFIG>")
set(POST_BUILD_COMMANDS
  COMMAND ${CMAKE_COMMAND} -E make_directory "${_EDITOR_CONFIG_BIN}"
  COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:ZRuntime>" "$<TARGET_FILE_DIR:${TARGET_NAME}>/"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "$<TARGET_FILE_DIR:${TARGET_NAME}>/" "${_EDITOR_CONFIG_BIN}"
  COMMAND ${CMAKE_COMMAND} -E copy "${ENGINE_ROOT_DIR}/${DEPLOY_CONFIG_DIR}/${TARGET_NAME}.ini" "${_EDITOR_CONFIG_BIN}"
  COMMAND ${CMAKE_COMMAND} -E copy "${ENGINE_ROOT_DIR}/${DEVELOP_CONFIG_DIR}/${TARGET_NAME}.ini" "$<TARGET_FILE_DIR:${TARGET_NAME}>/"
)
# Config/asset/jolt copy is done once by CopyEngineDataToOutput (engine/CMakeLists.txt).
if(WIN32)
    set(_RENDERDOC_MS_CFG "$<IF:$<STREQUAL:$<CONFIG>,Release>,Release,Development>")
    list(APPEND POST_BUILD_COMMANDS
        COMMAND ${CMAKE_COMMAND}
            -DRENDERDOC_SRC_DIR=${Z_ROOT_DIR}/tools/renderdoc/x64/${_RENDERDOC_MS_CFG}
            -DRENDERDOC_DST_DIR=$<TARGET_FILE_DIR:${TARGET_NAME}>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CopyRenderDocRuntime.cmake
        COMMAND ${CMAKE_COMMAND}
            -DDXC_RUNTIME_DST_DIR=$<TARGET_FILE_DIR:${TARGET_NAME}>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CopyDxCompilerRuntime.cmake
    )
endif()
add_custom_command(TARGET ${TARGET_NAME} POST_BUILD ${POST_BUILD_COMMANDS})

# Conditionally copy resource directory if it exists
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/resource")
    add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/resource" "${_EDITOR_CONFIG_BIN}/resource"
        COMMENT "Copying Editor resources..."
    )
endif()

#precompile
#set global vari used by precompile
set(Z_EDITOR_HEADS “${EDITOR_HEADERS}” PARENT_SCOPE)
