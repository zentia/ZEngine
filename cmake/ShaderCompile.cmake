function(compile_shader SHADERS TARGET_NAME SHADER_INCLUDE_FOLDER GENERATED_DIR GLSLANG_BIN)

    set(working_dir "${CMAKE_CURRENT_SOURCE_DIR}")

    set(ALL_GENERATED_SPV_FILES "")
    set(ALL_GENERATED_CPP_FILES "")

    # 被 #include 的头文件也必须是依赖。glslang 的 DEPENDS 只列了 shader 源文件本身，
    # 所以改 nanite_common.h / vt_common.h 这类共享头**不会**重新生成任何 SPIR-V ——
    # 二进制里的 shader 悄悄停留在旧契约上，而 C++ 侧已经按新契约走了（2026-10-27 做
    # VT 突变测试时发现：改了头、重建、测试照绿，因为 SPIR-V 根本没重编）。
    # 这里粗粒度处理：include 目录下任何文件变化都重编全部 shader（正确优先，shader 很小）。
    file(GLOB_RECURSE SHADER_INCLUDE_DEPENDENCIES CONFIGURE_DEPENDS "${SHADER_INCLUDE_FOLDER}/*")

    # 公式层的**唯一权威**在子模块里（engine/Libraries/*/shader/）：
    #   GPUParticle → shader/particle_sim.h
    #   GPUSpine    → shader/spine_skinning.h
    # engine/shader/include/ 下那两个只是转发头。所以子模块里的公式也必须进依赖，
    # 否则改了公式不会重编 SPIR-V —— 就是上面那段注释说的坑（改了头、重建、
    # 测试照绿，因为二进制里的 shader 还停在旧契约上）。
    file(GLOB_RECURSE LIB_SHADER_INCLUDE_DEPENDENCIES CONFIGURE_DEPENDS
         "${Z_ROOT_DIR}/engine/Libraries/*/shader/*")
    list(APPEND SHADER_INCLUDE_DEPENDENCIES ${LIB_SHADER_INCLUDE_DEPENDENCIES})

    if(UNIX AND GLSLANG_BIN AND NOT GLSLANG_BIN MATCHES "^/usr/")
        execute_process(COMMAND chmod a+x ${GLSLANG_BIN})
    endif()

    foreach(SHADER ${SHADERS})
    # Prepare a header name and a global variable for this shader
        get_filename_component(SHADER_NAME ${SHADER} NAME)
        string(REPLACE "." "_" HEADER_NAME ${SHADER_NAME})
        string(TOUPPER ${HEADER_NAME} GLOBAL_SHADER_VAR)

        set(SPV_FILE "${CMAKE_CURRENT_SOURCE_DIR}/${GENERATED_DIR}/spv/${SHADER_NAME}.spv")
        set(CPP_FILE "${CMAKE_CURRENT_SOURCE_DIR}/${GENERATED_DIR}/cpp/${HEADER_NAME}.h")

        add_custom_command(
            OUTPUT ${SPV_FILE}
            COMMAND ${GLSLANG_BIN} -I${SHADER_INCLUDE_FOLDER} -V100 -o ${SPV_FILE} ${SHADER}
            DEPENDS ${SHADER} ${SHADER_INCLUDE_DEPENDENCIES}
            WORKING_DIRECTORY "${working_dir}")

        list(APPEND ALL_GENERATED_SPV_FILES ${SPV_FILE})

        add_custom_command(
            OUTPUT ${CPP_FILE}
            COMMAND ${CMAKE_COMMAND} -DPATH=${SPV_FILE} -DHEADER="${CPP_FILE}" 
                -DGLOBAL="${GLOBAL_SHADER_VAR}" -P "${Z_ROOT_DIR}/cmake/GenerateShaderCPPFile.cmake"
            DEPENDS ${SPV_FILE}
            WORKING_DIRECTORY "${working_dir}")

        list(APPEND ALL_GENERATED_CPP_FILES ${CPP_FILE})

    endforeach()

    add_custom_target(${TARGET_NAME}
        DEPENDS ${ALL_GENERATED_SPV_FILES} ${ALL_GENERATED_CPP_FILES} SOURCES ${SHADERS})

endfunction()