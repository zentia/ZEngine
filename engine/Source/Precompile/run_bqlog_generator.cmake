if(DEFINED ENV{BQLOG_TOOL_PATH} AND NOT "$ENV{BQLOG_TOOL_PATH}" STREQUAL "" AND EXISTS "$ENV{BQLOG_TOOL_PATH}")
  message(STATUS "[Precompile] BqLog generator found: $ENV{BQLOG_TOOL_PATH}")
  execute_process(
    COMMAND "$ENV{BQLOG_TOOL_PATH}" engine_log "$ENV{BQLOG_CFG_PATH}" "$ENV{BQLOG_OUT_DIR}"
    RESULT_VARIABLE BQLOG_GEN_RESULT
  )
  if(NOT BQLOG_GEN_RESULT EQUAL 0)
    message(WARNING "[Precompile] BqLog generator returned non-zero exit code: ${BQLOG_GEN_RESULT}")
  endif()
else()
  message(STATUS "[Precompile] BqLog generator tool not found; skipping category generation.")
endif()



