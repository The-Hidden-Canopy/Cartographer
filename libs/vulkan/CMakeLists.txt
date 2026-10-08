find_program(CARTO_GLSLC_EXECUTABLE
    NAMES glslc
    HINTS "$ENV{VULKAN_SDK}/Bin"
)
if(NOT CARTO_GLSLC_EXECUTABLE)
    message(FATAL_ERROR
        "CARTO_ENABLE_VULKAN=ON requires glslc from the selected Vulkan SDK")
endif()

set(CARTO_VULKAN_SHADER_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(CARTO_VULKAN_VERTEX_SPIRV "${CARTO_VULKAN_SHADER_OUTPUT_DIR}/cartographer_triangle.vert.spv")
set(CARTO_VULKAN_FRAGMENT_SPIRV "${CARTO_VULKAN_SHADER_OUTPUT_DIR}/cartographer_triangle.frag.spv")
set(CARTO_VULKAN_SHADER_HEADER "${CARTO_VULKAN_SHADER_OUTPUT_DIR}/carto_vulkan_shaders.hpp")

add_custom_command(
    OUTPUT "${CARTO_VULKAN_VERTEX_SPIRV}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CARTO_VULKAN_SHADER_OUTPUT_DIR}"
    COMMAND "${CARTO_GLSLC_EXECUTABLE}" --target-env=vulkan1.1
        -o "${CARTO_VULKAN_VERTEX_SPIRV}"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/triangle.vert"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/triangle.vert"
    VERBATIM
)

add_custom_command(
    OUTPUT "${CARTO_VULKAN_FRAGMENT_SPIRV}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CARTO_VULKAN_SHADER_OUTPUT_DIR}"
    COMMAND "${CARTO_GLSLC_EXECUTABLE}" --target-env=vulkan1.1
        -o "${CARTO_VULKAN_FRAGMENT_SPIRV}"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/triangle.frag"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/triangle.frag"
    VERBATIM
)

add_custom_command(
    OUTPUT "${CARTO_VULKAN_SHADER_HEADER}"
    COMMAND ${CMAKE_COMMAND}
        -DVERTEX_INPUT:FILEPATH=${CARTO_VULKAN_VERTEX_SPIRV}
        -DFRAGMENT_INPUT:FILEPATH=${CARTO_VULKAN_FRAGMENT_SPIRV}
        -DOUTPUT:FILEPATH=${CARTO_VULKAN_SHADER_HEADER}
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_spirv.cmake"
    DEPENDS
        "${CARTO_VULKAN_VERTEX_SPIRV}"
        "${CARTO_VULKAN_FRAGMENT_SPIRV}"
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_spirv.cmake"
    VERBATIM
)

add_custom_target(carto_vulkan_shaders
    DEPENDS "${CARTO_VULKAN_SHADER_HEADER}"
)

add_library(carto_vulkan STATIC
    src/runtime.cpp
)

add_dependencies(carto_vulkan carto_vulkan_shaders)

target_include_directories(carto_vulkan
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        "${CARTO_VULKAN_SHADER_OUTPUT_DIR}"
)

target_link_libraries(carto_vulkan
    PUBLIC
        carto_core
        carto_geometry
        carto_render
        Vulkan::Vulkan
)

carto_target(carto_vulkan)

install(TARGETS carto_vulkan EXPORT CartographerTargets)
install(DIRECTORY include/ DESTINATION include)
