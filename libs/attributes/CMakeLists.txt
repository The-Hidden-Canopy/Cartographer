add_library(carto_attributes STATIC
    src/attributes.cpp
    src/provenance.cpp
    src/uv.cpp
)

target_include_directories(carto_attributes
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(carto_attributes PUBLIC carto_core)
carto_target(carto_attributes)

install(TARGETS carto_attributes EXPORT CartographerTargets)
install(DIRECTORY include/ DESTINATION include)
