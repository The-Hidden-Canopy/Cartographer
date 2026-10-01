add_library(carto_ai STATIC
    src/ai.cpp
)

target_include_directories(carto_ai
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(carto_ai PUBLIC carto_core carto_editor)
carto_target(carto_ai)

install(TARGETS carto_ai EXPORT CartographerTargets)
install(DIRECTORY include/ DESTINATION include)
