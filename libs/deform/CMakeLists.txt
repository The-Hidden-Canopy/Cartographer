add_library(carto_deform STATIC
    src/affine.cpp
    src/corrective.cpp
    src/delta_smooth.cpp
    src/dual_quaternion.cpp
    src/graph.cpp
    src/inverse_hypothesis.cpp
    src/morph.cpp
    src/receipt.cpp
    src/skin.cpp
)

target_include_directories(carto_deform
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(carto_deform
    PUBLIC
        carto_core
        carto_geometry
        carto_kinematic
        carto_character
)
carto_target(carto_deform)

install(TARGETS carto_deform EXPORT CartographerTargets)
install(DIRECTORY include/ DESTINATION include)
