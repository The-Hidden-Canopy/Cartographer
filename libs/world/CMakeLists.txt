add_library(carto_world STATIC
    src/model.cpp
)

target_include_directories(carto_world
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(carto_world PUBLIC carto_core carto_assets)
carto_target(carto_world)

install(TARGETS carto_world EXPORT CartographerTargets)
install(DIRECTORY include/ DESTINATION include)
