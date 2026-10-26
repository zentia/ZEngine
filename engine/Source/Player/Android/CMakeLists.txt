# =============================================================================
# ZGameAndroid — Android 宿主共享库（P0-1 移动端出包链）
# -----------------------------------------------------------------------------
# NativeActivity 加载 libZGameAndroid.so 并调用 android_main()。这个库只含
# 平台胶水（engine/Source/Player/Android/AndroidMain.cpp），引擎本身在 ZRuntime
# 里，启动逻辑在 ZPlayer 里。
#
# 由 AGP 的 externalNativeBuild 调起（见同目录 build.gradle），也可以手动跑：
#   cmake -S engine -B build/android \
#         -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
#         -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26
# =============================================================================

if(NOT ANDROID)
    message(FATAL_ERROR "ZGameAndroid is Android-only; it must not be added on other platforms.")
endif()

set(TARGET_NAME ZGameAndroid)

add_library(${TARGET_NAME} SHARED
    "${CMAKE_CURRENT_SOURCE_DIR}/AndroidMain.cpp"
)

set_target_properties(${TARGET_NAME} PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    FOLDER "Engine"
)

# ---------------------------------------------------------------------------
# NDK 根目录。不同调用方给的变量名不一样：AGP / NDK toolchain 用 ANDROID_NDK，
# 老脚本用 ANDROID_NDK_ROOT_PATH，命令行构建常常只有环境变量。逐个试。
# ---------------------------------------------------------------------------
if(NOT ZENGINE_ANDROID_NDK)
    if(DEFINED ANDROID_NDK)
        set(ZENGINE_ANDROID_NDK "${ANDROID_NDK}")
    elseif(DEFINED CMAKE_ANDROID_NDK)
        set(ZENGINE_ANDROID_NDK "${CMAKE_ANDROID_NDK}")
    elseif(DEFINED ANDROID_NDK_ROOT_PATH)
        set(ZENGINE_ANDROID_NDK "${ANDROID_NDK_ROOT_PATH}")
    elseif(DEFINED ENV{ANDROID_NDK_HOME})
        set(ZENGINE_ANDROID_NDK "$ENV{ANDROID_NDK_HOME}")
    elseif(DEFINED ENV{ANDROID_HOME})
        file(GLOB _zengine_ndks "$ENV{ANDROID_HOME}/ndk/*")
        list(SORT _zengine_ndks)
        list(LENGTH _zengine_ndks _zengine_ndk_count)
        if(_zengine_ndk_count GREATER 0)
            list(GET _zengine_ndks -1 ZENGINE_ANDROID_NDK)
        endif()
        unset(_zengine_ndks)
        unset(_zengine_ndk_count)
    endif()
endif()

# ---------------------------------------------------------------------------
# android_native_app_glue：android_main + ALooper 事件循环的 NDK 胶水。
# NativeActivity 必须靠它把 APP_CMD_* 送到 native 侧。
# ---------------------------------------------------------------------------
if(ZENGINE_ANDROID_NDK)
    string(REPLACE "\\" "/" _glue_source
        "${ZENGINE_ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c")

    if(NOT EXISTS "${_glue_source}")
        message(FATAL_ERROR
            "android_native_app_glue.c not found at ${_glue_source}. "
            "Point ZENGINE_ANDROID_NDK at the NDK root.")
    endif()

    target_sources(${TARGET_NAME} PRIVATE "${_glue_source}")
    target_include_directories(${TARGET_NAME} PRIVATE
        "${ZENGINE_ANDROID_NDK}/sources/android/native_app_glue")
    # glue 是 C，别让它按 C++ 编。
    set_source_files_properties("${_glue_source}" PROPERTIES LANGUAGE C)
else()
    message(FATAL_ERROR
        "Cannot locate the Android NDK. Set ANDROID_NDK / CMAKE_ANDROID_NDK / "
        "ANDROID_HOME so android_native_app_glue can be compiled in.")
endif()

# ZPlayer: 启动 + 每帧推进（内部再链 ZRuntime）。
target_link_libraries(${TARGET_NAME} PRIVATE ZPlayer)

# android: ANativeWindow / AAssetManager / ALooper
# log:     __android_log_print
# vulkan:  libvulkan.so（Android 7.0 / API 24 起系统提供）
target_link_libraries(${TARGET_NAME} PRIVATE android log vulkan)

target_compile_definitions(${TARGET_NAME} PRIVATE ZENGINE_ANDROID_PLAYER=1)

message(STATUS "ZGameAndroid: NativeActivity host library (abi=${ANDROID_ABI})")
