add_library(carto_production_authoring STATIC
    src/authoring.cpp
)

target_include_directories(carto_production_authoring
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(carto_production_authoring
    PUBLIC
        carto_core
        carto_assets
        carto_biological
)
carto_target(carto_production_authoring)

install(TARGETS carto_production_authoring EXPORT CartographerTargets)
install(DIRECTORY include/ DESTINATION include)
