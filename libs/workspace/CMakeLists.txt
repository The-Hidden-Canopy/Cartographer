add_library(carto_workspace STATIC
    src/session.cpp
    src/store.cpp
    src/commands.cpp
    src/controller.cpp
    src/proposals.cpp
)

target_include_directories(carto_workspace
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(carto_workspace
    PUBLIC
        carto_core
        carto_character
        carto_kinematic
        carto_temporal
        carto_project
        carto_journal
)
carto_target(carto_workspace)

install(TARGETS carto_workspace EXPORT CartographerTargets)
install(DIRECTORY include/ DESTINATION include)
