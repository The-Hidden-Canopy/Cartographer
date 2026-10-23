add_library(carto_device_cpu STATIC
    src/cpu_device.cpp
)

target_include_directories(carto_device_cpu
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(carto_device_cpu PUBLIC carto_core carto_gpu carto_device_ir carto_device)
carto_target(carto_device_cpu)

install(TARGETS carto_device_cpu EXPORT CartographerTargets)
install(DIRECTORY include/ DESTINATION include)
