function(run_godot mode)
    set(log_file "${LOG_DIR}/godot-${mode}.log")
    execute_process(COMMAND "${GODOT}" --headless --path "${DEMO}" --log-file "${log_file}" ${ARGN}
                    RESULT_VARIABLE status
                    OUTPUT_VARIABLE process_output
                    ERROR_VARIABLE process_error
                    TIMEOUT 120)
    set(engine_log "")
    if(EXISTS "${log_file}")
        file(READ "${log_file}" engine_log)
    endif()
    if(NOT status STREQUAL "0" OR engine_log MATCHES "(^|\n)(SCRIPT )?ERROR:")
        message(FATAL_ERROR "Godot ${mode} failed (${status}).\n${process_output}\n${process_error}\n${engine_log}")
    endif()
    if(mode STREQUAL "smoke" AND NOT engine_log MATCHES "Godot demo smoke test failures: 0")
        message(FATAL_ERROR "Godot exited before completing the demo smoke test.\n${engine_log}")
    endif()
    message(STATUS "${engine_log}")
endfunction()

# Give the editor's GDExtension documentation task time to finish on a cold scan.
# https://github.com/godotengine/godot/issues/111048
run_godot(import --editor --import --frame-delay 800)
run_godot(smoke --script "${SMOKE}" -- "${REFERENCE}" "${LOG_DIR}")
