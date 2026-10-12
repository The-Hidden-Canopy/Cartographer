add_library(carto_biological STATIC
    src/authoring.cpp
)

target_include_directories(carto_biological
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(carto_biological
    PUBLIC
        carto_core
        carto_assets
        carto_kinematic
        carto_representation
)
carto_target(carto_biological)

install(TARGETS carto_biological EXPORT CartographerTargets)
install(DIRECTORY include/ DESTINATION include)
