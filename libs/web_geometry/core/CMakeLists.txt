add_library(carto_web_geometry_core STATIC
    src/diagnostics.cpp
    src/overlay.cpp
    src/page_interest.cpp
    src/types.cpp
)

target_include_directories(carto_web_geometry_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../geometry/mesh/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(carto_web_geometry_core
    PUBLIC
        carto_assets
)

carto_target(carto_web_geometry_core)

install(TARGETS carto_web_geometry_core EXPORT CartographerTargets)
install(DIRECTORY include/ DESTINATION include)
