add_library(carto_face STATIC
    src/expression.cpp
    src/lipsync.cpp
    src/phoneme.cpp
    src/receipt.cpp
    src/region.cpp
)

target_include_directories(carto_face
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(carto_face
    PUBLIC
        carto_core
        carto_character
        carto_temporal
)
carto_target(carto_face)

install(TARGETS carto_face EXPORT CartographerTargets)
install(DIRECTORY include/ DESTINATION include)
