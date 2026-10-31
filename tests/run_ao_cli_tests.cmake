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
   NOT cpu_inspect_stdout MATCHES "verification=artifact-source-receipt-readback" OR
   NOT cpu_inspect_stdout MATCHES "platform_profile=\"carto.cpu-reference.v1" OR
   cpu_inspect_stdout MATCHES "evidence=")
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
       NOT gpu_inspect_stdout MATCHES "verification=artifact-source-receipt-readback" OR
       NOT gpu_inspect_stdout MATCHES "platform_profile=\"carto.d3d12." OR
       gpu_inspect_stdout MATCHES "evidence=")
        message(FATAL_ERROR "D3D12 AO CLI inspection failed: ${gpu_inspect_stdout}${gpu_inspect_stderr}")
    endif()

    set(multi_project "${work_root}/multi-tile.carto")
    set(multi_output "${work_root}/d3d12-multi-tile")
    execute_process(
        COMMAND "${CARTO_FIXTURE}" create-multi-tile "${multi_project}"
        RESULT_VARIABLE multi_fixture_result
        OUTPUT_VARIABLE multi_fixture_stdout
        ERROR_VARIABLE multi_fixture_stderr)
    if(NOT multi_fixture_result EQUAL 0)
        message(FATAL_ERROR "D3D12 multi-tile AO fixture failed: ${multi_fixture_stdout}${multi_fixture_stderr}")
    endif()
    file(SHA256 "${multi_project}" multi_source_before)
    execute_process(
        COMMAND "${CARTO_CLI}" bake-ao-tiles-d3d12 "${multi_project}" 1 1 8 8 4 1.0 42 "${multi_output}"
        RESULT_VARIABLE multi_bake_result
        OUTPUT_VARIABLE multi_bake_stdout
        ERROR_VARIABLE multi_bake_stderr)
    if(NOT multi_bake_result EQUAL 0 OR
       NOT multi_bake_stdout MATCHES "backend=d3d12" OR
       NOT multi_bake_stdout MATCHES "tiles=2")
        message(FATAL_ERROR "D3D12 multi-tile AO CLI bake failed: ${multi_bake_stdout}${multi_bake_stderr}")
    endif()
    execute_process(
        COMMAND "${CARTO_CLI}" inspect-bake-ao-tiles-d3d12 "${multi_project}" 1 1 8 8 4 1.0 42 "${multi_output}"
        RESULT_VARIABLE multi_inspect_result
        OUTPUT_VARIABLE multi_inspect_stdout
        ERROR_VARIABLE multi_inspect_stderr)
    if(NOT multi_inspect_result EQUAL 0 OR
       NOT multi_inspect_stdout MATCHES "verified_schedule kind=ambient-occlusion tiles=2")
        message(FATAL_ERROR "D3D12 multi-tile AO inspection failed: ${multi_inspect_stdout}${multi_inspect_stderr}")
    endif()
    file(SHA256 "${multi_project}" multi_source_after)
    if(NOT multi_source_before STREQUAL multi_source_after)
        message(FATAL_ERROR "D3D12 multi-tile AO bake mutated its source project")
    endif()

    set(multi_bent_output "${work_root}/d3d12-multi-tile-bent-normal")
    execute_process(
        COMMAND "${CARTO_CLI}" bake-bent-normal-tiles-d3d12 "${multi_project}" 1 1 8 8 4 1.0 42 "${multi_bent_output}"
        RESULT_VARIABLE multi_bent_bake_result
        OUTPUT_VARIABLE multi_bent_bake_stdout
        ERROR_VARIABLE multi_bent_bake_stderr)
    if(NOT multi_bent_bake_result EQUAL 0 OR
       NOT multi_bent_bake_stdout MATCHES "backend=d3d12" OR
       NOT multi_bent_bake_stdout MATCHES "tiles=2" OR
       NOT multi_bent_bake_stdout MATCHES "evidence=carto.d3d12.cpu-reference-parity")
        message(FATAL_ERROR "D3D12 multi-tile bent-normal CLI bake failed: ${multi_bent_bake_stdout}${multi_bent_bake_stderr}")
    endif()
    execute_process(
        COMMAND "${CARTO_CLI}" inspect-bake-bent-normal-tiles-d3d12 "${multi_project}" 1 1 8 8 4 1.0 42 "${multi_bent_output}"
        RESULT_VARIABLE multi_bent_inspect_result
        OUTPUT_VARIABLE multi_bent_inspect_stdout
        ERROR_VARIABLE multi_bent_inspect_stderr)
    if(NOT multi_bent_inspect_result EQUAL 0 OR
       NOT multi_bent_inspect_stdout MATCHES "verified_schedule kind=bent-normal backend=d3d12 tiles=2" OR
       NOT multi_bent_inspect_stdout MATCHES "verification=artifact-source-receipt-readback" OR
       multi_bent_inspect_stdout MATCHES "evidence=")
        message(FATAL_ERROR "D3D12 multi-tile bent-normal inspection failed: ${multi_bent_inspect_stdout}${multi_bent_inspect_stderr}")
    endif()

    set(cross_project "${work_root}/cross-tile.carto")
    set(cross_output "${work_root}/d3d12-cross-tile-output")
    execute_process(
        COMMAND "${CARTO_FIXTURE}" create-cross-tile "${cross_project}"
        RESULT_VARIABLE cross_fixture_result
        OUTPUT_VARIABLE cross_fixture_stdout
        ERROR_VARIABLE cross_fixture_stderr)
    if(NOT cross_fixture_result EQUAL 0)
        message(FATAL_ERROR "D3D12 cross-tile AO fixture failed: ${cross_fixture_stdout}${cross_fixture_stderr}")
    endif()
    execute_process(
        COMMAND "${CARTO_CLI}" bake-ao-tiles-d3d12 "${cross_project}" 1 1 8 8 4 1.0 42 "${cross_output}"
        RESULT_VARIABLE cross_bake_result
        OUTPUT_VARIABLE cross_bake_stdout
        ERROR_VARIABLE cross_bake_stderr)
    if(cross_bake_result EQUAL 0 OR EXISTS "${cross_output}")
        message(FATAL_ERROR "D3D12 AO accepted a cross-tile face or published partial output: ${cross_bake_stdout}${cross_bake_stderr}")
    endif()
    set(cross_bent_output "${work_root}/d3d12-cross-tile-bent-output")
    execute_process(
        COMMAND "${CARTO_CLI}" bake-bent-normal-tiles-d3d12 "${cross_project}" 1 1 8 8 4 1.0 42 "${cross_bent_output}"
        RESULT_VARIABLE cross_bent_bake_result
        OUTPUT_VARIABLE cross_bent_bake_stdout
        ERROR_VARIABLE cross_bent_bake_stderr)
    if(cross_bent_bake_result EQUAL 0 OR EXISTS "${cross_bent_output}")
        message(FATAL_ERROR "D3D12 bent-normal accepted a cross-tile face or published partial output: ${cross_bent_bake_stdout}${cross_bent_bake_stderr}")
    endif()

    execute_process(
        COMMAND "${CARTO_FIXTURE}" mutate "${multi_project}"
        RESULT_VARIABLE multi_mutate_result
        OUTPUT_VARIABLE multi_mutate_stdout
        ERROR_VARIABLE multi_mutate_stderr)
    if(NOT multi_mutate_result EQUAL 0)
        message(FATAL_ERROR "D3D12 multi-tile AO stale-source mutation failed: ${multi_mutate_stdout}${multi_mutate_stderr}")
    endif()
    execute_process(
        COMMAND "${CARTO_CLI}" inspect-bake-ao-tiles-d3d12 "${multi_project}" 1 1 8 8 4 1.0 42 "${multi_output}"
        RESULT_VARIABLE stale_multi_inspect_result
        OUTPUT_VARIABLE stale_multi_inspect_stdout
        ERROR_VARIABLE stale_multi_inspect_stderr)
    if(stale_multi_inspect_result EQUAL 0)
        message(FATAL_ERROR "D3D12 multi-tile AO inspector accepted artifacts after source mutation")
    endif()
    execute_process(
        COMMAND "${CARTO_CLI}" inspect-bake-bent-normal-tiles-d3d12 "${multi_project}" 1 1 8 8 4 1.0 42 "${multi_bent_output}"
        RESULT_VARIABLE stale_bent_inspect_result
        OUTPUT_VARIABLE stale_bent_inspect_stdout
        ERROR_VARIABLE stale_bent_inspect_stderr)
    if(stale_bent_inspect_result EQUAL 0)
        message(FATAL_ERROR "D3D12 multi-tile bent-normal inspector accepted artifacts after source mutation")
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
