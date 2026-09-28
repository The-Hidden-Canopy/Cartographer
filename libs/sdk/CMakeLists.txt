add_library(carto_sdk STATIC
    src/carto.cpp
)

target_include_directories(carto_sdk
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(carto_sdk PUBLIC carto_application)
carto_target(carto_sdk)

install(TARGETS carto_sdk EXPORT CartographerTargets)
install(DIRECTORY include/ DESTINATION include)
