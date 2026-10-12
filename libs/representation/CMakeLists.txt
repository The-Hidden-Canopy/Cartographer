add_library(carto_representation STATIC
    src/continuity.cpp
    src/error_envelope.cpp
    src/family.cpp
    src/runtime_evidence.cpp
    src/selection.cpp
)

target_include_directories(carto_representation
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(carto_representation
    PUBLIC
        carto_core
        carto_surface_core
        carto_web_geometry_core
)
carto_target(carto_representation)

install(TARGETS carto_representation EXPORT CartographerTargets)
install(DIRECTORY include/ DESTINATION include)
