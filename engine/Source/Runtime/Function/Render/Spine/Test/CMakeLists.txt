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

    # ---- 骨骼解算 ↔ 参考实现（spine-cpp 3.8）逐骨骼对拍 ----
    #
    # S4b 抄的是 IK / transform / path 三套迭代求解器，期望值**手算不出来**
    # ⇒ 夹具里写死期望值只会把实现的行为抄回给自己（循环论证）。
    # 唯一能真正钉住转录错误的是把参考实现编译进来比。
    #
    # 参考源码不在本仓库里（它是 OSGame 引擎里带的 spine-cpp），所以：
    #   * 默认取 G:/OSGame/unity-engine-source/Modules/Spine/spine-cpp
    #   * 那个路径不存在就**不注册**这条测试（它是加码，不是构建的前置条件）
    # 可用 -DZENGINE_SPINE_REF_ROOT=<路径> 覆盖。
    option(ZENGINE_BUILD_SPINE_REF_PARITY_TEST
           "Per-bone cross-check of the GPU-Spose solver against the reference spine-cpp 3.8"
           ON)
    set(ZENGINE_SPINE_REF_ROOT "G:/OSGame/unity-engine-source/Modules/Spine/spine-cpp"
        CACHE PATH "Root of the reference spine-cpp (3.8) sources")

    if(ZENGINE_BUILD_SPINE_REF_PARITY_TEST AND EXISTS "${ZENGINE_SPINE_REF_ROOT}/src/spine/Skeleton.cpp")
        # Atlas.cpp / AtlasAttachmentLoader.cpp **排除**：
        #   * Atlas.cpp 吃 Unity 的 `Configuration/PrefixConfigure.h` + `Runtime/Logging/LogAssert.h`
        #     → 再吃 baselib 的 `BaselibPlatformSpecificEnvironment.h`（该头文件在
        #       unity-engine-source 里**根本不存在**）⇒ 编不过。
        #   * 我们只比骨骼，用自己的空 AttachmentLoader ⇒ 不需要图集。
        # 代价：`SkeletonBinary::SkeletonBinary(Atlas*)`（SkeletonBinary.cpp:98）
        # 里 new 了 `AtlasAttachmentLoader`，Debug 下链接器按 .obj 粒度拉符号
        # ⇒ 那个构造函数仍要能解析。测试里给了一份"永不调用"的桩实现。
        file(GLOB _ref_src_all "${ZENGINE_SPINE_REF_ROOT}/src/spine/*.cpp")
        set(_ref_src "")
        foreach(_f IN LISTS _ref_src_all)
            get_filename_component(_n "${_f}" NAME)
            if(NOT _n STREQUAL "Atlas.cpp" AND NOT _n STREQUAL "AtlasAttachmentLoader.cpp")
                list(APPEND _ref_src "${_f}")
            endif()
        endforeach()

        add_library(SpineReference STATIC EXCLUDE_FROM_ALL ${_ref_src})
        set_target_properties(SpineReference PROPERTIES
            CXX_STANDARD 17
            FOLDER "Engine/Tests")
        # 参考实现自己的头是 `#include <spine/X.h>`，但它**内部**互相 include 时写的是
        # `Modules/Spine/spine-cpp/include/spine/X.h`（相对 Unity 引擎根）
        # ⇒ 除了 include/ 还必须把引擎根也挂上（spine_ref_probe 当初就是这么配的）。
        get_filename_component(_ref_engine_root "${ZENGINE_SPINE_REF_ROOT}" DIRECTORY)
        get_filename_component(_ref_engine_root "${_ref_engine_root}" DIRECTORY)
        get_filename_component(_ref_engine_root "${_ref_engine_root}" DIRECTORY)
        target_include_directories(SpineReference PUBLIC
            "${ZENGINE_SPINE_REF_ROOT}/include"
            "${_ref_engine_root}")
        target_compile_definitions(SpineReference PUBLIC SPINE_USE_STD_FUNCTIONS=0)
        if(MSVC)
            # 参考实现是第三方代码，不按本工程的警告等级要求它
            target_compile_options(SpineReference PRIVATE /W0)
        endif()

        add_executable(SpineGpuRefParityTest SpineGpuRefParityTest.cpp)
        set_target_properties(SpineGpuRefParityTest PROPERTIES
            CXX_STANDARD 23
            FOLDER "Engine/Tests")
        target_link_libraries(SpineGpuRefParityTest PRIVATE ZRuntime SpineReference)
        if(TARGET ZEnginePCH)
            target_link_libraries(SpineGpuRefParityTest PRIVATE ZEnginePCH)
            target_include_directories(SpineGpuRefParityTest PRIVATE ${ENGINE_ROOT_DIR}/Source/CommonPCH)
            add_dependencies(SpineGpuRefParityTest ZEnginePCH)
        endif()
        if(MSVC)
            target_compile_options(SpineGpuRefParityTest PRIVATE /W4)
        endif()
        message(STATUS "Configured GPU-Spine reference parity test (ref: ${ZENGINE_SPINE_REF_ROOT})")
    else()
        message(STATUS "GPU-Spine: 参考 spine-cpp 不在 ${ZENGINE_SPINE_REF_ROOT} ⇒ 不注册参考对拍")
    endif()

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
