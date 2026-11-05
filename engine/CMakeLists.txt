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
# iOS 同样是移动端：体积优化（-Oz / gc-sections）与符号隐藏都要生效，
# 否则 App Store 包体白大一圈，且引擎内部符号全暴露在 Mach-O 里。
if(ANDROID OR IOS OR Z_PLATFORM_OHOS OR OHOS OR CMAKE_SYSTEM_NAME STREQUAL "OHOS")
  set(_ZENGINE_MOBILE_PLATFORM ON)
endif()
option(ZENGINE_MOBILE_SIZE_OPTIMIZE "Optimize mobile shared libraries for smaller package size" ${_ZENGINE_MOBILE_PLATFORM})
option(ZENGINE_MOBILE_HIDE_INTERNAL_SYMBOLS "Hide non-exported mobile runtime symbols" ${_ZENGINE_MOBILE_PLATFORM})
# iOS-only, local to the shared runtime dylib: pass ld64 `-x` so the LOCAL
# symbols left behind by -exported_symbols_list are never written into the
# symbol table. Measured 2026-10-30: ~1.3 MB of __LINKEDIT on a 8.1 MB dylib.
# (Deliberately a link-time flag, not a post-build `strip -S`, which can
# damage LC_DYLD_INFO and make iOS 16+ dyld reject the image.)
option(ZENGINE_IOS_SHARED_STRIP_LOCAL_SYMBOLS "Strip local symbols from the iOS shared runtime at link time (-x)" ${_ZENGINE_MOBILE_PLATFORM})
# Drop whole subsystems from the iOS ZRuntimeShared dylib. Default OFF: this
# changes what the dylib *is*, not just how it is built. Safe only because the
# exported ABI is 104 C symbols that contain no rendering / physics / Spine
# entry point -- see the "what is left to cut" table in
# scripts/build_lib/ios/README.md. The static ZRuntime (iOS player, Editor,
# host tools) is untouched.
option(ZENGINE_IOS_SHARED_SLIM_MODULES "Drop SPIRV-Cross / GPUSpine / Jolt from the iOS shared runtime" OFF)
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
  # iOS is a mobile platform too (ZENGINE_MOBILE_SIZE_OPTIMIZE defaults to ON
  # for it), but the flags below used to be gated on Android/OHOS only -- so a
  # Release iOS build quietly stayed at CMake's default -O3 while Android got
  # -Oz. The AppleClang spelling of the section-GC half is -dead_strip (already
  # set on ZRuntimeShared), so only the compile side is added here.
  if(ZENGINE_MOBILE_SIZE_OPTIMIZE AND (ANDROID OR IOS OR Z_PLATFORM_OHOS OR OHOS OR CMAKE_SYSTEM_NAME STREQUAL "OHOS"))
    add_compile_options(
      "$<$<CONFIG:Release>:-Oz>"
      "$<$<CONFIG:MinSizeRel>:-Oz>"
      -ffunction-sections
      -fdata-sections
    )
    # -fvisibility=hidden is deliberately NOT applied on Apple: the iOS shared
    # runtime links with -flat_namespace + -undefined dynamic_lookup so that
    # libc++ VTT/vtable/typeinfo can bind against the copy embedded in the host
    # (UnityFramework). Marking those hidden at compile time would make local
    # copies instead and change type identity across the boundary.
    if(ZENGINE_MOBILE_HIDE_INTERNAL_SYMBOLS AND NOT APPLE)
      add_compile_options(-fvisibility=hidden -fvisibility-inlines-hidden)
    endif()
    if(NOT APPLE)
      add_link_options(
        "$<$<CONFIG:Release>:-Wl,--gc-sections>"
        "$<$<CONFIG:MinSizeRel>:-Wl,--gc-sections>"
      )
    endif()
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

    # X11 窗口层（Platform/Linux/）+ Vulkan xlib surface 都需要 X11 + Xrandr。
    # find_package 失败不致命：窗口层 .cpp 有 Z_PLATFORM_LINUX && !__EMSCRIPTEN__
    # 守卫，缺 X11 时该 TU 编译为空；但链接期需要这些库，故找到才启用窗口层。
    find_package(X11 REQUIRED)
    # Xrandr：libXrandr 由 FindX11 统一探测，并在找到时创建 X11::Xrandr 导入目标。
    # 发行版的 libxrandr-dev 只提供 pkg-config 文件，并不安装 XrandrConfig.cmake，
    # 所以裸的 find_package(Xrandr REQUIRED) 在 Ubuntu / Debian 上必然失败（它会
    # 退化到 config 模式去找 XrandrConfig.cmake）。先尝试 config 模式以兼容确实
    # 提供 config 包的机器，再退回 FindX11 的探测结果。
    find_package(Xrandr QUIET)
    if(NOT TARGET X11::Xrandr)
      message(FATAL_ERROR
        "Xrandr not found, but the Linux X11 window layer needs it. "
        "Install the Xrandr development package (Debian/Ubuntu: libxrandr-dev).")
    endif()
    set(ZENGINE_LINUX_X11 ON)
    message(STATUS "Linux: X11 window layer + xlib surface (X11 ${X11_VERSION})")
    # The X11 window layer cannot be compiled as written on a real Linux box: it
    # calls Xlib entry points that do not exist (XGetModifier, XCreateXCursor,
    # XWarpTo, XRandRGetScreenResources, KeyMapState, white_pixel, ...). Turn this
    # on to build the rest of ZRuntime headlessly with a no-op window layer.
    option(ZENGINE_LINUX_WINDOW_STUB
      "Compile the Linux X11 window layer as no-op stubs (headless builds)" OFF)
    if(ZENGINE_LINUX_WINDOW_STUB)
      add_compile_definitions(ZENGINE_LINUX_WINDOW_STUB)
      message(STATUS "Linux: X11 window layer compiled as stubs (ZENGINE_LINUX_WINDOW_STUB=ON)")
    endif()
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

  # The include path is the other half of that switch and has to be applied at
  # the same scope, or the two halves disagree.
  #
  # `Z_HAS_VULKAN=1` is global, so any TU -- including ones in executables that
  # deliberately do not link ZRuntime -- that reaches RHIStruct.h gets
  # `#include <vulkan/vulkan.h>`. The Vulkan include directory was only attached
  # to ZRuntime as PUBLIC, so those targets inherited the macro without the
  # header path and failed with C1083 even though nothing was wrong with them.
  # RenderGraphExecuteSmokeTest and RHIIndirectDrawSmokeTest both hit this on a
  # Windows + Vulkan build; a Windows + Vulkan=OFF build cannot reproduce it,
  # which is why it survived.
  if(vulkan_include)
    include_directories(${vulkan_include})
  endif()
endif()

# Lumen (CPU global illumination) is stripped from ZRuntime on Emscripten and on
# Windows builds that opt out of Vulkan -- see the two `list(FILTER ... EXCLUDE
# REGEX "...Function/Render/Lumen/...")` blocks in Source/Runtime/CMakeLists.txt.
# Mirror that here so pipeline / render-system code can `#include` and construct
# LumenRenderPass without breaking those configurations. Note this is NOT the same
# as Z_HAS_VULKAN: Apple builds Metal (no Vulkan) but do compile Lumen, which is
# exactly why guarding the pass on Z_HAS_VULKAN left it uncreated there.
if(NOT (EMSCRIPTEN OR CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
   AND NOT (WIN32 AND NOT ZENGINE_USE_VULKAN))
  add_compile_definitions(Z_HAS_LUMEN=1)
  message(STATUS "Engine: Lumen (CPU GI) enabled")
else()
  message(STATUS "Engine: Lumen (CPU GI) disabled (sources stripped from ZRuntime)")
endif()

# Whether the engine's GLSL shaders are available as SPIR-V blobs
# (shader/generated/cpp/*.h, produced by SHADER_COMPILE_TARGET at build time).
#
# This is deliberately NOT the same as Z_HAS_VULKAN, for the same reason Z_HAS_LUMEN
# is not: Apple builds Metal with no Vulkan in sight, yet MetalRHI runs those very
# SPIR-V blobs through SPIRV-Cross to get MSL (see MetalRHI::CreateShaderModule).
# Guarding "load the built-in mesh shader" on Z_HAS_VULKAN therefore left Apple with
# no shaders at all -- MainCameraRp1Pass threw on startup and nothing was ever drawn.
#
# Off where there is no SPIR-V path: Emscripten (WebGL2 eats GLSL, not SPIR-V) and
# Windows-without-Vulkan (that configuration compiles HLSL sources instead).
if(NOT (EMSCRIPTEN OR CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
   AND NOT (WIN32 AND NOT ZENGINE_USE_VULKAN))
  add_compile_definitions(Z_HAS_SPIRV_SHADERS=1)
  message(STATUS "Engine: built-in shaders available as SPIR-V (Vulkan path + Metal via SPIRV-Cross)")
else()
  message(STATUS "Engine: built-in shaders not taken from SPIR-V (HLSL / GLSL sources instead)")
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

# Per-config output dirs (bin/Debug, bin/Release) for exes and shared libs
include(cmake/ConfigOutputDirs.cmake)

# z_embed_app_icon(<target>): embeds a tool's resource/<Name>.ico as IDI_ICON1.
# Standalone tools that configure on their own (engine/tools/ZEnvironmentCollector)
# include this file directly.
include(cmake/AppIcon.cmake)

# z_embed_app_manifest(<target>): declares per-monitor DPI v2 by embedding the
# shared manifest. Same include rule as AppIcon above -- a tool that configures
# on its own pulls the file in itself. MSVC-only; a no-op elsewhere.
include(cmake/AppManifest.cmake)

# z_set_tool_output_dir(<target> [SUBDIR <subdir>]): sends a standalone GUI tool
# to <repo>/bin/Tools/<Config>[/<subdir>]/ so no tool's exe lands in the build
# tree. Pass SUBDIR when the tool ships more than its .exe.
include(cmake/ToolOutputDir.cmake)

# Optional Wwise 2019.2.x backend. Defines the WwiseSDK interface target (and
# ZENGINE_HAS_WWISE) when ZENGINE_WWISE_SDK_DIR points at a real SDK; otherwise
# it returns early and the Wwise translation unit compiles its "not compiled
# in" branch. Must run before Source/ so configure_runtime_library() can see it.
include(${CMAKE_CURRENT_LIST_DIR}/cmake/WwiseSDK.cmake)

add_subdirectory(3rdparty)

# Ensure modules (e.g. render_doc) are configured before the Runtime so their
# targets exist when the Runtime does its install/export logic. This avoids
# install(EXPORT ...) errors when a runtime target requires a module target
# that wasn't yet defined.
add_subdirectory(Source/Modules)

# ---------------------------------------------------------------------------
# iOS: no stack probing anywhere (ZEngine targets, 3rdparty and the PCH alike).
#
# On arm64 iOS, Clang inserts ___chkstk_darwin calls for frames >4KB. That symbol
# lives in macOS libSystem but is NOT exported by the iOS SDK stubs, so dyld
# rejects the binary ("Symbol not found: ___chkstk_darwin"). We ship a no-op in
# Platform/IOS/ZChkstkDarwin.c and disable the probing instead (iOS uses kernel
# guard pages for stack overflow detection).
#
# This MUST be directory-global, not per-target: ZEnginePCH is a shared
# precompiled header, and AppleClang refuses a PCH whose stack-check setting
# differs from the translation unit consuming it --
#   "stack checking was enabled in precompiled file ... but is currently disabled"
# (and the mirror error the other way round). Adding the flag to ZRuntime alone
# broke ZPlayer; adding it to the PCH alone broke ZPlayer the other way. One
# global switch is the only arrangement where every consumer agrees.
# ---------------------------------------------------------------------------
if(IOS)
  add_compile_options(-fno-stack-check)
endif()

add_subdirectory(Source/CommonPCH)

add_subdirectory(Source/Runtime)
# ZPlayer: platform-agnostic player shell (boot + per-frame tick). Added on every
# platform: the desktop ZGame links it for the shared startup sequence, and the
# Android / iOS hosts link it as the only thing they need to boot the engine.
add_subdirectory(Source/Player)
if(EMSCRIPTEN OR CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
  # Web (mini-program / browser): the desktop Editor is a GLFW host that pulls
  # in Vulkan/DX12/Metal/GLFW backends and cannot link under Emscripten. We
  # instead build a minimal browser host (ZWebLauncher) that creates a WebGL2
  # canvas and links libZRuntime.a - see engine/Source/WebLauncher/.
  message(STATUS "Web (Emscripten) build: skipping Editor/Profiler; adding ZWebLauncher target")
  add_subdirectory(Source/WebLauncher)
elseif(ANDROID)
  # Android (P0-1): the desktop Editor/Profiler are GLFW hosts and cannot link
  # here. ZGameAndroid is the NativeActivity host library -- it links ZRuntime
  # via ZPlayer and is what Gradle packages into the APK.
  message(STATUS "Android build: skipping Editor/Profiler; adding ZGameAndroid host library")
  add_subdirectory(Source/Player/Android)
elseif(IOS)
  # iOS (P0-1): the desktop Editor/Profiler are GLFW hosts and cannot link here.
  # ZGameIOS is a MACOSX_BUNDLE executable -- CMake emits a real .app (and, with
  # -G Xcode, an archivable project), which is what the iOS packaging chain needs.
  message(STATUS "iOS build: skipping Editor/Profiler; adding ZGameIOS app target")
  add_subdirectory(Source/Player/IOS)
elseif(Z_PLATFORM_OHOS OR OHOS OR CMAKE_SYSTEM_NAME STREQUAL "OHOS")
  message(STATUS "Mobile build: skipping Editor and Profiler targets")
elseif(APPLE)
  message(STATUS "macOS Metal build: adding Editor target")
  add_subdirectory(Source/Editor)
else()
  add_subdirectory(Source/Editor)
  # ZGame: standalone game runtime (UE "packaged game" equivalent). No editor
  # UI -- just RegisterGameRuntime() + GameEngine::Run().
  add_subdirectory(Source/Game)
  # ZInsights: standalone .ztrace viewer (Win32 + Direct2D). Self-skips on non-Windows.
  add_subdirectory(Source/Insights)
  # TexPreview: standalone block-compressed texture preview tool (ASTC, BC7)
  add_subdirectory(tools/TexPreview)
  # LuaMemPreview 不再是独立 exe：它是 ZEditor 的一个工具套件
  # （Source/Editor/ToolSuite/LuaMemPreview），由启动器或
  # `ZEditor -batchmode -suite=LuaMemPreview` 进入。原本那一份 WinMain + app
  # icon + 独立 target 删掉了 —— ZSlate 的 FSlateStandaloneApp 已经把宿主做完，
  # 再包一层 exe 只是多一份要单独构建、单独分发的东西。
endif()

# =============================================================================
# Standalone GUI tools that are normally configured on their own
# =============================================================================
# All GUI tools live in engine/tools/ (AGENTS.md section 8): TexPreview above
# (desktop branch), these here. Both are self-contained
# CMake projects -- `cmake -S engine/tools/<Tool> -B build/tools/<Tool>` -- and
# both are Windows-only.
#
# Ordering is load-bearing: this block sits after add_subdirectory(Source/Runtime),
# which is where ZSlate is defined. Both tools link ZSlate, and each guards its
# own `add_subdirectory(.../ZSlate)` with `if(NOT TARGET ZSlate)` for the
# standalone case -- pull this block above Source/Runtime and they would create a
# second ZSlate, colliding with the engine's.

# DataTableCook bakes a LocalizationTable CSV into a .zasset at build time.
# Declared BEFORE ZEnvironmentCollector because the collector's POST_BUILD
# step runs the cooker to produce the table it ships.
option(ZENGINE_BUILD_DATATABLE_COOK "Build the CSV -> .zasset table cooker" ON)
if(ZENGINE_BUILD_DATATABLE_COOK)
    add_subdirectory(tools/DataTableCook)
endif()

option(ZENGINE_BUILD_WINDOWS_ENVIRONMENT_CHECK "Build the standalone Windows environment checker" ON)

if(ZENGINE_BUILD_WINDOWS_ENVIRONMENT_CHECK AND WIN32)
    # ZEnvironmentCollector (GUI + -batchmode CLI) and ZDllLoadProbe need no
    # ZSlate and build fine here. The ZSlate front-end defaults to OFF in a
    # full-tree configure (ENVIRONMENT_CHECK_BUILD_GUI) and that default is
    # deliberate -- do not "fix" it:
    #
    # ZEnvironmentCollector sets CMAKE_MSVC_RUNTIME_LIBRARY to /MT because it
    # ships to machines with no VC redist. Standalone, ZSlate is added inside
    # that scope and inherits /MT. In a full-tree configure ZSlate already
    # exists as /MD (engine default), so linking it into a /MT collector fails
    # with LNK2038 (RuntimeLibrary mismatch). One ZSlate instance cannot be
    # both. Build the collector GUI from its own configure:
    #     cmake -S engine/tools/ZEnvironmentCollector -B <dir>
    add_subdirectory(tools/ZEnvironmentCollector)
endif()

# ClientProjBranches is a self-contained CMake project: it configures and
# builds fine on its own (cmake -S . -B build), which is how it is normally
# used. It is OFF here by default so a full engine configure does not pick up a
# second, unrelated tool with its own C++ standard and output layout.
option(ZENGINE_BUILD_CLIENT_PROJ_BRANCHES "Build the ClientProjBranches branch/stack-trace viewer" OFF)

if(ZENGINE_BUILD_CLIENT_PROJ_BRANCHES AND WIN32)
    add_subdirectory(tools/ClientProjBranches)
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

# =============================================================================
# Packaging: a runnable standalone layout
# -----------------------------------------------------------------------------
#   cmake --install <build> --prefix <dir>
#   cpack -C Release -G ZIP            (or: -G TGZ on Linux/macOS)
#
#   <dir>/
#   ├── ZGame(.exe)    the player
#   ├── ZEditor.ini    DEPLOYMENT profile. CommandSystem derives the config
#   │                  path from the executable's own directory, so the ini has
#   │                  to sit next to it -- a packaged player is started from an
#   │                  arbitrary working directory, not from the build tree.
#   ├── asset/         engine content: the default textures and the global
#   │                  rendering / particle resources RenderScene falls back to
#   │                  (asset/texture/default/*.jpg for untextured materials).
#   ├── resource/      icons + font
#   └── Project/<name>/   optional, see ZENGINE_PACKAGE_PROJECT below.
#
# Shader sources are NOT shipped: they come from the project's own
# GetShadersRoot(), and the engine's generated SPIR-V/DXIL headers are compiled
# into the binary.
#
# NOTE: CMAKE_INSTALL_PREFIX is NOT available as the packaging prefix -- the
# root CMakeLists.txt points it at <repo>/bin so that every target lands in one
# output directory. `cmake --install` MUST therefore be given --prefix
# explicitly. CPack is unaffected: it installs into its own staging directory.
# =============================================================================
if(TARGET ZGame)
  install(TARGETS ZGame RUNTIME DESTINATION .)

  install(FILES "${ENGINE_ROOT_DIR}/${DEPLOY_CONFIG_DIR}/ZEditor.ini" DESTINATION .)
  install(DIRECTORY "${ENGINE_ROOT_DIR}/asset" DESTINATION .)
  install(DIRECTORY "${ENGINE_ROOT_DIR}/resource" DESTINATION .)

  # Project content. The editor's bundle build already wrote
  # <project>/Build/<Platform>/{Bundles/, *.manifest.zasset} and ZGame finds it
  # by scanning Build/ (see AssetBundleManager::MountBuild), but the player also
  # needs the .zproject it is launched with -- and the compiled JS under
  # Intermediate/Scripts, which is still NOT packed into a bundle.
  # Intermediate/Cooked + DDC + Shaders are build caches, not runtime data.
  if(ZENGINE_PACKAGE_PROJECT)
    if(NOT IS_DIRECTORY "${ZENGINE_PACKAGE_PROJECT}")
      message(FATAL_ERROR "ZENGINE_PACKAGE_PROJECT is not a directory: ${ZENGINE_PACKAGE_PROJECT}")
    endif()
    # Strip any trailing separator: get_filename_component(... NAME) returns an
    # empty string for "path/to/project/", which would install into "Project/".
    string(REGEX REPLACE "[\\\\/]+$" "" _zpkg_project_src "${ZENGINE_PACKAGE_PROJECT}")
    get_filename_component(_zpkg_project_name "${_zpkg_project_src}" NAME)
    install(DIRECTORY "${_zpkg_project_src}/" DESTINATION "Project/${_zpkg_project_name}"
            PATTERN "Intermediate/Cooked" EXCLUDE
            PATTERN "Intermediate/DDC" EXCLUDE
            PATTERN "Intermediate/Shaders" EXCLUDE
            PATTERN "Logs" EXCLUDE)
    unset(_zpkg_project_src)
    unset(_zpkg_project_name)
    message(STATUS "Packaging project content: ${ZENGINE_PACKAGE_PROJECT} -> Project/")
  endif()
else()
  message(STATUS "Packaging: ZGame is not built on this platform; install rules skipped")
endif()

# =============================================================================
# PR-PAK1 第三轮：可运行的独立 Player 安装布局（component `ZGamePlayer`）
# -----------------------------------------------------------------------------
# 上面的规则是 SDK 时代的布局。实测（2026-11-05, cmake --install --config Debug）
# 得到 1653 个文件、顶层 0 个 DLL：ZGame.exe 装在 <prefix>/，ZRuntimeShared.dll
# 却被 Runtime/CMakeLists.txt 装到 <prefix>/bin/，53 个第三方 DLL 一个都没装，
# 同时整个 engine/asset（含 .scene 游戏内容）和全部公共头都被塞进包里。
# 这样的 Player 起不来，也和第三轮「零源资产」的安装结论矛盾。
#
# 这里新增一个独立 component，产出与 scripts/verify_player_install_e2e.py
# 同一份布局，且不改动任何既有 SDK 规则（它们保持 Unspecified component）：
#
#   cmake --install build --config Debug --component ZGamePlayer --prefix <dir>
#   ZENGINE_PLAYER_PROJECT=<已 bundle.build 的项目> cmake --install ...
#
# 第二个用法把 <项目>/*.zproject 与 Build/Standalone/{manifest,Bundles} 平铺到
# 安装根，得到完全自包含的目录。环境变量在 install 时求值，因为验证脚本的
# 项目是运行时生成的，不该为它重新 configure。
# =============================================================================
set(ZGAME_PLAYER_COMPONENT "ZGamePlayer")

if(TARGET ZGame)
  install(TARGETS ZGame RUNTIME DESTINATION .
          COMPONENT ${ZGAME_PLAYER_COMPONENT})
  install(FILES "${ENGINE_ROOT_DIR}/${DEPLOY_CONFIG_DIR}/ZEditor.ini"
          DESTINATION . COMPONENT ${ZGAME_PLAYER_COMPONENT})

  # bin/<Config> 是构建期就齐备的运行时目录（Game/CMakeLists.txt 的
  # POST_BUILD 会把 ZRuntimeShared.dll 拷到 exe 旁）。configure 时 DLL 可能还
  # 不存在，所以只能在 install 时用 file(GLOB) 枚举，不能写成 install(FILES)。
  install(CODE "
    file(GLOB _zplayer_dlls \"${BINARY_ROOT_DIR}/$<CONFIG>/*.dll\")
    if(NOT _zplayer_dlls)
      message(FATAL_ERROR \"ZGamePlayer: no runtime DLLs in ${BINARY_ROOT_DIR}/$<CONFIG> -- build ZGame first\")
    endif()
    foreach(_zplayer_dll IN LISTS _zplayer_dlls)
      file(COPY \"\${_zplayer_dll}\" DESTINATION \"\${CMAKE_INSTALL_PREFIX}\")
    endforeach()
    list(LENGTH _zplayer_dlls _zplayer_dll_count)
    message(STATUS \"ZGamePlayer: \${_zplayer_dll_count} runtime DLL(s) installed\")
  " COMPONENT ${ZGAME_PLAYER_COMPONENT})

  # 可选：把一个已经 bundle.build 过的项目平铺进安装目录，得到零源资产的
  # 自包含发布布局。不设置环境变量时只安装 Player 运行时骨架。
  install(CODE "
    set(_zplayer_project \"\$ENV{ZENGINE_PLAYER_PROJECT}\")
    if(NOT _zplayer_project STREQUAL \"\")
      if(NOT IS_DIRECTORY \"\${_zplayer_project}\")
        message(FATAL_ERROR \"ZENGINE_PLAYER_PROJECT is not a directory: \${_zplayer_project}\")
      endif()
      file(GLOB _zplayer_projects \"\${_zplayer_project}/*.zproject\")
      foreach(_zplayer_zproject IN LISTS _zplayer_projects)
        file(COPY \"\${_zplayer_zproject}\" DESTINATION \"\${CMAKE_INSTALL_PREFIX}\")
      endforeach()
      if(NOT IS_DIRECTORY \"\${_zplayer_project}/Build/Standalone\")
        message(FATAL_ERROR \"ZENGINE_PLAYER_PROJECT has no Build/Standalone output: \${_zplayer_project}\")
      endif()
      file(MAKE_DIRECTORY \"\${CMAKE_INSTALL_PREFIX}/Build/Standalone\")
      file(COPY \"\${_zplayer_project}/Build/Standalone/\"
           DESTINATION \"\${CMAKE_INSTALL_PREFIX}/Build/Standalone\")
      message(STATUS \"ZGamePlayer: staged packaged project \${_zplayer_project}\")
    endif()
  " COMPONENT ${ZGAME_PLAYER_COMPONENT})
endif()

set(CPACK_PACKAGE_NAME "ZGame")
set(CPACK_PACKAGE_VENDOR "ZEngine")
if(NOT CPACK_PACKAGE_VERSION)
  set(CPACK_PACKAGE_VERSION "0.1.0")
endif()
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "ZEngine standalone player")
set(CPACK_PACKAGE_FILE_NAME "ZGame-${CPACK_PACKAGE_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
# No extra top-level folder inside the archive: the layout above already is the
# package, and unpacking should give you the game directory directly.
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
if(WIN32)
  set(CPACK_GENERATOR "ZIP")
else()
  set(CPACK_GENERATOR "TGZ")
endif()

# CPack 也必须只打 Player 布局（第四轮续）。
#
# CPack 默认是 monolithic install：CPACK_INSTALL_CMAKE_PROJECTS 第三项为 ALL，
# 安装时 CMAKE_INSTALL_COMPONENT 为空，于是 cmake_install.cmake 里所有
# `if(<component> STREQUAL ... OR NOT CMAKE_INSTALL_COMPONENT)` 分支都会执行
# —— 1653 个 SDK 文件、公共头、3 个 .scene 又回到包里，且 exe 与
# ZRuntimeShared.dll 仍然分家。显式把 install project 钉到 ZGamePlayer
# component，让 ZIP / TGZ 与
#   `cmake --install build --config Debug --component ZGamePlayer`
# 完全同构。需要 SDK 包时把下面两行注释掉即可恢复旧行为。
set(CPACK_COMPONENTS_ALL "${ZGAME_PLAYER_COMPONENT}")
set(CPACK_INSTALL_CMAKE_PROJECTS
    "${CMAKE_BINARY_DIR};${CMAKE_PROJECT_NAME};${ZGAME_PLAYER_COMPONENT};/")
include(CPack)
