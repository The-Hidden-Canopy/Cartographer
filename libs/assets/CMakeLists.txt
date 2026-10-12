add_library(carto_assets STATIC
    src/blob_store.cpp
    src/texture.cpp
    src/texture_mip.cpp
    src/texture_source.cpp
)

target_include_directories(carto_assets
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(carto_assets PUBLIC carto_core)
carto_target(carto_assets)

install(TARGETS carto_assets EXPORT CartographerTargets)
install(DIRECTORY include/ DESTINATION include)
