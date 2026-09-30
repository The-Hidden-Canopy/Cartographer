add_executable(cartographer_gmib_game
    main.cpp
)

target_link_libraries(cartographer_gmib_game PRIVATE carto_simulation)
carto_target(cartographer_gmib_game)

install(TARGETS cartographer_gmib_game RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
