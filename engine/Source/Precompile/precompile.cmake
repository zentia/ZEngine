### CONFIGURE-TIME CODE GENERATION ==============================================================
# Generate engine_log.h at configure time so it exists before the build starts
# This ensures the file exists even after a clean configure (git clean -xfd)
set(BQLOG_CONFIG_FILE "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/bq_log_category_config.ini")
set(BQLOG_OUTPUT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Source/Runtime/Core/log/generated")
set(BQLOG_CLASS_NAME "engine_log")

if(EXISTS "${BQLOG_CONFIG_FILE}")
  message(STATUS "[Precompile] Generating BqLog categories at configure time...")
  execute_process(
    COMMAND ${CMAKE_COMMAND}
    -DCONFIG_FILE=${BQLOG_CONFIG_FILE}
    -DOUTPUT_DIR=${BQLOG_OUTPUT_DIR}
    -DCLASS_NAME=${BQLOG_CLASS_NAME}
    -P "${CMAKE_CURRENT_LIST_DIR}/generate_engine_log.cmake"
    RESULT_VARIABLE BQLOG_GEN_RESULT
  )
  if(NOT BQLOG_GEN_RESULT EQUAL 0)
    message(WARNING "[Precompile] BqLog generator returned non-zero exit code: ${BQLOG_GEN_RESULT}")
  endif()
else()
  message(WARNING "[Precompile] BqLog config file not found: ${BQLOG_CONFIG_FILE}")
endif()

### BUILDING ====================================================================================
set(PRECOMPILE_TARGET "ZPreCompile")

# Create a stamp file to track when precompile last ran successfully
# This file serves as the OUTPUT so CMake can properly check if regeneration is needed
set(PRECOMPILE_STAMP ${CMAKE_BINARY_DIR}/.precompile_stamp)

# Use add_custom_command with OUTPUT to enable proper dependency tracking
# This way CMake will only run the command if the stamp file is missing or dependencies changed
add_custom_command(
  OUTPUT ${PRECOMPILE_STAMP}

  # COMMAND # (DEBUG: DON'T USE )
  #     this will make configure_file() is called on each compile
  #   ${CMAKE_COMMAND} -E touch ${PRECOMPILE_PARAM_IN_PATH}a

  # If more than one COMMAND is specified they will be executed in order...
  COMMAND
  ${CMAKE_COMMAND} -E echo "************************************************************* "
  COMMAND
  ${CMAKE_COMMAND} -E echo "**** [Precompile] BEGIN "
  COMMAND
  ${CMAKE_COMMAND} -E echo "************************************************************* "

  COMMAND
  ${CMAKE_COMMAND} -E echo "[Precompile] Regenerating BqLog categories..."
  COMMAND
  ${CMAKE_COMMAND}
  -DCONFIG_FILE=${BQLOG_CONFIG_FILE}
  -DOUTPUT_DIR=${BQLOG_OUTPUT_DIR}
  -DCLASS_NAME=${BQLOG_CLASS_NAME}
  -P "${CMAKE_CURRENT_LIST_DIR}/generate_engine_log.cmake"

  ### BUILDING ====================================================================================
  COMMAND
  ${CMAKE_COMMAND} -E echo "+++ Precompile finished +++"
  COMMAND
  ${CMAKE_COMMAND} -E touch ${PRECOMPILE_STAMP}
)

# Create a target that depends on the stamp file
# This target will only build if the stamp file is out of date
add_custom_target(${PRECOMPILE_TARGET} ALL
  DEPENDS ${PRECOMPILE_STAMP}
  COMMENT "Meta parser code generation target"
)
