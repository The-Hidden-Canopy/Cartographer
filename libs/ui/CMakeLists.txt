add_library(carto_ui STATIC
    src/ui.cpp
)

target_include_directories(carto_ui
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(carto_ui PUBLIC carto_ai carto_application carto_core)
carto_target(carto_ui)

install(TARGETS carto_ui EXPORT CartographerTargets)
install(DIRECTORY include/ DESTINATION include)
