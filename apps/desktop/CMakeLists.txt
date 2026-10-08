set(CARTO_IMGUI_SOURCES
    ${CARTO_IMGUI_ROOT}/imgui.cpp
    ${CARTO_IMGUI_ROOT}/imgui_draw.cpp
    ${CARTO_IMGUI_ROOT}/imgui_tables.cpp
    ${CARTO_IMGUI_ROOT}/imgui_widgets.cpp
    ${CARTO_IMGUI_ROOT}/backends/imgui_impl_win32.cpp
    ${CARTO_IMGUI_ROOT}/backends/imgui_impl_vulkan.cpp
)

add_library(carto_imgui STATIC ${CARTO_IMGUI_SOURCES})
target_include_directories(carto_imgui PUBLIC
    ${CARTO_IMGUI_ROOT}
    ${CARTO_IMGUI_ROOT}/backends
)
target_link_libraries(carto_imgui PUBLIC Vulkan::Vulkan user32 gdi32 shell32 dwmapi)

add_executable(cartographer_desktop WIN32
    main.cpp
)

target_link_libraries(cartographer_desktop
    PRIVATE
        carto_application
        carto_ui
        carto_io
        carto_vulkan
        carto_imgui
        Vulkan::Vulkan
        user32
        gdi32
        shell32
        dwmapi
)

target_include_directories(cartographer_desktop PRIVATE
    ${CARTO_IMGUI_ROOT}
    ${CARTO_IMGUI_ROOT}/backends
)

carto_target(cartographer_desktop)

install(TARGETS cartographer_desktop RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
