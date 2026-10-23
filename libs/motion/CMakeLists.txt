add_library(carto_motion STATIC
    src/curve.cpp
    src/curve_edit.cpp
    src/layers.cpp
    src/mocap.cpp
    src/performance.cpp
    src/retarget.cpp
)

target_include_directories(carto_motion
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(carto_motion
    PUBLIC
        carto_core
        carto_temporal
        carto_kinematic
        carto_character
)
carto_target(carto_motion)

install(TARGETS carto_motion EXPORT CartographerTargets)
install(DIRECTORY include/ DESTINATION include)
