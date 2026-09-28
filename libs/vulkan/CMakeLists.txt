add_library(carto_vulkan STATIC
    src/runtime.cpp
)

target_include_directories(carto_vulkan
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(carto_vulkan
    PUBLIC
        carto_core
        carto_render
        Vulkan::Vulkan
)

carto_target(carto_vulkan)

install(TARGETS carto_vulkan EXPORT CartographerTargets)
install(DIRECTORY include/ DESTINATION include)
