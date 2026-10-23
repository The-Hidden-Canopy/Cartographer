add_library(carto_gpudef STATIC
    src/capability.cpp
    src/deform_cases.cpp
    src/emulation.cpp
    src/evidence.cpp
    src/layout.cpp
    src/parity.cpp
    src/perf.cpp
    src/throughput.cpp
)

target_include_directories(carto_gpudef
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(carto_gpudef
    PUBLIC
        carto_core
        carto_geometry
        carto_kinematic
        carto_character
        carto_deform
)
carto_target(carto_gpudef)

install(TARGETS carto_gpudef EXPORT CartographerTargets)
install(DIRECTORY include/ DESTINATION include)
