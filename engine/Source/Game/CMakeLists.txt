# =============================================================================
# ZGame — 独立游戏进程（对标 UE 打包后的游戏 exe）
#
# 与 ZEditor 的区别：不链接编辑器（无 Content Browser / Inspector / 布局系统），
# 只链接 ZRuntime + 游戏框架层，入口是 GameEngine::Run()。
# =============================================================================
set(TARGET_NAME ZGame)

set(ZGAME_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/ZGame.cpp
)

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${ZGAME_SOURCES})

if(WIN32)
    add_executable(${TARGET_NAME} WIN32 ${ZGAME_SOURCES})
else()
    add_executable(${TARGET_NAME} ${ZGAME_SOURCES})
endif()

set_target_properties(${TARGET_NAME} PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    OUTPUT_NAME "ZGame"
    FOLDER "Engine"
)

z_set_config_output_dirs(${TARGET_NAME} "${BINARY_ROOT_DIR}" RUNTIME)

target_include_directories(${TARGET_NAME} PRIVATE
    ${ENGINE_ROOT_DIR}/Source
    ${ENGINE_ROOT_DIR}/Source/CommonPCH
    ${ENGINE_ROOT_DIR}/Libraries/ZAppShell/include
)

# ZRuntime: 全部引擎系统。GameEngine 由 RegisterGameRuntime() 注册。
target_link_libraries(${TARGET_NAME} PUBLIC ZRuntime)

if(WIN32)
    # opengl32: ZRuntime 的部分渲染路径仍然引用（与 ZEditor 保持一致）
    target_link_libraries(${TARGET_NAME} PUBLIC opengl32)
    # shell32 / comdlg32: 项目路径解析与消息框
    target_link_libraries(${TARGET_NAME} PRIVATE shell32 comdlg32)
endif()

# 与 ZRuntime 共享同一份 PCH，避免重复编译公共头。
if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.16")
    target_link_libraries(${TARGET_NAME} PRIVATE ZEnginePCH)
    target_include_directories(${TARGET_NAME} PRIVATE ${ENGINE_ROOT_DIR}/Source/CommonPCH)
    add_dependencies(${TARGET_NAME} ZEnginePCH)
    target_precompile_headers(${TARGET_NAME} REUSE_FROM ZEnginePCH)
endif()

target_compile_options(${TARGET_NAME} PUBLIC "$<$<COMPILE_LANG_AND_ID:CXX,MSVC>:$<IF:$<BOOL:${ZENGINE_ENABLE_WERROR}>,/WX,/WX->>")

if(TARGET ShaderLab)
    target_link_libraries(${TARGET_NAME} PUBLIC ShaderLab)
endif()

# ZRuntime.dll 必须和 ZGame.exe 在同一目录，否则进程起不来。
add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:ZRuntime>" "$<TARGET_FILE_DIR:${TARGET_NAME}>/"
)

message(STATUS "ZGame: standalone game runtime (ZRuntime + GameFramework)")
