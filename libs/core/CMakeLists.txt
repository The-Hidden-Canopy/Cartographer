add_library(carto_core STATIC
    src/core.cpp
)

target_include_directories(carto_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

carto_target(carto_core)

install(TARGETS carto_core EXPORT CartographerTargets)
install(DIRECTORY include/ DESTINATION include)

