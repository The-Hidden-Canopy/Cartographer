add_library(carto_web_geometry_evaluation STATIC
    src/evaluation.cpp
)

target_include_directories(carto_web_geometry_evaluation
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(carto_web_geometry_evaluation
    PUBLIC
        carto_web_geometry_core
        carto_web_geometry_package
        carto_eval
)

carto_target(carto_web_geometry_evaluation)

install(TARGETS carto_web_geometry_evaluation EXPORT CartographerTargets)
install(DIRECTORY include/ DESTINATION include)
