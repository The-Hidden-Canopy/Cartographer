add_library(carto_render STATIC
    src/gpu_abi.cpp
    src/gpu_plan.cpp
    src/lighting.cpp
    src/material.cpp
    src/render_request.cpp
    src/render_scene.cpp
    src/temporal.cpp
    src/viewport.cpp
)

target_include_directories(carto_render
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(carto_render PUBLIC carto_core carto_assets carto_scene carto_geometry carto_render_graph carto_device)
carto_target(carto_render)

install(TARGETS carto_render EXPORT CartographerTargets)
install(DIRECTORY include/ DESTINATION include)
