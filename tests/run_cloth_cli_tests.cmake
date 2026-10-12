string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef nonce)
set(work_root "${CARTO_BINARY_DIR}/cloth_cli_${nonce}")
file(MAKE_DIRECTORY "${work_root}")

set(source_project "${work_root}/source.carto")
set(output_root "${work_root}/published")
set(failed_output "${work_root}/unsettled")

execute_process(
    COMMAND "${CARTO_FIXTURE}" create "${source_project}"
    RESULT_VARIABLE fixture_result
    OUTPUT_VARIABLE fixture_stdout
    ERROR_VARIABLE fixture_stderr)
if(NOT fixture_result EQUAL 0)
    message(FATAL_ERROR "cloth fixture creation failed: ${fixture_stdout}${fixture_stderr}")
endif()

execute_process(
    COMMAND "${CARTO_CLI}" bake-cloth-rest-pose "${source_project}" 1 1 1 1 3 0.000001 "${failed_output}"
    RESULT_VARIABLE unsettled_result
    OUTPUT_VARIABLE unsettled_stdout
    ERROR_VARIABLE unsettled_stderr)
if(unsettled_result EQUAL 0)
    message(FATAL_ERROR "unsettled cloth request unexpectedly published")
endif()
if(EXISTS "${failed_output}")
    message(FATAL_ERROR "unsettled cloth request left a published output directory")
endif()

execute_process(
    COMMAND "${CARTO_CLI}" bake-cloth-rest-pose "${source_project}" 1 4 1 1 3 0.000001 "${output_root}"
    RESULT_VARIABLE bake_result
    OUTPUT_VARIABLE bake_stdout
    ERROR_VARIABLE bake_stderr)
if(NOT bake_result EQUAL 0)
    message(FATAL_ERROR "cloth bake failed: ${bake_stdout}${bake_stderr}")
endif()

execute_process(
    COMMAND "${CARTO_CLI}" inspect-cloth-rest-pose "${output_root}"
    RESULT_VARIABLE inspect_result
    OUTPUT_VARIABLE inspect_stdout
    ERROR_VARIABLE inspect_stderr)
if(NOT inspect_result EQUAL 0)
    message(FATAL_ERROR "cloth package inspection failed: ${inspect_stdout}${inspect_stderr}")
endif()

execute_process(
    COMMAND "${CARTO_FIXTURE}" verify "${source_project}" "${output_root}"
    RESULT_VARIABLE verify_result
    OUTPUT_VARIABLE verify_stdout
    ERROR_VARIABLE verify_stderr)
if(NOT verify_result EQUAL 0)
    message(FATAL_ERROR "published cloth package failed verification: ${verify_stdout}${verify_stderr}")
endif()

set(corrupt_root "${work_root}/corrupt")
file(COPY "${output_root}/" DESTINATION "${corrupt_root}")
execute_process(
    COMMAND "${CARTO_FIXTURE}" corrupt-project-receipt "${corrupt_root}/project.carto"
    RESULT_VARIABLE corrupt_result
    OUTPUT_VARIABLE corrupt_stdout
    ERROR_VARIABLE corrupt_stderr)
if(NOT corrupt_result EQUAL 0)
    message(FATAL_ERROR "could not create the hostile cloth package: ${corrupt_stdout}${corrupt_stderr}")
endif()
execute_process(
    COMMAND "${CARTO_CLI}" inspect-cloth-rest-pose "${corrupt_root}"
    RESULT_VARIABLE corrupt_inspect_result
    OUTPUT_VARIABLE corrupt_inspect_stdout
    ERROR_VARIABLE corrupt_inspect_stderr)
if(corrupt_inspect_result EQUAL 0)
    message(FATAL_ERROR "cloth package inspector accepted a corrupted project receipt")
endif()

execute_process(
    COMMAND "${CARTO_CLI}" bake-cloth-rest-pose "${source_project}" 1 4 1 1 3 0.000001 "${output_root}"
    RESULT_VARIABLE overwrite_result
    OUTPUT_VARIABLE overwrite_stdout
    ERROR_VARIABLE overwrite_stderr)
if(overwrite_result EQUAL 0)
    message(FATAL_ERROR "cloth bake unexpectedly overwrote an existing output directory")
endif()
