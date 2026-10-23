if(NOT DEFINED CARTO_CLI OR NOT DEFINED CARTO_FIXTURE OR NOT DEFINED CARTO_BINARY_DIR)
    message(FATAL_ERROR "CLI, fixture, and binary directory are required")
endif()

string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef nonce)
set(work_root "${CARTO_BINARY_DIR}/gltf_cli_export_${nonce}")
file(MAKE_DIRECTORY "${work_root}")

set(project_path "${work_root}/source.carto")
set(gltf_path "${work_root}/scene.gltf")
set(glb_path "${work_root}/scene.glb")
execute_process(
    COMMAND "${CARTO_FIXTURE}" create "${project_path}"
    RESULT_VARIABLE fixture_result
    OUTPUT_VARIABLE fixture_stdout
    ERROR_VARIABLE fixture_stderr)
if(NOT fixture_result EQUAL 0)
    message(FATAL_ERROR "GLB CLI fixture creation failed: ${fixture_stdout}${fixture_stderr}")
endif()

file(SHA256 "${project_path}" source_before)
execute_process(
    COMMAND "${CARTO_CLI}" export-gltf "${project_path}" "${gltf_path}"
    RESULT_VARIABLE gltf_result
    OUTPUT_VARIABLE gltf_stdout
    ERROR_VARIABLE gltf_stderr)
if(NOT gltf_result EQUAL 0)
    message(FATAL_ERROR "glTF CLI export failed: ${gltf_stdout}${gltf_stderr}")
endif()
if(NOT EXISTS "${gltf_path}")
    message(FATAL_ERROR "glTF CLI export did not create its output")
endif()
file(SIZE "${gltf_path}" gltf_size)
if(gltf_size LESS 1)
    message(FATAL_ERROR "glTF CLI export created an empty output")
endif()
file(READ "${gltf_path}" gltf_prefix LIMIT 1 HEX)
if(NOT "${gltf_prefix}" STREQUAL "7b")
    message(FATAL_ERROR "glTF CLI export did not write a JSON glTF document")
endif()

execute_process(
    COMMAND "${CARTO_CLI}" export-glb "${project_path}" "${glb_path}"
    RESULT_VARIABLE glb_result
    OUTPUT_VARIABLE glb_stdout
    ERROR_VARIABLE glb_stderr)
if(NOT glb_result EQUAL 0)
    message(FATAL_ERROR "GLB CLI export failed: ${glb_stdout}${glb_stderr}")
endif()
if(NOT EXISTS "${glb_path}")
    message(FATAL_ERROR "GLB CLI export did not create its output")
endif()
file(SIZE "${glb_path}" glb_size)
if(glb_size LESS 28)
    message(FATAL_ERROR "GLB CLI export created an undersized output")
endif()
file(READ "${glb_path}" glb_header LIMIT 12 HEX)
string(SUBSTRING "${glb_header}" 0 8 glb_magic)
string(SUBSTRING "${glb_header}" 8 8 glb_version)
string(SUBSTRING "${glb_header}" 16 2 total_byte_0)
string(SUBSTRING "${glb_header}" 18 2 total_byte_1)
string(SUBSTRING "${glb_header}" 20 2 total_byte_2)
string(SUBSTRING "${glb_header}" 22 2 total_byte_3)
if(NOT glb_magic STREQUAL "676c5446" OR NOT glb_version STREQUAL "02000000")
    message(FATAL_ERROR "GLB CLI export wrote an invalid magic or version header: ${glb_header}")
endif()
math(EXPR declared_total "0x${total_byte_3}${total_byte_2}${total_byte_1}${total_byte_0}")
if(NOT declared_total EQUAL glb_size)
    message(FATAL_ERROR "GLB CLI header length ${declared_total} does not match file size ${glb_size}")
endif()

file(SHA256 "${project_path}" source_after)
if(NOT source_before STREQUAL source_after)
    message(FATAL_ERROR "glTF/GLB CLI export modified its source project")
endif()
