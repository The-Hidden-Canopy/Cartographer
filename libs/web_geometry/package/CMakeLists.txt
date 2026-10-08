add_library(carto_web_geometry_package STATIC
    src/page_payload.cpp
    src/package.cpp
)

target_include_directories(carto_web_geometry_package
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(carto_web_geometry_package
    PUBLIC
        carto_web_geometry_core
)

carto_target(carto_web_geometry_package)

install(TARGETS carto_web_geometry_package EXPORT CartographerTargets)
install(DIRECTORY include/ DESTINATION include)
