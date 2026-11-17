# A fresh prefix ensures a previously installed helper cannot hide omissions.
string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef suffix)
set(prefix "${BUILD_DIR}/installed-shader-tests/${suffix}")
execute_process(COMMAND "${CMAKE_COMMAND}" --install "${BUILD_DIR}"
    --prefix "${prefix}" --component CartographerShaders --config "${CONFIG}"
    RESULT_VARIABLE installed)
if(NOT installed EQUAL 0)
    message(FATAL_ERROR "Shader installation failed: ${installed}")
endif()
set(shaders "${prefix}/share/cartographer/shaders")
if(NOT EXISTS "${shaders}/cartographer_authored_anisotropy.hlsli")
    message(FATAL_ERROR "Installed shaders lack authored anisotropy helper")
endif()
execute_process(COMMAND "${FIXTURE}" --correlated-smith-only "${shaders}"
    RESULT_VARIABLE rendered)
if(NOT rendered EQUAL 0)
    message(FATAL_ERROR "Installed correlated-Smith scene test failed: ${rendered}")
endif()
