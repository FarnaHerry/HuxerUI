execute_process(
        COMMAND ${TEST_EMULATOR} "${TEST_EXECUTABLE}"
                WebTestRunnerPropagatesFailure --reporter compact --colour-mode none
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error
        TIMEOUT 30
)
# Catch::TestFailureExitCode distinguishes an assertion failure from a Node.js startup error.
if (NOT result STREQUAL "42"
        OR NOT output MATCHES "test cases: 1 \\| 1 failed"
        OR NOT output MATCHES "assertions: 1 \\| 1 failed")
    message(FATAL_ERROR "HuxerUI Web test failure did not reach Catch2's exit status: ${result}\n${output}\n${error}")
endif ()
message(STATUS "${output}")
