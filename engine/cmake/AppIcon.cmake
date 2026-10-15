# AppIcon.cmake
# =============================================================================
# z_embed_app_icon(<target> [NAME <name>] [ICON_DIR <dir>])
#
# Embeds an application icon into a Windows executable. One call replaces the
# three things every ZEngine GUI tool used to do by hand:
#
#   1. hand-writing  resource/<Name>.rc  (`IDI_ICON1 ICON "<Name>.ico"`)
#   2. `if(MSVC) list(APPEND SOURCES .../resource/<Name>.rc)` in its CMakeLists
#   3. `#ifndef IDI_ICON1 / #define IDI_ICON1 101` in its main.cpp
#
# Arguments:
#   target   - executable target to attach the resource to
#   NAME     - icon base name. Defaults to the target's OUTPUT_NAME, else the
#              target name. The file looked for is <ICON_DIR>/<NAME>.ico.
#   ICON_DIR - where the .ico lives. Defaults to <current source dir>/resource.
#
# Behaviour:
#   * If <ICON_DIR>/<NAME>.rc exists it is used as-is (escape hatch for a tool
#     that genuinely needs extra resources -- nothing currently does).
#   * Otherwise a <NAME>.rc is generated from AppIcon.rc.in into the target's
#     binary dir, next to a copy of the .ico -- rc.exe resolves the .ico
#     relative to the .rc, so the two have to sit together.
#   * IDI_ICON1=101 is added to the target's compile definitions, so no source
#     file has to #define it.
#   * No-op (with a STATUS message) off Windows, when the RC language is not
#     enabled, or when the .ico has not been generated yet. So the call can be
#     committed before the icon exists.
#
# The .rc only puts the icon in the *executable* (Explorer, Properties). The
# window's title bar, Alt-Tab entry and taskbar button come from the window
# class, which is a separate step:
#   * ZSlate tools: FStandaloneAppDesc::LargeIconResId / SmallIconResId = IDI_ICON1
#   * raw Win32   : WNDCLASSEXW::hIcon / hIconSm (WNDCLASSEX, not WNDCLASS --
#                   the plain class has no hIconSm, so the taskbar stays default)
# =============================================================================

set(_Z_APP_ICON_TEMPLATE "${CMAKE_CURRENT_LIST_DIR}/AppIcon.rc.in")

function(z_embed_app_icon TARGET)
    if(NOT TARGET ${TARGET})
        message(FATAL_ERROR "z_embed_app_icon: '${TARGET}' is not a target")
    endif()

    if(NOT WIN32)
        message(STATUS "[AppIcon] ${TARGET}: not Windows; app icon skipped")
        return()
    endif()

    # A .rc can only be compiled when CMake enabled the RC language (MSVC's
    # rc.exe, or MinGW's windres). Adding one otherwise is a hard configure
    # error, so bail out instead.
    get_property(_icon_enabled_languages GLOBAL PROPERTY ENABLED_LANGUAGES)
    list(FIND _icon_enabled_languages "RC" _icon_rc_index)
    if(_icon_rc_index EQUAL -1)
        message(STATUS "[AppIcon] ${TARGET}: RC language not enabled; app icon skipped")
        return()
    endif()

    cmake_parse_arguments(ICON "" "NAME;ICON_DIR" "" ${ARGN})
    if(ICON_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "z_embed_app_icon: unknown arguments: ${ICON_UNPARSED_ARGUMENTS}")
    endif()

    # ---- NAME ------------------------------------------------------------
    set(_icon_name "${ICON_NAME}")
    if(NOT _icon_name)
        get_target_property(_icon_name ${TARGET} OUTPUT_NAME)
        if(NOT _icon_name)
            set(_icon_name "${TARGET}")
        endif()
    endif()

    # ---- ICON_DIR --------------------------------------------------------
    set(_icon_dir "${ICON_ICON_DIR}")
    if(NOT _icon_dir)
        set(_icon_dir "${CMAKE_CURRENT_SOURCE_DIR}/resource")
    endif()

    set(_icon_rc "${_icon_dir}/${_icon_name}.rc")
    set(_icon_ico "${_icon_dir}/${_icon_name}.ico")

    if(NOT EXISTS "${_icon_rc}")
        if(NOT EXISTS "${_icon_ico}")
            message(STATUS "[AppIcon] ${TARGET}: ${_icon_ico} not found; no icon embedded. "
                           "Generate it with engine/resource/gen_icon.ps1")
            return()
        endif()

        # Generated + copied together into the same directory: rc.exe resolves
        # the .ico relative to the .rc it is compiling.
        set(_icon_gen_dir "${CMAKE_CURRENT_BINARY_DIR}/app_icon")
        set(_icon_rc "${_icon_gen_dir}/${_icon_name}.rc")
        configure_file("${_icon_ico}" "${_icon_gen_dir}/${_icon_name}.ico" COPYONLY)
        # Substituted into AppIcon.rc.in (@ONLY, so only our own @VAR@ tokens).
        set(APP_ICON_NAME "${_icon_name}")
        configure_file("${_Z_APP_ICON_TEMPLATE}" "${_icon_rc}" @ONLY)
        unset(APP_ICON_NAME)
        message(STATUS "[AppIcon] ${TARGET}: generated ${_icon_rc} from ${_icon_ico}")
    endif()

    target_sources(${TARGET} PRIVATE "${_icon_rc}")
    target_compile_definitions(${TARGET} PRIVATE IDI_ICON1=101)
    # Re-run rc.exe when the artwork changes but the generated .rc does not.
    set_source_files_properties("${_icon_rc}" PROPERTIES OBJECT_DEPENDS "${_icon_ico}")

    message(STATUS "[AppIcon] ${TARGET}: embedded ${_icon_name}.ico as IDI_ICON1=101")
endfunction()
