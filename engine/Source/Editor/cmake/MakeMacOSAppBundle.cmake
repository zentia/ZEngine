# =============================================================================
# MakeMacOSAppBundle.cmake —— 给 macOS 的 ZEditor 补一个 .app bundle
# -----------------------------------------------------------------------------
# 为什么要这个：`z_embed_app_icon()`（engine/cmake/AppIcon.cmake）只做 Windows 的
# .rc —— 它把 resource/<Name>.ico 嵌进 exe，Explorer / 任务栏 / Alt-Tab 就都有
# 图标了。macOS 侧**没有对等物**：ZEditor 是裸 Mach-O，不是 bundle，于是
#
#   * Finder 里它是一个通用可执行文件图标；
#   * Dock / Launchpad / Spotlight 里也没有图标可显示（运行时可以用
#     `[NSApp setApplicationIconImage:]` 补 —— editor 和 launcher 都已经在补了，
#     但那只能管住"进程活着的时候"）。
#
# 由 Editor/CMakeLists.txt 用 POST_BUILD 调用：
#
#   cmake -DICON_SOURCE=<png> -DEXE_PATH=$<TARGET_FILE:ZEditor>
#         -DBUNDLE_NAME=ZEditor -DBUNDLE_VERSION=0.1.0
#         -P MakeMacOSAppBundle.cmake
#
# 设计取舍：**真身不动**。可执行文件仍然构建到 bin/<Config>/ZEditor，bundle 里的
# Contents/MacOS/ZEditor 只是一个相对 symlink（../../../ZEditor）。
#
#   1. `cd bin/Debug && ./ZEditor` 这条调试路径不能断 —— 它是本仓库所有真机验证
#      的入口，而且所有资产（asset/ resource/ ZEditor.ini）都跟它同目录。
#   2. 资产不进 bundle。ZEditor 只链接系统框架（otool -L 里没有 @rpath /
#      @executable_path 依赖），所以通过 symlink 启动时 dyld 不会因为
#      `@executable_path` 指向 bundle 内部而找不到东西。
#   3. bundle 只是"让系统认识这个 App"的壳：图标、App 名、bundle id。
# =============================================================================

if(NOT ICON_SOURCE)
    message(FATAL_ERROR "MakeMacOSAppBundle: -DICON_SOURCE=<image> is required")
endif()
if(NOT EXISTS "${ICON_SOURCE}")
    message(FATAL_ERROR "MakeMacOSAppBundle: ICON_SOURCE does not exist: ${ICON_SOURCE}")
endif()
if(NOT EXE_PATH)
    message(FATAL_ERROR "MakeMacOSAppBundle: -DEXE_PATH=<built executable> is required")
endif()
if(NOT BUNDLE_NAME)
    set(BUNDLE_NAME "ZEditor")
endif()
if(NOT BUNDLE_VERSION)
    set(BUNDLE_VERSION "0.1.0")
endif()

get_filename_component(EXE_DIR  "${EXE_PATH}" DIRECTORY)
get_filename_component(EXE_FILE "${EXE_PATH}" NAME)

set(BUNDLE_DIR "${EXE_DIR}/${BUNDLE_NAME}.app")
set(CONTENTS   "${BUNDLE_DIR}/Contents")
set(MACOS_DIR  "${CONTENTS}/MacOS")
set(RES_DIR    "${CONTENTS}/Resources")

# -----------------------------------------------------------------------------
# 1. PNG -> .icns
#
# `iconutil` 只吃 .iconset 目录，而且要一整套尺寸（缺一个就直接失败，不给 icns）。
# 源图是 512x512，1024 那一档是放大来的 —— 图标放大到 1024 在 Dock 里看不出来，
# 但缺了它 iconutil 就报错，所以照给。
# -----------------------------------------------------------------------------
find_program(SIPS_EXECUTABLE sips)
find_program(ICONUTIL_EXECUTABLE iconutil)

set(HAVE_ICNS FALSE)

if(NOT SIPS_EXECUTABLE OR NOT ICONUTIL_EXECUTABLE)
    message(WARNING "MakeMacOSAppBundle: sips / iconutil not found -- "
                    "${BUNDLE_NAME}.app will be built without an icon file.")
else()
    set(ICONSET "${BUNDLE_DIR}.iconset")
    file(REMOVE_RECURSE "${ICONSET}")
    file(MAKE_DIRECTORY "${ICONSET}")

    foreach(ENTRY IN ITEMS
            "icon_16x16.png:16"
            "icon_16x16@2x.png:32"
            "icon_32x32.png:32"
            "icon_32x32@2x.png:64"
            "icon_128x128.png:128"
            "icon_128x128@2x.png:256"
            "icon_256x256.png:256"
            "icon_256x256@2x.png:512"
            "icon_512x512.png:512"
            "icon_512x512@2x.png:1024")
        string(REPLACE ":" ";" PARTS "${ENTRY}")
        list(GET PARTS 0 NAME)
        list(GET PARTS 1 SIZE)
        execute_process(COMMAND "${SIPS_EXECUTABLE}" -z ${SIZE} ${SIZE} "${ICON_SOURCE}"
                                --out "${ICONSET}/${NAME}"
                        RESULT_VARIABLE RC
                        OUTPUT_QUIET
                        ERROR_QUIET)
        if(NOT RC EQUAL 0)
            message(WARNING "MakeMacOSAppBundle: sips failed for ${NAME} (${SIZE}px)")
        endif()
    endforeach()

    file(MAKE_DIRECTORY "${RES_DIR}")
    execute_process(COMMAND "${ICONUTIL_EXECUTABLE}" -c icns "${ICONSET}"
                            -o "${RES_DIR}/${BUNDLE_NAME}.icns"
                    RESULT_VARIABLE RC
                    OUTPUT_VARIABLE ICONUTIL_OUT
                    ERROR_VARIABLE  ICONUTIL_ERR)
    file(REMOVE_RECURSE "${ICONSET}")

    if(RC EQUAL 0 AND EXISTS "${RES_DIR}/${BUNDLE_NAME}.icns")
        set(HAVE_ICNS TRUE)
    else()
        message(WARNING "MakeMacOSAppBundle: iconutil failed (${RC}): ${ICONUTIL_ERR}${ICONUTIL_OUT}")
    endif()
endif()

# -----------------------------------------------------------------------------
# 2. Contents/MacOS/<name> -> ../../../<name>
#
# 相对 symlink：整个 bin/ 目录被拷走 / 改名 / 换机器都不会失效，绝对 symlink 会。
# -----------------------------------------------------------------------------
file(MAKE_DIRECTORY "${MACOS_DIR}")
set(LINK "${MACOS_DIR}/${BUNDLE_NAME}")
# file(REMOVE) 对 symlink 删的是链接本身，不会碰目标 —— 但 EXISTS 对"指向不存在
# 目标的悬空链接"会返回 false，所以两个条件都要判。
if(EXISTS "${LINK}" OR IS_SYMLINK "${LINK}")
    file(REMOVE "${LINK}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -E create_symlink "../../../${EXE_FILE}" "${LINK}"
                RESULT_VARIABLE RC)
if(NOT RC EQUAL 0)
    message(FATAL_ERROR "MakeMacOSAppBundle: cannot create symlink at ${LINK}")
endif()

# -----------------------------------------------------------------------------
# 3. Info.plist
#
# CFBundleIconFile 只在 icns 真的生成了才写 —— 指向一个不存在的文件，系统不会
# 报错，只是静默退回通用图标，那正好是我们最想避免的"静默失败"。
# -----------------------------------------------------------------------------
set(ICON_KEYS "")
if(HAVE_ICNS)
    set(ICON_KEYS "    <key>CFBundleIconFile</key>
    <string>${BUNDLE_NAME}</string>
")
endif()

file(WRITE "${CONTENTS}/Info.plist"
"<?xml version=\"1.0\" encoding=\"UTF-8\"?>
<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">
<plist version=\"1.0\">
<dict>
    <key>CFBundleName</key>
    <string>${BUNDLE_NAME}</string>
    <key>CFBundleDisplayName</key>
    <string>${BUNDLE_NAME}</string>
    <key>CFBundleIdentifier</key>
    <string>com.zengine.${BUNDLE_NAME}</string>
    <key>CFBundleExecutable</key>
    <string>${BUNDLE_NAME}</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>CFBundleShortVersionString</key>
    <string>${BUNDLE_VERSION}</string>
    <key>CFBundleVersion</key>
    <string>${BUNDLE_VERSION}</string>
${ICON_KEYS}    <key>NSHighResolutionCapable</key>
    <true/>
    <key>NSSupportsAutomaticTermination</key>
    <false/>
</dict>
</plist>
")

message(STATUS "MakeMacOSAppBundle: ${BUNDLE_DIR} (icon: ${HAVE_ICNS})")
