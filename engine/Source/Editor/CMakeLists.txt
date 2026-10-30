set(TARGET_NAME ZEditor)

# ---------------------------------------------------------------------------
# Packaging default for release builds.
#
# Incremental packaging is an optimisation for the edit -> package -> test
# loop, not a guarantee a release should lean on: the cache is a local file
# nobody else can inspect, and a package produced from it cannot be
# reproduced from a clean checkout. So a RELEASE build compiles
# AssetBundleBuildOptions::force_rebuild to true by default -- a packaged
# build always re-cooks and rewrites everything.
#
# The build scripts pass this for `release` (see build_{windows,linux,macos}).
# It only changes the DEFAULT: `bundle.build --incremental` still opts in per
# run, and `--force` still forces it on a debug build.
# ---------------------------------------------------------------------------
option(ZENGINE_PACKAGING_FORCE_REBUILD
       "Make packaging ignore the build cache by default (release builds)"
       OFF)

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
list(FILTER EDITOR_SOURCES EXCLUDE REGEX ".*/Undo/[Tt]est/.*")
# Editor/Build/Test/ carries BuildCacheSmokeTest.cpp, which has its own main().
list(FILTER EDITOR_SOURCES EXCLUDE REGEX ".*/Build/[Tt]est/.*")
# Editor/Launcher/Test/ carries ZEditorLauncherWiringSmokeTest.cpp (own main()).
list(FILTER EDITOR_SOURCES EXCLUDE REGEX ".*/Launcher/[Tt]est/.*")
# Editor/Prefab/Test/ carries PrefabEndToEndSmokeTest.cpp (own main()).
list(FILTER EDITOR_SOURCES EXCLUDE REGEX ".*/Prefab/[Tt]est/.*")
# LuaMemPreview is Windows-only: it pulls <windows.h> / <commdlg.h> / <shellapi.h>
# for the open-file dialog and console attachment. The directory is self-contained
# (nothing outside it calls in), so dropping it whole costs nothing elsewhere.
if(NOT WIN32)
    list(FILTER EDITOR_SOURCES EXCLUDE REGEX ".*/ToolSuite/LuaMemPreview/.*")
    list(FILTER EDITOR_HEADERS EXCLUDE REGEX ".*/ToolSuite/LuaMemPreview/.*")
endif()

# ---- ZSlate-heavy editor windows (P2) ----
# Excluded until UIRenderer->ISlateRenderer migration and ZSlate::/::type bridging
# is complete. MenuController.cpp is kept -- it is what still draws the menu bar.
# ZSlateAnimationWindow: re-enabled (animation clip editor).
# ZSlateBlueprintWindow: re-enabled (blueprint graph editor).
# ZSlateConsoleWindow: re-enabled (the log console; dock layout restored).
# ZSlateContentBrowserWindow: re-enabled (asset selection entry for zasset preview).
# ZSlateDemoWindow: deleted (ZSlate widget showcase; not part of the editor).
# ZSlateGameWindow: re-enabled (the Game viewport; it is the Default layout's active tab).
# ZSlateHierarchyWindow: re-enabled (small, off its stub; dock layout restored).
# ZSlateInsightsWindow: deleted -- Insights is served by the standalone ZInsights app.
# ZSlateInspectorWindow: re-enabled (the property panel; dock layout restored).
# ZSlateMaterialEditorWindow: re-enabled (material graph editor).
# ZSlatePackageManagerWindow: re-enabled (package list / import panel).
# ZSlatePreviewWindow: re-enabled (native zasset preview: Texture2D / MeshData / Material / Shader).
# ZSlateSceneWindow: re-enabled (the 3D viewport; dock layout restored).
# ZSlateTimelineWindow: re-enabled (timeline / dope sheet).
# ZSlateUMGDesignerWindow: re-enabled (UMG widget designer canvas).
# InspectorShaderInspector: re-enabled. It is not a window of its own -- it backs
# the ZSlate Inspector's material/shader section, which links against it.
# DefaultLayout/ is back: its native dock tree is what places every editor window
# on screen, and with it stubbed out the editor shows only the menu bar.
# FloatingPanelManager: re-enabled (tear-off / undock support).

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

# ZSlate is header-mostly, but the launcher needs FSlateStandaloneApp, whose
# Win32 host and D3D11 backend are compiled into the ZSlate target. Every other
# ZSlate-heavy editor window is still filtered out above, so this is the only
# thing pulling the library in today.
target_link_libraries(${TARGET_NAME} PRIVATE ZSlate)

if(WIN32)
    # comdlg32: GetOpenFileNameW (launcher's "open project" dialog)
    # shell32:  ShellExecuteW, SHGetKnownFolderPath
    # d3d11:    ZSlate's standalone render backend
    target_link_libraries(${TARGET_NAME} PRIVATE comdlg32 shell32 d3d11)
endif()

# ZAppShell: header-only startup helpers (wide command line, console attach).
# Header-only so the /MT tools can use it too; nothing to link.
target_include_directories(
  ${TARGET_NAME}
  PRIVATE $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/engine/Libraries/ZAppShell/include>
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

if(ZENGINE_PACKAGING_FORCE_REBUILD)
    # Read by AssetBundleBuildOptions::force_rebuild's default initialiser.
    # PRIVATE: it is a default, not an ABI -- nothing else should branch on it.
    target_compile_definitions(${TARGET_NAME} PRIVATE ZENGINE_PACKAGING_FORCE_REBUILD=1)
    message(STATUS "Editor: packaging defaults to a full rebuild (force_rebuild)")
endif()
z_set_config_output_dirs(ZEditor "${BINARY_ROOT_DIR}" RUNTIME)

# App icon: embeds app_icon/ZEditor.ico as IDI_ICON1=101. After the OUTPUT_NAME
# line, since the icon base name defaults to it. This covers Explorer and the
# file's Properties page; the title bar / Alt-Tab / taskbar come from the window
# class, which WindowsWindow.cpp fills in.
#
# Deliberately NOT in resource/: a POST_BUILD step at the bottom of this file
# copies that whole directory into the runtime tree, and the .ico is baked into
# the executable already -- nothing reads it from disk. Keeping it out also
# leaves resource/ non-existent, which is what that copy step tests for.
z_embed_app_icon(${TARGET_NAME} ICON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/app_icon")

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

# ---------------------------------------------------------------------------
# Editor reflection (ZHeaderTool, Editor script domain).
#
# Same generator ZRuntime uses, run over the editor headers. SCRIPT_DOMAIN
# editor stamps every generated ZClassScriptBinding / ZScriptEnum as
# ZScriptDomain::Editor: they are published only into the editor-scripting VM
# (EditorScriptingManager) and declared in zengine-editor.d.ts, never in the
# zengine.d.ts that game scripts compile against -- the UnityEditor.dll /
# UnityEngine.dll split. Annotate an editor class with ZCLASS / ZFUNCTION to
# expose it to TypeScript editor scripts.
# ---------------------------------------------------------------------------
include(${CMAKE_SOURCE_DIR}/cmake/ZHeaderTool.cmake)
zengine_header_tool(
    MODULE        ZEditor
    TARGETS       ${TARGET_NAME}
    INCLUDE_ROOT  "${ENGINE_ROOT_DIR}/Source"
    HEADERS       ${EDITOR_HEADERS}
    SCRIPT_DOMAIN editor)

# ---------------------------------------------------------------------------
# Editor scripts (TypeScript editor layer).
#
# engine/EditorScripts/src/*.ts is compiled with tsc into
# <bin>/<config>/EditorScripts/*.js, which EditorScriptingManager loads into
# the editor VM at startup. Always-run target: tsc is incremental enough and a
# stamp cannot be per-config. A missing tsc or a type error only warns -- the
# editor boots without (or with the previous) script layer.
#
# Type-checking needs typings/zengine*.d.ts, which the editor itself writes on
# every start; the very first build of a clean checkout therefore reports type
# errors that disappear after the editor has run once.
# ---------------------------------------------------------------------------
find_program(ZENGINE_TSC NAMES tsc.cmd tsc
    HINTS "${ENGINE_ROOT_DIR}/EditorScripts/node_modules/.bin")
file(GLOB_RECURSE EDITOR_TS_SOURCES CONFIGURE_DEPENDS "${ENGINE_ROOT_DIR}/EditorScripts/src/*.ts")
add_custom_target(ZEditorScripts
    COMMAND ${CMAKE_COMMAND}
        "-DTSC=${ZENGINE_TSC}"
        "-DPROJECT_DIR=${ENGINE_ROOT_DIR}/EditorScripts"
        "-DOUT_DIR=${BINARY_ROOT_DIR}/$<CONFIG>/EditorScripts"
        -P "${CMAKE_SOURCE_DIR}/cmake/BuildEditorScripts.cmake"
    SOURCES ${EDITOR_TS_SOURCES}
            "${ENGINE_ROOT_DIR}/EditorScripts/tsconfig.json"
    COMMENT "Compiling editor TypeScript (EditorScripts)"
    VERBATIM)
set_target_properties(ZEditorScripts PROPERTIES FOLDER "Engine/CodeGen")
add_dependencies(${TARGET_NAME} ZEditorScripts)
if(NOT ZENGINE_TSC)
    message(WARNING "Editor: tsc not found; editor TypeScript layer will not be built "
                    "(npm i -g typescript, or npm i inside engine/EditorScripts)")
endif()

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

# cgltf: single-file glTF 2.0 reader (editor-only mesh import).
#
# 与 ufbx 同一套路：实现单独编一个 .c（cgltf 是 C99，当 C 编免掉一堆 C++ 告警），
# 头文件只放声明。缺了它 glTF 就是"假支持"（扩展名列在 CanImport 里但导入必失败），
# 所以是否列出扩展名由 ZENGINE_HAS_CGLTF 决定，见 MeshImporter.cpp。
set(ZENGINE_CGLTF_DIR "${ENGINE_ROOT_DIR}/3rdparty/cgltf")
if(EXISTS "${ZENGINE_CGLTF_DIR}/cgltf.h" AND EXISTS "${ZENGINE_CGLTF_DIR}/cgltf_impl.c")
    add_library(cgltf STATIC "${ZENGINE_CGLTF_DIR}/cgltf_impl.c")
    target_include_directories(cgltf PUBLIC "${ZENGINE_CGLTF_DIR}")
    set_target_properties(cgltf PROPERTIES FOLDER "ThirdParty/cgltf")
    target_compile_definitions(${TARGET_NAME} PRIVATE ZENGINE_HAS_CGLTF=1)
    target_link_libraries(${TARGET_NAME} PRIVATE cgltf)
    message(STATUS "Editor: cgltf glTF/GLB mesh import enabled")
else()
    message(WARNING "Editor: cgltf not found at ${ZENGINE_CGLTF_DIR}; glTF/GLB import disabled "
                    "(the extensions are hidden from CanImport so they cannot be picked and then fail)")
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
add_subdirectory(AssetPipeline/MeshImporter/Test)
add_subdirectory(Undo/Test)
add_subdirectory(Build/Test)
add_subdirectory(Launcher/Test)
add_subdirectory(Prefab/Test)

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

# ---- macOS: ZEditor.app ----
#
# `z_embed_app_icon()`（上面第 242 行）只做 Windows 的 .rc —— 它把 .ico 嵌进 exe，
# Explorer / 任务栏 / Alt-Tab 就都有图标。macOS 侧**没有对等物**：ZEditor 是裸
# Mach-O，Finder / Dock / Launchpad 里一律是通用图标。运行时那两个
# `setApplicationIconImage:`（MacOSApplication + Launcher）只能管住"进程活着的时候"。
#
# 所以这里补一个 .app bundle。真身**仍然留在 bin/<Config>/ZEditor**，bundle 里的
# Contents/MacOS/ZEditor 只是一个相对 symlink —— 资产和资源全在 bin/<Config>/ 下，
# 而且 ZEditor 只链系统框架（没有 @executable_path 依赖），通过 symlink 启动不会
# 破坏 dyld 查找。
#
# 放在上面那个 POST_BUILD 之后：它会 `copy_directory` 整个可执行目录，先建 bundle
# 的话下次构建就可能把 .app 拷到自己身上；后建则每次都重建 symlink 和 icns，天然
# 幂等。
if(APPLE)
    add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            -DICON_SOURCE=${ENGINE_ROOT_DIR}/resource/ZEngineEditorBigIcon.png
            -DEXE_PATH=$<TARGET_FILE:${TARGET_NAME}>
            -DBUNDLE_NAME=ZEditor
            -DBUNDLE_VERSION=${CMAKE_PROJECT_VERSION}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/MakeMacOSAppBundle.cmake
        COMMENT "Creating ZEditor.app bundle (macOS icon / Dock identity)..."
    )
endif()

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
