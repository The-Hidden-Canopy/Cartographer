add_library(carto_surface_core STATIC
    src/budget.cpp
    src/candidate.cpp
    src/constraint_policy.cpp
    src/core.cpp
    src/distribution.cpp
    src/evidence.cpp
    src/fidelity_package_json.cpp
    src/fracture_policy.cpp
    src/invalidation.cpp
    src/inheritance.cpp
    src/normal_closure.cpp
    src/process.cpp
    src/qem.cpp
    src/spectrum.cpp
    src/visibility_closure.cpp
)

target_include_directories(carto_surface_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(carto_surface_core
    PUBLIC
        carto_assets
        carto_geometry
)

carto_target(carto_surface_core)

install(TARGETS carto_surface_core EXPORT CartographerTargets)
install(DIRECTORY include/ DESTINATION include)
