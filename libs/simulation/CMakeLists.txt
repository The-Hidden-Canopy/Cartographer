add_library(carto_simulation STATIC
    src/simulation.cpp
    src/game.cpp
)

target_include_directories(carto_simulation
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(carto_simulation PUBLIC carto_core carto_scene)
carto_target(carto_simulation)

install(TARGETS carto_simulation EXPORT CartographerTargets)
install(DIRECTORY include/ DESTINATION include)
