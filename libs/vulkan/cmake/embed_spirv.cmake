if(NOT DEFINED VERTEX_INPUT OR NOT DEFINED FRAGMENT_INPUT OR NOT DEFINED OUTPUT)
    message(FATAL_ERROR "embed_spirv.cmake requires VERTEX_INPUT, FRAGMENT_INPUT, and OUTPUT")
endif()

function(read_spirv_words input_path symbol_name)
    file(READ "${input_path}" bytes HEX)
    string(LENGTH "${bytes}" byte_hex_length)
    math(EXPR remainder "${byte_hex_length} % 8")
    if(NOT remainder EQUAL 0)
        message(FATAL_ERROR "SPIR-V input is not aligned to 32-bit words: ${input_path}")
    endif()

    math(EXPR word_count "${byte_hex_length} / 8")
    set(words "")
    if(word_count GREATER 0)
        math(EXPR last_offset "${byte_hex_length} - 8")
        foreach(offset RANGE 0 ${last_offset} 8)
            string(SUBSTRING "${bytes}" ${offset} 2 byte0)
            math(EXPR byte1_offset "${offset} + 2")
            math(EXPR byte2_offset "${offset} + 4")
            math(EXPR byte3_offset "${offset} + 6")
            string(SUBSTRING "${bytes}" ${byte1_offset} 2 byte1)
            string(SUBSTRING "${bytes}" ${byte2_offset} 2 byte2)
            string(SUBSTRING "${bytes}" ${byte3_offset} 2 byte3)
            string(APPEND words "0x${byte3}${byte2}${byte1}${byte0}U,")
        endforeach()
    endif()

    set(${symbol_name}_COUNT "${word_count}" PARENT_SCOPE)
    set(${symbol_name}_WORDS "${words}" PARENT_SCOPE)
endfunction()

read_spirv_words("${VERTEX_INPUT}" triangle_vertex)
read_spirv_words("${FRAGMENT_INPUT}" triangle_fragment)

get_filename_component(output_directory "${OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${output_directory}")
file(WRITE "${OUTPUT}" "#pragma once\n\n#include <array>\n#include <cstdint>\n\nnamespace carto::vulkan::shaders {\n\n")
file(APPEND "${OUTPUT}"
    "inline constexpr std::array<std::uint32_t, ${triangle_vertex_COUNT}> triangle_vertex = {\n"
    "${triangle_vertex_WORDS}\n};\n\n"
    "inline constexpr std::array<std::uint32_t, ${triangle_fragment_COUNT}> triangle_fragment = {\n"
    "${triangle_fragment_WORDS}\n};\n\n"
    "} // namespace carto::vulkan::shaders\n")
