add_library(carto_production STATIC
    src/closure.cpp
    src/dependency.cpp
)

target_include_directories(carto_production
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(carto_production PUBLIC carto_core carto_assets)
carto_target(carto_production)

install(TARGETS carto_production EXPORT CartographerTargets)
install(DIRECTORY include/ DESTINATION include)
