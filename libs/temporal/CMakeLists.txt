add_library(carto_temporal STATIC
    src/channel.cpp
    src/events.cpp
    src/geometry_sequence.cpp
    src/mechanical.cpp
    src/root_motion.cpp
    src/sampling.cpp
    src/sequence.cpp
)

target_include_directories(carto_temporal
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(carto_temporal PUBLIC carto_core carto_assets)
carto_target(carto_temporal)

install(TARGETS carto_temporal EXPORT CartographerTargets)
install(DIRECTORY include/ DESTINATION include)
