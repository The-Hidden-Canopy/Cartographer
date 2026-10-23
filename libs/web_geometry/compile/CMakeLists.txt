add_library(carto_web_geometry_compile STATIC
    src/compiler.cpp
)

target_include_directories(carto_web_geometry_compile
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(carto_web_geometry_compile
    PUBLIC
        carto_web_geometry_core
        carto_attributes
        carto_geometry
)

carto_target(carto_web_geometry_compile)

install(TARGETS carto_web_geometry_compile EXPORT CartographerTargets)
install(DIRECTORY include/ DESTINATION include)
