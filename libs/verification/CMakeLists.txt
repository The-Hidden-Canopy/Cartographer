add_library(carto_verification STATIC
    src/capability_ledger.cpp
    src/evidence_report.cpp
    src/failure_injection.cpp
    src/regression_gate.cpp
)

target_include_directories(carto_verification
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(carto_verification
    PUBLIC
        carto_core
        carto_assets
        carto_project
        carto_production
)
carto_target(carto_verification)

install(TARGETS carto_verification EXPORT CartographerTargets)
install(DIRECTORY include/ DESTINATION include)
