# =============================================================================
# ZGameiOS — iOS App target（P0-1 移动端出包链）
# -----------------------------------------------------------------------------
# MACOSX_BUNDLE + CMAKE_SYSTEM_NAME=iOS 会让 CMake 直接产出一个 .app bundle，
# -G Xcode 时还能拿到可以 Archive / 上传的 Xcode 工程。这正是"iOS 出包链"要的
# 产物，比手写 .xcodeproj 可靠得多（手写工程在 CMake 之外还要再维护一份源列表）。
#
# 手动跑：
#   cmake -S engine -B build/ios -G Xcode \
#         -DCMAKE_TOOLCHAIN_FILE=engine/toolchain/ios.cmake \
#         -DZENGINE_IOS_DEVELOPMENT_TEAM=XXXXXXXXXX
#   cmake --build build/ios --config Release
#
# zbuild_ios.py 封装了上面两步 + 项目内容 staging + xcodebuild archive。
# =============================================================================

if(NOT IOS)
    message(FATAL_ERROR "ZGameIOS is iOS-only; it must not be added on other platforms.")
endif()

set(TARGET_NAME ZGameIOS)

add_executable(${TARGET_NAME} MACOSX_BUNDLE
    "${CMAKE_CURRENT_SOURCE_DIR}/IOSMain.mm"
)

set_target_properties(${TARGET_NAME} PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    FOLDER "Engine"
    OUTPUT_NAME "ZEngine"

    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist"
    MACOSX_BUNDLE_BUNDLE_NAME "ZEngine"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.zengine.player"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0"
    MACOSX_BUNDLE_BUNDLE_VERSION "1"

    XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "16.0"
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.zengine.player"
    XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Automatic"
    XCODE_ATTRIBUTE_ENABLE_BITCODE "NO"
    # 只编 arm64（真机）；模拟器构建要改成 x86_64;arm64 并换 sysroot。
    XCODE_ATTRIBUTE_ARCHS "arm64"
    XCODE_ATTRIBUTE_ONLY_ACTIVE_ARCH "NO"
    XCODE_ATTRIBUTE_SKIP_INSTALL "NO"
)

# 签名团队：外部给（-DZENGINE_IOS_DEVELOPMENT_TEAM=...）。没给时 Xcode 用
# Automatic signing，需要用户在工程里选一次团队。
if(ZENGINE_IOS_DEVELOPMENT_TEAM)
    set_target_properties(${TARGET_NAME} PROPERTIES
        XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${ZENGINE_IOS_DEVELOPMENT_TEAM}")
endif()

# ---------------------------------------------------------------------------
# 项目内容打进 bundle
#
# zbuild_ios.py 先把项目（含 Build/iOS/Bundles）放到 staging/zengine/，这里
# 把整棵按 Resources/... 的布局声明成 bundle resource —— Xcode 会原样拷进
# .app，IOSMain.mm 再从 [[NSBundle mainBundle] resourcePath]/zengine 读。
#
# 为什么不用 POST_BUILD 拷贝：那样拷进去的文件不进 Xcode 工程，Archive 时
# 容易漏（尤其是增量构建）。
# ---------------------------------------------------------------------------
set(ZENGINE_IOS_STAGING_DIR "${CMAKE_CURRENT_SOURCE_DIR}/staging"
    CACHE PATH "Directory holding the packaged project (staging/zengine) to embed in the app bundle")

if(EXISTS "${ZENGINE_IOS_STAGING_DIR}/zengine")
    file(GLOB_RECURSE _zengine_ios_project_files RELATIVE "${ZENGINE_IOS_STAGING_DIR}"
         "${ZENGINE_IOS_STAGING_DIR}/zengine/*")

    set(_zengine_ios_abs_files "")
    foreach(_rel ${_zengine_ios_project_files})
        if(IS_DIRECTORY "${ZENGINE_IOS_STAGING_DIR}/${_rel}")
            continue()
        endif()
        get_filename_component(_dir "${_rel}" DIRECTORY)
        set_source_files_properties("${ZENGINE_IOS_STAGING_DIR}/${_rel}" PROPERTIES
            MACOSX_PACKAGE_LOCATION "Resources/${_dir}")
        list(APPEND _zengine_ios_abs_files "${ZENGINE_IOS_STAGING_DIR}/${_rel}")
    endforeach()

    list(LENGTH _zengine_ios_abs_files _zengine_ios_file_count)
    if(_zengine_ios_file_count GREATER 0)
        target_sources(${TARGET_NAME} PRIVATE ${_zengine_ios_abs_files})
        message(STATUS "ZGameIOS: embedding ${_zengine_ios_file_count} project files from ${ZENGINE_IOS_STAGING_DIR}/zengine")
    endif()
    unset(_zengine_ios_abs_files)
    unset(_zengine_ios_file_count)
    unset(_zengine_ios_project_files)
else()
    message(WARNING "ZGameIOS: ${ZENGINE_IOS_STAGING_DIR}/zengine does not exist -- "
                    "the app will launch with no project. Run zbuild_ios.py first.")
endif()

# ZPlayer: 启动 + 每帧推进（内部再链 ZRuntime，ZRuntime 链 Metal / UIKit）。
target_link_libraries(${TARGET_NAME} PRIVATE ZPlayer)

message(STATUS "ZGameIOS: iOS app target configured (bundle id com.zengine.player)")
