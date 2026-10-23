add_library(carto_weights STATIC
    src/auto_weight.cpp
    src/inverse.cpp
    src/lowering.cpp
    src/paint.cpp
    src/transfer.cpp
)

target_include_directories(carto_weights
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(carto_weights
    PUBLIC
        carto_core
        carto_geometry
        carto_kinematic
        carto_character
)
carto_target(carto_weights)

install(TARGETS carto_weights EXPORT CartographerTargets)
install(DIRECTORY include/ DESTINATION include)
