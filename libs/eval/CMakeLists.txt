add_library(carto_eval STATIC
    src/cache.cpp
    src/graph.cpp
    src/node.cpp
    src/receipt.cpp
    src/scheduler.cpp
)

target_include_directories(carto_eval
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(carto_eval PUBLIC carto_core carto_assets)
carto_target(carto_eval)

install(TARGETS carto_eval EXPORT CartographerTargets)
install(DIRECTORY include/ DESTINATION include)
