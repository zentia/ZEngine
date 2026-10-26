# =============================================================================
# ZPlayer — 与平台无关的 Player 壳（P0-1 移动端出包链）
# -----------------------------------------------------------------------------
# 一个静态库，把"启动引擎 + 每帧推进"做成一组 C 入口（见 PlayerShell.h）。
#
# 为什么要单独一个库而不是让 Android / iOS 各写一份启动代码：
#   * 启动序列（崩溃处理 → 工作目录 → RegisterGameRuntime → START_SYSTEM →
#     启动 cvar → 挂载 bundles → 打开 startup_level）必须与桌面 ZGame 逐字一致，
#     否则移动端会出现"只有移动端有"的加载顺序 bug，且极难在桌面复现。
#   * 桌面 ZGame 也链这个库并复用同一批函数（见 Source/Game/ZGame.cpp），
#     于是这七个步骤在全仓库只有一份实现。
#
# 平台宿主（Android 的 GameActivity / JNI，iOS 的 UIKit App）只写胶水，
# 逻辑一律调到这儿。
# =============================================================================

set(TARGET_NAME ZPlayer)

set(ZPLAYER_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/PlayerShell.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/PlayerShell.h"
)

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${ZPLAYER_SOURCES})

# 静态库：Android 把它链进宿主 .so，iOS 链进 App，桌面链进 ZGame.exe。
add_library(${TARGET_NAME} STATIC ${ZPLAYER_SOURCES})

set_target_properties(${TARGET_NAME} PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    FOLDER "Engine"
)

target_include_directories(${TARGET_NAME} PUBLIC
    ${ENGINE_ROOT_DIR}/Source
    ${ENGINE_ROOT_DIR}/Source/CommonPCH
)

# ZRuntime：全部引擎系统。ZPlayer 只是它们之上的一层薄宿主 API。
target_link_libraries(${TARGET_NAME} PUBLIC ZRuntime)

# 与 ZRuntime 共享同一份 PCH，避免重复编译公共头。
if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.16" AND TARGET ZEnginePCH)
    target_link_libraries(${TARGET_NAME} PRIVATE ZEnginePCH)
    add_dependencies(${TARGET_NAME} ZEnginePCH)
    target_precompile_headers(${TARGET_NAME} REUSE_FROM ZEnginePCH)
endif()

target_compile_options(${TARGET_NAME} PUBLIC "$<$<COMPILE_LANG_AND_ID:CXX,MSVC>:$<IF:$<BOOL:${ZENGINE_ENABLE_WERROR}>,/WX,/WX->>")

message(STATUS "ZPlayer: platform-agnostic player shell (boot + per-frame tick)")
