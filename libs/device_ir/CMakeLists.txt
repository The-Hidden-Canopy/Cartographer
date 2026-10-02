add_library(carto_device_ir STATIC
    src/command.cpp
)

target_include_directories(carto_device_ir
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(carto_device_ir PUBLIC carto_core carto_gpu)
carto_target(carto_device_ir)

install(TARGETS carto_device_ir EXPORT CartographerTargets)
install(DIRECTORY include/ DESTINATION include)
