set(ENGINE_ROOT_DIR "${Z_ROOT_DIR}/engine")
set(ENGINE_TOOL_DIR "${Z_ROOT_DIR}/tools")
set(THIRD_PARTY_DIR "${ENGINE_ROOT_DIR}/3rdparty")
set(ENGINE_ASSET_DIR "/asset")
set(ENGINE_RESOURCE_DIR "/resource")

set(DEPLOY_CONFIG_DIR "configs/deployment")
set(DEVELOP_CONFIG_DIR "configs/development")

option(ENABLE_PHYSICS_DEBUG_RENDERER "Enable Physics Debug Renderer" OFF)

# Select rendering backend per platform.
#   - Apple (macOS / iOS)            : Metal
#   - Android / OHOS                 : Vulkan
#   - Windows                        : DirectX 12 (default runtime selection),
#                                      Vulkan compiled in as a switchable
#                                      first-class backend
#   - Linux desktop                  : Vulkan
#   - Emscripten / Web               : WebGL2 (no Vulkan, no Metal, no DX12)
# On Windows both Z_HAS_DX12 and Z_HAS_VULKAN are defined by default so that
# RHIFactory can construct either backend at runtime; pickDefaultGraphicsAPI()
# still returns DirectX12 on _WIN32. Vulkan can be opted out on Windows by
# passing -DZENGINE_USE_VULKAN=OFF (e.g. for slimmer CI builds without the
# Vulkan SDK installed).
if(EMSCRIPTEN OR CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
  # Web builds use the WebGL2 RHI and must not pull in any native graphics SDK.
  # Force Vulkan/Metal off here so Z_HAS_VULKAN never gets defined and the
  # Vulkan/Metal headers never get parsed (they cannot compile under Emscripten:
  # e.g. VK_DEFINE_NON_DISPATCHABLE_HANDLE expands to uint64_t and breaks
  # the {nullptr} brace-init pattern used throughout vulkan_rhi.h).
  set(ZENGINE_USE_METAL  OFF CACHE BOOL "Use Metal rendering backend"  FORCE)
  set(ZENGINE_USE_VULKAN OFF CACHE BOOL "Use Vulkan rendering backend" FORCE)
  message(STATUS "Engine: Emscripten target detected; forcing Vulkan/Metal OFF (WebGL2 only)")
elseif(APPLE)
  set(ZENGINE_USE_METAL ON CACHE BOOL "Use Metal rendering backend" FORCE)
  set(ZENGINE_USE_VULKAN OFF CACHE BOOL "Use Vulkan rendering backend" FORCE)
elseif(ANDROID OR Z_PLATFORM_OHOS OR OHOS OR CMAKE_SYSTEM_NAME STREQUAL "OHOS")
  set(ZENGINE_USE_METAL OFF CACHE BOOL "Use Metal rendering backend" FORCE)
  set(ZENGINE_USE_VULKAN ON CACHE BOOL "Use Vulkan rendering backend" FORCE)
elseif(WIN32)
  set(ZENGINE_USE_METAL OFF CACHE BOOL "Use Metal rendering backend" FORCE)
  # Windows ships DirectX 12 as the default runtime backend, but Vulkan is
  # also a fully supported, switchable backend on Windows. Default ON so
  # `Z_HAS_VULKAN` is defined and `VulkanRHI` is compiled in.
  option(ZENGINE_USE_VULKAN "Use Vulkan rendering backend" ON)
else()
  set(ZENGINE_USE_METAL OFF CACHE BOOL "Use Metal rendering backend" FORCE)
  set(ZENGINE_USE_VULKAN ON CACHE BOOL "Use Vulkan rendering backend" FORCE)
endif()

# On Windows/MSVC, Vulkan SDK's prebuilt glslang/SPIRV static libraries can be built
# against a newer STL than the local toolset. Prefer the external glslangValidator
# executable there unless the caller explicitly opts back into direct static linking.
# Apple/Metal builds do not link Vulkan SDK shader libraries.
if(APPLE OR (WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "MSVC") OR Z_PLATFORM_OHOS OR EMSCRIPTEN OR CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
  set(_ZENGINE_LINK_VULKAN_SDK_GLSLANG_LIBS_DEFAULT OFF)
else()
  set(_ZENGINE_LINK_VULKAN_SDK_GLSLANG_LIBS_DEFAULT ON)
endif()
option(
  ZENGINE_LINK_VULKAN_SDK_GLSLANG_LIBS
  "Link Vulkan SDK glslang/SPIRV static libraries for in-process runtime shader compilation"
  ${_ZENGINE_LINK_VULKAN_SDK_GLSLANG_LIBS_DEFAULT})
unset(_ZENGINE_LINK_VULKAN_SDK_GLSLANG_LIBS_DEFAULT)

# Allow turning warnings-as-errors on/off (default OFF for easier local builds)
option(ZENGINE_ENABLE_WERROR "Treat warnings as errors (enable /WX on MSVC)" OFF)

set(_ZENGINE_MOBILE_PLATFORM OFF)
if(ANDROID OR Z_PLATFORM_OHOS OR OHOS OR CMAKE_SYSTEM_NAME STREQUAL "OHOS")
  set(_ZENGINE_MOBILE_PLATFORM ON)
endif()
option(ZENGINE_MOBILE_SIZE_OPTIMIZE "Optimize mobile shared libraries for smaller package size" ${_ZENGINE_MOBILE_PLATFORM})
option(ZENGINE_MOBILE_HIDE_INTERNAL_SYMBOLS "Hide non-exported mobile runtime symbols" ${_ZENGINE_MOBILE_PLATFORM})
unset(_ZENGINE_MOBILE_PLATFORM)
option(ZENGINE_WINDOWS_SIZE_OPTIMIZE "Optimize Windows shared runtime for smaller package size" OFF)


# only support physics debug render at windows platform
if(NOT WIN32)
  if(ENABLE_PHYSICS_DEBUG_RENDERER)
    message(WARNING "Disable Physics Debug Renderer")
    set(ENABLE_PHYSICS_DEBUG_RENDERER OFF CACHE BOOL "" FORCE)
  endif()
endif()

if(ENABLE_PHYSICS_DEBUG_RENDERER)
  set(JOLT_ASSET_DIR "/jolt-asset")
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
  add_compile_options("/MP")
  set_property(DIRECTORY ${CMAKE_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT ZEditor)
  if(ZENGINE_WINDOWS_SIZE_OPTIMIZE)
    add_compile_options(
      "$<$<CONFIG:Release>:/O1>"
      "$<$<CONFIG:MinSizeRel>:/O1>"
      "$<$<CONFIG:Release>:/Gy>"
      "$<$<CONFIG:MinSizeRel>:/Gy>"
      "$<$<CONFIG:Release>:/Gw>"
      "$<$<CONFIG:MinSizeRel>:/Gw>"
      "$<$<CONFIG:Release>:/Zc:inline>"
      "$<$<CONFIG:MinSizeRel>:/Zc:inline>"
    )
    add_link_options(
      "$<$<CONFIG:Release>:/OPT:REF>"
      "$<$<CONFIG:MinSizeRel>:/OPT:REF>"
      "$<$<CONFIG:Release>:/OPT:ICF>"
      "$<$<CONFIG:MinSizeRel>:/OPT:ICF>"
      "$<$<CONFIG:Release>:/INCREMENTAL:NO>"
      "$<$<CONFIG:MinSizeRel>:/INCREMENTAL:NO>"
    )
    message(STATUS "Windows: Enabled size optimization flags (/O1, /Gy, /Gw, /OPT:REF, /OPT:ICF)")
  endif()
  # Enable warning level 4 (includes missing return value warnings)
  add_compile_options("/W4")
  # Treat warnings as errors (optional)
  if(ZENGINE_ENABLE_WERROR)
    add_compile_options("/WX")
    # Specifically treat missing return value warning (C4716) as error
    add_compile_options("/we4716")
    message(STATUS "MSVC: Enabled /W4 /WX /we4716 (warnings as errors, including missing return values)")
  else()
    message(STATUS "MSVC: Enabled /W4 (warnings as errors disabled, set -DZENGINE_ENABLE_WERROR=ON to enable)")
  endif()
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  # Enable return-type warning
  add_compile_options("-Wreturn-type")
  # Treat return-type warning as error
  add_compile_options("-Werror=return-type")
  if(Z_PLATFORM_OHOS OR OHOS OR CMAKE_SYSTEM_NAME STREQUAL "OHOS")
    add_compile_options("-Wno-unused-command-line-argument")
  endif()
  if(ZENGINE_MOBILE_SIZE_OPTIMIZE AND (ANDROID OR Z_PLATFORM_OHOS OR OHOS OR CMAKE_SYSTEM_NAME STREQUAL "OHOS"))
    add_compile_options(
      "$<$<CONFIG:Release>:-Oz>"
      "$<$<CONFIG:MinSizeRel>:-Oz>"
      -ffunction-sections
      -fdata-sections
    )
    if(ZENGINE_MOBILE_HIDE_INTERNAL_SYMBOLS)
      add_compile_options(-fvisibility=hidden -fvisibility-inlines-hidden)
    endif()
    add_link_options(
      "$<$<CONFIG:Release>:-Wl,--gc-sections>"
      "$<$<CONFIG:MinSizeRel>:-Wl,--gc-sections>"
    )
    if(ANDROID)
      add_link_options(
        "$<$<CONFIG:Release>:-Wl,--icf=safe>"
        "$<$<CONFIG:MinSizeRel>:-Wl,--icf=safe>"
      )
    endif()
    message(STATUS "${Z_PLATFORM_NAME}: Enabled mobile size optimization flags (-Oz, section GC; ICF on Android)")
  endif()
  message(STATUS "${CMAKE_CXX_COMPILER_ID}: Enabled -Wreturn-type -Werror=return-type (missing return values will cause compilation errors)")
endif()

# Same CPU arch for all targets (PCH + TUs) so clang-cl PCH reuse works. Jolt also uses AVX2.
# Interface lib ensures ZEnginePCH gets /arch:AVX2 via target_link_libraries (generator-friendly).
add_library(EngineArchAVX2 INTERFACE)
if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
  if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    add_compile_options("/arch:AVX2")
    target_compile_options(EngineArchAVX2 INTERFACE /arch:AVX2)
    message(STATUS "Windows x64: /arch:AVX2 enabled for all targets (PCH and consumers)")
  endif()
endif()

if(ZENGINE_USE_VULKAN)
  # Prefer LunarG VULKAN_SDK when set; on Linux desktop fall back to distro packages
  # (libvulkan-dev + glslang-tools) so CI and local apt-based builds configure cleanly.
  set(_ZENGINE_VULKAN_USE_SDK_LAYOUT ON)
  if(DEFINED ENV{VULKAN_SDK} AND NOT "$ENV{VULKAN_SDK}" STREQUAL "")
    set(VULKAN_SDK_ROOT "$ENV{VULKAN_SDK}")
    message(STATUS "Using Vulkan SDK from environment variable: ${VULKAN_SDK_ROOT}")
  elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT ANDROID AND NOT Z_PLATFORM_OHOS)
    set(_ZENGINE_VULKAN_USE_SDK_LAYOUT OFF)
    find_package(Vulkan REQUIRED)
    find_program(glslangValidator_EXECUTABLE glslangValidator)
    set(glslangValidator_executable ${glslangValidator_EXECUTABLE})
    set(vulkan_include ${Vulkan_INCLUDE_DIRS})
    if(TARGET Vulkan::Vulkan)
      set(vulkan_lib Vulkan::Vulkan)
    else()
      set(vulkan_lib ${Vulkan_LIBRARIES})
    endif()
    set(VULKAN_SDK_ROOT "")
    file(TO_CMAKE_PATH "/usr/bin" vk_layer_path_definition)
    add_compile_definitions("Z_VK_LAYER_PATH=${vk_layer_path_definition}")
    set(ZENGINE_LINK_VULKAN_SDK_GLSLANG_LIBS OFF CACHE BOOL
        "Link Vulkan SDK glslang/SPIRV static libraries for in-process runtime shader compilation"
        FORCE)
    message(STATUS "Linux: using system Vulkan (include=${Vulkan_INCLUDE_DIRS})")
  else()
    message(FATAL_ERROR "Critical configuration VULKAN_SDK environment variable not set")
  endif()

  if(_ZENGINE_VULKAN_USE_SDK_LAYOUT)
    set(vulkan_include ${VULKAN_SDK_ROOT}/Include)

    if(ANDROID OR Z_PLATFORM_OHOS)
      find_library(vulkan_lib vulkan REQUIRED)
      if(CMAKE_HOST_WIN32)
        set(glslangValidator_executable ${VULKAN_SDK_ROOT}/Bin/glslangValidator.exe)
      else()
        set(glslangValidator_executable ${VULKAN_SDK_ROOT}/bin/glslangValidator)
      endif()
      message(STATUS "${Z_PLATFORM_NAME}: using Vulkan system library: ${vulkan_lib}")
    elseif(WIN32)

      set(vulkan_lib ${VULKAN_SDK_ROOT}/Lib/vulkan-1.lib)
      set(glslangValidator_executable ${VULKAN_SDK_ROOT}/Bin/glslangValidator.exe)
      file(TO_CMAKE_PATH "${VULKAN_SDK_ROOT}/Bin" vk_layer_path_definition)
      add_compile_definitions("Z_VK_LAYER_PATH=${vk_layer_path_definition}")
    elseif(UNIX)
      set(vulkan_lib ${VULKAN_SDK_ROOT}/lib/libvulkan.so.1)
      set(glslangValidator_executable ${VULKAN_SDK_ROOT}/bin/glslangValidator)
      file(TO_CMAKE_PATH "${VULKAN_SDK_ROOT}/bin" vk_layer_path_definition)
      add_compile_definitions("Z_VK_LAYER_PATH=${vk_layer_path_definition}")
    else()

      message(FATAL_ERROR "Unknown Platform")
    endif()
  endif()
  unset(_ZENGINE_VULKAN_USE_SDK_LAYOUT)
else()
  set(VULKAN_SDK_ROOT "")
  set(vulkan_include "")
  set(vulkan_lib "")
  set(glslangValidator_executable "")
  message(STATUS "${Z_PLATFORM_NAME}: Vulkan disabled; using Metal backend")
endif()

# Propagate a stable preprocessor switch to every translation unit so source files can
# guard Vulkan-specific code with `#if defined(Z_HAS_VULKAN)`. Android/OHOS and Linux
# builds keep Vulkan on; Windows defaults to DX12 and Apple to Metal.
if(ZENGINE_USE_VULKAN)
  add_compile_definitions(Z_HAS_VULKAN=1)
endif()


# Metal/macOS builds disable Vulkan but still benefit from glslangValidator for
# offline SPIR-V generation when the tool is on PATH (brew install glslang).
if(NOT glslangValidator_executable)
  find_program(glslangValidator_EXECUTABLE glslangValidator)
  if(glslangValidator_EXECUTABLE)
    set(glslangValidator_executable ${glslangValidator_EXECUTABLE})
    message(STATUS "Found glslangValidator on PATH: ${glslangValidator_executable}")
  endif()
endif()

if(glslangValidator_executable)
  file(TO_CMAKE_PATH "${glslangValidator_executable}" glslangValidator_executable_normalized)
endif()

set(SHADER_COMPILE_TARGET ZShaderCompile)

add_subdirectory(shader)

# Per-config output dirs (bin/Debug, bin/Release) and z_set_config_archive_dirs for 3rdparty
include(cmake/ConfigOutputDirs.cmake)

add_subdirectory(3rdparty)

# Ensure modules (e.g. render_doc) are configured before the Runtime so their
# targets exist when the Runtime does its install/export logic. This avoids
# install(EXPORT ...) errors when a runtime target requires a module target
# that wasn't yet defined.
add_subdirectory(Source/Modules)
add_subdirectory(Source/CommonPCH)

add_subdirectory(Source/Runtime)
if(EMSCRIPTEN OR CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
  # Web (mini-program / browser): the desktop Editor is a GLFW host that pulls
  # in Vulkan/DX12/Metal/GLFW backends and cannot link under Emscripten. We
  # instead build a minimal browser host (ZWebLauncher) that creates a WebGL2
  # canvas and links libZRuntime.a - see engine/Source/WebLauncher/.
  message(STATUS "Web (Emscripten) build: skipping Editor/Profiler; adding ZWebLauncher target")
  add_subdirectory(Source/WebLauncher)
elseif(ANDROID OR IOS OR Z_PLATFORM_OHOS OR OHOS OR CMAKE_SYSTEM_NAME STREQUAL "OHOS")
  message(STATUS "Mobile build: skipping Editor and Profiler targets")
elseif(APPLE)
  message(STATUS "macOS Metal build: adding Editor target")
  add_subdirectory(Source/Editor)
else()
  add_subdirectory(Source/Editor)
  # Profiler now builds only the headless ZProfilerCsvExporter (the ImGui GUI was removed).
  add_subdirectory(Source/Profiler)
  # ZInsights: standalone .ztrace viewer (Win32 + Direct2D). Self-skips on non-Windows.
  add_subdirectory(Source/Insights)
endif()
#add_subdirectory(Source/test)


# Single custom target to copy configs + asset (+ jolt-asset) to all config output dirs.
# The Editor depends on it so the copy runs once when building it.
# Use remove that does not fail when directory is missing (cmake -E remove_directory can
# return non-zero on older CMake when dir does not exist, causing MSB8066).
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.28)
  set(_SAFE_REMOVE_COMMAND ${CMAKE_COMMAND} "-E" "rm" "-rf")
else()
  set(_SAFE_REMOVE_COMMAND)
endif()
set(_COPY_CONFIGS "Debug" "Release" "RelWithDebInfo" "MinSizeRel")
if(CMAKE_CONFIGURATION_TYPES)
  set(_COPY_CONFIGS ${CMAKE_CONFIGURATION_TYPES})
else()
  set(_COPY_CONFIGS ${CMAKE_BUILD_TYPE})
endif()
set(_COPY_CMDS)
foreach(_cfg IN LISTS _COPY_CONFIGS)
  list(APPEND _COPY_CMDS COMMAND ${CMAKE_COMMAND} -E make_directory "${BINARY_ROOT_DIR}/${_cfg}")
  if(_cfg STREQUAL "Release")
    list(APPEND _COPY_CMDS COMMAND ${CMAKE_COMMAND} -E copy_directory "${ENGINE_ROOT_DIR}/${DEPLOY_CONFIG_DIR}" "${BINARY_ROOT_DIR}/${_cfg}")
  else()
    list(APPEND _COPY_CMDS COMMAND ${CMAKE_COMMAND} -E copy_directory "${ENGINE_ROOT_DIR}/${DEVELOP_CONFIG_DIR}" "${BINARY_ROOT_DIR}/${_cfg}")
  endif()
  if(_SAFE_REMOVE_COMMAND)
    list(APPEND _COPY_CMDS COMMAND ${_SAFE_REMOVE_COMMAND} "${BINARY_ROOT_DIR}/${_cfg}/${ENGINE_ASSET_DIR}")
  else()
    if(WIN32)
      list(APPEND _COPY_CMDS COMMAND cmd /c "if exist \"${BINARY_ROOT_DIR}/${_cfg}/${ENGINE_ASSET_DIR}\" rmdir /s /q \"${BINARY_ROOT_DIR}/${_cfg}/${ENGINE_ASSET_DIR}\"")
    else()
      list(APPEND _COPY_CMDS COMMAND ${CMAKE_COMMAND} -E remove_directory "${BINARY_ROOT_DIR}/${_cfg}/${ENGINE_ASSET_DIR}")
    endif()
  endif()
  list(APPEND _COPY_CMDS COMMAND ${CMAKE_COMMAND} -E copy_directory "${ENGINE_ROOT_DIR}/${ENGINE_ASSET_DIR}" "${BINARY_ROOT_DIR}/${_cfg}/${ENGINE_ASSET_DIR}")
  if(_SAFE_REMOVE_COMMAND)
    list(APPEND _COPY_CMDS COMMAND ${_SAFE_REMOVE_COMMAND} "${BINARY_ROOT_DIR}/${_cfg}/${ENGINE_RESOURCE_DIR}")
  else()
    if(WIN32)
      list(APPEND _COPY_CMDS COMMAND cmd /c "if exist \"${BINARY_ROOT_DIR}/${_cfg}/${ENGINE_RESOURCE_DIR}\" rmdir /s /q \"${BINARY_ROOT_DIR}/${_cfg}/${ENGINE_RESOURCE_DIR}\"")
    else()
      list(APPEND _COPY_CMDS COMMAND ${CMAKE_COMMAND} -E remove_directory "${BINARY_ROOT_DIR}/${_cfg}/${ENGINE_RESOURCE_DIR}")
    endif()
  endif()
  list(APPEND _COPY_CMDS COMMAND ${CMAKE_COMMAND} -E copy_directory "${ENGINE_ROOT_DIR}/${ENGINE_RESOURCE_DIR}" "${BINARY_ROOT_DIR}/${_cfg}/${ENGINE_RESOURCE_DIR}")
  if(ENABLE_PHYSICS_DEBUG_RENDERER)
    if(_SAFE_REMOVE_COMMAND)
      list(APPEND _COPY_CMDS COMMAND ${_SAFE_REMOVE_COMMAND} "${BINARY_ROOT_DIR}/${_cfg}/${JOLT_ASSET_DIR}")
    else()
      if(WIN32)
        list(APPEND _COPY_CMDS COMMAND cmd /c "if exist \"${BINARY_ROOT_DIR}/${_cfg}/${JOLT_ASSET_DIR}\" rmdir /s /q \"${BINARY_ROOT_DIR}/${_cfg}/${JOLT_ASSET_DIR}\"")
      else()
        list(APPEND _COPY_CMDS COMMAND ${CMAKE_COMMAND} -E remove_directory "${BINARY_ROOT_DIR}/${_cfg}/${JOLT_ASSET_DIR}")
      endif()
    endif()
    list(APPEND _COPY_CMDS COMMAND ${CMAKE_COMMAND} -E copy_directory "${ENGINE_ROOT_DIR}/${JOLT_ASSET_DIR}" "${BINARY_ROOT_DIR}/${_cfg}/${JOLT_ASSET_DIR}")
  endif()
endforeach()
add_custom_target(CopyEngineDataToOutput ${_COPY_CMDS})
set_target_properties(CopyEngineDataToOutput PROPERTIES FOLDER "Engine")
if(TARGET ZEditor)
  add_dependencies(ZEditor CopyEngineDataToOutput)
endif()
unset(_COPY_CONFIGS)

unset(_COPY_CMDS)
unset(_cfg)

set(CODEGEN_TARGET "ZPreCompile")
include(Source/Precompile/precompile.cmake)
set_target_properties("${CODEGEN_TARGET}" PROPERTIES FOLDER "Engine")

add_dependencies(ZRuntime "${CODEGEN_TARGET}")
add_dependencies(ZEnginePCH "${CODEGEN_TARGET}")
