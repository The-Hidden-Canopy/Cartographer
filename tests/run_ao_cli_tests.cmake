string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef nonce)
set(work_root "${CARTO_BINARY_DIR}/ao_cli_${nonce}")
file(MAKE_DIRECTORY "${work_root}")

set(source_project "${work_root}/source.carto")
set(cpu_output "${work_root}/cpu")
execute_process(
    COMMAND "${CARTO_FIXTURE}" create "${source_project}"
    RESULT_VARIABLE fixture_result
    OUTPUT_VARIABLE fixture_stdout
    ERROR_VARIABLE fixture_stderr)
if(NOT fixture_result EQUAL 0)
    message(FATAL_ERROR "AO fixture creation failed: ${fixture_stdout}${fixture_stderr}")
endif()

file(SHA256 "${source_project}" source_before)
execute_process(
    COMMAND "${CARTO_CLI}" bake-ao "${source_project}" 1 1 8 8 0 0 4 1.0 42 "${cpu_output}"
    RESULT_VARIABLE cpu_bake_result
    OUTPUT_VARIABLE cpu_bake_stdout
    ERROR_VARIABLE cpu_bake_stderr)
if(NOT cpu_bake_result EQUAL 0)
    message(FATAL_ERROR "CPU AO CLI bake failed: ${cpu_bake_stdout}${cpu_bake_stderr}")
endif()
if(NOT cpu_bake_stdout MATCHES "evidence=carto.cpu-reference")
    message(FATAL_ERROR "CPU AO CLI bake did not report CPU evidence: ${cpu_bake_stdout}")
endif()
string(REGEX MATCH "artifact_digest=([0-9a-f]+)" cpu_digest_match "${cpu_bake_stdout}")
if(cpu_digest_match STREQUAL "")
    message(FATAL_ERROR "CPU AO CLI bake omitted its artifact digest: ${cpu_bake_stdout}")
endif()
set(cpu_artifact_digest "${CMAKE_MATCH_1}")

execute_process(
    COMMAND "${CARTO_CLI}" inspect-bake-ao "${source_project}" 1 "${cpu_output}"
    RESULT_VARIABLE cpu_inspect_result
    OUTPUT_VARIABLE cpu_inspect_stdout
    ERROR_VARIABLE cpu_inspect_stderr)
if(NOT cpu_inspect_result EQUAL 0 OR
   NOT cpu_inspect_stdout MATCHES "evidence=carto.cpu-reference")
    message(FATAL_ERROR "CPU AO CLI inspection failed: ${cpu_inspect_stdout}${cpu_inspect_stderr}")
endif()

execute_process(
    COMMAND "${CARTO_CLI}" bake-ao "${source_project}" 1 1 8 8 0 0 4 1.0 42 "${cpu_output}"
    RESULT_VARIABLE overwrite_result
    OUTPUT_VARIABLE overwrite_stdout
    ERROR_VARIABLE overwrite_stderr)
if(overwrite_result EQUAL 0)
    message(FATAL_ERROR "CPU AO CLI bake overwrote an existing output directory")
endif()

set(forged_output "${work_root}/forged-profile")
file(COPY "${cpu_output}/" DESTINATION "${forged_output}")
set(forged_ledger "${forged_output}/receipts.carto-production-receipts")
file(READ "${forged_ledger}" ledger_text)
string(REPLACE "carto.cpu-reference.v1" "carto.unsupported.v1" forged_text "${ledger_text}")
if(forged_text STREQUAL ledger_text)
    message(FATAL_ERROR "CPU AO receipt ledger did not contain its expected profile")
endif()
file(WRITE "${forged_ledger}" "${forged_text}")
execute_process(
    COMMAND "${CARTO_CLI}" inspect-bake-ao "${source_project}" 1 "${forged_output}"
    RESULT_VARIABLE forged_inspect_result
    OUTPUT_VARIABLE forged_inspect_stdout
    ERROR_VARIABLE forged_inspect_stderr)
if(forged_inspect_result EQUAL 0)
    message(FATAL_ERROR "AO inspector accepted a forged or unsupported profile")
endif()

if(CARTO_ENABLE_D3D12)
    set(gpu_output "${work_root}/d3d12")
    execute_process(
        COMMAND "${CARTO_CLI}" bake-ao-d3d12 "${source_project}" 1 1 8 8 0 0 4 1.0 42 "${gpu_output}"
        RESULT_VARIABLE gpu_bake_result
        OUTPUT_VARIABLE gpu_bake_stdout
        ERROR_VARIABLE gpu_bake_stderr)
    if(NOT gpu_bake_result EQUAL 0)
        message(FATAL_ERROR "D3D12 AO CLI bake failed: ${gpu_bake_stdout}${gpu_bake_stderr}")
    endif()
    if(NOT gpu_bake_stdout MATCHES "evidence=carto.d3d12.cpu-reference-parity" OR
       NOT gpu_bake_stdout MATCHES "adapter_id=")
        message(FATAL_ERROR "D3D12 AO CLI bake omitted worker evidence: ${gpu_bake_stdout}")
    endif()
    string(REGEX MATCH "artifact_digest=([0-9a-f]+)" gpu_digest_match "${gpu_bake_stdout}")
    if(gpu_digest_match STREQUAL "" OR
       NOT "${CMAKE_MATCH_1}" STREQUAL "${cpu_artifact_digest}")
        message(FATAL_ERROR "D3D12 AO CLI output differs from its CPU artifact: ${gpu_bake_stdout}")
    endif()
    execute_process(
        COMMAND "${CARTO_CLI}" inspect-bake-ao "${source_project}" 1 "${gpu_output}"
        RESULT_VARIABLE gpu_inspect_result
        OUTPUT_VARIABLE gpu_inspect_stdout
        ERROR_VARIABLE gpu_inspect_stderr)
    if(NOT gpu_inspect_result EQUAL 0 OR
       NOT gpu_inspect_stdout MATCHES "evidence=carto.d3d12.cpu-reference-parity")
        message(FATAL_ERROR "D3D12 AO CLI inspection failed: ${gpu_inspect_stdout}${gpu_inspect_stderr}")
    endif()
endif()

file(SHA256 "${source_project}" source_after_bakes)
if(NOT source_before STREQUAL source_after_bakes)
    message(FATAL_ERROR "AO CLI bake mutated its source project")
endif()

execute_process(
    COMMAND "${CARTO_FIXTURE}" mutate "${source_project}"
    RESULT_VARIABLE mutate_result
    OUTPUT_VARIABLE mutate_stdout
    ERROR_VARIABLE mutate_stderr)
if(NOT mutate_result EQUAL 0)
    message(FATAL_ERROR "AO stale-source fixture mutation failed: ${mutate_stdout}${mutate_stderr}")
endif()
execute_process(
    COMMAND "${CARTO_CLI}" inspect-bake-ao "${source_project}" 1 "${cpu_output}"
    RESULT_VARIABLE stale_inspect_result
    OUTPUT_VARIABLE stale_inspect_stdout
    ERROR_VARIABLE stale_inspect_stderr)
if(stale_inspect_result EQUAL 0)
    message(FATAL_ERROR "AO inspector accepted an artifact after its source mesh changed")
endif()
