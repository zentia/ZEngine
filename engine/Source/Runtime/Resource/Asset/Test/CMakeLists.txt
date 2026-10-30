# =====================================================================
# 资产可移植性冒烟测试
#
#   cmake -B build -DZENGINE_BUILD_ASSET_PATH_PORTABILITY_SMOKE_TEST=ON
#   cmake --build build --target AssetPathPortabilitySmokeTest
#   ctest --test-dir build -C Debug -R AssetPathPortabilitySmokeTest
#
# 本目录的测试可执行文件自带 main()，必须从 ZRuntime 的 GLOB_RECURSE 里
# carve-out（见 Runtime/CMakeLists.txt 的 REMOVE_ITEM），否则每个消费者 exe
# 都会 LNK2005 "main already defined"。
#
# 盯的是：资产文件（prefab / scene / mat / meta / json）里不许出现机器相关的
# 绝对路径。2026-10-30 之前 8 个 prefab 的 externals 表写的是
# `E:/Engine/ZEngine/engine/asset/...`，后果是换机器/换盘静默断链：
# ResolveExternalAssetPath 先按绝对路径试 exists()，试不到才退回工程根 ——
# 作者机器上一切正常，别人拉下来 PPtr 全变 null，且没有任何日志。
#
# 三层断言：扫描覆盖率（防空测试）、无绝对路径、externals 相对路径真能解析。
# =====================================================================

option(ZENGINE_BUILD_ASSET_PATH_PORTABILITY_SMOKE_TEST
       "Build the asset path portability smoke-test executable"
       OFF)

if(ZENGINE_BUILD_ASSET_PATH_PORTABILITY_SMOKE_TEST)
    add_executable(AssetPathPortabilitySmokeTest AssetPathPortabilitySmokeTest.cpp)

    set_target_properties(AssetPathPortabilitySmokeTest PROPERTIES
        CXX_STANDARD 23
        FOLDER "Engine/Tests"
    )

    # 这个测试只扫文件系统与文本，不碰任何引擎对象 —— 不链 ZRuntime，
    # 只链 include root（同 SpotShadowSmokeTest / UMGDelegatesSmokeTest 的路子）。
    # 好处：不拖进 whole-archive 的 58 个 .gen.obj，增量构建快得多。
    target_include_directories(AssetPathPortabilitySmokeTest PRIVATE
        ${ENGINE_ROOT_DIR}/Source)

    if(MSVC)
        target_compile_options(AssetPathPortabilitySmokeTest PRIVATE /W4)
    endif()

    # 需要能定位 <engine>/asset 目录。
    target_compile_definitions(AssetPathPortabilitySmokeTest PRIVATE
        "ZENGINE_ENGINE_ROOT_DIR=\"${ENGINE_ROOT_DIR}\"")

    add_test(NAME AssetPathPortabilitySmokeTest COMMAND AssetPathPortabilitySmokeTest)
    message(STATUS "Configured asset path portability smoke-test target: AssetPathPortabilitySmokeTest")
endif()
