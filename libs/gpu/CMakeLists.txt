add_library(carto_gpu STATIC
    src/gpu.cpp
    src/lifetime.cpp
)

target_include_directories(carto_gpu
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(carto_gpu PUBLIC carto_core)
carto_target(carto_gpu)

install(TARGETS carto_gpu EXPORT CartographerTargets)
install(DIRECTORY include/ DESTINATION include)
