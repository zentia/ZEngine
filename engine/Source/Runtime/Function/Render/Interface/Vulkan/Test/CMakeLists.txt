# =====================================================================
# PR-V2: Vulkan bindless smoke-test
# ---------------------------------------------------------------------
# Builds a standalone executable that validates the Vulkan bindless
# toolchain at compile time -- a sister of the DX12 PR5c smoke-test
# living next door under dx12/test/. Mirrors that target's CMake shape
# 1:1 to keep the two cross-backend smoke-tests easy to compare.
#
# Default OFF -- no normal build is affected. Opt in with:
#
#   cmake -B build -DZENGINE_BUILD_VK_BINDLESS_SMOKE_TEST=ON
#   cmake --build build --target VulkanBindlessSmokeTest
#   ./build/.../VulkanBindlessSmokeTest
#
# Why a separate target instead of CTest / GoogleTest:
#   - The engine doesn't ship a test harness yet (only `shader_lab_test`
#     and `DX12BindlessSmokeTest` exist, both manually-invoked main()s).
#     PR-V2 isn't the right place to introduce one.
#   - The test deliberately runs OUTSIDE the editor / launcher so it
#     can validate the toolchain on a headless host without dragging
#     in the window-system manager.
#
# Why the test is compile-time-only (no real VkDevice):
#   - bringing up VkInstance + VkPhysicalDevice + VkDevice from a CI
#     box without an installed ICD is a pile of platform-specific
#     loader edge-cases. PR-V3 will add a runtime probe-then-skip pass
#     once the first real bindless tonemap pass needs a live device.
#     For PR-V2 the contract is purely "the helper has the agreed
#     shape AND VulkanRHI overrides the bindless push" -- both fully
#     verifiable at compile time via static_assert.
# =====================================================================

option(ZENGINE_BUILD_VK_BINDLESS_SMOKE_TEST
       "Build the Vulkan bindless smoke-test executable (PR-V2)"
       OFF)

if(NOT ZENGINE_BUILD_VK_BINDLESS_SMOKE_TEST)
    return()
endif()

# Sanity gate: the test pulls in the vulkan/*.cpp tree transitively
# through ZRuntime. ZEngine compiles its Vulkan backend on every
# desktop / mobile platform that has a Vulkan SDK, so unlike the DX12
# smoke-test there is no host-OS exclusion here.

set(_smoke_target VulkanBindlessSmokeTest)

add_executable(${_smoke_target}
    VulkanBindlessSmokeTest.cpp
)

set_target_properties(${_smoke_target} PROPERTIES
    CXX_STANDARD 23
    FOLDER "Engine/Tests"
)

# Inject the absolute path to this directory so a future runtime
# extension of this test can locate its .glsl / .spv assets without
# depending on CWD. Currently unused (PR-V2 is compile-time-only),
# kept for symmetry with ZENGINE_BINDLESS_TEST_DIR on the DX12 side.
target_compile_definitions(${_smoke_target} PRIVATE
    ZENGINE_VK_BINDLESS_TEST_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

# We need ZRuntime for:
#   - VulkanBindlessTextureManager (the unit under test).
#   - VulkanRHI (the override-existence sniff).
#   - render_type.h's RHI_SHADER_STAGE_*_BIT enums.
# Linking ZRuntime PRIVATE so the smoke-test's main() doesn't get
# WHOLEARCHIVE'd into anything else (matches the
# REMOVE_ITEM-from-SOURCE_FILES policy for shader_lab_test and the
# DX12 smoke-test).
target_link_libraries(${_smoke_target} PRIVATE
    ZRuntime
)

# ZRuntime PRIVATE-links ZEnginePCH (which carries EASTL's include
# directories), so its INTERFACE include paths do NOT propagate to
# downstream consumers. The smoke-test transitively pulls EASTL via
# rhi.h -> render_type.h -> core/base/hash.h -> <EASTL/functional.h>,
# so we must wire the same PCH dependency every other engine
# subproject does (Editor / Launcher / WebLauncher / profiler /
# DX12 smoke-test all follow this exact pattern).
# We do NOT REUSE_FROM the PCH here -- the smoke-test is a stripped-
# down TU and the cost of a fresh PCH compile is < 1s on a one-file
# executable.
if(TARGET ZEnginePCH)
    target_link_libraries(${_smoke_target} PRIVATE ZEnginePCH)
    target_include_directories(${_smoke_target} PRIVATE ${ENGINE_ROOT_DIR}/Source/CommonPCH)
    add_dependencies(${_smoke_target} ZEnginePCH)
endif()

# vulkan_rhi.h includes <vma/vk_mem_alloc.h> and <vulkan/vulkan.h>.
# ZRuntime resolves both via its own INTERFACE include paths on every
# supported platform; if a host CMake config breaks that, the build
# error fires here with a clear "vulkan headers not found" rather
# than deep inside ZRuntime.
find_package(Vulkan QUIET)
if(Vulkan_FOUND)
    target_include_directories(${_smoke_target} PRIVATE ${Vulkan_INCLUDE_DIRS})
endif()

# Match the rest of the engine's MSVC warning policy without /WX so a
# new toolchain warning doesn't break this scratch build.
if(MSVC)
    target_compile_options(${_smoke_target} PRIVATE /W4)
endif()

message(STATUS "Configured Vulkan bindless smoke-test target: ${_smoke_target}")
