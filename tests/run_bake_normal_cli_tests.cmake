string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef nonce)
set(work_root "${CARTO_BINARY_DIR}/bake_normal_cli_${nonce}")
file(MAKE_DIRECTORY "${work_root}")

set(source_project "${work_root}/source.carto")
set(cpu_output "${work_root}/cpu")
execute_process(
    COMMAND "${CARTO_FIXTURE}" create "${source_project}"
    RESULT_VARIABLE fixture_result
    OUTPUT_VARIABLE fixture_stdout
    ERROR_VARIABLE fixture_stderr)
if(NOT fixture_result EQUAL 0)
    message(FATAL_ERROR "normal-map fixture creation failed: ${fixture_stdout}${fixture_stderr}")
endif()

file(SHA256 "${source_project}" source_before)
execute_process(
    COMMAND "${CARTO_CLI}" bake-normal "${source_project}" 1 1 1 8 8 0 0 1.0 1 "${cpu_output}"
    RESULT_VARIABLE cpu_bake_result
    OUTPUT_VARIABLE cpu_bake_stdout
    ERROR_VARIABLE cpu_bake_stderr)
if(NOT cpu_bake_result EQUAL 0)
    message(FATAL_ERROR "CPU normal-map CLI bake failed: ${cpu_bake_stdout}${cpu_bake_stderr}")
endif()
if(NOT cpu_bake_stdout MATCHES "evidence=carto.cpu-reference" OR
   NOT cpu_bake_stdout MATCHES "platform_profile=\"carto.cpu-reference.v1\"")
    message(FATAL_ERROR "CPU normal-map CLI bake did not report CPU evidence and its quoted profile: ${cpu_bake_stdout}")
endif()
string(REGEX MATCH "artifact_digest=([0-9a-f]+)" cpu_digest_match "${cpu_bake_stdout}")
if(cpu_digest_match STREQUAL "")
    message(FATAL_ERROR "CPU normal-map CLI bake omitted its artifact digest: ${cpu_bake_stdout}")
endif()

execute_process(
    COMMAND "${CARTO_CLI}" inspect-bake-normal "${source_project}" 1 1 "${cpu_output}"
    RESULT_VARIABLE cpu_inspect_result
    OUTPUT_VARIABLE cpu_inspect_stdout
    ERROR_VARIABLE cpu_inspect_stderr)
if(NOT cpu_inspect_result EQUAL 0 OR
   NOT cpu_inspect_stdout MATCHES "verification=artifact-source-receipt-readback" OR
   NOT cpu_inspect_stdout MATCHES "platform_profile=\"carto.cpu-reference.v1\"" OR
   cpu_inspect_stdout MATCHES "evidence=")
    message(FATAL_ERROR "CPU normal-map CLI inspection failed: ${cpu_inspect_stdout}${cpu_inspect_stderr}")
endif()

execute_process(
    COMMAND "${CARTO_CLI}" bake-normal "${source_project}" 1 1 1 8 8 0 0 1.0 1 "${cpu_output}"
    RESULT_VARIABLE overwrite_result
    OUTPUT_VARIABLE overwrite_stdout
    ERROR_VARIABLE overwrite_stderr)
if(overwrite_result EQUAL 0)
    message(FATAL_ERROR "CPU normal-map CLI bake overwrote an existing output directory")
endif()

set(multi_project "${work_root}/multi-tile.carto")
set(multi_output "${work_root}/multi-tile-output")
execute_process(
    COMMAND "${CARTO_FIXTURE}" create-multi-tile "${multi_project}"
    RESULT_VARIABLE multi_fixture_result
    OUTPUT_VARIABLE multi_fixture_stdout
    ERROR_VARIABLE multi_fixture_stderr)
if(NOT multi_fixture_result EQUAL 0)
    message(FATAL_ERROR "multi-tile fixture creation failed: ${multi_fixture_stdout}${multi_fixture_stderr}")
endif()
file(SHA256 "${multi_project}" multi_source_before)
execute_process(
    COMMAND "${CARTO_CLI}" bake-normal-tiles "${multi_project}" 1 1 1 8 8 1.0 1 "${multi_output}"
    RESULT_VARIABLE multi_bake_result
    OUTPUT_VARIABLE multi_bake_stdout
    ERROR_VARIABLE multi_bake_stderr)
if(NOT multi_bake_result EQUAL 0 OR
   NOT multi_bake_stdout MATCHES "kind=tangent-normal-tiles" OR
   NOT multi_bake_stdout MATCHES "platform_profile=\"carto.cpu-reference.v1\"" OR
   NOT multi_bake_stdout MATCHES "tile_count=2")
    message(FATAL_ERROR "multi-tile normal bake failed or omitted tile count: ${multi_bake_stdout}${multi_bake_stderr}")
endif()
execute_process(
    COMMAND "${CARTO_CLI}" inspect-bake-normal-tiles "${multi_project}" 1 1 "${multi_output}"
    RESULT_VARIABLE multi_inspect_result
    OUTPUT_VARIABLE multi_inspect_stdout
    ERROR_VARIABLE multi_inspect_stderr)
if(NOT multi_inspect_result EQUAL 0 OR
   NOT multi_inspect_stdout MATCHES "platform_profile=\"carto.cpu-reference.v1\"" OR
   NOT multi_inspect_stdout MATCHES "tile_count=2")
    message(FATAL_ERROR "multi-tile normal inspection failed: ${multi_inspect_stdout}${multi_inspect_stderr}")
endif()

set(symlink_output_root "${work_root}/symlink-normal-tiles")
file(CREATE_LINK "${multi_output}" "${symlink_output_root}"
    SYMBOLIC RESULT symlink_output_result)
if(symlink_output_result STREQUAL "0")
    execute_process(
        COMMAND "${CARTO_CLI}" inspect-bake-normal-tiles "${multi_project}" 1 1 "${symlink_output_root}"
        RESULT_VARIABLE symlink_output_inspect_result
        OUTPUT_VARIABLE symlink_output_inspect_stdout
        ERROR_VARIABLE symlink_output_inspect_stderr)
    if(symlink_output_inspect_result EQUAL 0)
        message(FATAL_ERROR "normal tile inspector followed a symlinked output root")
    endif()
else()
    message(STATUS "Skipped symlinked normal output-root case: ${symlink_output_result}")
endif()

set(symlink_manifest_output "${work_root}/symlink-normal-manifest")
file(COPY "${multi_output}/" DESTINATION "${symlink_manifest_output}")
set(symlink_manifest_path "${symlink_manifest_output}/receipts.carto-production-receipts")
file(REMOVE "${symlink_manifest_path}")
file(CREATE_LINK "${multi_output}/receipts.carto-production-receipts"
    "${symlink_manifest_path}" SYMBOLIC RESULT symlink_manifest_result)
if(symlink_manifest_result STREQUAL "0")
    execute_process(
        COMMAND "${CARTO_CLI}" inspect-bake-normal-tiles "${multi_project}" 1 1 "${symlink_manifest_output}"
        RESULT_VARIABLE symlink_manifest_inspect_result
        OUTPUT_VARIABLE symlink_manifest_inspect_stdout
        ERROR_VARIABLE symlink_manifest_inspect_stderr)
    if(symlink_manifest_inspect_result EQUAL 0)
        message(FATAL_ERROR "normal tile inspector followed a symlinked receipt manifest")
    endif()
else()
    message(STATUS "Skipped symlinked normal receipt-manifest case: ${symlink_manifest_result}")
endif()

set(symlink_payload_output "${work_root}/symlink-normal-payload")
file(COPY "${multi_output}/" DESTINATION "${symlink_payload_output}")
file(GLOB normal_payload_files "${multi_output}/blobs/*")
list(LENGTH normal_payload_files normal_payload_count)
if(normal_payload_count GREATER 0)
    list(GET normal_payload_files 0 normal_payload_source)
    get_filename_component(normal_payload_name "${normal_payload_source}" NAME)
    set(symlink_payload_path "${symlink_payload_output}/blobs/${normal_payload_name}")
    file(REMOVE "${symlink_payload_path}")
    file(CREATE_LINK "${normal_payload_source}" "${symlink_payload_path}"
        SYMBOLIC RESULT symlink_payload_result)
    if(symlink_payload_result STREQUAL "0")
        execute_process(
            COMMAND "${CARTO_CLI}" inspect-bake-normal-tiles "${multi_project}" 1 1 "${symlink_payload_output}"
            RESULT_VARIABLE symlink_payload_inspect_result
            OUTPUT_VARIABLE symlink_payload_inspect_stdout
            ERROR_VARIABLE symlink_payload_inspect_stderr)
        if(symlink_payload_inspect_result EQUAL 0)
            message(FATAL_ERROR "normal tile inspector followed a symlinked artifact payload")
        endif()
    else()
        message(STATUS "Skipped symlinked normal payload case: ${symlink_payload_result}")
    endif()
endif()
file(SHA256 "${multi_project}" multi_source_after_bake)
if(NOT multi_source_before STREQUAL multi_source_after_bake)
    message(FATAL_ERROR "multi-tile normal bake mutated its source project")
endif()

if(CARTO_ENABLE_D3D12)
    set(d3d12_multi_output "${work_root}/d3d12-multi-tile-output")
    execute_process(
        COMMAND "${CARTO_CLI}" bake-normal-tiles-d3d12 "${multi_project}" 1 1 1 8 8 1.0 1 "${d3d12_multi_output}"
        RESULT_VARIABLE d3d12_multi_bake_result
        OUTPUT_VARIABLE d3d12_multi_bake_stdout
        ERROR_VARIABLE d3d12_multi_bake_stderr)
    if(NOT d3d12_multi_bake_result EQUAL 0 OR
       NOT d3d12_multi_bake_stdout MATCHES "backend=d3d12" OR
       NOT d3d12_multi_bake_stdout MATCHES "tiles=2")
        message(FATAL_ERROR "D3D12 multi-tile normal bake failed: ${d3d12_multi_bake_stdout}${d3d12_multi_bake_stderr}")
    endif()
    execute_process(
        COMMAND "${CARTO_CLI}" inspect-bake-normal-tiles-d3d12 "${multi_project}" 1 1 "${d3d12_multi_output}"
        RESULT_VARIABLE d3d12_multi_inspect_result
        OUTPUT_VARIABLE d3d12_multi_inspect_stdout
        ERROR_VARIABLE d3d12_multi_inspect_stderr)
    if(NOT d3d12_multi_inspect_result EQUAL 0 OR
       NOT d3d12_multi_inspect_stdout MATCHES "kind=tangent-normal-tiles" OR
       NOT d3d12_multi_inspect_stdout MATCHES "tile_count=2" OR
       NOT d3d12_multi_inspect_stdout MATCHES "verification=artifact-source-receipt-readback" OR
       NOT d3d12_multi_inspect_stdout MATCHES "platform_profile=\"carto.d3d12." OR
       d3d12_multi_inspect_stdout MATCHES "evidence=")
        message(FATAL_ERROR "D3D12 multi-tile normal inspection failed: ${d3d12_multi_inspect_stdout}${d3d12_multi_inspect_stderr}")
    endif()
    execute_process(
        COMMAND "${CARTO_CLI}" inspect-bake-normal-tiles-d3d12 "${multi_project}" 1 1 "${multi_output}"
        RESULT_VARIABLE wrong_backend_inspect_result
        OUTPUT_VARIABLE wrong_backend_inspect_stdout
        ERROR_VARIABLE wrong_backend_inspect_stderr)
    if(wrong_backend_inspect_result EQUAL 0)
        message(FATAL_ERROR "D3D12 normal inspector accepted CPU-only tile receipts")
    endif()

    set(d3d12_cross_project "${work_root}/d3d12-cross-tile.carto")
    set(d3d12_cross_output "${work_root}/d3d12-cross-tile-output")
    execute_process(
        COMMAND "${CARTO_FIXTURE}" create-cross-tile "${d3d12_cross_project}"
        RESULT_VARIABLE d3d12_cross_fixture_result
        OUTPUT_VARIABLE d3d12_cross_fixture_stdout
        ERROR_VARIABLE d3d12_cross_fixture_stderr)
    if(NOT d3d12_cross_fixture_result EQUAL 0)
        message(FATAL_ERROR "D3D12 cross-tile normal fixture failed: ${d3d12_cross_fixture_stdout}${d3d12_cross_fixture_stderr}")
    endif()
    execute_process(
        COMMAND "${CARTO_CLI}" bake-normal-tiles-d3d12 "${d3d12_cross_project}" 1 1 1 8 8 1.0 1 "${d3d12_cross_output}"
        RESULT_VARIABLE d3d12_cross_bake_result
        OUTPUT_VARIABLE d3d12_cross_bake_stdout
        ERROR_VARIABLE d3d12_cross_bake_stderr)
    if(d3d12_cross_bake_result EQUAL 0 OR EXISTS "${d3d12_cross_output}")
        message(FATAL_ERROR "D3D12 normal bake accepted a cross-tile face or published partial output: ${d3d12_cross_bake_stdout}${d3d12_cross_bake_stderr}")
    endif()
endif()

set(multi_cache "${work_root}/multi-tile-cache")
set(cache_cold_output "${work_root}/multi-cache-cold")
execute_process(
    COMMAND "${CARTO_CLI}" bake-normal-tiles "${multi_project}" 1 1 1 8 8 1.0 1 "${cache_cold_output}" "${multi_cache}"
    RESULT_VARIABLE cache_cold_result
    OUTPUT_VARIABLE cache_cold_stdout
    ERROR_VARIABLE cache_cold_stderr)
if(NOT cache_cold_result EQUAL 0 OR
   NOT cache_cold_stdout MATCHES "compiled_tiles=2" OR
   NOT cache_cold_stdout MATCHES "reused_tiles=0")
    message(FATAL_ERROR "cold multi-tile cache bake did not compile every tile: ${cache_cold_stdout}${cache_cold_stderr}")
endif()

set(cache_warm_output "${work_root}/multi-cache-warm")
execute_process(
    COMMAND "${CARTO_CLI}" bake-normal-tiles "${multi_project}" 1 1 1 8 8 1.0 1 "${cache_warm_output}" "${multi_cache}"
    RESULT_VARIABLE cache_warm_result
    OUTPUT_VARIABLE cache_warm_stdout
    ERROR_VARIABLE cache_warm_stderr)
if(NOT cache_warm_result EQUAL 0 OR
   NOT cache_warm_stdout MATCHES "compiled_tiles=0" OR
   NOT cache_warm_stdout MATCHES "reused_tiles=2")
    message(FATAL_ERROR "warm multi-tile cache bake did not reuse every tile: ${cache_warm_stdout}${cache_warm_stderr}")
endif()

set(overlap_output "${multi_cache}/overlap-output")
execute_process(
    COMMAND "${CARTO_CLI}" bake-normal-tiles "${multi_project}" 1 1 1 8 8 1.0 1 "${overlap_output}" "${multi_cache}"
    RESULT_VARIABLE overlap_cache_result
    OUTPUT_VARIABLE overlap_cache_stdout
    ERROR_VARIABLE overlap_cache_stderr)
if(overlap_cache_result EQUAL 0 OR EXISTS "${overlap_output}")
    message(FATAL_ERROR "multi-tile normal bake allowed cache/output path overlap")
endif()

file(GLOB cache_entries "${multi_cache}/*.carto-bake-tile-cache")
list(LENGTH cache_entries cache_entry_count)
if(NOT cache_entry_count EQUAL 2)
    message(FATAL_ERROR "multi-tile cache did not publish one content-addressed entry per tile")
endif()
set(symlink_cache "${work_root}/symlink-multi-tile-cache")
file(MAKE_DIRECTORY "${symlink_cache}")
list(GET cache_entries 0 cache_entry_source)
get_filename_component(cache_entry_name "${cache_entry_source}" NAME)
file(CREATE_LINK "${cache_entry_source}" "${symlink_cache}/${cache_entry_name}"
    SYMBOLIC RESULT cache_symlink_result)
if(cache_symlink_result STREQUAL "0")
    set(symlink_cache_output "${work_root}/symlink-cache-output")
    execute_process(
        COMMAND "${CARTO_CLI}" bake-normal-tiles "${multi_project}" 1 1 1 8 8 1.0 1 "${symlink_cache_output}" "${symlink_cache}"
        RESULT_VARIABLE symlink_cache_result
        OUTPUT_VARIABLE symlink_cache_stdout
        ERROR_VARIABLE symlink_cache_stderr)
    if(symlink_cache_result EQUAL 0 OR EXISTS "${symlink_cache_output}")
        message(FATAL_ERROR "multi-tile normal bake accepted a symlinked cache entry")
    endif()
else()
    message(STATUS "Skipped symlinked bake-cache entry case: ${cache_symlink_result}")
endif()

set(corrupt_cache "${work_root}/corrupt-multi-tile-cache")
file(COPY "${multi_cache}/" DESTINATION "${corrupt_cache}")
file(GLOB corrupt_cache_entries "${corrupt_cache}/*.carto-bake-tile-cache")
list(GET corrupt_cache_entries 0 corrupt_cache_entry)
file(WRITE "${corrupt_cache_entry}" "corrupt cache bytes")
set(corrupt_cache_output "${work_root}/corrupt-cache-output")
execute_process(
    COMMAND "${CARTO_CLI}" bake-normal-tiles "${multi_project}" 1 1 1 8 8 1.0 1 "${corrupt_cache_output}" "${corrupt_cache}"
    RESULT_VARIABLE corrupt_cache_result
    OUTPUT_VARIABLE corrupt_cache_stdout
    ERROR_VARIABLE corrupt_cache_stderr)
if(corrupt_cache_result EQUAL 0 OR EXISTS "${corrupt_cache_output}")
    message(FATAL_ERROR "multi-tile normal bake accepted a corrupt expected cache entry")
endif()
file(SHA256 "${multi_project}" multi_source_after_cache)
if(NOT multi_source_before STREQUAL multi_source_after_cache)
    message(FATAL_ERROR "multi-tile cache read/write mutated its source project")
endif()

set(forged_multi_output "${work_root}/forged-multi-tile")
file(COPY "${multi_output}/" DESTINATION "${forged_multi_output}")
set(forged_multi_ledger "${forged_multi_output}/receipts.carto-production-receipts")
file(READ "${forged_multi_ledger}" multi_ledger_text)
string(REPLACE "carto.cpu-reference.v1" "carto.unsupported.v1" forged_multi_text "${multi_ledger_text}")
if(forged_multi_text STREQUAL multi_ledger_text)
    message(FATAL_ERROR "multi-tile receipt ledger did not contain its expected profile")
endif()
file(WRITE "${forged_multi_ledger}" "${forged_multi_text}")
execute_process(
    COMMAND "${CARTO_CLI}" inspect-bake-normal-tiles "${multi_project}" 1 1 "${forged_multi_output}"
    RESULT_VARIABLE forged_multi_inspect_result
    OUTPUT_VARIABLE forged_multi_inspect_stdout
    ERROR_VARIABLE forged_multi_inspect_stderr)
if(forged_multi_inspect_result EQUAL 0)
    message(FATAL_ERROR "multi-tile inspector accepted a forged receipt profile")
endif()

set(cross_project "${work_root}/cross-tile.carto")
set(cross_output "${work_root}/cross-tile-output")
execute_process(
    COMMAND "${CARTO_FIXTURE}" create-cross-tile "${cross_project}"
    RESULT_VARIABLE cross_fixture_result
    OUTPUT_VARIABLE cross_fixture_stdout
    ERROR_VARIABLE cross_fixture_stderr)
if(NOT cross_fixture_result EQUAL 0)
    message(FATAL_ERROR "cross-tile fixture creation failed: ${cross_fixture_stdout}${cross_fixture_stderr}")
endif()
execute_process(
    COMMAND "${CARTO_CLI}" bake-normal-tiles "${cross_project}" 1 1 1 8 8 1.0 1 "${cross_output}"
    RESULT_VARIABLE cross_bake_result
    OUTPUT_VARIABLE cross_bake_stdout
    ERROR_VARIABLE cross_bake_stderr)
if(cross_bake_result EQUAL 0 OR EXISTS "${cross_output}")
    message(FATAL_ERROR "multi-tile normal bake accepted a cross-tile face or published partial output")
endif()

execute_process(
    COMMAND "${CARTO_FIXTURE}" mutate "${multi_project}"
    RESULT_VARIABLE multi_mutate_result
    OUTPUT_VARIABLE multi_mutate_stdout
    ERROR_VARIABLE multi_mutate_stderr)
if(NOT multi_mutate_result EQUAL 0)
    message(FATAL_ERROR "multi-tile stale-source fixture mutation failed: ${multi_mutate_stdout}${multi_mutate_stderr}")
endif()
set(stale_cache_output "${work_root}/stale-cache-output")
execute_process(
    COMMAND "${CARTO_CLI}" bake-normal-tiles "${multi_project}" 1 1 1 8 8 1.0 1 "${stale_cache_output}" "${multi_cache}"
    RESULT_VARIABLE stale_cache_result
    OUTPUT_VARIABLE stale_cache_stdout
    ERROR_VARIABLE stale_cache_stderr)
if(NOT stale_cache_result EQUAL 0 OR
   NOT stale_cache_stdout MATCHES "compiled_tiles=2" OR
   NOT stale_cache_stdout MATCHES "reused_tiles=0")
    message(FATAL_ERROR "multi-tile normal bake reused entries after source mutation: ${stale_cache_stdout}${stale_cache_stderr}")
endif()
execute_process(
    COMMAND "${CARTO_CLI}" inspect-bake-normal-tiles "${multi_project}" 1 1 "${multi_output}"
    RESULT_VARIABLE stale_multi_inspect_result
    OUTPUT_VARIABLE stale_multi_inspect_stdout
    ERROR_VARIABLE stale_multi_inspect_stderr)
if(stale_multi_inspect_result EQUAL 0)
    message(FATAL_ERROR "multi-tile normal inspector accepted artifacts after source mutation")
endif()
if(CARTO_ENABLE_D3D12)
    execute_process(
        COMMAND "${CARTO_CLI}" inspect-bake-normal-tiles-d3d12 "${multi_project}" 1 1 "${d3d12_multi_output}"
        RESULT_VARIABLE stale_d3d12_multi_inspect_result
        OUTPUT_VARIABLE stale_d3d12_multi_inspect_stdout
        ERROR_VARIABLE stale_d3d12_multi_inspect_stderr)
    if(stale_d3d12_multi_inspect_result EQUAL 0)
        message(FATAL_ERROR "D3D12 multi-tile normal inspector accepted artifacts after source mutation")
    endif()
endif()

set(forged_output "${work_root}/forged-profile")
file(COPY "${cpu_output}/" DESTINATION "${forged_output}")
set(forged_ledger "${forged_output}/receipts.carto-production-receipts")
file(READ "${forged_ledger}" ledger_text)
string(REPLACE "carto.cpu-reference.v1" "carto.unsupported.v1" forged_text "${ledger_text}")
if(forged_text STREQUAL ledger_text)
    message(FATAL_ERROR "CPU normal-map receipt ledger did not contain its expected profile")
endif()
file(WRITE "${forged_ledger}" "${forged_text}")
execute_process(
    COMMAND "${CARTO_CLI}" inspect-bake-normal "${source_project}" 1 1 "${forged_output}"
    RESULT_VARIABLE forged_inspect_result
    OUTPUT_VARIABLE forged_inspect_stdout
    ERROR_VARIABLE forged_inspect_stderr)
if(forged_inspect_result EQUAL 0)
    message(FATAL_ERROR "normal-map inspector accepted a forged or unsupported profile")
endif()

file(SHA256 "${source_project}" source_after_bakes)
if(NOT source_before STREQUAL source_after_bakes)
    message(FATAL_ERROR "normal-map CLI bake mutated its source project")
endif()

execute_process(
    COMMAND "${CARTO_FIXTURE}" mutate "${source_project}"
    RESULT_VARIABLE mutate_result
    OUTPUT_VARIABLE mutate_stdout
    ERROR_VARIABLE mutate_stderr)
if(NOT mutate_result EQUAL 0)
    message(FATAL_ERROR "normal-map stale-source fixture mutation failed: ${mutate_stdout}${mutate_stderr}")
endif()
execute_process(
    COMMAND "${CARTO_CLI}" inspect-bake-normal "${source_project}" 1 1 "${cpu_output}"
    RESULT_VARIABLE stale_inspect_result
    OUTPUT_VARIABLE stale_inspect_stdout
    ERROR_VARIABLE stale_inspect_stderr)
if(stale_inspect_result EQUAL 0)
    message(FATAL_ERROR "normal-map inspector accepted an artifact after its source mesh changed")
endif()
