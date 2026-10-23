add_library(carto_market STATIC
    src/market.cpp
)

target_include_directories(carto_market
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(carto_market PUBLIC carto_core carto_assets carto_plugin_package)
carto_target(carto_market)

install(TARGETS carto_market EXPORT CartographerTargets)
install(DIRECTORY include/ DESTINATION include)
