add_executable(cartographer_d3d12_desktop WIN32
    main.cpp
)

target_link_libraries(cartographer_d3d12_desktop
    PRIVATE
        carto_d3d12
        user32
        gdi32
)

carto_target(cartographer_d3d12_desktop)

install(TARGETS cartographer_d3d12_desktop
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
