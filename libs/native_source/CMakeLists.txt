add_library(carto_native_source STATIC src/project_mesh_source.cpp)
target_include_directories(carto_native_source PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_link_libraries(carto_native_source PUBLIC carto_project carto_production carto_engineering)
carto_target(carto_native_source)
install(TARGETS carto_native_source EXPORT CartographerTargets)
install(DIRECTORY include/ DESTINATION include)
