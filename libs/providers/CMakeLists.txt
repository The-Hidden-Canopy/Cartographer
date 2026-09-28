add_library(carto_providers STATIC
    src/registry.cpp
)

target_include_directories(carto_providers
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(carto_providers PUBLIC carto_core)
carto_target(carto_providers)

install(TARGETS carto_providers EXPORT CartographerTargets)
install(DIRECTORY include/ DESTINATION include)
