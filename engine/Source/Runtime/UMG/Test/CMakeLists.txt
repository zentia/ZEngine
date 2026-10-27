# =====================================================================
# UMG named-event registry smoke test (P0 data binding).
# ---------------------------------------------------------------------
# Verifies the pure core in Core/UMGDelegates.h
# (ZUMG::UMGEventRegistry: Bind / Invoke / Unbind / UnbindEvent / Clear).
# The class is header-only, but GetUMGEventRegistry() lives in Core/UMGDelegates.cpp,
# so that .cpp is compiled WITH the test (it is dependency-free pure C++). The
# target still links only the include root, not ZRuntime.
#
# This is the P0 data-binding core: a Designer-authored asset stores an event
# NAME on a widget (e.g. UButton::OnClickEvent); the game binds a handler by
# that name at startup; the widget fires it via Invoke on click. The registry
# owns the handler lifetime (Unbind on owner teardown prevents dangling).
#
#   cmake -B build -DZENGINE_BUILD_UMG_DELEGATES_SMOKE_TEST=ON
#   cmake --build build --target UMGDelegatesSmokeTest
# =====================================================================

option(ZENGINE_BUILD_UMG_DELEGATES_SMOKE_TEST
       "Build the UMG named-event registry smoke-test executable"
       OFF)

if(NOT ZENGINE_BUILD_UMG_DELEGATES_SMOKE_TEST)
    return()
endif()

add_executable(UMGDelegatesSmokeTest
    UMGDelegatesSmokeTest.cpp
    ../Core/UMGDelegates.cpp   # GetUMGEventRegistry() singleton (dependency-free)
)

set_target_properties(UMGDelegatesSmokeTest PROPERTIES
    CXX_STANDARD 20
    FOLDER "Engine/Tests"
)

# Header-only include chain: only the engine/Source root is needed.
target_include_directories(UMGDelegatesSmokeTest PRIVATE
    ${ENGINE_ROOT_DIR}/Source
)

if(MSVC)
    target_compile_options(UMGDelegatesSmokeTest PRIVATE /W4)
endif()

add_test(NAME UMGDelegatesSmokeTest COMMAND UMGDelegatesSmokeTest)

message(STATUS "Configured UMG delegates smoke-test target: UMGDelegatesSmokeTest")

# =====================================================================
# UMG widget-tree traversal smoke test (P0 focus nav + P1 tick).
# ---------------------------------------------------------------------
# Verifies the header-only pure core in Core/UMGTree.h (TickWidgetTree /
# CollectFocusable / NextFocusIndex): depth-first tick with Collapsed-subtree
# skip, focusable collection in tree order, and the Tab wrap/clamp index
# arithmetic. Pure algorithm over UWidget's virtual child access, no engine deps,
# links only the include root.
#
#   cmake -B build -DZENGINE_BUILD_UMG_TREE_SMOKE_TEST=ON
#   cmake --build build --target UMGTreeSmokeTest
# =====================================================================

option(ZENGINE_BUILD_UMG_TREE_SMOKE_TEST
       "Build the UMG widget-tree traversal smoke-test executable"
       OFF)

if(NOT ZENGINE_BUILD_UMG_TREE_SMOKE_TEST)
    return()
endif()

add_executable(UMGTreeSmokeTest UMGTreeSmokeTest.cpp)

set_target_properties(UMGTreeSmokeTest PROPERTIES
    CXX_STANDARD 20
    FOLDER "Engine/Tests"
)

# Header-only include chain: only the engine/Source root is needed. UMGTree.h is
# a template over the node type, so the test drives it with a minimal stub struct
# (no ZSlate / engine-math headers required).
target_include_directories(UMGTreeSmokeTest PRIVATE
    ${ENGINE_ROOT_DIR}/Source
)

if(MSVC)
    target_compile_options(UMGTreeSmokeTest PRIVATE /W4)
endif()

add_test(NAME UMGTreeSmokeTest COMMAND UMGTreeSmokeTest)

message(STATUS "Configured UMG tree smoke-test target: UMGTreeSmokeTest")
