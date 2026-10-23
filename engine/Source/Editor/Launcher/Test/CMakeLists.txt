# =====================================================================
# ZEditor Launcher wiring smoke test.
# ---------------------------------------------------------------------
# Text-only contract over Main.cpp / ZEditorLauncher.{h,cpp,mm}: it pins the
# "double clicking ZEditor flashes and dies" path -- no -p must reach a real
# launcher, and picking a project must relaunch self with -p. No engine
# dependency at all (the startup branch runs before START_SYSTEM(), so no test
# process can ever execute it); the source files are passed in as macros
# because a CTest run has no useful working directory.
#
# Default OFF; opt in with:
#
#   cmake -B build -DZENGINE_BUILD_LAUNCHER_WIRING_SMOKE_TEST=ON
# =====================================================================

option(ZENGINE_BUILD_LAUNCHER_WIRING_SMOKE_TEST
       "Build the ZEditor launcher wiring smoke-test executable"
       OFF)

if(NOT ZENGINE_BUILD_LAUNCHER_WIRING_SMOKE_TEST)
    return()
endif()

add_executable(ZEditorLauncherWiringSmokeTest
    ZEditorLauncherWiringSmokeTest.cpp
)

set_target_properties(ZEditorLauncherWiringSmokeTest PROPERTIES
    CXX_STANDARD 20
    FOLDER "Engine/Tests"
)

# 被测源码的路径在 configure 期就钉死：CTest 的工作目录是 build 树里的某个
# 子目录，跟源码树没有任何稳定关系，运行期猜相对路径只会时灵时不灵。
target_compile_definitions(ZEditorLauncherWiringSmokeTest PRIVATE
    ZEDITOR_MAIN_CPP="${CMAKE_CURRENT_SOURCE_DIR}/../../Main/Main.cpp"
    ZEDITOR_LAUNCHER_DIR="${CMAKE_CURRENT_SOURCE_DIR}/.."
)

add_test(NAME ZEditorLauncherWiringSmokeTest COMMAND ZEditorLauncherWiringSmokeTest)

message(STATUS "Configured launcher wiring smoke-test target: ZEditorLauncherWiringSmokeTest")
