if(NOT DEFINED CARTO_CLI OR NOT DEFINED CARTO_FIXTURE OR NOT DEFINED CARTO_BINARY_DIR)
    message(FATAL_ERROR "CLI, scene fixture and binary directory are required")
endif()
string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef nonce)
set(root "${CARTO_BINARY_DIR}/scene_cli_${nonce}")
execute_process(COMMAND "${CARTO_FIXTURE}" --write-fixtures "${root}"
    RESULT_VARIABLE result OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Independent scene fixture failed: ${stdout}${stderr}")
endif()
foreach(format IN ITEMS gltf glb)
    set(source "${root}/source.${format}")
    set(project "${root}/imported-${format}.carto")
    file(SHA256 "${source}" before)
    execute_process(COMMAND "${CARTO_CLI}" import-gltf "${source}" "${project}" --confirm-reuse-rights
        RESULT_VARIABLE result OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
    if(NOT result EQUAL 0 OR NOT stdout MATCHES "nodes=3 meshes=1 materials=1")
        message(FATAL_ERROR "Actual ${format} CLI scene import failed: ${stdout}${stderr}")
    endif()
    execute_process(COMMAND "${CARTO_CLI}" export-glb "${project}" "${root}/reopened-${format}.glb"
        RESULT_VARIABLE result OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
    if(NOT result EQUAL 0 OR NOT stdout MATCHES "materials=1")
        message(FATAL_ERROR "Imported ${format} project did not reopen/export with materials: ${stdout}${stderr}")
    endif()
    file(SHA256 "${project}" project_before)
    execute_process(COMMAND "${CARTO_CLI}" import-gltf "${source}" "${project}" --confirm-reuse-rights
        RESULT_VARIABLE result OUTPUT_QUIET ERROR_QUIET)
    file(SHA256 "${project}" project_after)
    if(result EQUAL 0 OR NOT project_before STREQUAL project_after)
        message(FATAL_ERROR "Scene CLI import replaced an existing project")
    endif()
    file(SHA256 "${source}" after)
    if(NOT before STREQUAL after)
        message(FATAL_ERROR "Scene CLI import modified source asset")
    endif()
    set(orphan "${root}/orphan-${format}.carto")
    file(WRITE "${orphan}.journal" "")
    execute_process(COMMAND "${CARTO_CLI}" import-gltf "${source}" "${orphan}" --confirm-reuse-rights
        RESULT_VARIABLE result OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
    file(SIZE "${orphan}.journal" journal_size)
    if(result EQUAL 0 OR EXISTS "${orphan}" OR NOT journal_size EQUAL 0 OR
       NOT stderr MATCHES "create-only save refuses")
        message(FATAL_ERROR "${format} import adopted an orphan journal: ${stdout}${stderr}")
    endif()
endforeach()
# Imported scenes with materials must prepare for runtime handoff and report
# exact admission pins; a publication is refused for any other source project.
set(published "${root}/imported-glb.carto.prepared")
execute_process(COMMAND "${CARTO_CLI}" prepare "${root}/imported-glb.carto" "${published}" scene
    RESULT_VARIABLE result OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT result EQUAL 0 OR NOT stdout MATCHES "nodes=3")
    message(FATAL_ERROR "Imported scene with materials did not prepare: ${stdout}${stderr}")
endif()
execute_process(COMMAND "${CARTO_CLI}" inspect-prepared "${published}" --source "${root}/imported-glb.carto"
    RESULT_VARIABLE result OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT result EQUAL 0 OR NOT stdout MATCHES "freshness=current" OR
   NOT stdout MATCHES "pin source_digest=[0-9a-f]+" OR
   NOT stdout MATCHES "pin profile_digest=[0-9a-f]+" OR NOT stdout MATCHES "bindings=1")
    message(FATAL_ERROR "Prepared scene did not report current admission pins: ${stdout}${stderr}")
endif()
execute_process(COMMAND "${CARTO_CLI}" inspect-prepared "${published}" --source "${root}/imported-gltf.carto"
    RESULT_VARIABLE result OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(result EQUAL 0 OR NOT stdout MATCHES "freshness=foreign_source")
    message(FATAL_ERROR "Prepared scene was accepted for a different source project: ${stdout}${stderr}")
endif()
execute_process(COMMAND "${CARTO_CLI}" import-gltf "${root}/source.gltf" "${root}/missing-confirmation.carto"
    RESULT_VARIABLE result OUTPUT_QUIET ERROR_QUIET)
if(result EQUAL 0 OR EXISTS "${root}/missing-confirmation.carto")
    message(FATAL_ERROR "CLI skipped explicit source reuse confirmation")
endif()
file(READ "${root}/source.gltf" json)
string(REPLACE "data:application/octet-stream;base64," "https://example.invalid/" json "${json}")
file(WRITE "${root}/external.gltf" "${json}")
execute_process(COMMAND "${CARTO_CLI}" import-gltf "${root}/external.gltf" "${root}/unsafe.carto" --confirm-reuse-rights
    RESULT_VARIABLE result OUTPUT_QUIET ERROR_QUIET)
if(result EQUAL 0 OR EXISTS "${root}/unsafe.carto")
    message(FATAL_ERROR "External scene reference was not rejected before project creation")
endif()
message(STATUS "GLTF/GLB CLI import, managed reopen/export, prepared handoff, source/target protection and rights checks passed")
