add_library(carto_condition_authoring STATIC
    src/authoring.cpp
)

target_include_directories(carto_condition_authoring
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(carto_condition_authoring PUBLIC carto_core carto_assets)
carto_target(carto_condition_authoring)

install(TARGETS carto_condition_authoring EXPORT CartographerTargets)
install(DIRECTORY include/ DESTINATION include)
