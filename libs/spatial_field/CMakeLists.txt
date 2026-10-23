add_library(carto_spatial_field STATIC
    src/scalar_field.cpp
)

target_include_directories(carto_spatial_field
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(carto_spatial_field PUBLIC carto_assets carto_core)
carto_target(carto_spatial_field)

install(TARGETS carto_spatial_field EXPORT CartographerTargets)
install(DIRECTORY include/ DESTINATION include)
