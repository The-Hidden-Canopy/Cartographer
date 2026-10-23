add_library(carto_runtime_api STATIC
    src/client.cpp
    src/winhttp_transport.cpp
)

target_include_directories(carto_runtime_api
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(carto_runtime_api PUBLIC
    carto_core carto_assets carto_botanical_contract)
if(WIN32)
    target_link_libraries(carto_runtime_api PRIVATE winhttp)
endif()
carto_target(carto_runtime_api)

install(TARGETS carto_runtime_api EXPORT CartographerTargets)
install(DIRECTORY include/ DESTINATION include)
