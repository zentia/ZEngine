# Compile engine/EditorScripts with tsc. Invoked by the ZEditorScripts target:
#   cmake -DTSC=<tsc> -DPROJECT_DIR=<engine/EditorScripts> -DOUT_DIR=<bin/cfg/EditorScripts> -P BuildEditorScripts.cmake
#
# Never fails the build: the editor treats the script layer as optional, and
# tsc emits JavaScript even when type-checking reports errors.

if(NOT TSC)
    message(STATUS "EditorScripts: tsc not available, skipping")
    return()
endif()

file(MAKE_DIRECTORY "${OUT_DIR}")
execute_process(
    COMMAND "${TSC}" -p "${PROJECT_DIR}/tsconfig.json" --outDir "${OUT_DIR}"
    WORKING_DIRECTORY "${PROJECT_DIR}"
    RESULT_VARIABLE tsc_result
    OUTPUT_VARIABLE tsc_output
    ERROR_VARIABLE tsc_error)

if(tsc_result EQUAL 0)
    message(STATUS "EditorScripts: compiled -> ${OUT_DIR}")
else()
    # Defang tsc's diagnostics before echoing them.
    #
    # tsc reports in the canonical MSVC format -- `src/Foo.ts(9,5): error
    # TS2584: ...` -- which MSBuild's output parser picks up as a REAL build
    # error. It then fails this custom-build step (MSB8066, exit -1) and, since
    # ZEditor depends on ZEditorScripts, the whole editor fails to link. That
    # defeated the "never fails the build" contract above in the worst possible
    # way: the very first build of a clean checkout cannot produce an editor, so
    # the editor can never run, so it can never generate typings/*.d.ts, so tsc
    # can never type-check. A deadlock.
    #
    # Rewriting the `: error TS` / `: warning TS` infix breaks the pattern
    # MSBuild matches on while keeping the message readable for a human.
    string(REPLACE ": error TS" ": [tsc] TS-error " tsc_output "${tsc_output}")
    string(REPLACE ": warning TS" ": [tsc] TS-warning " tsc_output "${tsc_output}")
    string(REPLACE ": error TS" ": [tsc] TS-error " tsc_error "${tsc_error}")
    string(REPLACE ": warning TS" ": [tsc] TS-warning " tsc_error "${tsc_error}")
    message(STATUS "EditorScripts: tsc reported type errors (exit ${tsc_result}); JS is still emitted:\n${tsc_output}${tsc_error}")
endif()
