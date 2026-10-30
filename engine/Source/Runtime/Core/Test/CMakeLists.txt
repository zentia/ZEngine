# =====================================================================
# CI 覆盖率门禁
#
#   cmake -B build -DZENGINE_BUILD_CI_COVERAGE_SMOKE_TEST=ON
#   cmake --build build --target CiSmokeTestCoverageSmokeTest
#   ctest --test-dir build -C Debug -R CiSmokeTestCoverageSmokeTest
#
# 本目录的测试可执行文件自带 main()，必须从 ZRuntime 的 GLOB_RECURSE 里
# carve-out（见 Runtime/CMakeLists.txt 的 REMOVE_ITEM），否则每个消费者 exe
# 都会 LNK2005 "main already defined"。
#
# 只扫文件系统与文本 —— 不链 ZRuntime，只链 include root（同
# AssetPathPortabilitySmokeTest），增量构建快得多。
#
# 盯的是：一个 smoke test 只有在某个 workflow 里出现 `-D<NAME>=ON` 才算真的
# 在跑。2026-10-30 实测 90 个开关里只有 38 个被启用，其余 52 个从未在 CI 上
# 跑过（典型样本：Lumen 那六组被注释提到但没启用，而它们本该由 Linux 宿主
# 负责 —— 当时 Linux 一个测试都没接）。
# =====================================================================

option(ZENGINE_BUILD_CI_COVERAGE_SMOKE_TEST
       "Build the CI smoke-test coverage gate executable"
       OFF)

if(NOT ZENGINE_BUILD_CI_COVERAGE_SMOKE_TEST)
    return()
endif()

add_executable(CiSmokeTestCoverageSmokeTest CiSmokeTestCoverageSmokeTest.cpp)

set_target_properties(CiSmokeTestCoverageSmokeTest PROPERTIES
    CXX_STANDARD 23
    FOLDER "Engine/Tests"
)

target_include_directories(CiSmokeTestCoverageSmokeTest PRIVATE
    ${ENGINE_ROOT_DIR}/Source)

if(MSVC)
    target_compile_options(CiSmokeTestCoverageSmokeTest PRIVATE /W4)
endif()

# 需要同时定位仓库根（.github/workflows 在它下面）和工程根（CMakeLists.txt
# 都在它下面）。两者是父子关系，但显式传两个比在 C++ 里猜 parent() 稳。
target_compile_definitions(CiSmokeTestCoverageSmokeTest PRIVATE
    "ZENGINE_REPO_ROOT_DIR=\"${Z_ROOT_DIR}\""
    "ZENGINE_ENGINE_ROOT_DIR=\"${ENGINE_ROOT_DIR}\"")

add_test(NAME CiSmokeTestCoverageSmokeTest COMMAND CiSmokeTestCoverageSmokeTest)
message(STATUS "Configured CI coverage gate target: CiSmokeTestCoverageSmokeTest")
