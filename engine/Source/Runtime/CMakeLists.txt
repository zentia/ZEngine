# PackageProject.cmake will be used to make our target installable
set(TARGET_NAME ZRuntime)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(RUNTIME_COMPILE_DEFINITIONS)

# ---- Add ZSlate library (standalone UI framework) ----
set(ZSLATE_PATH "${ENGINE_ROOT_DIR}/Libraries/ZSlate")
if(EXISTS "${ZSLATE_PATH}/CMakeLists.txt")
    add_subdirectory("${ZSLATE_PATH}" "${CMAKE_CURRENT_BINARY_DIR}/ZSlate")
    message(STATUS "Runtime: ZSlate library enabled from ${ZSLATE_PATH}")
else()
    message(WARNING "Runtime: ZSlate library not found at ${ZSLATE_PATH}")
endif()

# ---- Add source files ----

# Note: globbing sources is considered bad practice as CMake's generators may not detect new files
# automatically. Keep that in mind when changing files, or explicitly mention them here.
file(GLOB_RECURSE HEADER_FILES CONFIGURE_DEPENDS "*.h" "*.inl")
file(GLOB_RECURSE SOURCE_FILES CONFIGURE_DEPENDS "*.cpp")

# Slate files have been fully migrated to the standalone ZSlate library
# (Runtime/Slate/ directory removed in P3)

# Determine platform-specific Platform directory and collect platform source files
set(PLATFORM_DIR "${ENGINE_ROOT_DIR}/Source/Platform")
if(WIN32)
    set(PLATFORM_SUBDIR "${PLATFORM_DIR}/Win")
    message(STATUS "Runtime: Using Windows platform directory: ${PLATFORM_SUBDIR}")
elseif(ANDROID)
    set(PLATFORM_SUBDIR "${PLATFORM_DIR}/Android")
    message(STATUS "Runtime: Using Android platform directory: ${PLATFORM_SUBDIR}")
elseif(Z_PLATFORM_OHOS OR OHOS OR CMAKE_SYSTEM_NAME STREQUAL "OHOS")
    set(PLATFORM_SUBDIR "${PLATFORM_DIR}/OHOS")
    message(STATUS "Runtime: Using OHOS platform directory: ${PLATFORM_SUBDIR}")
elseif(IOS)
    set(PLATFORM_SUBDIR "${PLATFORM_DIR}/IOS")
    message(STATUS "Runtime: Using iOS platform directory: ${PLATFORM_SUBDIR}")
elseif(EMSCRIPTEN OR CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
    set(PLATFORM_SUBDIR "${PLATFORM_DIR}/Web")
    message(STATUS "Runtime: Using Web (Emscripten) platform directory: ${PLATFORM_SUBDIR}")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(PLATFORM_SUBDIR "${PLATFORM_DIR}/Linux")
    message(STATUS "Runtime: Using Linux platform directory: ${PLATFORM_SUBDIR}")
else()
    message(WARNING "Runtime: Unknown platform, Platform directory may not be set correctly")
    set(PLATFORM_SUBDIR "")
endif()

# Collect platform-specific source files if platform directory exists
if(PLATFORM_SUBDIR AND EXISTS "${PLATFORM_SUBDIR}")
    file(GLOB_RECURSE PLATFORM_HEADERS CONFIGURE_DEPENDS "${PLATFORM_SUBDIR}/*.h" "${PLATFORM_SUBDIR}/*.inl")
    file(GLOB_RECURSE PLATFORM_SOURCES CONFIGURE_DEPENDS "${PLATFORM_SUBDIR}/*.cpp")
    
    # Also collect .m and .mm files for iOS/Android if needed
    if(IOS OR ANDROID)
        file(GLOB_RECURSE PLATFORM_OBJC_SOURCES CONFIGURE_DEPENDS "${PLATFORM_SUBDIR}/*.m" "${PLATFORM_SUBDIR}/*.mm")
        list(APPEND PLATFORM_SOURCES ${PLATFORM_OBJC_SOURCES})
    endif()
    list(APPEND HEADER_FILES ${PLATFORM_HEADERS})
    list(APPEND SOURCE_FILES ${PLATFORM_SOURCES})
    
    list(LENGTH PLATFORM_HEADERS PLATFORM_HEADER_COUNT)
    list(LENGTH PLATFORM_SOURCES PLATFORM_SOURCE_COUNT)
    message(STATUS "Runtime: Found ${PLATFORM_HEADER_COUNT} platform header files")
    message(STATUS "Runtime: Found ${PLATFORM_SOURCE_COUNT} platform source files")
elseif(PLATFORM_SUBDIR)
    message(WARNING "Runtime: Platform directory does not exist: ${PLATFORM_SUBDIR}")
endif()

# Separate regular files from platform files for source_group
# Regular files are in CMAKE_CURRENT_SOURCE_DIR, not in platform directory
# First, collect regular files (before platform files were added)
file(GLOB_RECURSE REGULAR_HEADER_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.h" "${CMAKE_CURRENT_SOURCE_DIR}/*.inl")
file(GLOB_RECURSE REGULAR_SOURCE_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

if(APPLE)
    file(GLOB_RECURSE OBJC_SOURCE_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.m" "${CMAKE_CURRENT_SOURCE_DIR}/*.mm")
    list(APPEND SOURCE_FILES ${OBJC_SOURCE_FILES})
endif()

# ---------------------------------------------------------------------------
# WebGL2 backend is mini-program / browser only (Emscripten toolchain).
# On every other platform we strip it from the unity build because it depends
# on <GLES3/gl3.h> and emscripten/html5_webgl.h which are unavailable to the
# native compilers (MSVC / clang / Apple clang / NDK clang).
# ---------------------------------------------------------------------------
if(EMSCRIPTEN OR CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
    set(ZENGINE_RHI_WEBGL2 ON CACHE INTERNAL "WebGL2 RHI backend enabled (Emscripten build)")
    message(STATUS "Runtime: Emscripten detected; enabling WebGL2 RHI backend")
    # ----------------------------------------------------------------
    # Drop every native-RHI translation unit so the link step does not
    # pull in DX12 / Vulkan / Metal code. We also strip everything that
    # transitively #includes "vulkan_rhi.h" (passes/*, virtual_texture_*,
    # debug_draw_buffer, lumen/*, ShaderLab/*); these will be replaced
    # by WebGL2-specific pipeline code as it lands.
    #
    # Mirrors the policy used by the Apple and "WIN32 AND NOT ZENGINE_USE_VULKAN"
    # branches below.
    # ----------------------------------------------------------------
    list(FILTER SOURCE_FILES         EXCLUDE REGEX "(^|.*/)Function/Render/Interface/DX12/.*\\.cpp$")
    list(FILTER SOURCE_FILES         EXCLUDE REGEX "(^|.*/)Function/Render/Interface/Vulkan/.*\\.cpp$")
    list(FILTER SOURCE_FILES         EXCLUDE REGEX "(^|.*/)Function/Render/Interface/Metal/.*\\.(cpp|mm|m)$")
    list(FILTER SOURCE_FILES         EXCLUDE REGEX "(^|.*/)Function/Render/Passes/.*\\.cpp$")
    list(FILTER SOURCE_FILES         EXCLUDE REGEX "(^|.*/)Function/Render/Texture/VirtualTexture.*\\.cpp$")
    list(FILTER SOURCE_FILES         EXCLUDE REGEX "(^|.*/)Function/Render/DebugDraw/DebugDrawBuffer\\.cpp$")
    list(FILTER SOURCE_FILES         EXCLUDE REGEX "(^|.*/)Function/Render/Lumen/.*\\.cpp$")
    list(FILTER SOURCE_FILES         EXCLUDE REGEX "(^|.*/)Function/ShaderLab/.*\\.cpp$")
    list(FILTER REGULAR_SOURCE_FILES EXCLUDE REGEX "(^|.*/)Function/Render/Interface/DX12/.*\\.cpp$")
    list(FILTER REGULAR_SOURCE_FILES EXCLUDE REGEX "(^|.*/)Function/Render/Interface/Vulkan/.*\\.cpp$")
    list(FILTER REGULAR_SOURCE_FILES EXCLUDE REGEX "(^|.*/)Function/Render/Interface/Metal/.*\\.(cpp|mm|m)$")
    list(FILTER REGULAR_SOURCE_FILES EXCLUDE REGEX "(^|.*/)Function/Render/Passes/.*\\.cpp$")
    list(FILTER REGULAR_SOURCE_FILES EXCLUDE REGEX "(^|.*/)Function/Render/Texture/VirtualTexture.*\\.cpp$")
    list(FILTER REGULAR_SOURCE_FILES EXCLUDE REGEX "(^|.*/)Function/Render/DebugDraw/DebugDrawBuffer\\.cpp$")
    list(FILTER REGULAR_SOURCE_FILES EXCLUDE REGEX "(^|.*/)Function/Render/Lumen/.*\\.cpp$")
    list(FILTER REGULAR_SOURCE_FILES EXCLUDE REGEX "(^|.*/)Function/ShaderLab/.*\\.cpp$")
    # Jolt is stubbed on Web (engine/3rdparty/CMakeLists.txt); strip the
    # native physics translation units. Public APIs are kept linkable by
    # the PhysicsStubs.cpp shim under engine/Source/Platform/Web/.
    list(FILTER SOURCE_FILES         EXCLUDE REGEX "(^|.*/)[Ff]unction/[Pp]hysics/.*\\.cpp$")
    list(FILTER REGULAR_SOURCE_FILES EXCLUDE REGEX "(^|.*/)[Ff]unction/[Pp]hysics/.*\\.cpp$")
    # Strip Platform/Windows/ files (not applicable to Web/Emscripten)
    list(FILTER HEADER_FILES         EXCLUDE REGEX "(^|.*/)Function/Render/Platform/Windows/.*")
    list(FILTER SOURCE_FILES         EXCLUDE REGEX "(^|.*/)Function/Render/Platform/Windows/.*\\.cpp$")
    list(FILTER REGULAR_HEADER_FILES EXCLUDE REGEX "(^|.*/)Function/Render/Platform/Windows/.*")
    list(FILTER REGULAR_SOURCE_FILES EXCLUDE REGEX "(^|.*/)Function/Render/Platform/Windows/.*\\.cpp$")
    message(STATUS "Runtime(Web): Vulkan/DX12/Metal disabled; using WebGL2 RHI; excluded Passes/VirtualTexture/DebugDrawBuffer/Lumen/ShaderLab/physics sources")
else()
    set(ZENGINE_RHI_WEBGL2 OFF CACHE INTERNAL "WebGL2 RHI backend disabled (non-Emscripten build)")
    list(FILTER SOURCE_FILES         EXCLUDE REGEX "(^|.*/)Function/Render/Interface/WebGL2/.*\\.cpp$")
    list(FILTER REGULAR_SOURCE_FILES EXCLUDE REGEX "(^|.*/)Function/Render/Interface/WebGL2/.*\\.cpp$")
endif()

if(ANDROID OR Z_PLATFORM_OHOS OR OHOS OR CMAKE_SYSTEM_NAME STREQUAL "OHOS")
    list(FILTER SOURCE_FILES EXCLUDE REGEX "(^|.*/)Function/Render/Interface/DX12/.*\\.cpp$")
    list(FILTER SOURCE_FILES EXCLUDE REGEX "(^|.*/)Function/Render/Interface/Metal/.*\\.(cpp|mm|m)$")
    list(FILTER REGULAR_SOURCE_FILES EXCLUDE REGEX "(^|.*/)Function/Render/Interface/DX12/.*\\.cpp$")
    list(FILTER REGULAR_SOURCE_FILES EXCLUDE REGEX "(^|.*/)Function/Render/Interface/Metal/.*\\.(cpp|mm|m)$")
    # DX12-only passes (DX12MainCameraPass / MainCameraRp2Pass) include <d3d12.h>;
    # strip them on Vulkan mobile.
    list(FILTER SOURCE_FILES EXCLUDE REGEX "(^|.*/)Function/Render/Passes/DX12.*\\.cpp$")
    list(FILTER SOURCE_FILES EXCLUDE REGEX "(^|.*/)Function/Render/Passes/MainCameraRp2Pass\\.cpp$")
    list(FILTER REGULAR_SOURCE_FILES EXCLUDE REGEX "(^|.*/)Function/Render/Passes/DX12.*\\.cpp$")
    list(FILTER REGULAR_SOURCE_FILES EXCLUDE REGEX "(^|.*/)Function/Render/Passes/MainCameraRp2Pass\\.cpp$")
    # DX12-only ShaderLab compiler includes <d3d12.h>; the Vulkan compiler (ShaderLabCompiler.cpp) is kept.
    list(FILTER SOURCE_FILES EXCLUDE REGEX "(^|.*/)Function/ShaderLab/ShaderLabDx12.*\\.(cpp|h)$")
    list(FILTER REGULAR_SOURCE_FILES EXCLUDE REGEX "(^|.*/)Function/ShaderLab/ShaderLabDx12.*\\.(cpp|h)$")
    # Strip Platform/Windows/ files (not applicable to Android/OHOS)
    list(FILTER HEADER_FILES         EXCLUDE REGEX "(^|.*/)Function/Render/Platform/Windows/.*")
    list(FILTER SOURCE_FILES         EXCLUDE REGEX "(^|.*/)Function/Render/Platform/Windows/.*\\.cpp$")
    list(FILTER REGULAR_HEADER_FILES EXCLUDE REGEX "(^|.*/)Function/Render/Platform/Windows/.*")
    list(FILTER REGULAR_SOURCE_FILES EXCLUDE REGEX "(^|.*/)Function/Render/Platform/Windows/.*\\.cpp$")
elseif(APPLE)
    list(FILTER SOURCE_FILES EXCLUDE REGEX "(^|.*/)Function/Render/[Ii]nterface/[Dd][Xx]12/.*\\.cpp$")
    list(FILTER SOURCE_FILES EXCLUDE REGEX "(^|.*/)Function/Render/[Ii]nterface/[Vv]ulkan/.*\\.cpp$")
    # Passes/ 里只有一部分是平台相关的，别整目录排除。
    #
    # 整目录排除的代价是：MainCameraRp1Pass（GBuffer + 延迟光照）、
    # MainCameraFramebufferResources、ShadowPassShared 这些**完全平台无关**的 TU 在
    # Apple 上一次都不会被编译 —— 改错一行、少包含一个头、拼错一个成员，本机永远
    # 发现不了，只能等 Windows CI。Metal RHI 已内置 SPIR-V -> MSL（SPIRV-Cross），
    # shader 也不是障碍：这些 pass 用的就是同一份 SPIR-V blob。
    #
    # 下面只排除真正拉 Vulkan / DX12 头的文件（逐个用 Apple 的真实编译参数验过）：
    #   MainCameraPass / ColorGradingPass / CombineUIPass / FXAAPass / PickPass
    #                                                     -> include VulkanRHI.h
    #   BloomPass / TAAPass                               -> include VulkanRHI.h
    #   DX12MainCameraPass / BindlessTonemapPass -> d3d12 / DX12RHI.h
    #
    # ParticlePass 原来也在这个名单里，现已解耦放出：它 include VulkanRHI.h 只为
    # vulkan.h 的 typedef（VkDeviceSize / VkSpecialization* / VkPipelineStageFlags）
    # 和 VK_TRUE，RHI 层全有对应物；唯一真调 Vulkan API 的 vkCreatePipelineCache
    # 还是在一段注释里。见 ParticlePass.cpp 的头注释。
    #
    # MainCameraRp2Pass 已经不在名单里了：它 include DX12RHI.h 只为 <d3d12.h>，
    # 而整个 TU 一个 d3d12 符号都不用（全程走 RHI 抽象，连 input attachment 都没
    # 碰 —— 两个 descriptor 都是 COMBINED_IMAGE_SAMPLER）。把那句 include 收进
    # _WIN32 守卫之后它在 Apple 上编译通过，于是本机也能校验这个 TU。
    #
    # ⚠️ 它在 Apple 上**只是参与编译，不在运行时被使用**：它的 LoadShader 只认
    # HLSL，而 MetalRHI 的 CreateShaderModuleFromFile 只收 .metal。Apple 真正跑的
    # RP2 是 AppleRp2Chain（MSL，离屏那一半），见那个文件的头注释。
    #
    # 留下的都是干净的：MainCameraRp1Pass / MainCameraRp2Pass /
    # MainCameraFramebufferResources / MainCameraPassShaderCommon /
    # ShadowPassShared / DirectionalLightPass / PointLightPass / UIPass。
    # 注意 MainCameraPass\\.cpp$ 不会误伤 MainCameraRp1Pass.cpp /
    # MainCameraFramebufferResources.cpp / MainCameraPassShaderCommon.cpp。
    set(ZENGINE_APPLE_EXCLUDED_PASSES
        "(^|.*/)Function/Render/Passes/(DX12.*|BindlessTonemapPass|MainCameraPass|ColorGradingPass|CombineUIPass|FXAAPass|PickPass|BloomPass|TAAPass)\\.cpp$")
    list(FILTER SOURCE_FILES EXCLUDE REGEX "${ZENGINE_APPLE_EXCLUDED_PASSES}")
    list(FILTER SOURCE_FILES EXCLUDE REGEX "(^|.*/)Function/Render/Texture/VirtualTexture.*\\.cpp$")
    list(FILTER SOURCE_FILES EXCLUDE REGEX "(^|.*/)Function/Render/[Dd]ebug[Dd]raw/DebugDrawBuffer\\.cpp$")
    list(FILTER REGULAR_SOURCE_FILES EXCLUDE REGEX "(^|.*/)Function/Render/[Ii]nterface/[Dd][Xx]12/.*\\.cpp$")
    list(FILTER REGULAR_SOURCE_FILES EXCLUDE REGEX "(^|.*/)Function/Render/[Ii]nterface/[Vv]ulkan/.*\\.cpp$")
    list(FILTER SOURCE_FILES EXCLUDE REGEX "(^|.*/)Function/ShaderLab/ShaderLabDx12.*\\.cpp$")
    list(FILTER REGULAR_SOURCE_FILES EXCLUDE REGEX "(^|.*/)Function/ShaderLab/ShaderLabDx12.*\\.cpp$")
    # ShaderLab's SPIR-V compiler TU is the only thing that pulls in Vulkan's
    # ShaderCompiler, and that TU is removed on Apple (see REMOVE_ITEM above).
    # Keep the parser + HLSL extract in ZRuntime -- same split the Vulkan-free
    # Windows branch already makes -- or ZEditor fails to link with three
    # undefined ShaderCompiler symbols.
    list(FILTER SOURCE_FILES EXCLUDE REGEX "(^|.*/)Function/ShaderLab/ShaderLabCompiler\\.cpp$")
    list(FILTER REGULAR_SOURCE_FILES EXCLUDE REGEX "(^|.*/)Function/ShaderLab/ShaderLabCompiler\\.cpp$")
    list(FILTER REGULAR_SOURCE_FILES EXCLUDE REGEX "${ZENGINE_APPLE_EXCLUDED_PASSES}")
    list(FILTER REGULAR_SOURCE_FILES EXCLUDE REGEX "(^|.*/)Function/Render/Texture/VirtualTexture.*\\.cpp$")
    list(FILTER REGULAR_SOURCE_FILES EXCLUDE REGEX "(^|.*/)Function/Render/[Dd]ebug[Dd]raw/DebugDrawBuffer\\.cpp$")
    list(REMOVE_ITEM SOURCE_FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/Function/Render/Interface/DX12/DX12RHI.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Function/Render/Interface/DX12/DX12ShaderCompiler.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Function/Render/Interface/Vulkan/ShaderCompiler.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Function/Render/Interface/Vulkan/VulkanRHI.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Function/Render/Interface/Vulkan/VulkanUtil.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Function/Render/Interface/Vulkan/VulkanVma.cpp")
    list(REMOVE_ITEM REGULAR_SOURCE_FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/Function/Render/Interface/DX12/DX12RHI.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Function/Render/Interface/DX12/DX12ShaderCompiler.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Function/Render/Interface/Vulkan/ShaderCompiler.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Function/Render/Interface/Vulkan/VulkanRHI.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Function/Render/Interface/Vulkan/VulkanUtil.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Function/Render/Interface/Vulkan/VulkanVma.cpp")
    # Strip Platform/Windows/ files (not applicable to Apple)
    list(FILTER HEADER_FILES         EXCLUDE REGEX "(^|.*/)Function/Render/Platform/Windows/.*")
    list(FILTER SOURCE_FILES         EXCLUDE REGEX "(^|.*/)Function/Render/Platform/Windows/.*\\.cpp$")
    list(FILTER REGULAR_HEADER_FILES EXCLUDE REGEX "(^|.*/)Function/Render/Platform/Windows/.*")
    list(FILTER REGULAR_SOURCE_FILES EXCLUDE REGEX "(^|.*/)Function/Render/Platform/Windows/.*\\.cpp$")
    list(APPEND SOURCE_FILES "${PLATFORM_DIR}/Android/LocalFileSystemAndroid.cpp")
    list(APPEND HEADER_FILES "${PLATFORM_DIR}/Android/LocalFileSystemAndroid.h")
elseif(WIN32 AND NOT ZENGINE_USE_VULKAN)
    # Windows default: build DX12 only, strip every Vulkan translation unit. Passes and a
    # handful of render resources currently depend on VulkanRHI headers, so we exclude
    # them entirely and rely on the DX12 render pipeline path at runtime.
    list(FILTER SOURCE_FILES EXCLUDE REGEX "(^|.*/)Function/Render/Interface/Metal/.*\\.(cpp|mm|m)$")
    list(FILTER SOURCE_FILES EXCLUDE REGEX "(^|.*/)Function/Render/Interface/Vulkan/.*\\.cpp$")
    list(FILTER SOURCE_FILES EXCLUDE REGEX "(^|.*/)Function/Render/Texture/VirtualTexture.*\\.cpp$")
    list(FILTER SOURCE_FILES EXCLUDE REGEX "(^|.*/)Function/Render/DebugDraw/DebugDrawBuffer\\.cpp$")
    list(FILTER SOURCE_FILES EXCLUDE REGEX "(^|.*/)Function/Render/Lumen/.*\\.cpp$")
    # ShaderLab parser + HLSL extract stay in ZRuntime (Vulkan-free); only the
    # SPIR-V compiler TU is excluded so DX12ShaderCompiler can extract .shader.
    list(FILTER SOURCE_FILES EXCLUDE REGEX "(^|.*/)Function/ShaderLab/ShaderLabCompiler\\.cpp$")
    list(FILTER SOURCE_FILES EXCLUDE REGEX "(^|.*/)Function/ShaderLab/ShaderLabTest\\.cpp$")
    list(FILTER REGULAR_SOURCE_FILES EXCLUDE REGEX "(^|.*/)Function/Render/Interface/Metal/.*\\.(cpp|mm|m)$")
    list(FILTER REGULAR_SOURCE_FILES EXCLUDE REGEX "(^|.*/)Function/Render/Interface/Vulkan/.*\\.cpp$")
    list(FILTER REGULAR_SOURCE_FILES EXCLUDE REGEX "(^|.*/)Function/Render/Texture/VirtualTexture.*\\.cpp$")
    list(FILTER REGULAR_SOURCE_FILES EXCLUDE REGEX "(^|.*/)Function/Render/DebugDraw/DebugDrawBuffer\\.cpp$")
    list(FILTER REGULAR_SOURCE_FILES EXCLUDE REGEX "(^|.*/)Function/Render/Lumen/.*\\.cpp$")
    list(FILTER REGULAR_SOURCE_FILES EXCLUDE REGEX "(^|.*/)Function/ShaderLab/ShaderLabCompiler\\.cpp$")
    list(FILTER REGULAR_SOURCE_FILES EXCLUDE REGEX "(^|.*/)Function/ShaderLab/ShaderLabTest\\.cpp$")

    # Exclude all pass source files first, then restore the DX12-only passes.
    # CMake's regex engine does not support look-ahead, so we do this in two steps.
    set(_WIN_DX12_PASS_FILES)
    foreach(_src IN LISTS SOURCE_FILES)
        if(_src MATCHES "(^|.*/)Function/Render/Passes/DX12.*\\.cpp$" OR
           _src MATCHES "(^|.*/)Function/Render/Passes/MainCameraFramebufferResources\\.cpp$" OR
           _src MATCHES "(^|.*/)Function/Render/Passes/MainCameraRp1Pass\\.cpp$" OR
           _src MATCHES "(^|.*/)Function/Render/Passes/MainCameraPassShaderCommon\\.cpp$" OR
           _src MATCHES "(^|.*/)Function/Render/Passes/BindlessTonemapPass\\.cpp$" OR
           _src MATCHES "(^|.*/)Function/Render/Passes/MainCameraRp2Pass\\.cpp$" OR
           _src MATCHES "(^|.*/)Function/Render/Passes/DirectionalLightPass\\.cpp$" OR
           _src MATCHES "(^|.*/)Function/Render/Passes/PointLightPass\\.cpp$" OR
           _src MATCHES "(^|.*/)Function/Render/Passes/ShadowPassShared\\.cpp$" OR
           _src MATCHES "(^|.*/)Function/Render/Passes/UIPass\\.cpp$")
            list(APPEND _WIN_DX12_PASS_FILES "${_src}")
        endif()
    endforeach()
    list(FILTER SOURCE_FILES EXCLUDE REGEX "(^|.*/)Function/Render/Passes/.*\\.cpp$")
    list(APPEND SOURCE_FILES ${_WIN_DX12_PASS_FILES})
    list(APPEND SOURCE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/Function/Render/RenderResourceDx12.cpp")

    set(_WIN_DX12_PASS_FILES_REG)
    foreach(_src IN LISTS REGULAR_SOURCE_FILES)
        if(_src MATCHES "(^|.*/)Function/Render/Passes/DX12.*\\.cpp$" OR
           _src MATCHES "(^|.*/)Function/Render/Passes/MainCameraFramebufferResources\\.cpp$" OR
           _src MATCHES "(^|.*/)Function/Render/Passes/MainCameraRp1Pass\\.cpp$" OR
           _src MATCHES "(^|.*/)Function/Render/Passes/MainCameraPassShaderCommon\\.cpp$" OR
           _src MATCHES "(^|.*/)Function/Render/Passes/BindlessTonemapPass\\.cpp$" OR
           _src MATCHES "(^|.*/)Function/Render/Passes/MainCameraRp2Pass\\.cpp$" OR
           _src MATCHES "(^|.*/)Function/Render/Passes/DirectionalLightPass\\.cpp$" OR
           _src MATCHES "(^|.*/)Function/Render/Passes/PointLightPass\\.cpp$" OR
           _src MATCHES "(^|.*/)Function/Render/Passes/ShadowPassShared\\.cpp$" OR
           _src MATCHES "(^|.*/)Function/Render/Passes/UIPass\\.cpp$")
            list(APPEND _WIN_DX12_PASS_FILES_REG "${_src}")
        endif()
    endforeach()
    list(FILTER REGULAR_SOURCE_FILES EXCLUDE REGEX "(^|.*/)Function/Render/Passes/.*\\.cpp$")
    list(APPEND REGULAR_SOURCE_FILES ${_WIN_DX12_PASS_FILES_REG})
    list(APPEND REGULAR_SOURCE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/Function/Render/RenderResourceDx12.cpp")

    unset(_WIN_DX12_PASS_FILES)
    unset(_WIN_DX12_PASS_FILES_REG)
    message(STATUS "Runtime(Windows): Vulkan disabled; excluded vulkan/metal/Passes(non-DX12)/VirtualTexture/DebugDrawBuffer/Lumen sources")
elseif(WIN32 AND ZENGINE_USE_VULKAN)
    # Win+Vulkan ships both RHIs; mesh upload must dispatch to the DX12 path at runtime.
    list(APPEND SOURCE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/Function/Render/RenderResourceDx12.cpp")
    list(APPEND REGULAR_SOURCE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/Function/Render/RenderResourceDx12.cpp")
    message(STATUS "Runtime(Windows+Vulkan): including RenderResourceDx12 mesh upload dispatch")
endif()

if(APPLE)
    set_source_files_properties(
        "${CMAKE_CURRENT_SOURCE_DIR}/RegisterRuntime.cpp"
        PROPERTIES LANGUAGE OBJCXX SKIP_PRECOMPILE_HEADERS ON)
    set_source_files_properties(${OBJC_SOURCE_FILES} PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
endif()

#source_group(TREE "${ENGINE_ROOT_DIR}/schema" FILES ${SCHEMA_FILES})

# Organize regular files under runtime directory
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${REGULAR_HEADER_FILES} ${REGULAR_SOURCE_FILES})
# Organize platform files under platform directory
# Use PLATFORM_DIR as root so Visual Studio shows "Platform/Win" directory structure
if(PLATFORM_SUBDIR AND EXISTS "${PLATFORM_SUBDIR}" AND (PLATFORM_HEADERS OR PLATFORM_SOURCES))
    # Ensure PLATFORM_DIR and the file list are normalized via the SAME path
    # canonicalization that CMake uses internally for source_group(TREE) prefix
    # comparisons (CMake 4.x tightened these checks so a string mismatch in
    # case is enough to fail). file(REAL_PATH) resolves both to the on-disk
    # casing, eliminating the "Source" vs "source" mismatch on Windows.
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

# Collect any .natvis files that live in the runtime source tree and add them to the
# header list so they are present when the Visual Studio project is generated.
if(CMAKE_GENERATOR MATCHES "Visual Studio" OR CMAKE_GENERATOR MATCHES "Visual Studio.*")
    file(GLOB_RECURSE RUNTIME_NATVIS_FILES CONFIGURE_DEPENDS "${ENGINE_ROOT_DIR}/Source/Runtime/*.natvis")
    if(RUNTIME_NATVIS_FILES)
        message(STATUS "Runtime: Found ${RUNTIME_NATVIS_FILES} natvis visualizer files (will add to project)")
        # Mark them as Natvis items for Visual Studio
        foreach(_natvis_file IN LISTS RUNTIME_NATVIS_FILES)
            set_source_files_properties(${_natvis_file} PROPERTIES VS_ITEM_TYPE "Natvis")
        endforeach()
        # Place natvis files into a source group (so VS shows them under the runtime tree)
        # Use file(REAL_PATH) for both the root and each natvis file so that
        # CMake 4.x's strict prefix check sees identical canonicalised casing.
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

        list(APPEND HEADER_FILES ${RUNTIME_NATVIS_FILES})
    endif()
endif()

# Exclude shared PCH source/header (we use REUSE_FROM ZEnginePCH)
list(REMOVE_ITEM SOURCE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/pch.cpp")
list(REMOVE_ITEM HEADER_FILES "${CMAKE_CURRENT_SOURCE_DIR}/pch.h")

# ---------------------------------------------------------------------------
# Standalone demo / smoke-test translation units that define their own main().
# They live under the runtime tree purely as developer scratchpads and have a
# parallel CMakeLists.txt to be built as separate executables. They must not
# end up inside ZRuntime.lib, otherwise /WHOLEARCHIVE (enabled below for
# reflection auto-registration) will pull their main() into every consumer
# exe and trigger LNK2005 "main already defined".
# ---------------------------------------------------------------------------
list(REMOVE_ITEM SOURCE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/Function/ShaderLab/ShaderLabTest.cpp")
# PR5c: DX12 bindless smoke-test main. Same exclusion rationale as
# shader_lab_test above. Built only when ZENGINE_BUILD_BINDLESS_SMOKE_TEST=ON
# (see Function/Render/Interface/DX12/Test/CMakeLists.txt).
list(REMOVE_ITEM SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Function/Render/Interface/DX12/Test/DX12BindlessSmokeTest.cpp")
list(REMOVE_ITEM REGULAR_SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Function/Render/Interface/DX12/Test/DX12BindlessSmokeTest.cpp")
# GPU-Spine 蒙皮验收夹具的 main。同一套排除理由：它有独立 exe，且由
# ZENGINE_BUILD_SPINE_SKINNING_PARITY_TEST 单独控制是否构建。
list(REMOVE_ITEM SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Function/Render/Interface/DX12/Test/SpineSkinningParityTest.cpp")
list(REMOVE_ITEM REGULAR_SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Function/Render/Interface/DX12/Test/SpineSkinningParityTest.cpp")
# GPU-Spine DX12 光栅对拍的 main（同上，独立 exe，同一开关）。
list(REMOVE_ITEM SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Function/Render/Interface/DX12/Test/SpineGpuDx12RasterTest.cpp")
list(REMOVE_ITEM REGULAR_SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Function/Render/Interface/DX12/Test/SpineGpuDx12RasterTest.cpp")
# ShaderLab Unity-compat smoke-test main. Same exclusion rationale. Built only
# when ZENGINE_BUILD_SHADERLAB_UNITY_COMPAT_TEST=ON (see
# Function/ShaderLab/Test/CMakeLists.txt).
list(REMOVE_ITEM SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Function/ShaderLab/Test/ShaderLabUnityCompatTest.cpp")
list(REMOVE_ITEM REGULAR_SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Function/ShaderLab/Test/ShaderLabUnityCompatTest.cpp")
# ShaderLab #include resolver smoke-test main. Same exclusion rationale. Built
# only when ZENGINE_BUILD_SHADERLAB_INCLUDE_TEST=ON (see
# Function/ShaderLab/Test/CMakeLists.txt). Note the resolver itself,
# ShaderLabIncludeProcessor.cpp, is NOT excluded -- it is library code and
# belongs in ZRuntime.
list(REMOVE_ITEM SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Function/ShaderLab/Test/ShaderLabIncludeResolverTest.cpp")
list(REMOVE_ITEM REGULAR_SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Function/ShaderLab/Test/ShaderLabIncludeResolverTest.cpp")
# ShaderLab SPIR-V cache-manifest smoke-test main. Same exclusion rationale.
# Built only when ZENGINE_BUILD_SHADERLAB_CACHE_TEST=ON (see
# Function/ShaderLab/Test/CMakeLists.txt). As with the resolver, the code under
# test (ShaderLabCacheManifest.cpp) is NOT excluded -- it is library code.
list(REMOVE_ITEM SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Function/ShaderLab/Test/ShaderLabCacheManifestTest.cpp")
list(REMOVE_ITEM REGULAR_SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Function/ShaderLab/Test/ShaderLabCacheManifestTest.cpp")
# ShaderLab legacy output-semantic rewrite smoke-test main. Same exclusion
# rationale. Built only when ZENGINE_BUILD_SHADERLAB_SEMANTIC_TEST=ON (see
# Function/ShaderLab/Test/CMakeLists.txt). ShaderLabSemanticRewrite.cpp is NOT
# excluded -- it is library code.
list(REMOVE_ITEM SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Function/ShaderLab/Test/ShaderLabSemanticRewriteTest.cpp")
list(REMOVE_ITEM REGULAR_SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Function/ShaderLab/Test/ShaderLabSemanticRewriteTest.cpp")
# ShaderLab surface-shader desugaring smoke-test main. Same exclusion
# rationale. Built only when ZENGINE_BUILD_SHADERLAB_SURFACE_TEST=ON (see
# Function/ShaderLab/Test/CMakeLists.txt). ShaderLabSurface.cpp is NOT
# excluded -- it is library code.
list(REMOVE_ITEM SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Function/ShaderLab/Test/ShaderLabSurfaceTest.cpp")
list(REMOVE_ITEM REGULAR_SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Function/ShaderLab/Test/ShaderLabSurfaceTest.cpp")
# PR-V2: Vulkan bindless smoke-test main. Same exclusion rationale as
# the DX12 entry above. Built only when ZENGINE_BUILD_VK_BINDLESS_SMOKE_TEST=ON
# (see Function/Render/Interface/Vulkan/Test/CMakeLists.txt).
list(REMOVE_ITEM SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Function/Render/Interface/Vulkan/Test/VulkanBindlessSmokeTest.cpp")
list(REMOVE_ITEM REGULAR_SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Function/Render/Interface/Vulkan/Test/VulkanBindlessSmokeTest.cpp")
# PR-A1: audio DSP smoke-test main. Same carve-out rationale as every entry
# above: it owns main() and must not be whole-archived into ZRuntime. Built
# only when ZENGINE_BUILD_AUDIO_SMOKE_TEST=ON (see Function/Audio/Test/
# CMakeLists.txt).
list(REMOVE_ITEM SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Function/Audio/Test/AudioMixerSmokeTest.cpp")
list(REMOVE_ITEM REGULAR_SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Function/Audio/Test/AudioMixerSmokeTest.cpp")
# P0-6: network layer smoke-test main. Same carve-out rationale as every entry
# above: it owns main() and must not be whole-archived into ZRuntime. Built
# only when ZENGINE_BUILD_NETWORK_SMOKE_TEST=ON (see Function/Network/Test/
# CMakeLists.txt).
list(REMOVE_ITEM SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Function/Network/Test/NetworkSmokeTest.cpp")
list(REMOVE_ITEM REGULAR_SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Function/Network/Test/NetworkSmokeTest.cpp")
# PR-PAK1: AssetBundle smoke-test main. Same carve-out rationale as every
# entry above: it owns main() and must not be whole-archived into ZRuntime.
# Built only when ZENGINE_BUILD_ASSET_BUNDLE_SMOKE_TEST=ON (see
# AssetBundle/Test/CMakeLists.txt).
list(REMOVE_ITEM SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/AssetBundle/Test/AssetBundleSmokeTest.cpp")
list(REMOVE_ITEM REGULAR_SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/AssetBundle/Test/AssetBundleSmokeTest.cpp")
# PlayerSettings smoke-test main. Same carve-out rationale: it owns main() and
# must not be whole-archived into ZRuntime. Built only when
# ZENGINE_BUILD_PLAYER_SETTINGS_SMOKE_TEST=ON.
list(REMOVE_ITEM SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Function/PlayerSettings/Test/PlayerSettingsSmokeTest.cpp")
list(REMOVE_ITEM REGULAR_SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Function/PlayerSettings/Test/PlayerSettingsSmokeTest.cpp")
# PR-SK1: skeletal skinning smoke-test main. Same carve-out rationale as every
# entry above: it owns main() and must not be whole-archived into ZRuntime.
# Built only when ZENGINE_BUILD_ANIMATION_SMOKE_TEST=ON (see
# Function/Animation/Test/CMakeLists.txt).
list(REMOVE_ITEM SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Function/Animation/Test/AnimationSkinningSmokeTest.cpp")
list(REMOVE_ITEM REGULAR_SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Function/Animation/Test/AnimationSkinningSmokeTest.cpp")
# PR-SK3: animator state-machine smoke-test main. Same rationale; built only
# when ZENGINE_BUILD_ANIMATOR_SMOKE_TEST=ON (see
# Function/Animation/Test/CMakeLists.txt).
list(REMOVE_ITEM SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Function/Animation/Test/AnimatorSmokeTest.cpp")
list(REMOVE_ITEM REGULAR_SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Function/Animation/Test/AnimatorSmokeTest.cpp")
# PR-SE2: SerializedFile schema-evolution smoke-test main. Same
# exclusion rationale as the bindless smoke-tests above -- the smoke
# test owns its own main() and must NOT be wholearchive'd into
# ZRuntime, or every downstream consumer (ZEditor, ZLauncher, etc.)
# would hit LNK2005 "main already defined". Built only when
# ZENGINE_BUILD_SCHEMA_EVOLUTION_SMOKE_TEST=ON (see
# Core/Serialize/test/CMakeLists.txt).
list(REMOVE_ITEM SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Core/Serialize/Test/SchemaEvolutionSmokeTest.cpp")
list(REMOVE_ITEM REGULAR_SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Core/Serialize/Test/SchemaEvolutionSmokeTest.cpp")
# PR-SE3a-refine pptr smoke-test main carve-out
list(REMOVE_ITEM SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Core/Serialize/Test/PptrSmokeTest.cpp")
list(REMOVE_ITEM REGULAR_SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Core/Serialize/Test/PptrSmokeTest.cpp")
# YAML round-trip smoke-test main carve-out
list(REMOVE_ITEM SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Core/Serialize/Test/YamlRoundTripSmokeTest.cpp")
list(REMOVE_ITEM REGULAR_SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Core/Serialize/Test/YamlRoundTripSmokeTest.cpp")
# Reflection codegen (ZHeaderTool) smoke-test main carve-out
list(REMOVE_ITEM SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Core/Serialize/Test/ReflectionCodegenSmokeTest.cpp")
list(REMOVE_ITEM REGULAR_SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Core/Serialize/Test/ReflectionCodegenSmokeTest.cpp")
list(REMOVE_ITEM SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Core/Serialize/Test/SmokeTestAppleLinkStubs.cpp")
list(REMOVE_ITEM REGULAR_SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Core/Serialize/Test/SmokeTestAppleLinkStubs.cpp")
# ProjectInfo path-field regression gate: owns main(), same LNK2005 carve-out.
list(REMOVE_ITEM SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Core/Serialize/Test/ProjectInfoPathsSmokeTest.cpp")
list(REMOVE_ITEM REGULAR_SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Core/Serialize/Test/ProjectInfoPathsSmokeTest.cpp")
# Derived Data Cache smoke-test main carve-out (texture cook P3)
list(REMOVE_ITEM SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Resource/Cache/Test/DerivedDataCacheSmokeTest.cpp")
list(REMOVE_ITEM REGULAR_SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Resource/Cache/Test/DerivedDataCacheSmokeTest.cpp")
# AI layer smoke-test main carve-out (blackboard + behaviour tree, no engine
# systems). Same rationale: it owns main() and must not be whole-archived into
# ZRuntime, or every downstream consumer would hit LNK2005.
list(REMOVE_ITEM SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Function/AI/Test/AISmokeTest.cpp")
list(REMOVE_ITEM REGULAR_SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Function/AI/Test/AISmokeTest.cpp")
# RenderGraph compiler smoke-test main carve-out. The graph compiler itself
# (Function/Render/Graph/*.cpp) IS part of ZRuntime -- only the test's main()
# has to stay out of the whole-archive.
list(REMOVE_ITEM SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Function/Render/Graph/Test/RenderGraphSmokeTest.cpp")
list(REMOVE_ITEM REGULAR_SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Function/Render/Graph/Test/RenderGraphSmokeTest.cpp")
# Same carve-out for the executor smoke test (also owns a main()).
list(REMOVE_ITEM SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Function/Render/Graph/Test/RenderGraphExecuteSmokeTest.cpp")
list(REMOVE_ITEM REGULAR_SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Function/Render/Graph/Test/RenderGraphExecuteSmokeTest.cpp")
# RHI indirect-draw contract smoke-test main carve-out (same rationale).
list(REMOVE_ITEM SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Function/Render/Interface/Test/RHIIndirectDrawSmokeTest.cpp")
list(REMOVE_ITEM REGULAR_SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Function/Render/Interface/Test/RHIIndirectDrawSmokeTest.cpp")
# Lumen scene-geometry smoke-test main carve-out (same rationale). Built only
# when ZENGINE_BUILD_LUMEN_SCENE_GEOMETRY_SMOKE_TEST=ON (see
# Function/Render/Lumen/Test/CMakeLists.txt).
list(REMOVE_ITEM SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Function/Render/Lumen/Test/LumenSceneGeometrySmokeTest.cpp")
list(REMOVE_ITEM REGULAR_SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Function/Render/Lumen/Test/LumenSceneGeometrySmokeTest.cpp")
# Lumen software ray-tracing smoke-test main carve-out (same rationale). Built only
# when ZENGINE_BUILD_LUMEN_RAY_TRACING_SMOKE_TEST=ON (see
# Function/Render/Lumen/Test/CMakeLists.txt).
list(REMOVE_ITEM SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Function/Render/Lumen/Test/LumenRayTracingSmokeTest.cpp")
list(REMOVE_ITEM REGULAR_SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Function/Render/Lumen/Test/LumenRayTracingSmokeTest.cpp")
# Lumen surface-cache smoke-test main carve-out (same rationale). Built only when
# ZENGINE_BUILD_LUMEN_SURFACE_CACHE_SMOKE_TEST=ON (see
# Function/Render/Lumen/Test/CMakeLists.txt).
list(REMOVE_ITEM SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Function/Render/Lumen/Test/LumenSurfaceCacheSmokeTest.cpp")
list(REMOVE_ITEM REGULAR_SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Function/Render/Lumen/Test/LumenSurfaceCacheSmokeTest.cpp")
# GPU-Spine importer test main carve-out (same rationale). Built only when
# ZENGINE_BUILD_SPINE_IMPORT_TEST=ON (see Function/Render/Spine/Test/CMakeLists.txt).
list(REMOVE_ITEM SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Function/Render/Spine/Test/SpineGpuImportTest.cpp")
list(REMOVE_ITEM REGULAR_SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Function/Render/Spine/Test/SpineGpuImportTest.cpp")
# GPU-Spine reference-parity test main carve-out (same rationale). It has its own
# main() and links the reference spine-cpp; pulling it into ZRuntime would give
# LNK2005 (duplicate main) and drag third-party headers into the runtime.
list(REMOVE_ITEM SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Function/Render/Spine/Test/SpineGpuRefParityTest.cpp")
list(REMOVE_ITEM REGULAR_SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Function/Render/Spine/Test/SpineGpuRefParityTest.cpp")
# GPU-Spine CPU benchmark main carve-out (same rationale). It has its own main()
# and deliberately does NOT link ZRuntime, so it must not be compiled into it.
list(REMOVE_ITEM SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Function/Render/Spine/Test/SpineGpuBenchmark.cpp")
list(REMOVE_ITEM REGULAR_SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Function/Render/Spine/Test/SpineGpuBenchmark.cpp")
# GPU-Spine hot-loop test main carve-out (same rationale): own main(), and it
# replaces the global operator new, so it must not be compiled into ZRuntime.
list(REMOVE_ITEM SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Function/Render/Spine/Test/SpineGpuHotLoopTest.cpp")
list(REMOVE_ITEM REGULAR_SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Function/Render/Spine/Test/SpineGpuHotLoopTest.cpp")
# Lumen global-illumination smoke-test main carve-out (same rationale). Built only
# when ZENGINE_BUILD_LUMEN_GLOBAL_ILLUMINATION_SMOKE_TEST=ON (see
# Function/Render/Lumen/Test/CMakeLists.txt).
list(REMOVE_ITEM SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Function/Render/Lumen/Test/LumenGlobalIlluminationSmokeTest.cpp")
list(REMOVE_ITEM REGULAR_SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Function/Render/Lumen/Test/LumenGlobalIlluminationSmokeTest.cpp")
# Lumen render-pass wiring smoke-test main carve-out (same rationale). Built only
# when ZENGINE_BUILD_LUMEN_RENDER_PASS_WIRING_SMOKE_TEST=ON (see
# Function/Render/Lumen/Test/CMakeLists.txt).
list(REMOVE_ITEM SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Function/Render/Lumen/Test/LumenRenderPassWiringSmokeTest.cpp")
list(REMOVE_ITEM REGULAR_SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Function/Render/Lumen/Test/LumenRenderPassWiringSmokeTest.cpp")
# Lumen per-frame bridge smoke-test main carve-out (same rationale). Built only
# when ZENGINE_BUILD_LUMEN_PER_FRAME_BRIDGE_SMOKE_TEST=ON (see
# Function/Render/Lumen/Test/CMakeLists.txt).
list(REMOVE_ITEM SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Function/Render/Lumen/Test/LumenPerFrameBridgeSmokeTest.cpp")
list(REMOVE_ITEM REGULAR_SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Function/Render/Lumen/Test/LumenPerFrameBridgeSmokeTest.cpp")
# Lumen reflections smoke-test main carve-out (same rationale). Built only when
# ZENGINE_BUILD_LUMEN_REFLECTIONS_SMOKE_TEST=ON (see
# Function/Render/Lumen/Test/CMakeLists.txt).
list(REMOVE_ITEM SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Function/Render/Lumen/Test/LumenReflectionsSmokeTest.cpp")
list(REMOVE_ITEM REGULAR_SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Function/Render/Lumen/Test/LumenReflectionsSmokeTest.cpp")
# Nanite cluster-builder smoke-test main carve-out (same rationale). Built only
# when ZENGINE_BUILD_NANITE_CLUSTER_SMOKE_TEST=ON (see
# Function/Render/Nanite/Test/CMakeLists.txt).
list(REMOVE_ITEM SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Function/Render/Nanite/Test/NaniteClusterBuilderSmokeTest.cpp")
list(REMOVE_ITEM REGULAR_SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Function/Render/Nanite/Test/NaniteClusterBuilderSmokeTest.cpp")
# Nanite culling / LOD smoke-test main carve-out (same rationale). Built only
# when ZENGINE_BUILD_NANITE_CULLING_SMOKE_TEST=ON (see
# Function/Render/Nanite/Test/CMakeLists.txt).
list(REMOVE_ITEM SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Function/Render/Nanite/Test/NaniteCullingSmokeTest.cpp")
list(REMOVE_ITEM REGULAR_SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Function/Render/Nanite/Test/NaniteCullingSmokeTest.cpp")
# Nanite scene-wiring smoke-test main carve-out (same rationale). Built only
# when ZENGINE_BUILD_NANITE_SCENE_WIRING_SMOKE_TEST=ON (see
# Function/Render/Nanite/Test/CMakeLists.txt).
list(REMOVE_ITEM SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Function/Render/Nanite/Test/NaniteSceneWiringSmokeTest.cpp")
list(REMOVE_ITEM REGULAR_SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Function/Render/Nanite/Test/NaniteSceneWiringSmokeTest.cpp")
# Nanite GPU-culling smoke-test main carve-out (same rationale). Built only
# when ZENGINE_BUILD_NANITE_GPU_CULLING_SMOKE_TEST=ON (see
# Function/Render/Nanite/Test/CMakeLists.txt).
list(REMOVE_ITEM SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Function/Render/Nanite/Test/NaniteGPUCullingSmokeTest.cpp")
list(REMOVE_ITEM REGULAR_SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Function/Render/Nanite/Test/NaniteGPUCullingSmokeTest.cpp")
# AppleSceneBlit smoke-test main carve-out (same rationale). Built only when
# ZENGINE_BUILD_APPLE_SCENE_BLIT_SMOKE_TEST=ON (see
# Function/Render/Test/CMakeLists.txt). It is the only guard on the MSL that
# carries the Metal scene onto the swapchain.
list(REMOVE_ITEM SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Function/Render/Test/AppleSceneBlitSmokeTest.cpp")
list(REMOVE_ITEM REGULAR_SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Function/Render/Test/AppleSceneBlitSmokeTest.cpp")
# Metal SPIR-V shader smoke-test main carve-out (same rationale). Guards the
# built-in shader -> MSL translation (see Function/Render/Test/CMakeLists.txt).
list(REMOVE_ITEM SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Function/Render/Test/MetalSpirvShaderSmokeTest.cpp")
list(REMOVE_ITEM REGULAR_SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Function/Render/Test/MetalSpirvShaderSmokeTest.cpp")
# 方向光阴影接线冒烟测试的 main carve-out（同理由）。它钉的是"Metal 上方向光阴影
# 被一条历史遗留判据挡死"和"Apple 的 Tick 从不复位 storage ring buffer"这两条静默
# 失效（见 Function/Render/Test/CMakeLists.txt）。
list(REMOVE_ITEM SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Function/Render/Test/DirectionalLightShadowSmokeTest.cpp")
list(REMOVE_ITEM REGULAR_SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Function/Render/Test/DirectionalLightShadowSmokeTest.cpp")
# Metal 场景深度冒烟测试的 main carve-out（同理由）。它钉的是"Metal 从不建场景深度
# 图 ⇒ GetDepthImageInfo() 恒 {UNDEFINED, nullptr}"（洞 #16）与"Metal 把 input
# attachment 当 color attachment 编号 ⇒ 延迟光照写错附件"（洞 #17）这两条静默失效
# （见 Function/Render/Test/CMakeLists.txt）。
list(REMOVE_ITEM SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Function/Render/Test/MetalSceneDepthSmokeTest.cpp")
list(REMOVE_ITEM REGULAR_SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Function/Render/Test/MetalSceneDepthSmokeTest.cpp")
# SSAO 冒烟测试的 main carve-out（同理由）。它钉的是 AppleSsaoPass 的 MSL（只在运行时
# 编译）、NDC→像素映射、视口越界、以及"延迟光照真的绑上了 AO 图"这条生效链
# （见 Function/Render/Test/CMakeLists.txt）。
list(REMOVE_ITEM SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Function/Render/Test/AppleSsaoSmokeTest.cpp")
list(REMOVE_ITEM REGULAR_SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Function/Render/Test/AppleSsaoSmokeTest.cpp")
# 分配器配对冒烟测试的 main carve-out（同理由）。Only built when
# ZENGINE_BUILD_MEMORY_PAIRING_SMOKE_TEST=ON (see Core/Memory/Test/CMakeLists.txt).
list(REMOVE_ITEM SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Core/Memory/Test/MemoryPairingSmokeTest.cpp")
list(REMOVE_ITEM REGULAR_SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Core/Memory/Test/MemoryPairingSmokeTest.cpp")
# Addressables async-loading smoke-test main carve-out. Same rationale: it owns
# main() and must not be whole-archived into ZRuntime, or every consumer exe
# gets LNK2005 "main already defined". Built only when
# ZENGINE_BUILD_ADDRESSABLES_SMOKE_TEST=ON (see
# Resource/Addressables/Test/CMakeLists.txt).
list(REMOVE_ITEM SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Resource/Addressables/Test/AddressablesSmokeTest.cpp")
list(REMOVE_ITEM REGULAR_SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Resource/Addressables/Test/AddressablesSmokeTest.cpp")
# NavMeshTest: same rationale -- it owns its own main() and must not be
# wholearchive'd into ZRuntime or every consumer exe gets LNK2005.
list(REMOVE_ITEM SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Function/AI/Test/NavMeshTest.cpp")
list(REMOVE_ITEM REGULAR_SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Function/AI/Test/NavMeshTest.cpp")
# SceneGenTool: same rationale -- it owns main() and must not be
# wholearchive'd into ZRuntime or every consumer exe gets LNK2005.
list(REMOVE_ITEM SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Function/AI/Test/SceneGenTool.cpp")
list(REMOVE_ITEM REGULAR_SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Function/AI/Test/SceneGenTool.cpp")
# AssetMigrator: same rationale -- it owns main() and must not be
# wholearchive'd into ZRuntime or every consumer exe gets LNK2005.
list(REMOVE_ITEM SOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/Function/AI/Test/AssetMigrator.cpp")
  list(REMOVE_ITEM REGULAR_SOURCE_FILES
       "${CMAKE_CURRENT_SOURCE_DIR}/Function/AI/Test/AssetMigrator.cpp")
  list(REMOVE_ITEM SOURCE_FILES
       "${CMAKE_CURRENT_SOURCE_DIR}/Function/AI/Test/ComponentRegistryCheck.cpp")
  list(REMOVE_ITEM SOURCE_FILES
       "${CMAKE_CURRENT_SOURCE_DIR}/Function/AI/Test/ObjectDefinitionMigrator.cpp")
  list(REMOVE_ITEM REGULAR_SOURCE_FILES
       "${CMAKE_CURRENT_SOURCE_DIR}/Function/AI/Test/ObjectDefinitionMigrator.cpp")
  list(REMOVE_ITEM REGULAR_SOURCE_FILES
       "${CMAKE_CURRENT_SOURCE_DIR}/Function/AI/Test/ComponentRegistryCheck.cpp")

# UGUI control layer retired (P4). The shared GPU render layer + Font/UITypes/
# WindowUI moved to Runtime/UI/ and are now picked up by the main GLOB_RECURSE
# above (compiled directly into ZRuntime, whole-archived for reflection).


# ---- Create libraries ----
# Create both static and shared libraries:
# - Static library (ZRuntime): for internal use by Editor, Profiler, Launcher (enables serialization)
# - Shared library (ZRuntimeShared): for external use (solves Singleton cross-DLL issues)

# Static library target (for internal use)
add_library(${TARGET_NAME} STATIC ${HEADER_FILES} ${SOURCE_FILES})

set_target_properties(${TARGET_NAME} PROPERTIES CXX_STANDARD 23)
set_target_properties(${TARGET_NAME} PROPERTIES FOLDER "Engine")
# Disable Unity Builds for ZRuntime (causes class redefinition errors like RuntimeAssetManager)
set_target_properties(${TARGET_NAME} PROPERTIES UNITY_BUILD OFF)
z_set_config_archive_dirs(${TARGET_NAME} "${CMAKE_CURRENT_BINARY_DIR}")

# Shared library target (for external use)
set(SHARED_TARGET_NAME "${TARGET_NAME}Shared")
add_library(${SHARED_TARGET_NAME} SHARED ${HEADER_FILES} ${SOURCE_FILES})

set_target_properties(${SHARED_TARGET_NAME} PROPERTIES CXX_STANDARD 23)
set_target_properties(${SHARED_TARGET_NAME} PROPERTIES FOLDER "Engine")
set_target_properties(${SHARED_TARGET_NAME} PROPERTIES UNITY_BUILD OFF)
# The Metal (macOS) backend excludes the Vulkan render path -- Passes/, the
# Vulkan ShaderCompiler, RenderResource's Vulkan-only out-of-line virtuals and
# RenderSystem::GetFrameDrawList -- so the shared runtime variant has unresolved
# symbols there. Keep the target configured (it is referenced all over this file)
# but leave it out of the default "all" build on that configuration. iOS keeps
# Vulkan via MoltenVK and is unaffected.
if(APPLE AND NOT IOS AND NOT ZENGINE_USE_VULKAN)
    set_target_properties(${SHARED_TARGET_NAME} PROPERTIES EXCLUDE_FROM_ALL TRUE)
    message(STATUS "Runtime: ZRuntimeShared excluded from the default build (Metal backend has no Vulkan render path)")
endif()
# Ensure the shared target has a unique output name so its import library
# doesn't collide with the static library archive (ZRuntime.lib). Using a
# distinct output name avoids the import lib overwriting the static lib when
# both are placed into the same output directory during multi-target builds.
set_target_properties(${SHARED_TARGET_NAME} PROPERTIES OUTPUT_NAME "${SHARED_TARGET_NAME}")

# Output DLL to bin/Debug, bin/Release, etc. (same layout as Editor/Launcher/Profiler)
z_set_config_output_dirs(${SHARED_TARGET_NAME} "${BINARY_ROOT_DIR}" LIBRARY)

# ---- Reflection code generation (ZHeaderTool, UHT-style) ----
# Headers annotated with ZCLASS / ZPROPERTY get <Name>.generated.h and
# <Name>.gen.cpp (type registration + Transfer()). See cmake/ZHeaderTool.cmake.
include(${Z_ROOT_DIR}/cmake/ZHeaderTool.cmake)
zengine_header_tool(
    MODULE       ZRuntime
    TARGETS      ${TARGET_NAME} ${SHARED_TARGET_NAME}
    INCLUDE_ROOT "${ENGINE_ROOT_DIR}/Source"
    HEADERS      ${HEADER_FILES}
    # The shared PCH (CommonPCH/pch.h) transitively includes reflected
    # headers such as ProjectInfo.h.
    INCLUDE_CONSUMERS ZEnginePCH)

# Consumers of the shared runtime should get the import macro defined so headers
target_compile_definitions(${SHARED_TARGET_NAME} INTERFACE ZRUNTIME_SHARED_IMPORTS)

# Create an alias so other CMakeLists can link to ZRuntime and receive the shared target
add_library(ZRuntime::Shared ALIAS ${SHARED_TARGET_NAME})

# ---- Bundle compression (zlib) ---------------------------------------------
# `.zbundle` v1 wrote STORED blobs. Compression is decided PER ENTRY (a cooked
# BC7/ASTC texture is already block-compressed and deflating it only costs
# time), but a player still needs an INFLATE side, or a compressed bundle is
# unreadable.
#
# zlib is a vendored submodule (engine/3rdparty/zlib, built by
# engine/3rdparty/CMakeLists.txt as `zlibstatic`), so zlib.h is available on
# every host -- no system package, no configure-time git clone. Real
# (non-imported) targets are global, so `zlibstatic` is visible here even
# though it was declared under engine/3rdparty. Linking the same target
# minizip-ng uses guarantees there is only ONE zlib in the graph.
#
# When it is absent (Emscripten, a 3rdparty tree without the submodule) we
# fall back to a system zlib, and if that misses too we build without it: the
# writer then refuses to emit a compressed entry and the reader rejects one
# with a clear message, instead of handing back deflate bytes that get parsed
# as a .zasset.
set(_zengine_zlib_target "")
if(NOT EMSCRIPTEN)
    if(TARGET zlibstatic)
        # Vendored submodule -- the normal path.
        set(_zengine_zlib_target zlibstatic)
    else()
        # Imported targets are directory-scoped: minizip-ng may have found zlib
        # for itself, but ZLIB::ZLIB is not visible from this scope, so resolve
        # it again here.
        find_package(ZLIB QUIET)
        if(TARGET ZLIB::ZLIB)
            set(_zengine_zlib_target ZLIB::ZLIB)
        elseif(TARGET zlib)
            set(_zengine_zlib_target zlib)
        endif()
    endif()
endif()

if(_zengine_zlib_target)
    # PRIVATE is enough for the static lib: consumers still link zlib transitively
    # ($<LINK_ONLY:>), and nothing outside AssetBundleFormat.cpp needs the macro.
    target_link_libraries(${TARGET_NAME} PRIVATE ${_zengine_zlib_target})
    target_link_libraries(${SHARED_TARGET_NAME} PRIVATE ${_zengine_zlib_target})
    target_compile_definitions(${TARGET_NAME} PRIVATE ZENGINE_BUNDLE_HAS_ZLIB=1)
    target_compile_definitions(${SHARED_TARGET_NAME} PRIVATE ZENGINE_BUNDLE_HAS_ZLIB=1)
    message(STATUS "Runtime: bundle compression enabled (zlib via ${_zengine_zlib_target})")
else()
    message(STATUS "Runtime: bundle compression unavailable (no zlib target); bundles stay STORED")
endif()
unset(_zengine_zlib_target)

# Win64 builds may compile both Vulkan and DX12; runtime can still select DX12.
# Keep shader search roots available whenever DX12 sources are in the link set.
if(WIN32)
    string(REPLACE "\\" "/" _zengine_shader_root "${CMAKE_SOURCE_DIR}/engine/shader")
    string(REPLACE "\\" "/" _zengine_dx12_utility_shaders
           "${CMAKE_SOURCE_DIR}/engine/Source/Runtime/Function/Render/Interface/DX12/Utility/Shaders")
    target_compile_definitions(${TARGET_NAME} PRIVATE
        "ZENGINE_SHADER_ROOT=\"${_zengine_shader_root}\""
        "ZENGINE_DX12_UTILITY_SHADER_ROOT=\"${_zengine_dx12_utility_shaders}\"")
    target_compile_definitions(${SHARED_TARGET_NAME} PRIVATE
        "ZENGINE_SHADER_ROOT=\"${_zengine_shader_root}\""
        "ZENGINE_DX12_UTILITY_SHADER_ROOT=\"${_zengine_dx12_utility_shaders}\"")
endif()

# Configure shared library export/import
# On non-Windows, ZRUNTIME_SHARED_EXPORTS marks EXPORT_RUNTIME symbols with default visibility.
target_compile_definitions(${SHARED_TARGET_NAME} PRIVATE ZRUNTIME_SHARED_EXPORTS)
if(WIN32)
    # Ensure shared section for Singleton cross-module sharing (Windows only)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC" OR (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"))
        target_link_options(${SHARED_TARGET_NAME} PRIVATE /SECTION:SingletonShared,RWS)

        # Windows runtime defaults to DX12, but Vulkan support is still compiled into some
        # optional rendering/editor code paths. Delay-load vulkan-1.dll so external hosts
        # such as Unity can load ZRuntimeShared.dll on machines without a Vulkan runtime.
        if(ZENGINE_USE_VULKAN)
            target_link_libraries(${SHARED_TARGET_NAME} PRIVATE delayimp)
            target_link_options(${SHARED_TARGET_NAME} PRIVATE /DELAYLOAD:vulkan-1.dll)
        endif()
    endif()
endif()


if(IOS)
    # --- Compile options ---
    # Disable stack probing: on arm64 iOS, Clang inserts ___chkstk_darwin calls
    # for functions with >4KB stack frames. This symbol lives in macOS libSystem
    # but is NOT exported by the iOS SDK stubs, so dyld rejects the dylib:
    #   Symbol not found: ___chkstk_darwin
    #   Expected in: /usr/lib/libSystem.B.dylib
    # We provide our own no-op implementation in Platform/IOS/ZChkstkDarwin.c
    # (iOS uses kernel guard pages for stack overflow detection, so explicit
    # probing is unnecessary).
    target_compile_options(${SHARED_TARGET_NAME} PRIVATE -fno-stack-check)
    target_compile_options(${TARGET_NAME} PRIVATE -fno-stack-check)

    # --- Link options ---
    target_link_options(${SHARED_TARGET_NAME} PRIVATE
        "LINKER:-dead_strip"
        "LINKER:-dead_strip_dylibs"
        # Force old-style LC_DYLD_INFO load command instead of chained fixups.
        # On iOS 16+, ld64 defaults to chained fixups (LC_DYLD_CHAINED_FIXUPS +
        # LC_DYLD_EXPORTS_TRIE), but UnityFramework's dyld does NOT understand
        # the chained fixups format and will reject the dylib with:
        #   "missing LC_DYLD_INFO load command"
        # -no_fixup_chains tells ld64 to emit the legacy LC_DYLD_INFO instead.
        "LINKER:-no_fixup_chains"
        # Use flat namespace: break dyld's two-level namespace binding so that
        # undefined symbols are searched across ALL loaded dylibs, not just the
        # specific install_name recorded at build time.
        # Combined with -undefined dynamic_lookup, this lets dyld resolve
        # libc++ VTT/vtable symbols from UnityFramework's embedded libc++ at
        # runtime, even though the linker originally expected them from
        # /usr/lib/libc++.1.dylib.
        "LINKER:-flat_namespace"
        "LINKER:-undefined"
        "LINKER:dynamic_lookup"
    )
    if(ZENGINE_IOS_EXPORT_SYMBOLS_FILE)
        target_link_options(${SHARED_TARGET_NAME} PRIVATE "LINKER:-exported_symbols_list,${ZENGINE_IOS_EXPORT_SYMBOLS_FILE}")
    endif()
endif()

if(ZENGINE_MOBILE_HIDE_INTERNAL_SYMBOLS AND (ANDROID OR Z_PLATFORM_OHOS OR OHOS OR CMAKE_SYSTEM_NAME STREQUAL "OHOS"))
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_link_options(${SHARED_TARGET_NAME} PRIVATE -Wl,--exclude-libs,ALL)
    endif()
endif()

# Precompiled headers: reuse shared ZEnginePCH (C++23)
# Link ZEnginePCH so this target inherits its include dirs (EASTL, etc.) for IntelliSense and compile
target_link_libraries(${TARGET_NAME} PRIVATE ZEnginePCH)
target_link_libraries(${SHARED_TARGET_NAME} PRIVATE ZEnginePCH)
target_include_directories(${TARGET_NAME} PRIVATE ${ENGINE_ROOT_DIR}/Source/CommonPCH)
target_include_directories(${SHARED_TARGET_NAME} PRIVATE ${ENGINE_ROOT_DIR}/Source/CommonPCH)
add_dependencies(${TARGET_NAME} ZEnginePCH)
add_dependencies(${SHARED_TARGET_NAME} ZEnginePCH)
target_precompile_headers(${TARGET_NAME} REUSE_FROM ZEnginePCH)
target_precompile_headers(${SHARED_TARGET_NAME} REUSE_FROM ZEnginePCH)
message(STATUS "Precompiled headers enabled for ${TARGET_NAME} and ${SHARED_TARGET_NAME} (REUSE_FROM ZEnginePCH)")

# Helper function to configure library properties
function(configure_runtime_library target)
    # Compile options
    target_compile_options(${target} PUBLIC "$<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/permissive->")
    # Add warnings-as-errors options only when explicitly enabled by the top-level option
    if(ZENGINE_ENABLE_WERROR)
        target_compile_options(${target} PUBLIC "$<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/WX>")
        target_compile_options(${target} PUBLIC "$<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/we4716>")
    endif()
    
    # Link dependencies
    target_link_libraries(${target} PUBLIC BqLog)
    # ZSlate: standalone UI framework (when available)
    if(TARGET ZSlate)
        target_link_libraries(${target} PRIVATE ZSlate)
        message(STATUS "Runtime: Linking with ZSlate library")
        
        # ZSlate ZEngine platform implementation (bridges ZSlate <-> UiGpuResources)
        set(ZSLATE_PLATFORM_DIR "${ENGINE_ROOT_DIR}/Libraries/ZSlate/platform/ZEngine")
        if(EXISTS "${ZSLATE_PLATFORM_DIR}/ZEngineSlateRenderer.cpp")
            target_include_directories(${target} PRIVATE "${ZSLATE_PLATFORM_DIR}")
            target_sources(${target} PRIVATE
                "${ZSLATE_PLATFORM_DIR}/ZEngineSlateRenderer.cpp"
            )
            message(STATUS "Runtime: ZSlate ZEngine platform implementation enabled")
        endif()
    endif()
    target_link_libraries(${target} PRIVATE tinyobjloader stb tweeny)
    # ZRuntime is ImGui-free: the 2D UI renders through native ZSlate (BatchedUIRenderer +
    # ZFontAtlas, which vendors its own stb_truetype). ImGui now links only into the
    # standalone dev tools (ZLauncher, ZProfiler) via their own target_link_libraries.

    # render_doc is a desktop development / auxiliary library and links a host RenderDoc import library.
    # The renderdoc.lib is a Windows DLL import lib (PE/COFF) and is not a valid wasm-ld input,
    # so Web builds skip it as well.
    if(NOT ANDROID AND NOT IOS AND NOT APPLE AND NOT Z_PLATFORM_OHOS AND NOT OHOS AND NOT CMAKE_SYSTEM_NAME STREQUAL "OHOS"
       AND NOT EMSCRIPTEN AND NOT CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
        target_link_libraries(${target} PRIVATE render_doc)
    endif()
    target_link_libraries(${target} PUBLIC Jolt)
    if(APPLE)
        target_link_libraries(${target} PUBLIC "-framework Metal" "-framework MetalKit" "-framework QuartzCore" "-framework Foundation")
        if(IOS)
            target_link_libraries(${target} PUBLIC "-framework UIKit")
        else()
            target_link_libraries(${target} PUBLIC "-framework AppKit")
            # SPIR-V -> MSL for the Metal backend (shader modules are SPIR-V).
            if(TARGET spirv-cross-msl)
                target_link_libraries(${target} PRIVATE spirv-cross-msl)
            endif()
            endif()
            elseif(ZENGINE_USE_VULKAN)
        target_link_libraries(${target} PUBLIC ${vulkan_lib})
    endif()
    if(WIN32)
        # DirectX 12 runtime libraries (always needed on Windows, regardless of Vulkan toggle)
        target_link_libraries(${target} PUBLIC d3d12 dxgi d3dcompiler)
        # WASAPI audio output (PR-A1). mmdevapi provides IMMDeviceEnumerator /
        # IAudioClient; ole32 (CoCreateInstance) is already in CMake's default
        # MSVC library set.
        target_link_libraries(${target} PUBLIC Mmdevapi)
    endif()

    # ---- Network (P0-6): HTTP / WebSocket / UDP ----------------------------
    #
    # libcurl is already vendored at 3rdparty/curl and built as libcurl_static,
    # but nothing in Runtime linked it, so the network layer had no backend to
    # talk to. Wiring it here is what turns NetworkSystem from "everything
    # returns Unsupported" into a real transport.
    #
    # Emscripten has no curl target at all (3rdparty skips it), and a curl-less
    # build is legal: HttpRequest/WebSocket then compile down to
    # NetworkStatus::Unsupported instead of failing. That is exactly what
    # ZENGINE_NETWORK_HAS_CURL gates, so it is only defined when the library is
    # really there.
    if(TARGET libcurl_static)
        target_link_libraries(${target} PRIVATE libcurl_static)
        target_compile_definitions(${target} PRIVATE ZENGINE_NETWORK_HAS_CURL=1)
        if(WIN32)
            # curl's own build puts CURL_STATICLIB on libcurl_static but not on
            # its INTERFACE, so consumers still see __declspec(dllimport) in
            # curl.h unless they define it themselves.
            target_compile_definitions(${target} PRIVATE CURL_STATICLIB=1)
            target_link_libraries(${target} PRIVATE crypt32 ws2_32 wldap32 normaliz)
        elseif(APPLE)
            # SystemConfiguration (proxy settings) and CoreFoundation (DNS
            # configuration) are pulled in by curl's own build for its own
            # objects; a static archive does not carry them along, so repeat
            # them for the final link.
            target_link_libraries(${target} PRIVATE
                "-framework CoreFoundation" "-framework SystemConfiguration")
        endif()
        message(STATUS "Runtime: network layer linked against libcurl_static")
    else()
        message(STATUS "Runtime: libcurl unavailable; network layer degrades to NetworkStatus::Unsupported")
    endif()

    target_link_libraries(${target} PUBLIC mimalloc-static)
    target_link_libraries(${target} PUBLIC lmdb)
endfunction()

# Configure both libraries
configure_runtime_library(${TARGET_NAME})
configure_runtime_library(${SHARED_TARGET_NAME})

# ---------------------------------------------------------------------------
# Reflection auto-registration: prevent linker dead-stripping of TUs whose
# only "use" is a static-init registrar.
#
# ZEngine's reflection / serialization registers types via
# IMPLEMENT_REGISTER_CLASS(...) -> an anonymous-namespace static object whose
# ctor runs before main() and links the type into the global type table that
# RegisterCore() later walks (AutoTypeRegistration::ExecuteAllRegistrations).
#
# Because ZRuntime is built as a STATIC library, any TU in ZRuntime.lib that
# is not strongly referenced by the consumer (Editor/Profiler/Launcher/etc.)
# is dropped by the linker:
#   - MSVC link.exe drops the whole .obj;
#   - GNU/Clang ld drops it from .a unless --whole-archive is in effect;
#   - Apple ld64 drops it unless -force_load is in effect.
# When that happens, the static registrar never constructs, TypeOf<T>() ends
# up referring to an "[UNREGISTERED]" placeholder Type with factory==nullptr,
# and AssetManager::ReadObject<T>() returns nullptr at runtime.
#
# Mirrors UE's IMPLEMENT_MODULE / IMPLEMENT_PRIMARY_GAME_MODULE policy of
# forcing whole-archive linkage for static modules that publish reflection
# data, and Unity's USE_WHOLE_ARCHIVE on its native plugins.
#
# Applied as INTERFACE on the static library: every consumer (exe / dll) that
# links ZRuntime inherits the option automatically, so the same .obj that
# carries the static registrar is forced into the final image.
#
# ZRuntimeShared (SHARED) does NOT need the option for its own link step:
# the SHARED target compiles the same source list directly into its own
# objects, and a SHARED/EXE link uses every input .obj unconditionally
# (dead-strip only applies to objects pulled from .lib archives).
# ---------------------------------------------------------------------------
if(MSVC OR (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"))
    target_link_options(${TARGET_NAME} INTERFACE "/WHOLEARCHIVE:$<TARGET_FILE_NAME:${TARGET_NAME}>")
    message(STATUS "Runtime: enabled /WHOLEARCHIVE for ${TARGET_NAME} consumers (reflection auto-registration)")
elseif(APPLE)
    # ld64 uses -force_load <path>; absolute path required.
    target_link_options(${TARGET_NAME} INTERFACE "LINKER:-force_load,$<TARGET_FILE:${TARGET_NAME}>")
    message(STATUS "Runtime: enabled -force_load for ${TARGET_NAME} consumers (reflection auto-registration)")
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    # GNU ld / lld / gold: bracket the archive with --whole-archive markers.
    target_link_options(${TARGET_NAME} INTERFACE
        "LINKER:--whole-archive" "$<TARGET_FILE:${TARGET_NAME}>" "LINKER:--no-whole-archive")
    message(STATUS "Runtime: enabled --whole-archive for ${TARGET_NAME} consumers (reflection auto-registration)")
else()
    message(WARNING "Runtime: unknown linker; reflection auto-registration TUs may be stripped from ${TARGET_NAME}.lib")
endif()

# Link PuertsCore for core pesapi functions (SetLogCallback, GetRegisterApi, GetPapiVersion, pesapi_* wrappers)
# PuertsCore is required regardless of PAPI_TYPE as it provides the core scripting infrastructure
if(TARGET PuertsCore)
    target_link_libraries(${TARGET_NAME} PUBLIC PuertsCore)
    target_link_libraries(${SHARED_TARGET_NAME} PUBLIC PuertsCore)
    message(STATUS "Runtime: Linking PuertsCore for scripting core support")
endif()

# Link the appropriate PAPI library based on PAPI_TYPE (provides backend-specific pesapi implementations).
# ZEngine is JS-only -- supported VMs: quickjs (default, mobile/web) and v8 (desktop/console).
if(PAPI_TYPE STREQUAL "quickjs")
    if(TARGET PapiQuickjs)
        target_link_libraries(${TARGET_NAME} PUBLIC PapiQuickjs)
        target_link_libraries(${SHARED_TARGET_NAME} PUBLIC PapiQuickjs)
        message(STATUS "Runtime: Linking PapiQuickjs for QuickJS scripting support")
    endif()
elseif(PAPI_TYPE STREQUAL "v8")
    # puerts' papi-v8 declares the target as PapiV8 (capital V); see
    # engine/3rdparty/puerts/unity/native/papi-v8/CMakeLists.txt. CMake
    # target names are case-sensitive.
    if(TARGET PapiV8)
        target_link_libraries(${TARGET_NAME} PUBLIC PapiV8)
        target_link_libraries(${SHARED_TARGET_NAME} PUBLIC PapiV8)
        message(STATUS "Runtime: Linking PapiV8 for V8 scripting support")
    endif()
endif()

# Force static PuertsCore + PAPI symbols into ZRuntimeShared.dll (Windows).
if(WIN32 AND TARGET PuertsCore)
    target_link_options(${SHARED_TARGET_NAME} PRIVATE "/WHOLEARCHIVE:$<TARGET_FILE:PuertsCore>")
endif()
if(WIN32 AND TARGET PapiQuickjs)
    target_link_options(${SHARED_TARGET_NAME} PRIVATE "/WHOLEARCHIVE:$<TARGET_FILE:PapiQuickjs>")
elseif(WIN32 AND TARGET PapiV8)
    target_link_options(${SHARED_TARGET_NAME} PRIVATE "/WHOLEARCHIVE:$<TARGET_FILE:PapiV8>")
endif()



# Add PAPI type compile definitions based on PAPI_TYPE (for both libraries).
# JS-only -- no PAPI_LUA / PAPI_PYTHON / PAPI_NODEJS.
if(PAPI_TYPE STREQUAL "quickjs")
    target_compile_definitions(${TARGET_NAME} PRIVATE PAPI_QUICKJS=1)
    target_compile_definitions(${SHARED_TARGET_NAME} PRIVATE PAPI_QUICKJS=1)
elseif(PAPI_TYPE STREQUAL "v8")
    target_compile_definitions(${TARGET_NAME} PRIVATE PAPI_V8=1)
    target_compile_definitions(${SHARED_TARGET_NAME} PRIVATE PAPI_V8=1)
endif()

if(ENABLE_PHYSICS_DEBUG_RENDERER)
    add_compile_definitions(ENABLE_PHYSICS_DEBUG_RENDERER)
    target_link_libraries(${TARGET_NAME} PUBLIC TestFramework d3d12.lib shcore.lib)
    target_link_libraries(${SHARED_TARGET_NAME} PUBLIC TestFramework d3d12.lib shcore.lib)
endif()

# Configure Vulkan include directories only for Vulkan-enabled builds.
if(ZENGINE_USE_VULKAN AND vulkan_include)
    target_include_directories(
        ${TARGET_NAME}
        PUBLIC $<BUILD_INTERFACE:${vulkan_include}>)
    target_include_directories(
        ${SHARED_TARGET_NAME}
        PUBLIC $<BUILD_INTERFACE:${vulkan_include}>)
endif()

# Configure runtime shader compilation.
# On Windows/MSVC, Vulkan SDK's prebuilt glslang/SPIRV static libraries may be built
# against a newer STL than the local toolset, which leads to unresolved __std_* symbols
# at link time. Prefer the external glslangValidator executable there unless explicitly
# opted back into direct static linking.
foreach(_runtime_shader_target IN ITEMS ${TARGET_NAME} ${SHARED_TARGET_NAME})
    target_compile_definitions(${_runtime_shader_target} PRIVATE ZENGINE_RUNTIME_GLSLANG_LINKED=0)
endforeach()

if(glslangValidator_executable_normalized AND EXISTS "${glslangValidator_executable_normalized}")
    foreach(_runtime_shader_target IN ITEMS ${TARGET_NAME} ${SHARED_TARGET_NAME})
        target_compile_definitions(
            ${_runtime_shader_target}
            PRIVATE Z_GLSLANG_VALIDATOR_EXECUTABLE="${glslangValidator_executable_normalized}")
    endforeach()
    message(STATUS "Runtime: Found glslangValidator executable: ${glslangValidator_executable_normalized}")
else()
    message(WARNING "Runtime: glslangValidator executable not found. Runtime shader compilation fallback will be unavailable.")
endif()

if(ZENGINE_USE_VULKAN AND ZENGINE_LINK_VULKAN_SDK_GLSLANG_LIBS AND NOT ANDROID AND NOT Z_PLATFORM_OHOS)
    foreach(_runtime_shader_target IN ITEMS ${TARGET_NAME} ${SHARED_TARGET_NAME})
        target_compile_definitions(${_runtime_shader_target} PRIVATE ZENGINE_RUNTIME_GLSLANG_LINKED=1)
    endforeach()

    if(WIN32)

        # Windows: glslang libraries are typically in Lib directory
        # Find debug version (glslangd.lib) and release version (glslang.lib)
        find_library(GLSLANG_LIBRARY_DEBUG
            NAMES glslangd glslangd.lib
            PATHS ${VULKAN_SDK_ROOT}/Lib
            NO_DEFAULT_PATH
        )
        find_library(GLSLANG_LIBRARY_RELEASE
            NAMES glslang glslang.lib
            PATHS ${VULKAN_SDK_ROOT}/Lib
            NO_DEFAULT_PATH
        )
        # Select the appropriate library based on build configuration
        if(GLSLANG_LIBRARY_DEBUG OR GLSLANG_LIBRARY_RELEASE)
            # Link glslang into both static and shared runtime. For the shared runtime we
            # expose the dependency publicly so consumers linking to ZRuntime::Shared also
            # get the necessary link libraries pulled in automatically.
            target_link_libraries(${TARGET_NAME} PRIVATE
                $<$<CONFIG:Debug>:${GLSLANG_LIBRARY_DEBUG}>
                $<$<NOT:$<CONFIG:Debug>>:${GLSLANG_LIBRARY_RELEASE}>
            )
            target_link_libraries(${SHARED_TARGET_NAME} PUBLIC
                $<$<CONFIG:Debug>:${GLSLANG_LIBRARY_DEBUG}>
                $<$<NOT:$<CONFIG:Debug>>:${GLSLANG_LIBRARY_RELEASE}>
            )
            if(GLSLANG_LIBRARY_DEBUG)
                message(STATUS "Found glslang debug library: ${GLSLANG_LIBRARY_DEBUG}")
            endif()
            if(GLSLANG_LIBRARY_RELEASE)
                message(STATUS "Found glslang release library: ${GLSLANG_LIBRARY_RELEASE}")
            endif()
        else()
            message(WARNING "glslang library not found. Runtime shader compilation may not work.")
        endif()
        # Find glslang-default-resource-limits library (contains GetDefaultResources())
        # Try multiple possible names as different Vulkan SDK versions may use different names
        find_library(GLSLANG_DEFAULT_RESOURCE_LIMITS_DEBUG
            NAMES glslang-default-resource-limitsd
            glslang-default-resource-limitsd.lib
            glslang-default-resource-limits-staticd
            glslang-default-resource-limits-staticd.lib
            PATHS ${VULKAN_SDK_ROOT}/Lib
            NO_DEFAULT_PATH
        )
        find_library(GLSLANG_DEFAULT_RESOURCE_LIMITS_RELEASE
            NAMES glslang-default-resource-limits
            glslang-default-resource-limits.lib
            glslang-default-resource-limits-static
            glslang-default-resource-limits-static.lib
            PATHS ${VULKAN_SDK_ROOT}/Lib
            NO_DEFAULT_PATH
        )
        # Select the appropriate library based on build configuration
        if(GLSLANG_DEFAULT_RESOURCE_LIMITS_DEBUG OR GLSLANG_DEFAULT_RESOURCE_LIMITS_RELEASE)
            target_link_libraries(${TARGET_NAME} PRIVATE
                $<$<CONFIG:Debug>:${GLSLANG_DEFAULT_RESOURCE_LIMITS_DEBUG}>
                $<$<NOT:$<CONFIG:Debug>>:${GLSLANG_DEFAULT_RESOURCE_LIMITS_RELEASE}>
            )
            target_link_libraries(${SHARED_TARGET_NAME} PUBLIC
                $<$<CONFIG:Debug>:${GLSLANG_DEFAULT_RESOURCE_LIMITS_DEBUG}>
                $<$<NOT:$<CONFIG:Debug>>:${GLSLANG_DEFAULT_RESOURCE_LIMITS_RELEASE}>
            )
            if(GLSLANG_DEFAULT_RESOURCE_LIMITS_DEBUG)
                message(STATUS "Found glslang-default-resource-limits debug library: ${GLSLANG_DEFAULT_RESOURCE_LIMITS_DEBUG}")
            endif()
            if(GLSLANG_DEFAULT_RESOURCE_LIMITS_RELEASE)
                message(STATUS "Found glslang-default-resource-limits release library: ${GLSLANG_DEFAULT_RESOURCE_LIMITS_RELEASE}")
            endif()
        else()
            message(WARNING "glslang-default-resource-limits library not found. GetDefaultResources() may not be available.")
        endif()
        # Find debug version (SPIRV-Toolsd.lib) and release version (SPIRV-Tools.lib)
        find_library(SPIRV_TOOLS_LIBRARY_DEBUG
            NAMES SPIRV-Toolsd SPIRV-Toolsd.lib
            PATHS ${VULKAN_SDK_ROOT}/Lib
            NO_DEFAULT_PATH
        )
        find_library(SPIRV_TOOLS_LIBRARY_RELEASE
            NAMES SPIRV-Tools SPIRV-Tools.lib
            PATHS ${VULKAN_SDK_ROOT}/Lib
            NO_DEFAULT_PATH
        )
        # Select the appropriate library based on build configuration
        if(SPIRV_TOOLS_LIBRARY_DEBUG OR SPIRV_TOOLS_LIBRARY_RELEASE)
            target_link_libraries(${TARGET_NAME} PRIVATE
                $<$<CONFIG:Debug>:${SPIRV_TOOLS_LIBRARY_DEBUG}>
                $<$<NOT:$<CONFIG:Debug>>:${SPIRV_TOOLS_LIBRARY_RELEASE}>
            )
            target_link_libraries(${SHARED_TARGET_NAME} PUBLIC
                $<$<CONFIG:Debug>:${SPIRV_TOOLS_LIBRARY_DEBUG}>
                $<$<NOT:$<CONFIG:Debug>>:${SPIRV_TOOLS_LIBRARY_RELEASE}>
            )
            if(SPIRV_TOOLS_LIBRARY_DEBUG)
                message(STATUS "Found SPIRV-Tools debug library: ${SPIRV_TOOLS_LIBRARY_DEBUG}")
            endif()
            if(SPIRV_TOOLS_LIBRARY_RELEASE)
                message(STATUS "Found SPIRV-Tools release library: ${SPIRV_TOOLS_LIBRARY_RELEASE}")
            endif()
        endif()
        # Find SPIRV-Tools Optimizer library (required by glslang)
        # Find debug version (SPIRV-Tools-optd.lib) and release version (SPIRV-Tools-opt.lib)
        find_library(SPIRV_TOOLS_OPT_LIBRARY_DEBUG
            NAMES SPIRV-Tools-optd SPIRV-Tools-optd.lib
            PATHS ${VULKAN_SDK_ROOT}/Lib
            NO_DEFAULT_PATH
        )
        find_library(SPIRV_TOOLS_OPT_LIBRARY_RELEASE
            NAMES SPIRV-Tools-opt SPIRV-Tools-opt.lib
            PATHS ${VULKAN_SDK_ROOT}/Lib
            NO_DEFAULT_PATH
        )
        # Select the appropriate library based on build configuration
        if(SPIRV_TOOLS_OPT_LIBRARY_DEBUG OR SPIRV_TOOLS_OPT_LIBRARY_RELEASE)
            target_link_libraries(${TARGET_NAME} PRIVATE
                $<$<CONFIG:Debug>:${SPIRV_TOOLS_OPT_LIBRARY_DEBUG}>
                $<$<NOT:$<CONFIG:Debug>>:${SPIRV_TOOLS_OPT_LIBRARY_RELEASE}>
            )
            target_link_libraries(${SHARED_TARGET_NAME} PUBLIC
                $<$<CONFIG:Debug>:${SPIRV_TOOLS_OPT_LIBRARY_DEBUG}>
                $<$<NOT:$<CONFIG:Debug>>:${SPIRV_TOOLS_OPT_LIBRARY_RELEASE}>
            )
            if(SPIRV_TOOLS_OPT_LIBRARY_DEBUG)
                message(STATUS "Found SPIRV-Tools-Opt debug library: ${SPIRV_TOOLS_OPT_LIBRARY_DEBUG}")
            endif()
            if(SPIRV_TOOLS_OPT_LIBRARY_RELEASE)
                message(STATUS "Found SPIRV-Tools-Opt release library: ${SPIRV_TOOLS_OPT_LIBRARY_RELEASE}")
            endif()
        else()
            message(WARNING "SPIRV-Tools-Opt library not found. glslang optimizer functionality may not work.")
        endif()
    elseif(UNIX)
        if(APPLE)
            find_library(GLSLANG_LIBRARY
                NAMES glslang
                PATHS ${VULKAN_SDK_ROOT}/lib
                NO_DEFAULT_PATH
            )
            find_library(GLSLANG_DEFAULT_RESOURCE_LIMITS_LIBRARY
                NAMES glslang-default-resource-limits
                PATHS ${VULKAN_SDK_ROOT}/lib
                NO_DEFAULT_PATH
            )
        else()
            find_library(GLSLANG_LIBRARY
                NAMES glslang
                PATHS ${VULKAN_SDK_ROOT}/lib
                NO_DEFAULT_PATH
            )
            find_library(GLSLANG_DEFAULT_RESOURCE_LIMITS_LIBRARY
                NAMES glslang-default-resource-limits
                PATHS ${VULKAN_SDK_ROOT}/lib
                NO_DEFAULT_PATH
            )
        endif()
        if(GLSLANG_LIBRARY)
            target_link_libraries(${TARGET_NAME} PRIVATE ${GLSLANG_LIBRARY})
            target_link_libraries(${SHARED_TARGET_NAME} PRIVATE ${GLSLANG_LIBRARY})
            message(STATUS "Found glslang library: ${GLSLANG_LIBRARY}")
        else()
            message(WARNING "glslang library not found. Runtime shader compilation may not work.")
        endif()
        if(GLSLANG_DEFAULT_RESOURCE_LIMITS_LIBRARY)
            target_link_libraries(${TARGET_NAME} PRIVATE ${GLSLANG_DEFAULT_RESOURCE_LIMITS_LIBRARY})
            target_link_libraries(${SHARED_TARGET_NAME} PRIVATE ${GLSLANG_DEFAULT_RESOURCE_LIMITS_LIBRARY})
            message(STATUS "Found glslang-default-resource-limits library: ${GLSLANG_DEFAULT_RESOURCE_LIMITS_LIBRARY}")
        else()
            message(WARNING "glslang-default-resource-limits library not found. GetDefaultResources() may not be available.")
        endif()
    endif()
else()
    message(STATUS "Runtime: skipping Vulkan SDK glslang/SPIRV static libraries; shader compilation will fall back to glslangValidator when available.")
endif()


# Configure dependencies and include directories for both libraries
add_dependencies(${TARGET_NAME} ${SHADER_COMPILE_TARGET})
add_dependencies(${SHARED_TARGET_NAME} ${SHADER_COMPILE_TARGET})

# Helper function to configure include directories
function(configure_runtime_includes target)
    target_include_directories(
        ${target}
        PUBLIC $<BUILD_INTERFACE:${ENGINE_ROOT_DIR}/shader/generated/cpp>)
    
    target_include_directories(
        ${target}
        PUBLIC $<BUILD_INTERFACE:${ENGINE_ROOT_DIR}/Source>
        $<INSTALL_INTERFACE:include/${TARGET_NAME}-${PROJECT_VERSION}>
    )
    
    target_include_directories(
        ${target}
        PUBLIC $<BUILD_INTERFACE:${ENGINE_ROOT_DIR}/Source>
        $<BUILD_INTERFACE:${ENGINE_ROOT_DIR}/Source/Runtime>
        $<INSTALL_INTERFACE:include/${TARGET_NAME}-${PROJECT_VERSION}>
    )
    
    target_include_directories(
        ${target}
        PUBLIC $<BUILD_INTERFACE:${ENGINE_ROOT_DIR}/Source/Runtime/Function/Render/Interface>
        $<INSTALL_INTERFACE:include/${TARGET_NAME}-${PROJECT_VERSION}>
    )
    
    target_include_directories(
        ${target}
        PUBLIC $<BUILD_INTERFACE:${THIRD_PARTY_DIR}/mimalloc/include>
    )
    
    target_include_directories(
        ${target}
        PUBLIC $<BUILD_INTERFACE:${THIRD_PARTY_DIR}/bqlog/include>
    )
    
    target_include_directories(
        ${target}
        PUBLIC $<BUILD_INTERFACE:${THIRD_PARTY_DIR}/rapidjson/include>
    )
    
    target_include_directories(
        ${target}
        PUBLIC $<BUILD_INTERFACE:${THIRD_PARTY_DIR}/xxHash>
    )

    target_include_directories(
        ${target}
        PUBLIC $<BUILD_INTERFACE:${THIRD_PARTY_DIR}/puerts/unity/native/puerts/include>
    )
    
    # Add platform-specific include directory
    if(PLATFORM_SUBDIR AND EXISTS "${PLATFORM_SUBDIR}")
        target_include_directories(
            ${target}
            PUBLIC $<BUILD_INTERFACE:${PLATFORM_SUBDIR}>
        )
    endif()
endfunction()

configure_runtime_includes(${TARGET_NAME})
configure_runtime_includes(${SHARED_TARGET_NAME})

if(PLATFORM_SUBDIR AND EXISTS "${PLATFORM_SUBDIR}")
    message(STATUS "Runtime: Added platform include directory: ${PLATFORM_SUBDIR}")
endif()
# ---- Create an installable target ----
# this allows users to install and find the library via `find_package()`.

# the location where the project's version header will be placed should match the project's regular
# header paths
string(TOLOWER ${TARGET_NAME}/version.h VERSION_HEADER_LOCATION)

### precompile
# set global vari used by precompile
set(Z_RUNTIME_HEADS "${HEADER_FILES}" PARENT_SCOPE)

# Install rules: install the shared runtime (DLL + import lib) and headers so external
# projects can consume `ZRuntime` via the installed files. Running `cmake --install` will
# place the runtime into the configured destinations.
if(NOT DEFINED CMAKE_INSTALL_BINDIR)
    set(CMAKE_INSTALL_BINDIR "bin")
endif()
if(NOT DEFINED CMAKE_INSTALL_LIBDIR)
    set(CMAKE_INSTALL_LIBDIR "lib")
endif()
if(NOT DEFINED CMAKE_INSTALL_INCLUDEDIR)
    set(CMAKE_INSTALL_INCLUDEDIR "include/${TARGET_NAME}")
endif()

# Install the shared target (DLL on Windows). Also install the static archive for completeness.
set(_${TARGET_NAME}_INSTALL_TARGETS)

# Always include the runtime static and shared targets
list(APPEND _${TARGET_NAME}_INSTALL_TARGETS ${SHARED_TARGET_NAME} ${TARGET_NAME})

# Conditionally include auxiliary/runtime dependency targets only if they exist
foreach(_t IN ITEMS BqLog tinyobjloader stb tweeny render_doc Jolt mimalloc-static EASTL lmdb)
    if(TARGET ${_t})
        list(APPEND _${TARGET_NAME}_INSTALL_TARGETS ${_t})
    endif()
endforeach()

# Option to enable CMake export targets (requires all PUBLIC dependencies to also be exported)
option(ZRUNTIME_EXPORT_TARGETS "Enable CMake export targets for find_package support" OFF)

if(NOT _${TARGET_NAME}_INSTALL_TARGETS)
    message(WARNING "No installable targets found for ${TARGET_NAME} - skipping install(TARGETS ...) call")
else()
    if(ZRUNTIME_EXPORT_TARGETS)
        install(TARGETS ${_${TARGET_NAME}_INSTALL_TARGETS}
            EXPORT ${TARGET_NAME}Targets
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}            # DLL (Windows)
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}            # .so / .dylib
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}            # .lib / .a
            FRAMEWORK DESTINATION ${CMAKE_INSTALL_LIBDIR}          # .framework
        )
    else()
        install(TARGETS ${_${TARGET_NAME}_INSTALL_TARGETS}
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}            # DLL (Windows)
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}            # .so / .dylib
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}            # .lib / .a
            FRAMEWORK DESTINATION ${CMAKE_INSTALL_LIBDIR}          # .framework
        )
    endif()
endif()

# Install public headers (copy all headers from the runtime source tree). This is a simple
# approach; projects may refine which headers to install explicitly.
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.h" PATTERN "*.inl"
)

# Export the targets for find_package support when used with CMake's package registry
if(ZRUNTIME_EXPORT_TARGETS)
    install(EXPORT ${TARGET_NAME}Targets
        FILE ${TARGET_NAME}Targets.cmake
        NAMESPACE ${TARGET_NAME}::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${TARGET_NAME}
    )
endif()

# ---------------------------------------------------------------------------
# ShaderLab side-target.
#
# On Vulkan-capable hosts the ShaderLab cpp files (lexer / parser / compiler)
# are picked up by the GLOB_RECURSE above and end up inside ZRuntime, so
# nothing extra is needed here.
#
# On the three configurations where we EXCLUDE'd ShaderLab/*.cpp from
# ZRuntime (Web / Apple / Windows-without-Vulkan -- see the regex filters
# earlier in this file), the Editor still references ShaderLabParser
# (inspector_window, project_window, shader_preview_renderer). To satisfy
# those symbols without dragging the Vulkan-dependent shader_lab_compiler
# into a non-Vulkan build, we add the Function/ShaderLab subdirectory as
# an independent STATIC target that contains only the Vulkan-free
# translation units (lexer + parser). engine/Source/Editor/CMakeLists.txt
# then links it under the same condition.
# ---------------------------------------------------------------------------
set(_ZENGINE_NEEDS_SHADERLAB_SIDETARGET FALSE)
if(EMSCRIPTEN OR CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
    set(_ZENGINE_NEEDS_SHADERLAB_SIDETARGET TRUE)
elseif(APPLE)
    set(_ZENGINE_NEEDS_SHADERLAB_SIDETARGET TRUE)
endif()

if(_ZENGINE_NEEDS_SHADERLAB_SIDETARGET)
    if(NOT TARGET ShaderLab)
        add_subdirectory(Function/ShaderLab)
        message(STATUS "Runtime: ShaderLab side-target enabled (parser-only build) for Editor link")
    endif()
endif()
unset(_ZENGINE_NEEDS_SHADERLAB_SIDETARGET)

# ---------------------------------------------------------------------------
# PR5c: DX12 bindless smoke-test side-target.
#
# Always add the subdirectory so the option ZENGINE_BUILD_BINDLESS_SMOKE_TEST
# shows up in cmake-gui. The subdirectory's CMakeLists.txt early-returns
# unless the option is ON AND the host is Windows, so non-Windows / opt-out
# builds pay zero configuration cost beyond a single early `return()`.
#
# Sanity: only attempt this on platforms that compile dx12/*.cpp into
# ZRuntime (i.e. Windows). On Apple / Mobile / Web the whole dx12/ tree
# is excluded, so DX12ShaderCompiler isn't available and the smoke-test
# can't link anyway.
# ---------------------------------------------------------------------------
if(WIN32)
    add_subdirectory(Function/Render/Interface/DX12/Test)
    # ShaderLab -> DX12 Unity-compatibility smoke-test. Same opt-in shape as
    # the bindless test above (see Function/ShaderLab/Test/CMakeLists.txt).
    add_subdirectory(Function/ShaderLab/Test)
endif()

# ---------------------------------------------------------------------------
# PR-V2: Vulkan bindless smoke-test side-target.
#
# Always added so the option ZENGINE_BUILD_VK_BINDLESS_SMOKE_TEST shows up
# in cmake-gui. The subdirectory's CMakeLists.txt early-returns unless the
# option is ON, so opt-out builds pay zero configuration cost beyond a
# single early `return()`.
#
# Unlike the DX12 entry above there is no host-OS gate -- ZEngine's Vulkan
# backend compiles into ZRuntime on every desktop and mobile platform that
# has a Vulkan SDK, so the smoke-test can link anywhere ZRuntime can.
# ---------------------------------------------------------------------------
add_subdirectory(Function/Render/Interface/Vulkan/Test)

# ---------------------------------------------------------------------------
# Indirect-draw contract smoke-test.
#
# Deliberately NOT gated on a backend or an OS: the point of the test is the
# data contract that all four backends share, and Apple builds none of the
# Vulkan/DX12 backends -- this is the only thing that checks them there.
# ---------------------------------------------------------------------------
add_subdirectory(Function/Render/Interface/Test)

# ---------------------------------------------------------------------------
# PR-SE2: SerializedFile schema-evolution smoke-test side-target.
#
# Always added so the option ZENGINE_BUILD_SCHEMA_EVOLUTION_SMOKE_TEST shows
# up in cmake-gui. The subdirectory's CMakeLists.txt early-returns unless the
# option is ON, so opt-out builds pay zero configuration cost beyond a single
# early `return()`.
#
# No host-OS gate: SerializedFile is platform-agnostic Core code that compiles
# everywhere ZRuntime does, so the smoke-test can link on every supported host.
# ---------------------------------------------------------------------------
add_subdirectory(Core/Serialize/Test)

# 分配器配对冒烟测试（§3.26）。Own main()；只在
# ZENGINE_BUILD_MEMORY_PAIRING_SMOKE_TEST=ON 时构建。盯的是"Produce（new）出来的
# 对象被 mimalloc 释放"这类只在 Apple 上致命、其余平台全程隐形的错配。
add_subdirectory(Core/Memory/Test)

# Derived Data Cache smoke test (texture cook P3). Own main(); carved out of
# the ZRuntime glob above and built only when ZENGINE_BUILD_DDC_SMOKE_TEST=ON.
add_subdirectory(Resource/Cache/Test)

# AI layer smoke test. Own main(); carved out of the ZRuntime glob above and
# built only when ZENGINE_BUILD_AI_SMOKE_TEST=ON. Runs headless -- no world,
# no physics, no RHI -- so it links on every host.
add_subdirectory(Function/AI/Test)

# Physics P0-2 smoke test (character controller, triggers, joints). Opt in with
# -DZENGINE_BUILD_PHYSICS_CHARACTER_SMOKE_TEST=ON; headless, no RHI.
add_subdirectory(Function/Physics/Test)

# Audio DSP smoke test (PR-A1). Own main(); carved out of the ZRuntime glob
# above and built only when ZENGINE_BUILD_AUDIO_SMOKE_TEST=ON. Links ZRuntime
# for the target helper but exercises only the dependency-free DSP layer, so it
# runs headless on every host -- including CI machines with no audio device.
add_subdirectory(Function/Audio/Test)

# Network layer smoke test (P0-6). Own main(); carved out of the ZRuntime glob
# above and built only when ZENGINE_BUILD_NETWORK_SMOKE_TEST=ON. Offline by
# construction -- every check stays in-process or on loopback, so it runs
# headless on a CI machine with no internet and no DNS.
add_subdirectory(Function/Network/Test)

# AssetBundle smoke test (PR-PAK1). Own main(); carved out of the ZRuntime glob
# above and built only when ZENGINE_BUILD_ASSET_BUNDLE_SMOKE_TEST=ON. Pure file
# + memory work, so it runs headless on every host.
add_subdirectory(AssetBundle/Test)

# PlayerSettings smoke test. Own main(); carved out of the ZRuntime glob above
# and built only when ZENGINE_BUILD_PLAYER_SETTINGS_SMOKE_TEST=ON. Pure file
# work, so it runs headless on every host.
add_subdirectory(Function/PlayerSettings/Test)

# Skeletal skinning smoke test (PR-SK1). Own main(); carved out of the ZRuntime
# glob above and built only when ZENGINE_BUILD_ANIMATION_SMOKE_TEST=ON. Pure
# CPU math, so it runs headless on every host -- no GPU, no window.
add_subdirectory(Function/Animation/Test)

# Addressables async-loading smoke test. Own main(); carved out of the ZRuntime
# glob above and built only when ZENGINE_BUILD_ADDRESSABLES_SMOKE_TEST=ON. Pure
# file + scheduling work, so it runs headless on every host -- no GPU, no window.
add_subdirectory(Resource/Addressables/Test)

# RenderGraph compiler smoke test. Own main(); carved out of the ZRuntime glob
# above and built only when ZENGINE_BUILD_RENDERGRAPH_SMOKE_TEST=ON. The
# compiler is RHI-free by design, so this one needs no GPU and no window.
add_subdirectory(Function/Render/Graph/Test)

# Lumen scene-geometry smoke test. Own main(); carved out of the ZRuntime glob
# above and built only when ZENGINE_BUILD_LUMEN_SCENE_GEOMETRY_SMOKE_TEST=ON.
# Pure CPU work (mesh cache -> distance field voxelizer), so no GPU/window.
#
# Gated on the same condition the Lumen sources are gated on: the
# "Windows without Vulkan" and Emscripten branches strip
# Function/Render/Lumen/*.cpp out of ZRuntime, and the test links ZRuntime, so
# it would fail to link there.
if(NOT (WIN32 AND NOT ZENGINE_USE_VULKAN) AND NOT (EMSCRIPTEN OR CMAKE_SYSTEM_NAME STREQUAL "Emscripten"))
    add_subdirectory(Function/Render/Lumen/Test)
endif()

# Nanite cluster-builder smoke test. Own main(); carved out of the ZRuntime glob
# above and built only when ZENGINE_BUILD_NANITE_CLUSTER_SMOKE_TEST=ON. Pure CPU
# (split -> hierarchy -> geometry bytes), so no GPU/window. Not gated on the
# Lumen conditions: Function/Render/Nanite is never stripped from ZRuntime.
add_subdirectory(Function/Render/Nanite/Test)

# AppleSceneBlit smoke test. Own main(); carved out of the ZRuntime glob above
# and built only when ZENGINE_BUILD_APPLE_SCENE_BLIT_SMOKE_TEST=ON. On Apple it
# creates a headless MetalRHI (no window) and runs the blit; elsewhere it stops
# at the source-text contract checks.
add_subdirectory(Function/Render/Test)

# GPU-Spine importer test. Own main(); carved out of the ZRuntime glob above and
# built only when ZENGINE_BUILD_SPINE_IMPORT_TEST=ON. Pure CPU (atlas + json -> bake),
# so it needs no GPU and no window.
add_subdirectory(Function/Render/Spine/Test)

# =============================================================================
# ASTC texture decompression support (Phase 6)
# =============================================================================
if(TARGET astcenc)
    target_link_libraries(${TARGET_NAME} PUBLIC astcenc)
    target_link_libraries(${SHARED_TARGET_NAME} PUBLIC astcenc)
    message(STATUS "Runtime: Linked astcenc for ASTC texture decompression")
endif()