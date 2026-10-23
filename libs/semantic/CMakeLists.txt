add_library(carto_semantic STATIC
    src/inspection.cpp
    src/lowering.cpp
    src/migration.cpp
    src/package.cpp
    src/query.cpp
    src/schema.cpp
    src/sidecar.cpp
)

target_include_directories(carto_semantic
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(carto_semantic PUBLIC carto_core carto_assets carto_world)
carto_target(carto_semantic)

install(TARGETS carto_semantic EXPORT CartographerTargets)
install(DIRECTORY include/ DESTINATION include)
