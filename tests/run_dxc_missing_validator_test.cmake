if(NOT EXISTS "${CARTO_DXC_TEST_COMPILER}" OR NOT EXISTS "${CARTO_DXC_TEST_EXECUTABLE}")
    message(FATAL_ERROR "DXC negative-test compiler or executable is unavailable")
endif()
file(MAKE_DIRECTORY "${CARTO_DXC_TEST_DIRECTORY}")
if(EXISTS "${CARTO_DXC_TEST_DIRECTORY}/dxil.dll")
    message(FATAL_ERROR "DXC negative-test directory unexpectedly contains a validator")
endif()
file(COPY_FILE "${CARTO_DXC_TEST_COMPILER}" "${CARTO_DXC_TEST_DIRECTORY}/dxcompiler.dll" ONLY_IF_DIFFERENT)
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "CARTO_DXCOMPILER_PATH=${CARTO_DXC_TEST_DIRECTORY}/dxcompiler.dll"
        "${CARTO_DXC_TEST_EXECUTABLE}" --expect-missing-validator
    RESULT_VARIABLE CARTO_DXC_TEST_RESULT
    OUTPUT_VARIABLE CARTO_DXC_TEST_OUTPUT
    ERROR_VARIABLE CARTO_DXC_TEST_ERROR)
if(NOT CARTO_DXC_TEST_RESULT EQUAL 0)
    message(FATAL_ERROR "Missing-validator test failed: ${CARTO_DXC_TEST_OUTPUT}${CARTO_DXC_TEST_ERROR}")
endif()
message(STATUS "${CARTO_DXC_TEST_OUTPUT}")
