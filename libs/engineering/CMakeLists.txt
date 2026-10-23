add_library(carto_engineering STATIC
    src/authoring.cpp
    src/cad_provider.cpp
    src/drafting.cpp
    src/topology_correspondence.cpp
)

target_include_directories(carto_engineering
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(carto_engineering PUBLIC carto_core carto_assets)
carto_target(carto_engineering)

install(TARGETS carto_engineering EXPORT CartographerTargets)
install(DIRECTORY include/ DESTINATION include)
