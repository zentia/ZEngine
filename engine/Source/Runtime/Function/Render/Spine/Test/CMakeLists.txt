# =====================================================================
# GPU-Spine 导入器测试：atlas + json → 烘焙，全程 CPU，无 GPU / 无窗口
# ---------------------------------------------------------------------
# 钉的是「静默就会错」的那几条规则（规格 §11：任何静默调整都必须报错）：
#   * 影响数 > MAX_INFLUENCES        → 报错并报出附件名，绝不静默截断
#   * 权重 unorm8 的和必须恰好 255
#   * weighted 顶点段起点 8 B 对齐（Load2 的前提）
#   * spine 版本不是 3.8            → 报错（4.x 布局不同）
# 这些在运行时全都表现为"画得出来但形状错"，只能在编译期/测试期挡住。
#
# Opt in with:
#   cmake -B build -DZENGINE_BUILD_SPINE_IMPORT_TEST=ON
#   cmake --build build --target SpineGpuImportTest --config Debug
# =====================================================================

option(ZENGINE_BUILD_SPINE_IMPORT_TEST
       "Build the GPU-Spine importer test (atlas + json -> bake, CPU only)"
       OFF)

if(ZENGINE_BUILD_SPINE_IMPORT_TEST)
    set(_import_target SpineGpuImportTest)

    add_executable(${_import_target}
        SpineGpuImportTest.cpp
    )

    set_target_properties(${_import_target} PROPERTIES
        CXX_STANDARD 23
        FOLDER "Engine/Tests"
    )

    target_link_libraries(${_import_target} PRIVATE
        ZRuntime
    )

    if(TARGET ZEnginePCH)
        target_link_libraries(${_import_target} PRIVATE ZEnginePCH)
        target_include_directories(${_import_target} PRIVATE ${ENGINE_ROOT_DIR}/Source/CommonPCH)
        add_dependencies(${_import_target} ZEnginePCH)
    endif()

    if(MSVC)
        target_compile_options(${_import_target} PRIVATE /W4)
    endif()

    add_test(NAME ${_import_target} COMMAND ${_import_target})

    message(STATUS "Configured GPU-Spine importer test target: ${_import_target}")
endif()
