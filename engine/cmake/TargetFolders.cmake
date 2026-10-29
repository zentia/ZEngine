# TargetFolders.cmake
# -----------------------------------------------------------------------------
# Visual Studio (and Xcode) only place a target inside a solution folder when
# BOTH of these hold:
#   1. the global property USE_FOLDERS is ON, and
#   2. the target actually has a FOLDER property.
#
# The engine sets FOLDER by hand on most targets (ThirdParty/*, Engine, Engine/Tests,
# Tools, ...), but USE_FOLDERS was never turned on, so the generated solution was
# completely flat -- every one of the ~112 projects sat at the solution root and
# the hand-written FOLDER values were silently ignored.
#
# Including this file turns USE_FOLDERS on. Calling z_assign_default_target_folders()
# -- AFTER every add_subdirectory(), so that all targets exist -- fills the gaps:
# any target still without a FOLDER gets one derived from its source directory.
# Targets that already set FOLDER are left alone, so the hand-written grouping wins.
# -----------------------------------------------------------------------------

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# _z_folder_from_source_dir(<out-var> <source-dir>)
#
# Maps a target's source directory to a solution folder path, reusing the same
# top-level names the hand-written FOLDER assignments already use:
#   engine/3rdparty/...        -> ThirdParty/...
#   engine/Source/Modules/...  -> Modules/...
#   engine/Source/...          -> Engine/...
#   engine/tools/...           -> Tools/...
#   engine/Libraries/...       -> Libraries/...
#   tests/...                  -> Tests/...
# Anything outside the source tree (FetchContent _deps, ... ) -> External/<dir name>.
function(_z_folder_from_source_dir out_var source_dir)
    file(RELATIVE_PATH _rel "${CMAKE_SOURCE_DIR}" "${source_dir}")
    if(NOT _rel OR _rel STREQUAL ".")
        set(${out_var} "" PARENT_SCOPE)
        return()
    endif()

    # Anything that lives in the build tree (FetchContent _deps, generated
    # subprojects, ...) is not part of the engine layout -- park it under
    # External/ instead of leaking paths like "build/_deps/<name>-src".
    file(RELATIVE_PATH _rel_bin "${CMAKE_BINARY_DIR}" "${source_dir}")
    if(_rel MATCHES "^\\.\\." OR NOT _rel_bin MATCHES "^\\.\\.")
        get_filename_component(_name "${source_dir}" NAME)
        set(${out_var} "External/${_name}" PARENT_SCOPE)
        return()
    endif()

    set(_folder "")
    if(_rel MATCHES "^engine/3rdparty/(.*)$")
        set(_folder "ThirdParty/${CMAKE_MATCH_1}")
    elseif(_rel MATCHES "^engine/Source/Modules/(.*)$")
        set(_folder "Modules/${CMAKE_MATCH_1}")
    elseif(_rel MATCHES "^engine/Source/(.*)$")
        set(_folder "Engine/${CMAKE_MATCH_1}")
    elseif(_rel MATCHES "^engine/tools/(.*)$")
        set(_folder "Tools/${CMAKE_MATCH_1}")
    elseif(_rel MATCHES "^engine/Libraries/(.*)$")
        set(_folder "Libraries/${CMAKE_MATCH_1}")
    elseif(_rel MATCHES "^engine/(.*)$")
        set(_folder "Engine/${CMAKE_MATCH_1}")
    elseif(_rel STREQUAL "engine")
        set(_folder "Engine")
    elseif(_rel MATCHES "^tests/(.*)$")
        set(_folder "Tests/${CMAKE_MATCH_1}")
    else()
        set(_folder "${_rel}")
    endif()

    # Strip a trailing separator: "Engine/Runtime/" -> "Engine/Runtime".
    string(REGEX REPLACE "/+$" "" _folder "${_folder}")
    set(${out_var} "${_folder}" PARENT_SCOPE)
endfunction()

# z_assign_default_target_folders()
#
# Walk every directory that has been added and give each target that has no FOLDER
# one derived from where it is defined. Safe to call more than once; it never
# overwrites an explicit FOLDER.
function(z_assign_default_target_folders)
    set(_queue "${CMAKE_SOURCE_DIR}")
    set(_assigned 0)
    set(_untouched 0)
    set(_report "")

    while(_queue)
        list(POP_FRONT _queue _dir)

        get_property(_subdirs DIRECTORY "${_dir}" PROPERTY SUBDIRECTORIES)
        foreach(_sub IN LISTS _subdirs)
            list(APPEND _queue "${_sub}")
        endforeach()

        # BUILDSYSTEM_TARGETS excludes ALIAS/IMPORTED targets, which have no
        # project file and therefore nothing to group.
        get_property(_targets DIRECTORY "${_dir}" PROPERTY BUILDSYSTEM_TARGETS)
        foreach(_target IN LISTS _targets)
            get_target_property(_existing "${_target}" FOLDER)
            if(_existing)
                math(EXPR _untouched "${_untouched}+1")
                continue()
            endif()
            get_target_property(_src "${_target}" SOURCE_DIR)
            if(NOT _src)
                continue()
            endif()
            _z_folder_from_source_dir(_folder "${_src}")
            if(NOT _folder)
                continue()
            endif()
            set_target_properties(${_target} PROPERTIES FOLDER "${_folder}")
            string(APPEND _report "\n  ${_target} -> ${_folder}")
            math(EXPR _assigned "${_assigned}+1")
        endforeach()
    endwhile()

    message(STATUS "Solution folders: ${_assigned} target(s) grouped by source directory, ${_untouched} already had an explicit FOLDER${_report}")
endfunction()
