# =====================================================================
# ZHeaderTool integration (reflection code generation, UHT-style).
# ---------------------------------------------------------------------
#   zengine_header_tool(
#       MODULE        ZRuntime                       # name used for dirs / target
#       TARGETS       ZRuntime ZRuntimeShared        # targets compiling the module
#       INCLUDE_ROOT  ${ENGINE_ROOT_DIR}/Source      # "Runtime/..." includes are relative to this
#       HEADERS       ${HEADER_FILES}                # every header of the module
#       INCLUDE_CONSUMERS ZEnginePCH)                # targets that include reflected headers
#                                                    # without linking TARGETS (e.g. the shared PCH)
#
# Configure time: `ZHeaderTool.py list` decides which headers carry ZCLASS
# and therefore which <Name>.gen.cpp files are added to TARGETS.
#
# Build time: one custom command per module re-scans HEADERS, writes
# <Name>.generated.h (-> ${gen}/Inc, PUBLIC include dir) and <Name>.gen.cpp
# (-> ${gen}/Src) only when content changed (BYPRODUCTS => Ninja restat), and
# touches a stamp. When a header gains / loses ZCLASS, the tool rewrites the
# manifest (a CMAKE_CONFIGURE_DEPENDS file) and fails once; the next build
# re-configures and picks up the new .gen.cpp list.
#
# Python 3.9+ (stdlib only) is required on the build host. It is a host-side
# tool, so cross builds (Android / OHOS / iOS / Emscripten) need nothing extra.
# =====================================================================

set(ZHT_TOOL_DIR "${CMAKE_CURRENT_LIST_DIR}/../engine/tools/ZHeaderTool")
get_filename_component(ZHT_TOOL_DIR "${ZHT_TOOL_DIR}" ABSOLUTE)
set(ZHT_SCRIPT "${ZHT_TOOL_DIR}/ZHeaderTool.py")

function(zengine_header_tool)
    cmake_parse_arguments(ZHT "" "MODULE;INCLUDE_ROOT;SCRIPT_DOMAIN" "TARGETS;HEADERS;INCLUDE_CONSUMERS" ${ARGN})
    if(NOT ZHT_MODULE OR NOT ZHT_TARGETS OR NOT ZHT_INCLUDE_ROOT)
        message(FATAL_ERROR "zengine_header_tool: MODULE, TARGETS and INCLUDE_ROOT are required")
    endif()
    # Which script VM the module's bindings are visible from (runtime|editor).
    if(NOT ZHT_SCRIPT_DOMAIN)
        set(ZHT_SCRIPT_DOMAIN runtime)
    endif()

    if(NOT Python3_EXECUTABLE)
        find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
    endif()

    set(gen_dir "${CMAKE_BINARY_DIR}/Generated/ZHT/${ZHT_MODULE}")
    set(inc_dir "${gen_dir}/Inc")
    set(src_dir "${gen_dir}/Src")
    set(headers_file "${gen_dir}/headers.txt")
    set(manifest "${gen_dir}/annotated.txt")
    set(stamp "${gen_dir}/zht.stamp")
    file(MAKE_DIRECTORY "${inc_dir}" "${src_dir}")

    # The header list can exceed Windows' command-line limit; pass it via a
    # file. file(CONFIGURE) only rewrites it when the content changed.
    list(REMOVE_DUPLICATES ZHT_HEADERS)
    list(SORT ZHT_HEADERS)
    list(JOIN ZHT_HEADERS "\n" headers_content)
    file(CONFIGURE OUTPUT "${headers_file}" CONTENT "${headers_content}\n" @ONLY)

    execute_process(
        COMMAND "${Python3_EXECUTABLE}" "${ZHT_SCRIPT}" list
                --headers-file "${headers_file}" --output "${manifest}"
        RESULT_VARIABLE zht_result
        ERROR_VARIABLE zht_error)
    if(NOT zht_result EQUAL 0)
        message(FATAL_ERROR "ZHeaderTool list failed for ${ZHT_MODULE}:\n${zht_error}")
    endif()
    # Re-configure whenever the build-time tool rewrites the manifest.
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${manifest}")

    file(STRINGS "${manifest}" annotated)
    set(gen_outputs)
    set(gen_sources)
    foreach(header IN LISTS annotated)
        get_filename_component(base "${header}" NAME_WE)
        list(APPEND gen_outputs "${inc_dir}/${base}.generated.h" "${src_dir}/${base}.gen.cpp")
        list(APPEND gen_sources "${src_dir}/${base}.gen.cpp")
    endforeach()

    file(GLOB zht_tool_sources CONFIGURE_DEPENDS "${ZHT_TOOL_DIR}/*.py" "${ZHT_TOOL_DIR}/zht/*.py")

    add_custom_command(
        OUTPUT "${stamp}"
        BYPRODUCTS ${gen_outputs}
        COMMAND "${Python3_EXECUTABLE}" "${ZHT_SCRIPT}" generate
                --module "${ZHT_MODULE}"
                --script-domain "${ZHT_SCRIPT_DOMAIN}"
                --headers-file "${headers_file}"
                --include-root "${ZHT_INCLUDE_ROOT}"
                --out-inc "${inc_dir}"
                --out-src "${src_dir}"
                --manifest "${manifest}"
                --stamp "${stamp}"
                --dump-ir "${gen_dir}/ir.json"
        DEPENDS ${ZHT_HEADERS} "${headers_file}" ${zht_tool_sources}
        COMMENT "ZHeaderTool: generating reflection code for ${ZHT_MODULE}"
        VERBATIM)

    set(zht_target "${ZHT_MODULE}_ZHeaderTool")
    add_custom_target(${zht_target} DEPENDS "${stamp}")
    set_target_properties(${zht_target} PROPERTIES FOLDER "Engine/CodeGen")

    if(gen_sources)
        set_source_files_properties(${gen_sources} PROPERTIES GENERATED TRUE)
        source_group("Generated" FILES ${gen_sources})
    endif()

    foreach(target IN LISTS ZHT_TARGETS)
        add_dependencies(${target} ${zht_target})
        # Per-target opt-out: a target may skip individual <Name>.gen.cpp files
        # by listing their base names in <Target>_ZHT_EXCLUDE_BASENAMES. Used by
        # the slim iOS shared runtime, which drops a subsystem's .cpp but must
        # also drop the generated Transfer()/registrar TU that calls into it --
        # otherwise that TU is the one holding the now-missing symbols.
        set(_zht_target_sources ${gen_sources})
        if(${target}_ZHT_EXCLUDE_BASENAMES)
            set(_zht_target_sources)
            foreach(src IN LISTS gen_sources)
                get_filename_component(_zht_base "${src}" NAME_WE)
                if(NOT _zht_base IN_LIST ${target}_ZHT_EXCLUDE_BASENAMES)
                    list(APPEND _zht_target_sources "${src}")
                endif()
            endforeach()
            unset(_zht_base)
        endif()
        target_sources(${target} PRIVATE ${_zht_target_sources})
        target_include_directories(${target} PUBLIC "$<BUILD_INTERFACE:${inc_dir}>")
    endforeach()

    # Targets that compile reflected headers but do not link TARGETS still need
    # the generated include dir and must not start before the tool has run.
    foreach(target IN LISTS ZHT_INCLUDE_CONSUMERS)
        if(TARGET ${target})
            add_dependencies(${target} ${zht_target})
            target_include_directories(${target} PUBLIC "$<BUILD_INTERFACE:${inc_dir}>")
        endif()
    endforeach()

    list(LENGTH annotated annotated_count)
    message(STATUS "ZHeaderTool: ${ZHT_MODULE} has ${annotated_count} reflected header(s)")
endfunction()
