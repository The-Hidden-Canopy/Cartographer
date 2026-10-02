add_library(carto_device STATIC
    src/device.cpp
    src/kernel_evidence.cpp
)

target_include_directories(carto_device
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(carto_device PUBLIC carto_core carto_gpu carto_device_ir)
carto_target(carto_device)

install(TARGETS carto_device EXPORT CartographerTargets)
install(DIRECTORY include/ DESTINATION include)
