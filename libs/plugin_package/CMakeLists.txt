add_library(carto_plugin_package STATIC
    src/manifest.cpp
    src/package.cpp
    src/sha512.cpp
    src/ed25519.cpp
    src/canonical_json.cpp
    src/signature.cpp
    src/install.cpp
)

target_include_directories(carto_plugin_package
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(carto_plugin_package PUBLIC carto_core carto_assets carto_plugin_protocol carto_providers)
carto_target(carto_plugin_package)

install(TARGETS carto_plugin_package EXPORT CartographerTargets)
install(DIRECTORY include/ DESTINATION include)
