add_library(carto_kinematic STATIC
    src/graph.cpp
    src/identity.cpp
    src/limits.cpp
    src/lowering.cpp
    src/skin.cpp
)

target_include_directories(carto_kinematic
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(carto_kinematic
    PUBLIC
        carto_core
        carto_scene
        carto_assets
)
carto_target(carto_kinematic)

install(TARGETS carto_kinematic EXPORT CartographerTargets)
install(DIRECTORY include/ DESTINATION include)
