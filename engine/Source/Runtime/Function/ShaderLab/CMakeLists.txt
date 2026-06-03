# ShaderLab Module CMakeLists.txt
#
# This module is normally compiled directly into ZRuntime (the GLOB_RECURSE
# in engine/source/runtime/CMakeLists.txt picks the cpp files up). On three
# platforms where Vulkan is unavailable -- Web (Emscripten), Apple, and
# Windows when ZENGINE_USE_VULKAN is OFF -- those cpp files are explicitly
# EXCLUDE'd from ZRuntime because ShaderLabCompiler.{h,cpp} pulls in
# Function/Render/Interface/Vulkan/ShaderCompiler.h.
#
# The Editor still needs ShaderLabParser (used by inspector_window,
# project_window, shader_preview_renderer), and parser/lexer/asset are
# Vulkan-free. So when this CMakeLists is invoked (only on those three
# platforms via add_subdirectory in runtime/CMakeLists.txt) we build a
# trimmed-down ShaderLab STATIC that contains only the Vulkan-free
# translation units, and let ZEditor link it.
#
# On Vulkan-capable platforms (Linux / Android / OHOS / iOS / Win+Vulkan)
# this CMakeLists is NOT add_subdirectory'd -- the cpp files go straight
# into ZRuntime as before, so nothing changes for those configs.

# Always-safe sources (no Vulkan / no DX12 / no Metal dependency).
set(SHADER_LAB_VK_FREE_SOURCES
    ShaderLabLexer.cpp
    ShaderLabParser.cpp
    ShaderLabHlslExtract.cpp
    ShaderLabVariant.cpp
    ShaderLabDx12Compiler.cpp
)

# Compiler currently depends on the Vulkan shader_compiler header. We only
# add it when the host build can satisfy that include path; otherwise we
# ship a parser-only ShaderLab.
set(SHADER_LAB_SOURCES ${SHADER_LAB_VK_FREE_SOURCES})

if(ZENGINE_USE_VULKAN)
    list(APPEND SHADER_LAB_SOURCES ShaderLabCompiler.cpp)
endif()

# Create ShaderLab library
add_library(ShaderLab STATIC ${SHADER_LAB_SOURCES})

# ShaderLab headers reference paths like
#   "Runtime/Function/ShaderLab/ShaderLabParser.h"  (cpp side)
#   "Runtime/Core/Base/Macro.h"                       (transitive)
# Both resolve relative to engine/Source. The local dir is also exposed so
# the Editor can include "ShaderLabParser.h" directly if it wants.
target_include_directories(ShaderLab PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${ENGINE_ROOT_DIR}/Source
)

# Compile features: ShaderLab uses std::variant / std::optional in
# shader_lab_asset.h, so C++17 is the floor. We promote to ZEngine's
# project-wide C++23 to stay consistent with ZRuntime / ZEditor.
target_compile_features(ShaderLab PUBLIC cxx_std_17)
set_target_properties(ShaderLab PROPERTIES CXX_STANDARD 23)

# Folder organisation in IDE.
set_target_properties(ShaderLab PROPERTIES FOLDER "Engine")

# PRIVATE-link to ZRuntime. Rationale:
# - ShaderLabParser.cpp / ShaderLabLexer.cpp #include
#   "Runtime/Core/Base/Macro.h", which transitively pulls in
#   engine_log.h, which pulls in <bq_log/bq_log.h>. The bqlog include
#   directory lives on ZRuntime's INTERFACE_INCLUDE_DIRECTORIES, not on
#   any path we can reach via just ENGINE_ROOT_DIR/Source. Linking
#   ZRuntime PRIVATE pulls in those header search paths for our cpp
#   compilation step.
# - PRIVATE keeps it off our INTERFACE so ZEditor (which add_subdirectory's
#   us only on configs where ZRuntime does NOT also contain the parser
#   cpp) doesn't double-propagate through us. ZEditor links ZRuntime
#   directly already, so this is purely cosmetic.
# - No circular dependency: this CMakeLists is only add_subdirectory'd
#   on Web / Apple / Windows-without-Vulkan, where ShaderLab/*.cpp has
#   been EXCLUDE'd from ZRuntime -- ZRuntime never depends on ShaderLab,
#   so ShaderLab -> ZRuntime is a one-way edge.
target_link_libraries(ShaderLab PRIVATE ZRuntime)

# Platform-specific compile options.
if(MSVC)
    # /W4 inherited from upstream config; keep it but drop /WX so the rest
    # of the engine's warning policy still wins via target propagation.
    target_compile_options(ShaderLab PRIVATE /W4)
else()
    target_compile_options(ShaderLab PRIVATE -Wall -Wextra)
endif()
