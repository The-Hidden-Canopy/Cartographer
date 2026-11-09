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
    native_presentation.hpp
    native_upload_lifetime.hpp
    bake_workflow.hpp
    branding.hpp
    reference_image_decoder.cpp
    reference_image_decoder.hpp
    reference_image_projection.hpp
    carto_studio_dock.hpp
    studio_style.hpp
    studio_draft.hpp
    studio_chrome.hpp
    studio_file_picker.cpp
    studio_file_picker.hpp
    studio_file_picker_rules.hpp
    studio_file_picker_paths.hpp
    studio_native_chrome.hpp
    studio_browser.hpp
    studio_scene_camera.hpp
    studio_viewshed.hpp
    studio_view_settings.hpp
    studio_viewport_capture.hpp
    studio_sweep_source.hpp
    uv_inspector.hpp
)

target_link_libraries(cartographer_desktop
    PRIVATE
        carto_application
        carto_assets
        carto_ui
        carto_io
        carto_production
        carto_vulkan
        carto_imgui
        Vulkan::Vulkan
        user32
        gdi32
        shell32
        shlwapi
        dwmapi
        winmm
        windowscodecs
        ole32
)

if(CARTO_ENABLE_D3D12)
    target_link_libraries(cartographer_desktop PRIVATE carto_d3d12_scene)
    target_compile_definitions(cartographer_desktop PRIVATE
        CARTO_DESKTOP_HAS_D3D12_SCENE=1)
endif()

if(TARGET carto_d3d12_bake)
    target_link_libraries(cartographer_desktop PRIVATE carto_d3d12_bake)
    target_compile_definitions(cartographer_desktop PRIVATE
        CARTO_DESKTOP_HAS_D3D12_BAKE=1)
endif()

target_include_directories(cartographer_desktop PRIVATE
    ${CARTO_IMGUI_ROOT}
    ${CARTO_IMGUI_ROOT}/backends
)

carto_target(cartographer_desktop)

install(TARGETS cartographer_desktop RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
