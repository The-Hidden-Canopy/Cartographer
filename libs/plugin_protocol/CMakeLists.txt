add_library(carto_plugin_protocol STATIC
    src/protocol.cpp
)

target_include_directories(carto_plugin_protocol
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(carto_plugin_protocol PUBLIC carto_core carto_providers)
carto_target(carto_plugin_protocol)

install(TARGETS carto_plugin_protocol EXPORT CartographerTargets)
install(DIRECTORY include/ DESTINATION include)
