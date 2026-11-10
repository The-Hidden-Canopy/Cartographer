add_library(carto_assets STATIC
    src/asset_lock.cpp
    src/blob_store.cpp
    src/texture.cpp
    src/texture_atlas.cpp
    src/texture_channel_pack.cpp
    src/texture_image_decoder.cpp
    src/texture_mip.cpp
    src/texture_paint.cpp
    src/texture_source.cpp
    src/texture_decode.cpp
    src/hdr_environment.cpp
    src/source_registry.cpp
)

if(WIN32)
    target_link_libraries(carto_assets PRIVATE windowscodecs ole32)
endif()

target_include_directories(carto_assets
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(carto_assets PUBLIC carto_core carto_botanical_contract)
if(WIN32)
    target_link_libraries(carto_assets PRIVATE windowscodecs ole32)
endif()
carto_target(carto_assets)

install(TARGETS carto_assets EXPORT CartographerTargets)
install(DIRECTORY include/ DESTINATION include)
