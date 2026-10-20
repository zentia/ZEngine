# =====================================================================
# UndoSystem smoke test (editor undo/redo transaction stack).
# ---------------------------------------------------------------------
# Standalone executable over the engine-agnostic core (UndoSystem.cpp has
# no engine dependency), so it builds and runs without ZRuntime, a device
# or a project. Default OFF; opt in with:
#
#   cmake -B build -DZENGINE_BUILD_UNDO_SMOKE_TEST=ON
#   cmake --build build --target UndoSystemSmokeTest
# =====================================================================

option(ZENGINE_BUILD_UNDO_SMOKE_TEST
       "Build the editor UndoSystem smoke-test executable"
       OFF)

if(NOT ZENGINE_BUILD_UNDO_SMOKE_TEST)
    return()
endif()

add_executable(UndoSystemSmokeTest
    UndoSystemSmokeTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../UndoSystem.cpp
)

set_target_properties(UndoSystemSmokeTest PROPERTIES
    CXX_STANDARD 20
    FOLDER "Engine/Tests"
)

# "Editor/Undo/UndoSystem.h" is included engine-root relative.
target_include_directories(UndoSystemSmokeTest PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../..
)

# Registered with CTest (exit code is the verdict: EXIT_FAILURE on any failed check).
add_test(NAME UndoSystemSmokeTest COMMAND UndoSystemSmokeTest)

message(STATUS "Configured UndoSystem smoke-test target: UndoSystemSmokeTest")
