add_library(carto_physical STATIC
    src/condition.cpp
    src/construction.cpp
    src/geometry_evidence.cpp
    src/ground_pressure.cpp
    src/mass.cpp
    src/material_intent.cpp
    src/surface_ledger.cpp
    src/twin.cpp
)

target_include_directories(carto_physical
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(carto_physical
    PUBLIC
        carto_core
        carto_assets
        carto_geometry
        carto_surface_core
)
carto_target(carto_physical)

install(TARGETS carto_physical EXPORT CartographerTargets)
install(DIRECTORY include/ DESTINATION include)
