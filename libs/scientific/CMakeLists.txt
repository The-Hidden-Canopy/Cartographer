add_library(carto_scientific STATIC
    src/model.cpp
)

target_include_directories(carto_scientific
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(carto_scientific PUBLIC carto_core carto_assets)
carto_target(carto_scientific)

install(TARGETS carto_scientific EXPORT CartographerTargets)
install(DIRECTORY include/ DESTINATION include)
