add_library(carto_journal STATIC
    src/checkpoint.cpp
    src/journal.cpp
)

target_include_directories(carto_journal
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(carto_journal PUBLIC carto_core carto_assets)
carto_target(carto_journal)

install(TARGETS carto_journal EXPORT CartographerTargets)
install(DIRECTORY include/ DESTINATION include)
