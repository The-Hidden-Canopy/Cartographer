add_library(carto_botanical_contract STATIC src/source.cpp)
target_include_directories(carto_botanical_contract PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_link_libraries(carto_botanical_contract PUBLIC carto_core)
carto_target(carto_botanical_contract)
install(TARGETS carto_botanical_contract EXPORT CartographerTargets)
install(DIRECTORY include/ DESTINATION include)
