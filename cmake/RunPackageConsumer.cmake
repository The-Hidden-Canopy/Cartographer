foreach(required IN ITEMS
        CARTO_BINARY_DIR
        CARTO_CONSUMER_SOURCE_DIR
        CARTO_GENERATOR
        CARTO_CTEST_COMMAND)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "RunPackageConsumer requires ${required}")
    endif()
endforeach()

if(NOT DEFINED CARTO_CONFIG)
    set(CARTO_CONFIG "")
endif()
if(CARTO_CONFIG STREQUAL "" AND DEFINED CARTO_BUILD_TYPE AND
        NOT CARTO_BUILD_TYPE STREQUAL "")
    set(CARTO_CONFIG "${CARTO_BUILD_TYPE}")
endif()
if(NOT CARTO_CONFIG STREQUAL "" AND
        NOT CARTO_CONFIG MATCHES "^[A-Za-z0-9_.-]+$")
    message(FATAL_ERROR "Cartographer package consumer configuration is unsafe")
endif()

set(smoke_config "${CARTO_CONFIG}")
if(smoke_config STREQUAL "")
    set(smoke_config "NoConfig")
endif()
set(smoke_root "${CARTO_BINARY_DIR}/package-consumer-smoke/${smoke_config}")
set(install_root "${smoke_root}/install")
set(consumer_build "${smoke_root}/build")
file(REMOVE_RECURSE "${smoke_root}")

set(install_command
    "${CMAKE_COMMAND}" --install "${CARTO_BINARY_DIR}" --prefix "${install_root}")
if(NOT CARTO_CONFIG STREQUAL "")
    list(APPEND install_command --config "${CARTO_CONFIG}")
endif()
execute_process(
    COMMAND ${install_command}
    RESULT_VARIABLE install_result
    OUTPUT_VARIABLE install_output
    ERROR_VARIABLE install_error
)
if(NOT install_result EQUAL 0)
    message(FATAL_ERROR
        "Cartographer package install failed (${install_result})\n"
        "${install_output}\n${install_error}")
endif()

set(configure_command
    "${CMAKE_COMMAND}"
    -S "${CARTO_CONSUMER_SOURCE_DIR}"
    -B "${consumer_build}"
    -G "${CARTO_GENERATOR}"
    "-DCMAKE_PREFIX_PATH=${install_root}"
)
if(NOT CARTO_CONFIG STREQUAL "")
    list(APPEND configure_command "-DCMAKE_BUILD_TYPE=${CARTO_CONFIG}")
endif()
if(DEFINED CARTO_GENERATOR_PLATFORM AND NOT CARTO_GENERATOR_PLATFORM STREQUAL "")
    list(APPEND configure_command -A "${CARTO_GENERATOR_PLATFORM}")
endif()
if(DEFINED CARTO_GENERATOR_TOOLSET AND NOT CARTO_GENERATOR_TOOLSET STREQUAL "")
    list(APPEND configure_command -T "${CARTO_GENERATOR_TOOLSET}")
endif()
execute_process(
    COMMAND ${configure_command}
    RESULT_VARIABLE configure_result
    OUTPUT_VARIABLE configure_output
    ERROR_VARIABLE configure_error
)
if(NOT configure_result EQUAL 0)
    message(FATAL_ERROR
        "Cartographer package consumer configure failed (${configure_result})\n"
        "${configure_output}\n${configure_error}")
endif()

set(build_command "${CMAKE_COMMAND}" --build "${consumer_build}")
if(NOT CARTO_CONFIG STREQUAL "")
    list(APPEND build_command --config "${CARTO_CONFIG}")
endif()
execute_process(
    COMMAND ${build_command}
    RESULT_VARIABLE build_result
    OUTPUT_VARIABLE build_output
    ERROR_VARIABLE build_error
)
if(NOT build_result EQUAL 0)
    message(FATAL_ERROR
        "Cartographer package consumer build failed (${build_result})\n"
        "${build_output}\n${build_error}")
endif()

set(test_command
    "${CARTO_CTEST_COMMAND}"
    --test-dir "${consumer_build}"
    --output-on-failure)
if(NOT CARTO_CONFIG STREQUAL "")
    list(APPEND test_command -C "${CARTO_CONFIG}")
endif()
execute_process(
    COMMAND ${test_command}
    RESULT_VARIABLE test_result
    OUTPUT_VARIABLE test_output
    ERROR_VARIABLE test_error
)
if(NOT test_result EQUAL 0)
    message(FATAL_ERROR
        "Cartographer package consumer test failed (${test_result})\n"
        "${test_output}\n${test_error}")
endif()

message(STATUS "Cartographer installed-package consumer passed")
