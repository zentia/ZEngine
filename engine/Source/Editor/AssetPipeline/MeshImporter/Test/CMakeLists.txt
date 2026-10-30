# =====================================================================
# glTF / GLB 几何导入冒烟测试
#
#   cmake -B build -DZENGINE_BUILD_GLTF_MESH_IMPORT_SMOKE_TEST=ON
#   cmake --build build --target GltfMeshImportSmokeTest
#   ctest --test-dir build -C Debug -R GltfMeshImportSmokeTest
#
# 目标自带 main()，靠 Editor/CMakeLists.txt 里的
# `list(FILTER EDITOR_SOURCES EXCLUDE REGEX ".*/AssetPipeline/.*/[Tt]est/.*")`
# 排除出 ZEditor —— 那条规则已经覆盖了本目录，不需要再加过滤。
#
# 为什么需要它
# ------------
# 在 2026-10-30 之前 `CanImport` 无条件列出 .gltf/.glb，而 Import() 对应分支只有
# 一句 "glTF/GLB import is not implemented yet" —— 能选、能点、必然失败（假支持）。
#
# 真支持之后风险升级成"导入成功但几何是错的"，所以这里钉的是：
#   1. .gltf(外置 .bin) / .gltf(data: URI) / .glb 三条容器路径结果逐字节一致；
#   2. 节点世界变换生效，且**法线走逆转置**（非均匀缩放下用原矩阵 = 光照全错
#      而模型"看着导入成功了"）；
#   3. 不支持的东西**显式报错** —— LINES 图元 / 蒙皮 / 无几何 / 坏文件；
#   4. 源码级回归门：扩展名清单必须由解析器宏门控，防"假支持"复发。
#
# 只编 GltfMeshReader.cpp（不带引擎系统依赖）+ 链 ZRuntime（拿 MeshData）与
# cgltf。不链 ZEditor —— 那会把整个编辑器拖进来。
# =====================================================================

option(ZENGINE_BUILD_GLTF_MESH_IMPORT_SMOKE_TEST
       "Build the glTF/GLB mesh import smoke-test executable"
       OFF)

if(ZENGINE_BUILD_GLTF_MESH_IMPORT_SMOKE_TEST)
    if(NOT TARGET cgltf)
        message(WARNING "GltfMeshImportSmokeTest requires the cgltf target; "
                        "cgltf was not found under engine/3rdparty/cgltf. Test disabled.")
        return()
    endif()

    add_executable(GltfMeshImportSmokeTest
        GltfMeshImportSmokeTest.cpp
        "${CMAKE_CURRENT_SOURCE_DIR}/../GltfMeshReader.cpp")

    set_target_properties(GltfMeshImportSmokeTest PROPERTIES
        CXX_STANDARD 23
        FOLDER "Engine/Tests"
    )

    target_include_directories(GltfMeshImportSmokeTest PRIVATE
        ${ENGINE_ROOT_DIR}/Source)

    target_link_libraries(GltfMeshImportSmokeTest PRIVATE ZRuntime cgltf)

    if(TARGET ZEnginePCH)
        target_link_libraries(GltfMeshImportSmokeTest PRIVATE ZEnginePCH)
        target_include_directories(GltfMeshImportSmokeTest PRIVATE ${ENGINE_ROOT_DIR}/Source/CommonPCH)
        add_dependencies(GltfMeshImportSmokeTest ZEnginePCH)
        target_precompile_headers(GltfMeshImportSmokeTest REUSE_FROM ZEnginePCH)
    endif()

    if(MSVC)
        target_compile_options(GltfMeshImportSmokeTest PRIVATE /W4)
    endif()

    # E 组读源码做回归门。
    target_compile_definitions(GltfMeshImportSmokeTest PRIVATE
        "ZENGINE_ENGINE_ROOT_DIR=\"${ENGINE_ROOT_DIR}\"")

    add_test(NAME GltfMeshImportSmokeTest COMMAND GltfMeshImportSmokeTest)
    message(STATUS "Configured glTF mesh import smoke-test target: GltfMeshImportSmokeTest")
endif()
