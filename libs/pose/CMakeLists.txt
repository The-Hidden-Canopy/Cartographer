add_library(carto_pose STATIC
    src/contact.cpp
    src/constraints.cpp
    src/controls.cpp
    src/fk.cpp
    src/ik.cpp
    src/ik_fk_match.cpp
    src/pose_library.cpp
    src/rotation.cpp
)

target_include_directories(carto_pose
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(carto_pose
    PUBLIC
        carto_core
        carto_kinematic
        carto_character
)
carto_target(carto_pose)

install(TARGETS carto_pose EXPORT CartographerTargets)
install(DIRECTORY include/ DESTINATION include)
