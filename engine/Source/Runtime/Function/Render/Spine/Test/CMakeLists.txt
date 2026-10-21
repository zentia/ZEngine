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

    # ---- 跨语言对拍（C++ 写 → C# 读 → C# 写，必须逐字节相同）----
    # 两端各自自测等于各自对上了自己的想象；字节是全格式唯一的公共事实。
    # 没装 dotnet 就跳过（这条检查是**加码**，不是构建的前置条件）。
    #
    # Opt out with:
    #   cmake -B build -DZENGINE_BUILD_SPINE_CSHARP_PARITY_TEST=OFF
    find_program(ZENGINE_DOTNET_EXECUTABLE NAMES dotnet)
    option(ZENGINE_BUILD_SPINE_CSHARP_PARITY_TEST
           "Cross-language check: bake containers in C++, read+rewrite them in C#, require byte-identical output"
           ON)

    if(ZENGINE_BUILD_SPINE_CSHARP_PARITY_TEST)
        if(ZENGINE_DOTNET_EXECUTABLE)
            add_test(NAME SpineGpuCSharpParityTest
                     COMMAND ${CMAKE_COMMAND}
                             -DDUMP_EXE=$<TARGET_FILE:${_import_target}>
                             -DDUMP_DIR=${CMAKE_BINARY_DIR}/spine_containers
                             -DCSPROJ=${CMAKE_CURRENT_SOURCE_DIR}/../CSharp/Test/ZEngine.SpineGpu.Test.csproj
                             -DDOTNET=${ZENGINE_DOTNET_EXECUTABLE}
                             -P ${CMAKE_CURRENT_SOURCE_DIR}/../CSharp/Test/RunParityTest.cmake)
            message(STATUS "Configured GPU-Spine cross-language parity test (dotnet: ${ZENGINE_DOTNET_EXECUTABLE})")
        else()
            message(STATUS "GPU-Spine: 没找到 dotnet ⇒ 跨语言对拍不注册（C# 源码仍在 CSharp/ 下）")
        endif()
    endif()
endif()
