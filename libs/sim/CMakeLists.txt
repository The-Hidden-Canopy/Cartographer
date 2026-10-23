add_library(carto_sim STATIC
    src/cache.cpp
    src/collision_filter.cpp
    src/groom.cpp
)

target_include_directories(carto_sim
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(carto_sim
    PUBLIC
        carto_core
        carto_geometry
        carto_character
)
carto_target(carto_sim)

install(TARGETS carto_sim EXPORT CartographerTargets)
install(DIRECTORY include/ DESTINATION include)
