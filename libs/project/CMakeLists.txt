add_library(carto_project STATIC
    src/file_lock.cpp
    src/package.cpp
    src/project.cpp
    src/recovery.cpp
    src/transaction.cpp
)

target_include_directories(carto_project
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(carto_project PUBLIC carto_core carto_scene carto_geometry carto_assets carto_journal carto_eval)
carto_target(carto_project)

install(TARGETS carto_project EXPORT CartographerTargets)
install(DIRECTORY include/ DESTINATION include)
