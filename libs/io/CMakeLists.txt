add_library(carto_io STATIC
    src/builtin_provider.cpp
    src/obj.cpp
    src/ply.cpp
    src/stl.cpp
    src/gltf_export.cpp
)

target_include_directories(carto_io
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(carto_io PUBLIC carto_core carto_geometry carto_project carto_providers)
carto_target(carto_io)

install(TARGETS carto_io EXPORT CartographerTargets)
install(DIRECTORY include/ DESTINATION include)
