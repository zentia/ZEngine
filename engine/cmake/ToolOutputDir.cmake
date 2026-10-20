# ToolOutputDir.cmake
# =============================================================================
# z_set_tool_output_dir(<target> ...)
#
# Sends a standalone GUI tool's executable to one shared, predictable place
# instead of wherever the build tree happens to be:
#
#     <repo>/bin/Tools/<Config>/<name>.exe
#
# A tool that ships more than its .exe passes SUBDIR and gets its own level, so
# its extra files cannot be mistaken for another tool's:
#
#     <repo>/bin/Tools/<Config>/<subdir>/<name>.exe
#     <repo>/bin/Tools/<Config>/<subdir>/profiles/...
#
# ZEnvironmentCollector is the one that needs it: ZEnvironmentCollector.exe
# plus ZDllLoadProbe.exe, profiles/ and two LICENSE files, all in
# bin/Tools/<Config>/ZEnvironmentCollector/ -- which is also its
# distribution unit (zip the folder).
#
# Before this, the four tools landed in three different shapes, which is why
# "where did my exe go" had to be answered per project:
#
#   TexPreview / LuaMemPreview   bin[/<subdir>]/<Config>/   (full tree only)
#   ZEnvironmentCollector        bin[/<subdir>]/<Config>/   (full tree)
#                                <build>/bin/               (standalone, no
#                                                            <Config> level)
#   ClientProjBranches           <build>/<Config>/          (no setting at all)
#
# Why a dedicated Tools/ level instead of bin/<Config>/ next to ZEditor:
#   * the tools have nothing to do with the engine's runtime layout -- no
#     configs, no assets, no ZRuntime DLL -- so sharing the directory only
#     makes the engine output harder to read;
#   * ZEnvironmentCollector ships to machines with no VC redist and is built
#     /MT, so its directory is also its distribution unit (profiles/ and the
#     LICENSE files are copied next to the exe). Keeping it separate means
#     "zip this folder" is a correct packaging step.
#
# Repo root resolution, in order:
#   1. Z_ROOT_DIR (set by the root CMakeLists.txt in a full-tree configure).
#   2. Walk up from CMAKE_CURRENT_SOURCE_DIR looking for engine/CMakeLists.txt,
#      so a standalone `cmake -S tools/<Tool>` configure finds the root without
#      any hardcoded path.
#   3. If BINARY_ROOT_DIR is already defined (full tree) the tools root is
#      "${BINARY_ROOT_DIR}/Tools" -- i.e. it follows ZENGINE_OUTPUT_SUBDIR too,
#      so a cross-compile layout such as bin/win64/x64/ becomes
#      bin/win64/x64/Tools/<Config>/ and the tools stay next to their engine.
#
# The function is a no-op wrapper over z_set_config_output_dirs(); it exists
# only so the "tools go to bin/Tools/<Config>" decision is made in one place.
#
# Build trees are a separate convention (documented, not enforced here):
#     cmake -S <tool dir> -B build/tools/<ToolName>
# =============================================================================

include("${CMAKE_CURRENT_LIST_DIR}/ConfigOutputDirs.cmake")

function(z_set_tool_output_dir)
    # Targets are positional; SUBDIR is an optional keyword that puts them one
    # level deeper, at <tools root>/<Config>/<subdir>. A tool that ships more
    # than its .exe -- a helper executable, a profiles/ directory, LICENSE
    # files -- should pass it, so those files cannot be mistaken for another
    # tool's in the shared bin/Tools/<Config>/.
    cmake_parse_arguments(_tool "" "SUBDIR" "" ${ARGN})
    if(NOT _tool_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "z_set_tool_output_dir requires at least one target")
    endif()

    # ---- locate the repo root --------------------------------------------
    set(_tool_root "${Z_ROOT_DIR}")
    if(NOT _tool_root)
        set(_tool_probe "${CMAKE_CURRENT_SOURCE_DIR}")
        while(TRUE)
            if(EXISTS "${_tool_probe}/engine/CMakeLists.txt")
                set(_tool_root "${_tool_probe}")
                break()
            endif()
            get_filename_component(_tool_parent "${_tool_probe}" DIRECTORY)
            if(NOT _tool_parent OR "${_tool_parent}" STREQUAL "${_tool_probe}")
                break()
            endif()
            set(_tool_probe "${_tool_parent}")
        endwhile()
    endif()

    # ---- tools output root ------------------------------------------------
    if(NOT _tool_root)
        # Last resort: inside the build tree rather than nowhere.
        set(_tool_out "${CMAKE_BINARY_DIR}/bin/Tools")
    elseif(BINARY_ROOT_DIR)
        # Full-tree configure: sit next to whatever the engine emits. Trailing
        # slashes removed so the path never reads bin//Tools.
        string(REGEX REPLACE "/+$" "" _tool_bin_root "${BINARY_ROOT_DIR}")
        set(_tool_out "${_tool_bin_root}/Tools")
    else()
        set(_tool_out "${_tool_root}/bin/Tools")
    endif()

    set(_tool_suffix "")
    if(_tool_SUBDIR)
        set(_tool_suffix "/${_tool_SUBDIR}")
    endif()

    foreach(_tool_target IN LISTS _tool_UNPARSED_ARGUMENTS)
        if(NOT TARGET ${_tool_target})
            message(FATAL_ERROR "z_set_tool_output_dir: '${_tool_target}' is not a target")
        endif()
        z_set_config_output_dirs(${_tool_target} "${_tool_out}" RUNTIME "${_tool_SUBDIR}")
        message(STATUS "[ToolOutputDir] ${_tool_target} -> ${_tool_out}/<Config>${_tool_suffix}")
    endforeach()
endfunction()
