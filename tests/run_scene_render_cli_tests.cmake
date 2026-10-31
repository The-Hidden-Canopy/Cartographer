cmake_minimum_required(VERSION 3.21)
foreach(required FIXTURE RENDER SHADERS OUTPUT_ROOT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "Missing ${required}")
    endif()
endforeach()
string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef run_id)
set(root "${OUTPUT_ROOT}/scene-render-${run_id}")
file(MAKE_DIRECTORY "${root}")
execute_process(COMMAND "${FIXTURE}" --write-fixture "${root}/room.carto"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Fixture refused: ${output}${error}")
endif()
file(SHA256 "${root}/room.carto" source_before)
set(arguments --identity fixture-room --shaders "${SHADERS}"
    --camera 5 3.1 5.5 -0.3 0.8 -1 --camera-up 0 0 1
    --vertical-fov 0.9 --clip-range 0.02 500 --size 640 400)
foreach(frame first repeat)
    execute_process(COMMAND "${RENDER}" "${root}/room.carto" "${root}/${frame}.bmp" ${arguments}
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Scene render refused: ${output}${error}")
    endif()
    file(READ "${root}/${frame}.bmp.receipt.json" receipt)
    string(REGEX MATCHALL "render_graph_passes" pass_fields "${receipt}")
    list(LENGTH pass_fields pass_field_count)
    if(NOT pass_field_count EQUAL 1)
        message(FATAL_ERROR "Capture receipt must contain one render_graph_passes field")
    endif()
    string(JSON algorithm GET "${receipt}" algorithm)
    if(NOT algorithm STREQUAL "carto.d3d12.scene-direct-capture.v31")
        message(FATAL_ERROR "Capture must identify the current renderer algorithm")
    endif()
    string(JSON camera_source_kind GET "${receipt}" camera_source kind)
    string(JSON source_digest GET "${receipt}" source_digest)
    string(JSON pixel_digest GET "${receipt}" pixel_digest)
    string(JSON request_digest GET "${receipt}" request_digest)
    string(JSON bmp_digest GET "${receipt}" bmp_digest)
    string(JSON camera_up_x GET "${receipt}" camera_up 0)
    string(JSON camera_up_y GET "${receipt}" camera_up 1)
    string(JSON camera_up_z GET "${receipt}" camera_up 2)
    string(JSON vertical_fov GET "${receipt}" vertical_fov)
    string(JSON clip_near GET "${receipt}" clip_range 0)
    string(JSON clip_far GET "${receipt}" clip_range 1)
    string(JSON draw_count GET "${receipt}" draw_count)
    string(JSON local_light_count LENGTH "${receipt}" local_lights)
    string(JSON local_lighting_model_type TYPE "${receipt}" local_lighting_model)
    string(JSON temporal_output_state_type TYPE "${receipt}" temporal_output_state)
    string(JSON shader_count LENGTH "${receipt}" shader_digests)
    string(JSON pass_count LENGTH "${receipt}" render_graph_passes)
    string(JSON first_pass GET "${receipt}" render_graph_passes 0)
    string(JSON second_pass GET "${receipt}" render_graph_passes 1)
    string(JSON third_pass GET "${receipt}" render_graph_passes 2)
    string(JSON fourth_pass GET "${receipt}" render_graph_passes 3)
    string(JSON gpu_timing_requested GET "${receipt}" gpu_timing_requested)
    string(JSON gpu_timing_type TYPE "${receipt}" gpu_timing)
    if(NOT pass_count EQUAL 4 OR NOT first_pass STREQUAL "scene.shadow" OR
       NOT second_pass STREQUAL "scene.pbr" OR NOT third_pass STREQUAL "scene.tonemap" OR
       NOT fourth_pass STREQUAL "scene.capture-readback")
        message(FATAL_ERROR "Capture receipt must identify its actually recorded scene passes")
    endif()
    string(JSON graph_pass_count LENGTH "${receipt}" render_graph_passes)
    string(JSON graph_pbr GET "${receipt}" render_graph_passes 0)
    string(JSON graph_tonemap GET "${receipt}" render_graph_passes 2)
    string(JSON limitation_count LENGTH "${receipt}" limitations)
    file(SHA256 "${root}/${frame}.bmp" actual_bmp_digest)
    file(SIZE "${root}/${frame}.bmp" bmp_bytes)
    if(NOT source_digest STREQUAL source_before OR NOT bmp_digest STREQUAL actual_bmp_digest OR
       NOT draw_count EQUAL 14 OR NOT shader_count EQUAL 7 OR NOT graph_pass_count EQUAL 4 OR
       NOT graph_pbr STREQUAL "scene.shadow" OR NOT graph_tonemap STREQUAL "scene.tonemap" OR
       NOT camera_up_x STREQUAL "0" OR NOT camera_up_y STREQUAL "0" OR
       NOT camera_up_z STREQUAL "1" OR NOT vertical_fov STREQUAL "0.9" OR
       NOT clip_near STREQUAL "0.02" OR NOT clip_far STREQUAL "500" OR
       limitation_count LESS 3 OR gpu_timing_requested OR
       NOT local_light_count EQUAL 0 OR
       NOT local_lighting_model_type STREQUAL "NULL" OR
       NOT temporal_output_state_type STREQUAL "NULL" OR
       NOT camera_source_kind STREQUAL "explicit" OR
       NOT gpu_timing_type STREQUAL "NULL" OR
       NOT bmp_bytes EQUAL 1024054)
        message(FATAL_ERROR "Source, image, draw, shader or limitation receipt does not match actual capture")
    endif()
    if(frame STREQUAL "first")
        set(first_pixels "${pixel_digest}")
        set(first_request "${request_digest}")
        set(first_bmp "${actual_bmp_digest}")
    elseif(NOT first_pixels STREQUAL pixel_digest OR NOT first_request STREQUAL request_digest OR
           NOT first_bmp STREQUAL actual_bmp_digest)
        message(FATAL_ERROR "Fixed project/camera/device capture is not reproducible")
    endif()
endforeach()
set(explicit_orthographic_output "${root}/explicit-orthographic.bmp")
execute_process(COMMAND "${RENDER}" "${root}/room.carto"
    "${explicit_orthographic_output}" --identity fixture-room
    --shaders "${SHADERS}" --camera 0 0 3 0 0 0
    --projection orthographic --orthographic-height 2.5 --size 96 96
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Explicit orthographic capture refused: ${output}${error}")
endif()
file(READ "${explicit_orthographic_output}.receipt.json"
    explicit_orthographic_receipt)
string(JSON explicit_orthographic_projection GET
    "${explicit_orthographic_receipt}" camera_projection)
string(JSON explicit_orthographic_height GET
    "${explicit_orthographic_receipt}" orthographic_height)
if(NOT explicit_orthographic_projection STREQUAL "orthographic" OR
   NOT explicit_orthographic_height STREQUAL "2.5")
    message(FATAL_ERROR "Explicit orthographic receipt lost its projection settings")
endif()
execute_process(COMMAND "${RENDER}" "${root}/room.carto"
    "${root}/orthographic-height-with-perspective.bmp"
    --identity fixture-room --shaders "${SHADERS}"
    --camera 0 0 3 0 0 0 --orthographic-height 2.5 --size 96 96
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(result EQUAL 0 OR
   EXISTS "${root}/orthographic-height-with-perspective.bmp" OR
   EXISTS "${root}/orthographic-height-with-perspective.bmp.receipt.json")
    message(FATAL_ERROR "Orthographic extent without orthographic projection was accepted")
endif()
set(timed_output "${root}/gpu-timed.bmp")
execute_process(COMMAND "${RENDER}" "${root}/room.carto" "${timed_output}"
    ${arguments} --gpu-timing
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "GPU-timed scene render refused: ${output}${error}")
endif()
file(READ "${timed_output}.receipt.json" timed_receipt)
string(JSON timed_requested GET "${timed_receipt}" gpu_timing_requested)
string(JSON timed_type TYPE "${timed_receipt}" gpu_timing)
string(JSON timed_scope GET "${timed_receipt}" gpu_timing scope)
string(JSON timed_elapsed_type TYPE "${timed_receipt}" gpu_timing elapsed_time_ns)
string(JSON timed_frequency_type TYPE "${timed_receipt}" gpu_timing timestamp_frequency_hz)
string(JSON timed_frequency GET "${timed_receipt}" gpu_timing timestamp_frequency_hz)
string(JSON timed_request GET "${timed_receipt}" request_digest)
string(JSON timed_pixels GET "${timed_receipt}" pixel_digest)
string(JSON timed_pass_count LENGTH "${timed_receipt}" render_graph_passes)
string(JSON timed_pass GET "${timed_receipt}" render_graph_passes 4)
if(NOT timed_requested OR NOT timed_type STREQUAL "OBJECT" OR
   NOT timed_scope STREQUAL "scene_render_graph_passes" OR
   NOT timed_elapsed_type STREQUAL "NUMBER" OR
   NOT timed_frequency_type STREQUAL "NUMBER" OR timed_frequency LESS_EQUAL 0 OR
   NOT timed_request STREQUAL first_request OR
   NOT timed_pixels STREQUAL first_pixels OR NOT timed_pass_count EQUAL 5 OR
   NOT timed_pass STREQUAL "scene.gpu-timing-resolve")
    message(FATAL_ERROR "GPU timing receipt is missing measured queue evidence or changed capture identity")
endif()
execute_process(COMMAND "${RENDER}" "${root}/room.carto" "${root}/duplicate-gpu-timing.bmp"
    ${arguments} --gpu-timing --gpu-timing
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(result EQUAL 0 OR EXISTS "${root}/duplicate-gpu-timing.bmp" OR
   EXISTS "${root}/duplicate-gpu-timing.bmp.receipt.json")
    message(FATAL_ERROR "Duplicate GPU timing option was not refused before publication")
endif()
set(local_light_project "${root}/local-light-room.carto")
execute_process(COMMAND "${FIXTURE}" --write-local-light-fixture
    "${local_light_project}"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Authored local-light fixture refused: ${output}${error}")
endif()
file(READ "${local_light_project}.light-id" local_light_id)
string(STRIP "${local_light_id}" local_light_id)
file(READ "${local_light_project}.spot-id" local_spot_id)
string(STRIP "${local_spot_id}" local_spot_id)
set(local_light_output "${root}/local-light.bmp")
execute_process(COMMAND "${RENDER}" "${local_light_project}"
    "${local_light_output}" --identity fixture-local-light --shaders "${SHADERS}"
    --camera 5 3.1 5.5 -0.3 0.8 -1 --camera-up 0 0 1
    --vertical-fov 0.9 --clip-range 0.02 500 --size 640 400
    --light 1 0 0 99 98 97
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Authored local-light capture refused: ${output}${error}")
endif()
file(READ "${local_light_output}.receipt.json" local_light_receipt)
string(JSON local_light_count LENGTH "${local_light_receipt}" local_lights)
string(JSON local_light_kind GET "${local_light_receipt}" local_lights 0 kind)
string(JSON local_light_source_id GET "${local_light_receipt}" local_lights 0 source_object_id)
string(JSON local_light_position_x GET "${local_light_receipt}" local_lights 0 position_world 0)
string(JSON local_light_radiance_r GET "${local_light_receipt}" local_lights 0 radiance_linear 0)
string(JSON local_light_radiance_g GET "${local_light_receipt}" local_lights 0 radiance_linear 1)
string(JSON local_light_radiance_b GET "${local_light_receipt}" local_lights 0 radiance_linear 2)
string(JSON local_light_range GET "${local_light_receipt}" local_lights 0 range_world)
string(JSON local_light_shadowed GET "${local_light_receipt}" local_lights 0 shadowed)
string(JSON local_spot_kind GET "${local_light_receipt}" local_lights 1 kind)
string(JSON local_spot_source_id GET "${local_light_receipt}" local_lights 1 source_object_id)
string(JSON local_spot_direction_z GET "${local_light_receipt}" local_lights 1 emission_direction_world 2)
string(JSON local_spot_inner_cone GET "${local_light_receipt}" local_lights 1 cone_radians 0)
string(JSON local_spot_outer_cone GET "${local_light_receipt}" local_lights 1 cone_radians 1)
string(JSON local_spot_shadowed GET "${local_light_receipt}" local_lights 1 shadowed)
string(JSON local_light_model_radiance GET "${local_light_receipt}" local_lighting_model radiance)
string(JSON local_light_model_attenuation GET "${local_light_receipt}" local_lighting_model distance_attenuation)
string(JSON local_light_model_floor GET "${local_light_receipt}" local_lighting_model near_field_floor_meters)
string(JSON local_light_model_range_fade GET "${local_light_receipt}" local_lighting_model range_fade)
string(JSON local_light_model_shadowing GET "${local_light_receipt}" local_lighting_model shadowing)
string(JSON effective_light_x GET "${local_light_receipt}" light_direction 0)
string(JSON effective_light_z GET "${local_light_receipt}" light_direction 2)
string(JSON effective_radiance_r GET "${local_light_receipt}" light_radiance 0)
string(JSON effective_radiance_g GET "${local_light_receipt}" light_radiance 1)
string(JSON effective_radiance_b GET "${local_light_receipt}" light_radiance 2)
if(NOT local_light_count EQUAL 2 OR NOT local_light_kind STREQUAL "point" OR
   NOT local_light_source_id STREQUAL local_light_id OR
   NOT local_light_position_x STREQUAL "1.25" OR
   NOT local_light_radiance_r STREQUAL "1.5" OR
   NOT local_light_radiance_g STREQUAL "0.75" OR
   NOT local_light_radiance_b STREQUAL "3" OR
   NOT local_light_range STREQUAL "6" OR local_light_shadowed OR
   NOT local_spot_kind STREQUAL "spot" OR
   NOT local_spot_source_id STREQUAL local_spot_id OR
   NOT local_spot_direction_z STREQUAL "-1" OR
   NOT local_spot_inner_cone STREQUAL "0.25" OR
   NOT local_spot_outer_cone STREQUAL "0.5" OR NOT local_spot_shadowed OR
   NOT local_light_model_radiance STREQUAL "linear_lookdev_scale_not_calibrated_lumens" OR
   NOT local_light_model_attenuation STREQUAL "inverse_square" OR
   NOT local_light_model_floor STREQUAL "0.01" OR
   NOT local_light_model_range_fade STREQUAL "quartic_smooth" OR
   NOT local_light_model_shadowing STREQUAL "at_most_one_spotlight" OR
   NOT effective_light_x STREQUAL "0" OR NOT effective_light_z STREQUAL "1" OR
   NOT effective_radiance_r STREQUAL "0" OR
   NOT effective_radiance_g STREQUAL "0" OR
   NOT effective_radiance_b STREQUAL "0")
    message(FATAL_ERROR "Receipt reports caller fallback instead of the effective saved local-light request")
endif()
set(changed_camera_arguments --identity fixture-room --shaders "${SHADERS}"
    --camera 5 3.1 5.5 -0.3 0.8 -1 --camera-up 0 1 0
    --vertical-fov 0.75 --clip-range 0.1 250 --size 640 400)
execute_process(COMMAND "${RENDER}" "${root}/room.carto" "${root}/changed-camera.bmp"
    ${changed_camera_arguments}
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Changed camera capture refused: ${output}${error}")
endif()
file(READ "${root}/changed-camera.bmp.receipt.json" changed_camera_receipt)
string(JSON changed_camera_request GET "${changed_camera_receipt}" request_digest)
string(JSON changed_camera_up_y GET "${changed_camera_receipt}" camera_up 1)
string(JSON changed_camera_fov GET "${changed_camera_receipt}" vertical_fov)
string(JSON changed_camera_clip_near GET "${changed_camera_receipt}" clip_range 0)
string(JSON changed_camera_clip_far GET "${changed_camera_receipt}" clip_range 1)
if(changed_camera_request STREQUAL first_request OR
   NOT changed_camera_up_y STREQUAL "1" OR
   NOT changed_camera_fov STREQUAL "0.75" OR
   NOT changed_camera_clip_near STREQUAL "0.1" OR
   NOT changed_camera_clip_far STREQUAL "250")
    message(FATAL_ERROR "Camera settings did not alter request identity and receipt provenance")
endif()
set(authored_camera_project "${root}/authored-camera-room.carto")
execute_process(COMMAND "${FIXTURE}" --write-authored-camera-fixture
    "${authored_camera_project}"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Authored camera fixture refused: ${output}${error}")
endif()
file(READ "${authored_camera_project}.camera-id" authored_camera_id)
string(STRIP "${authored_camera_id}" authored_camera_id)
set(authored_camera_output "${root}/authored-camera.bmp")
execute_process(COMMAND "${RENDER}" "${authored_camera_project}" "${authored_camera_output}"
    --identity fixture-authored-camera --shaders "${SHADERS}"
    --authored-camera "${authored_camera_id}" --size 640 400
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Authored camera capture refused: ${output}${error}")
endif()
file(READ "${authored_camera_output}.receipt.json" authored_camera_receipt)
string(JSON authored_camera_kind GET "${authored_camera_receipt}" camera_source kind)
string(JSON authored_camera_receipt_id GET "${authored_camera_receipt}" camera_source object_id)
string(JSON authored_camera_identity GET "${authored_camera_receipt}" camera_source identity)
string(JSON authored_camera_revision GET "${authored_camera_receipt}" camera_source revision)
string(JSON authored_camera_position_y GET "${authored_camera_receipt}" camera_position 1)
string(JSON authored_camera_position_z GET "${authored_camera_receipt}" camera_position 2)
string(JSON authored_camera_fov GET "${authored_camera_receipt}" vertical_fov)
if(NOT authored_camera_kind STREQUAL "authored_project" OR
   NOT authored_camera_receipt_id STREQUAL authored_camera_id OR
   NOT authored_camera_identity STREQUAL "fixture-render-camera" OR
   NOT authored_camera_revision STREQUAL "1" OR
   NOT authored_camera_position_y STREQUAL "2" OR
   NOT authored_camera_position_z STREQUAL "6" OR
   authored_camera_fov LESS 0.899999 OR authored_camera_fov GREATER 0.900001)
    message(FATAL_ERROR "Authored camera output receipt lost its source or saved projection")
endif()
file(READ "${authored_camera_project}.orthographic-camera-id"
    orthographic_camera_id)
string(STRIP "${orthographic_camera_id}" orthographic_camera_id)
set(orthographic_camera_output "${root}/orthographic-camera.bmp")
execute_process(COMMAND "${RENDER}" "${authored_camera_project}"
    "${orthographic_camera_output}" --identity fixture-orthographic-camera
    --shaders "${SHADERS}" --authored-camera "${orthographic_camera_id}"
    --size 640 400
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Orthographic camera capture refused: ${output}${error}")
endif()
file(READ "${orthographic_camera_output}.receipt.json" orthographic_camera_receipt)
string(JSON orthographic_camera_kind GET
    "${orthographic_camera_receipt}" camera_source kind)
string(JSON orthographic_camera_projection GET
    "${orthographic_camera_receipt}" camera_projection)
string(JSON orthographic_camera_height GET
    "${orthographic_camera_receipt}" orthographic_height)
string(JSON orthographic_camera_receipt_id GET
    "${orthographic_camera_receipt}" camera_source object_id)
if(NOT orthographic_camera_kind STREQUAL "authored_project" OR
   NOT orthographic_camera_projection STREQUAL "orthographic" OR
   NOT orthographic_camera_height STREQUAL "3.5" OR
   NOT orthographic_camera_receipt_id STREQUAL orthographic_camera_id)
    message(FATAL_ERROR "Orthographic camera receipt lost its source or projection extent")
endif()
execute_process(COMMAND "${RENDER}" "${authored_camera_project}"
    "${root}/conflicting-camera-sources.bmp" --identity fixture-authored-camera
    --shaders "${SHADERS}" --authored-camera "${authored_camera_id}"
    --camera 0 0 5 0 0 0 --size 640 400
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(result EQUAL 0 OR EXISTS "${root}/conflicting-camera-sources.bmp" OR
   EXISTS "${root}/conflicting-camera-sources.bmp.receipt.json")
    message(FATAL_ERROR "Explicit and authored camera sources were not rejected as ambiguous")
endif()
execute_process(COMMAND "${RENDER}" "${authored_camera_project}"
    "${root}/authored-camera-override.bmp" --identity fixture-authored-camera
    --shaders "${SHADERS}" --authored-camera "${authored_camera_id}"
    --vertical-fov 0.7 --size 640 400
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(result EQUAL 0 OR EXISTS "${root}/authored-camera-override.bmp" OR
   EXISTS "${root}/authored-camera-override.bmp.receipt.json")
    message(FATAL_ERROR "Authored camera projection was silently overridden")
endif()
execute_process(COMMAND "${RENDER}" "${authored_camera_project}"
    "${root}/authored-camera-projection-override.bmp"
    --identity fixture-authored-camera --shaders "${SHADERS}"
    --authored-camera "${authored_camera_id}" --projection orthographic
    --size 640 400
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(result EQUAL 0 OR
   EXISTS "${root}/authored-camera-projection-override.bmp" OR
   EXISTS "${root}/authored-camera-projection-override.bmp.receipt.json")
    message(FATAL_ERROR "Authored camera projection mode was silently overridden")
endif()
file(GLOB staging_entries LIST_DIRECTORIES TRUE "${root}/*.pending-*")
if(staging_entries)
    message(FATAL_ERROR "Successful scene captures left staging entries: ${staging_entries}")
endif()
set(environment_sh "${root}/environment.sh")
file(WRITE "${environment_sh}"
    "CARTO_DIFFUSE_IRRADIANCE_SH_V1\n"
    "3.544907701811032 3.544907701811032 3.544907701811032\n"
    "0 0 0\n0 0 0\n0 0 0\n0 0 0\n0 0 0\n0 0 0\n0 0 0\n0 0 0\n")
file(SHA256 "${environment_sh}" expected_environment_digest)
execute_process(COMMAND "${RENDER}" "${root}/room.carto" "${root}/environment.bmp"
    ${arguments} --diffuse-irradiance-sh "${environment_sh}"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Diffuse environment capture refused: ${output}${error}")
endif()
file(READ "${root}/environment.bmp.receipt.json" environment_receipt)
string(JSON actual_environment_digest GET "${environment_receipt}" environment_digest)
string(JSON environment_color_space_type TYPE "${environment_receipt}" environment_color_space)
string(JSON environment_request_digest GET "${environment_receipt}" request_digest)
if(NOT actual_environment_digest STREQUAL expected_environment_digest OR
   NOT environment_color_space_type STREQUAL "NULL" OR
   environment_request_digest STREQUAL first_request)
    message(FATAL_ERROR "Environment receipt or request identity does not bind the supplied SH file")
endif()
set(environment_image "${root}/environment.tga")
execute_process(COMMAND "${FIXTURE}" write-equirectangular "${environment_image}"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Environment image fixture refused: ${output}${error}")
endif()
file(SHA256 "${environment_image}" expected_environment_image_digest)
execute_process(COMMAND "${RENDER}" "${root}/room.carto" "${root}/environment-image.bmp"
    ${arguments} --environment-equirectangular "${environment_image}"
    --environment-color-space linear
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Equirectangular environment capture refused: ${output}${error}")
endif()
file(READ "${root}/environment-image.bmp.receipt.json" environment_image_receipt)
string(JSON actual_environment_image_digest GET "${environment_image_receipt}" environment_digest)
string(JSON environment_image_color_space GET "${environment_image_receipt}" environment_color_space)
string(JSON environment_image_request_digest GET "${environment_image_receipt}" request_digest)
if(NOT actual_environment_image_digest STREQUAL expected_environment_image_digest OR
   NOT environment_image_color_space STREQUAL "linear" OR
   environment_image_request_digest STREQUAL first_request)
    message(FATAL_ERROR "Equirectangular source digest or request identity was not preserved")
endif()
execute_process(COMMAND "${RENDER}" "${root}/room.carto" "${root}/missing-color-space.bmp"
    ${arguments} --environment-equirectangular "${environment_image}"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(result EQUAL 0 OR EXISTS "${root}/missing-color-space.bmp" OR
   EXISTS "${root}/missing-color-space.bmp.receipt.json")
    message(FATAL_ERROR "Environment image without explicit color space was not refused")
endif()
execute_process(COMMAND "${RENDER}" "${root}/room.carto" "${root}/conflicting-environment.bmp"
    ${arguments} --diffuse-irradiance-sh "${environment_sh}"
    --environment-equirectangular "${environment_image}"
    --environment-color-space linear
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(result EQUAL 0 OR EXISTS "${root}/conflicting-environment.bmp" OR
   EXISTS "${root}/conflicting-environment.bmp.receipt.json")
    message(FATAL_ERROR "Conflicting environment sources were not refused")
endif()
execute_process(COMMAND "${RENDER}" "${root}/room.carto" "${root}/first.bmp" ${arguments}
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(result EQUAL 0)
    message(FATAL_ERROR "Existing capture was overwritten")
endif()
file(SHA256 "${root}/first.bmp" after_refusal)
if(NOT after_refusal STREQUAL first_bmp)
    message(FATAL_ERROR "Overwrite refusal changed the existing artifact")
endif()
file(WRITE "${root}/receipt-conflict.bmp.receipt.json" "preserve receipt")
execute_process(COMMAND "${RENDER}" "${root}/room.carto" "${root}/receipt-conflict.bmp" ${arguments}
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(result EQUAL 0 OR EXISTS "${root}/receipt-conflict.bmp")
    message(FATAL_ERROR "A pre-existing receipt did not block capture publication")
endif()
file(READ "${root}/receipt-conflict.bmp.receipt.json" preserved_receipt)
if(NOT preserved_receipt STREQUAL "preserve receipt")
    message(FATAL_ERROR "Receipt-conflict refusal changed the existing receipt")
endif()
execute_process(COMMAND "${RENDER}" "${root}/room.carto" "${root}/invalid.bmp"
    --identity fixture-room --shaders "${SHADERS}" --camera 1 1 1 1 1 1 --size 640 400
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(result EQUAL 0 OR EXISTS "${root}/invalid.bmp" OR EXISTS "${root}/invalid.bmp.receipt.json")
    message(FATAL_ERROR "Invalid camera was not refused before artifact publication")
endif()
function(expect_camera_refusal label)
    set(output_path "${root}/${label}.bmp")
    execute_process(COMMAND "${RENDER}" "${root}/room.carto" "${output_path}" ${ARGN}
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(result EQUAL 0 OR EXISTS "${output_path}" OR EXISTS "${output_path}.receipt.json")
        message(FATAL_ERROR "${label} camera input was not refused before publication")
    endif()
endfunction()
expect_camera_refusal(duplicate-camera-up ${arguments} --camera-up 0 1 0)
expect_camera_refusal(duplicate-camera ${arguments} --camera 0 0 1 0 0 0)
expect_camera_refusal(duplicate-vertical-fov ${arguments} --vertical-fov 0.75)
expect_camera_refusal(duplicate-clip-range ${arguments} --clip-range 0.1 250)
expect_camera_refusal(parallel-camera-up
    --identity fixture-room --shaders "${SHADERS}"
    --camera 0 0 1 0 0 0 --camera-up 0 0 4 --size 640 400)
expect_camera_refusal(invalid-camera-fov
    --identity fixture-room --shaders "${SHADERS}"
    --camera 5 3.1 5.5 -0.3 0.8 -1 --vertical-fov 0 --size 640 400)
expect_camera_refusal(invalid-camera-clip-range
    --identity fixture-room --shaders "${SHADERS}"
    --camera 5 3.1 5.5 -0.3 0.8 -1 --clip-range 2 1 --size 640 400)
execute_process(COMMAND "${RENDER}" "${root}/room.carto" "${root}/duplicate-environment.bmp"
    ${arguments} --diffuse-irradiance-sh first.sh --diffuse-irradiance-sh second.sh
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(result EQUAL 0 OR EXISTS "${root}/duplicate-environment.bmp" OR
   EXISTS "${root}/duplicate-environment.bmp.receipt.json")
    message(FATAL_ERROR "Duplicate diffuse irradiance inputs were not refused before publication")
endif()
execute_process(COMMAND "${FIXTURE}" --write-parameterized-fixture
    "${root}/parameterized.carto"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Parameterized graph fixture refused: ${output}${error}")
endif()
file(SHA256 "${root}/parameterized.carto" parameterized_source_before)
execute_process(COMMAND "${RENDER}" "${root}/parameterized.carto"
    "${root}/parameterized.bmp" ${arguments}
    --graph-parameter fixture/parameterized roughness scalar 0.35
    --graph-surface-input fixture/parameterized authored_mask scalar 0.2
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Explicit material graph input refused: ${output}${error}")
endif()
file(READ "${root}/parameterized.bmp.receipt.json" graph_receipt)
string(JSON graph_input_count LENGTH "${graph_receipt}" material_graph_inputs)
string(JSON graph_input_identity GET "${graph_receipt}" material_graph_inputs 0 identity)
string(JSON graph_input_revision GET "${graph_receipt}" material_graph_inputs 0 revision)
string(JSON graph_input_source_digest GET "${graph_receipt}" material_graph_inputs 0 graph_digest)
string(JSON graph_input_values_digest GET "${graph_receipt}" material_graph_inputs 0 inputs_digest)
string(JSON graph_input_request_digest GET "${graph_receipt}" request_digest)
string(LENGTH "${graph_input_source_digest}" graph_input_source_digest_length)
string(LENGTH "${graph_input_values_digest}" graph_input_values_digest_length)
if(NOT graph_input_count EQUAL 1 OR
   NOT graph_input_identity STREQUAL "fixture/parameterized" OR
   NOT graph_input_revision EQUAL 1 OR
   NOT graph_input_source_digest_length EQUAL 64 OR
   NOT graph_input_values_digest_length EQUAL 64)
    message(FATAL_ERROR "Capture receipt lost the explicit graph source or input digest")
endif()
execute_process(COMMAND "${RENDER}" "${root}/parameterized.carto"
    "${root}/parameterized-repeat.bmp" ${arguments}
    --graph-parameter fixture/parameterized roughness scalar 0.35
    --graph-surface-input fixture/parameterized authored_mask scalar 0.4
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Changed explicit material graph input refused: ${output}${error}")
endif()
file(READ "${root}/parameterized-repeat.bmp.receipt.json" changed_graph_receipt)
string(JSON changed_graph_request_digest GET "${changed_graph_receipt}" request_digest)
string(JSON changed_graph_inputs_digest GET "${changed_graph_receipt}" material_graph_inputs 0 inputs_digest)
if(graph_input_request_digest STREQUAL changed_graph_request_digest OR
   graph_input_values_digest STREQUAL changed_graph_inputs_digest)
    message(FATAL_ERROR "Request and receipt identities do not bind graph input values")
endif()
execute_process(COMMAND "${RENDER}" "${root}/parameterized.carto"
    "${root}/missing-graph-input.bmp" ${arguments}
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(result EQUAL 0 OR EXISTS "${root}/missing-graph-input.bmp" OR
   EXISTS "${root}/missing-graph-input.bmp.receipt.json")
    message(FATAL_ERROR "Missing required graph input was not refused before publication")
endif()
execute_process(COMMAND "${RENDER}" "${root}/parameterized.carto"
    "${root}/unused-graph-input.bmp" ${arguments}
    --graph-parameter fixture/parameterized roughness scalar 0.35
    --graph-surface-input fixture/parameterized authored_mask scalar 0.2
    --graph-parameter fixture/parameterized unrelated scalar 0.2
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(result EQUAL 0 OR EXISTS "${root}/unused-graph-input.bmp" OR
   EXISTS "${root}/unused-graph-input.bmp.receipt.json")
    message(FATAL_ERROR "Output-unreachable graph input was not refused before publication")
endif()
execute_process(COMMAND "${RENDER}" "${root}/parameterized.carto"
    "${root}/foreign-graph-input.bmp" ${arguments}
    --graph-parameter foreign/graph roughness scalar 0.35
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(result EQUAL 0 OR EXISTS "${root}/foreign-graph-input.bmp" OR
   EXISTS "${root}/foreign-graph-input.bmp.receipt.json")
    message(FATAL_ERROR "Foreign graph input source was not refused before publication")
endif()
execute_process(COMMAND "${RENDER}" "${root}/parameterized.carto"
    "${root}/mistyped-graph-input.bmp" ${arguments}
    --graph-parameter fixture/parameterized roughness color 0.35 0.35 0.35 1.0
    --graph-surface-input fixture/parameterized authored_mask scalar 0.2
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(result EQUAL 0 OR EXISTS "${root}/mistyped-graph-input.bmp" OR
   EXISTS "${root}/mistyped-graph-input.bmp.receipt.json")
    message(FATAL_ERROR "Mistyped graph parameter was not refused before publication")
endif()
execute_process(COMMAND "${RENDER}" "${root}/parameterized.carto"
    "${root}/duplicate-graph-input.bmp" ${arguments}
    --graph-parameter fixture/parameterized roughness scalar 0.35
    --graph-parameter fixture/parameterized roughness scalar 0.4
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(result EQUAL 0 OR EXISTS "${root}/duplicate-graph-input.bmp" OR
   EXISTS "${root}/duplicate-graph-input.bmp.receipt.json")
    message(FATAL_ERROR "Duplicate graph parameter values were not refused before publication")
endif()
file(SHA256 "${root}/parameterized.carto" parameterized_source_after)
if(NOT parameterized_source_before STREQUAL parameterized_source_after)
    message(FATAL_ERROR "Graph input rendering changed the authoritative project")
endif()
file(SHA256 "${root}/room.carto" source_after)
if(NOT source_before STREQUAL source_after)
    message(FATAL_ERROR "Rendering changed the authoritative project")
endif()
message(STATUS "PASS saved-project GPU capture/repeat/receipt/overwrite/camera/source boundaries")
message(STATUS "Actual native capture: ${root}/first.bmp")
